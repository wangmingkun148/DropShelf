#!/usr/bin/env bash
# 从构建结果生成 Linux 发行版安装包：AppImage、deb、rpm、pacman（.pkg.tar.zst）。
# 用法：linux/package-formats.sh [构建目录] [输出目录]（默认 linux/build 和 dist）
# nfpm、linuxdeploy、appimagetool 和 GTK 插件会自动下载到构建目录下的 packaging-tools。
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/linux/build}"
out="${2:-$root/dist}"
tools="$build/packaging-tools"
arch="$(uname -m)"
case "$arch" in
    x86_64) nfpm_arch=x86_64; package_arch=amd64 ;;
    aarch64) nfpm_arch=arm64; package_arch=arm64 ;;
    *) echo "不认识的架构：$arch" >&2; exit 1 ;;
esac

version="$(sed -n 's:.*<Version>\([0-9.]*\)</Version>.*:\1:p' "$root/src/DropShelf/DropShelf.csproj" | head -1)"
if [ -z "$version" ]; then
    echo "读不到版本号" >&2
    exit 1
fi
if [ ! -x "$build/dropshelf" ]; then
    echo "找不到 $build/dropshelf，先运行 cmake --build" >&2
    exit 1
fi

mkdir -p "$tools" "$out"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

download() { curl -fsSL --retry 3 --max-time 300 -o "$2" "$1"; }

# 工具下载到构建目录里复用；nfpm 固定版本，linuxdeploy/appimagetool 用上游 continuous。
if [ ! -x "$tools/nfpm" ]; then
    download "https://github.com/goreleaser/nfpm/releases/download/v2.49.0/nfpm_2.49.0_Linux_${nfpm_arch}.tar.gz" "$tmp/nfpm.tar.gz"
    tar -xzf "$tmp/nfpm.tar.gz" -C "$tmp" nfpm
    install -m755 "$tmp/nfpm" "$tools/nfpm"
fi
if [ ! -x "$tools/linuxdeploy" ]; then
    # linuxdeploy 用 continuous：固定版本 1-alpha-20251107-1 里的 patchelf 会给带
    # .relr.dyn 的库写入损坏的重定位表，打出来的 AppImage 启动即段错误。
    download "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage" "$tools/linuxdeploy"
    chmod +x "$tools/linuxdeploy"
fi
if [ ! -x "$tools/appimagetool" ]; then
    # 和 linuxdeploy 一样用 continuous：1.9.1 打包时会另行下载 type2 runtime，离线/限流时直接失败。
    download "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${arch}.AppImage" "$tools/appimagetool"
    chmod +x "$tools/appimagetool"
fi
if [ ! -x "$tools/linuxdeploy-plugin-gtk" ]; then
    download "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/7a3fbc31a9e5075073ff8790f26effbac5f84453/linuxdeploy-plugin-gtk.sh" "$tools/linuxdeploy-plugin-gtk"
    chmod +x "$tools/linuxdeploy-plugin-gtk"
fi

# 安装树：包里的路径和 cmake --install 一致。
stage="$tmp/stage"
mkdir -p "$stage"
DESTDIR="$stage" cmake --install "$build" --prefix /usr >/dev/null

desktop="io.github.wangmingkun148.DropShelf.desktop"
icon="io.github.wangmingkun148.DropShelf.png"

# ── deb / rpm / pacman（nfpm） ──
cat > "$tmp/nfpm.yaml" <<EOF
name: dropshelf
arch: $package_arch
platform: linux
version: $version
release: "1"
section: utils
priority: optional
maintainer: DropShelf Project <noreply@github.com>
description: |
  A temporary file shelf for Wayland desktops.
  Shake the mouse while dragging and a shelf pops up.
vendor: DropShelf
homepage: https://github.com/pingod/DropShelf
license: MIT
contents:
  - src: $stage/usr
    dst: /usr
overrides:
  deb:
    depends:
      - libgtk-4-1
      - libgtk4-layer-shell0
      - libgdk-pixbuf-2.0-0
      - libarchive13 | libarchive13t64
      - libcurl4 | libcurl4t64
      - fontconfig
  rpm:
    depends:
      - gtk4
      - gtk4-layer-shell
      - gdk-pixbuf2
      - libarchive
      - libcurl
      - fontconfig
  archlinux:
    depends:
      - gtk4
      - gtk4-layer-shell
      - gdk-pixbuf2
      - libarchive
      - curl
      - fontconfig
EOF
for format in deb rpm archlinux; do
    "$tools/nfpm" package --config "$tmp/nfpm.yaml" --packager "$format" --target "$out"
done

# ── AppImage ──
appdir="$tmp/AppDir"
mkdir -p "$appdir"
cp -a "$stage/usr" "$appdir/usr"

# linuxdeploy 把 --custom-apprun 装成 AppRun.wrapped，外面那层 AppRun 会先套 GTK 钩子；
# 钩子为 GTK3 设了 GDK_BACKEND=x11，DropShelf 是 Wayland 程序，这里最后把它去掉。
cat > "$tmp/AppRun" <<'EOF'
#!/usr/bin/env bash
set -e
this_dir="$(readlink -f "$(dirname "$0")")"
unset GDK_BACKEND
exec "$this_dir/usr/bin/dropshelf" "$@"
EOF
chmod +x "$tmp/AppRun"

# GTK 4 本身不带 /usr/lib/gtk-4.0 模块目录（桌面上通常由 fcitx 等输入法包提供），
# 干净环境里 GTK 插件复制这个目录会失败，缺的时候先建一个空的。
gtk4_libdir="$(pkg-config --variable=libdir gtk4)"
mkdir -p "$gtk4_libdir/gtk-4.0" 2>/dev/null || true

# linuxdeploy 自带的 strip 是旧版 binutils，遇到 .relr.dyn 会报 “unknown type [0x13] section”，
# 所以让它别剥离（NO_STRIP）；continuous 的 patchelf 能正确处理这种库。
PATH="$tools:$PATH" APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1 DEPLOY_GTK_VERSION=4 \
    "$tools/linuxdeploy" --appdir "$appdir" \
    --executable "$build/dropshelf" \
    --desktop-file "$stage/usr/share/applications/$desktop" \
    --icon-file "$stage/usr/share/icons/hicolor/256x256/apps/$icon" \
    --custom-apprun "$tmp/AppRun" \
    --plugin gtk

ARCH="$arch" APPIMAGE_EXTRACT_AND_RUN=1 \
    "$tools/appimagetool" "$appdir" "$out/DropShelf-$version-$arch.AppImage"

ls -lh "$out"