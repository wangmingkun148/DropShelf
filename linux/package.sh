#!/usr/bin/env bash
# 便携版：把可执行文件、桌面文件、图标和许可证打成一个解压即用的 tar.gz。
# 用法：linux/package.sh [输出目录]（默认是仓库根目录下的 dist）
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/linux/build"
out="${1:-$root/dist}"

cmake -S "$root/linux" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build"

version="$(sed -n 's:.*project(DropShelf VERSION \([0-9.]*\) LANGUAGES.*:\1:p' "$root/native/CMakeLists.txt" | head -1)"
if [[ -z "$version" ]]; then
    echo "读不到版本号" >&2
    exit 1
fi

name="DropShelf-$version-linux-$(uname -m)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
target="$stage/$name"
mkdir -p "$target/bin" "$target/share/applications" "$target/share/metainfo" "$target/share/icons" "$target/share/dropshelf/fonts" "$target/share/licenses/dropshelf"

install -Dm755 "$build/dropshelf" "$target/bin/dropshelf"
install -Dm644 "$root/native/platform/linux/fonts/DropShelfIcons.ttf" "$target/share/dropshelf/fonts/DropShelfIcons.ttf"
install -Dm644 "$root/native/platform/linux/fonts/DropShelfIcons-LICENSE.txt" "$target/share/licenses/dropshelf/DropShelfIcons-LICENSE.txt"
install -Dm644 "$root/linux/io.github.wangmingkun148.DropShelf.desktop" "$target/share/applications/"
install -Dm644 "$root/linux/io.github.wangmingkun148.DropShelf.metainfo.xml" "$target/share/metainfo/"
cp -r "$root/linux/icons/hicolor" "$target/share/icons/" 2>/dev/null || true
install -Dm644 "$root/LICENSE" "$target/share/licenses/dropshelf/LICENSE"
install -Dm644 "$root/README.md" "$target/README.md" 2>/dev/null || true
cp "$root/linux/README-linux.md" "$target/" 2>/dev/null || true

mkdir -p "$out"
tar -C "$stage" -czf "$out/$name.tar.gz" "$name"
echo "$out/$name.tar.gz"