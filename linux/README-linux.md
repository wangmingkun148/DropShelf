# DropShelf on Linux (Wayland)

这是 DropShelf 的原生 Wayland 版本：C + GTK 4 + gtk4-layer-shell，不经过 X11 兼容层。
置物架、探角、菜单都是 layer-shell 的 overlay 层，天然置顶；主窗口、引导和“关于”是普通窗口。

## 依赖

- GTK 4（4.14 或更新的版本）
- gtk4-layer-shell
- gdk-pixbuf
- fontconfig（运行时注册随程序附带的图标字体）
- libcurl（检查更新）
- libarchive / `bsdtar`（解压 7z；ZIP 用内置的 miniz，不需要这个）
- gdk-pixbuf 的图片加载器：JPEG、PNG、WebP 一般在 gdk-pixbuf2 里，
  HEIC/AVIF 需要系统装了对应的加载器（如 libheif、libavif）
- 支持 wlr-layer-shell 的合成器：Hyprland / Sway / KDE（KDE 需要 compositor 支持）

Arch：

```bash
sudo pacman -S --needed gtk4 gtk4-layer-shell gdk-pixbuf2 libcurl libarchive fontconfig
```

## 构建和运行

```bash
cmake -S linux -B linux/build -G Ninja
cmake --build linux/build
./linux/build/dropshelf --shelf        # 打开一个置物架
./linux/build/dropshelf                # 后台运行（托盘、手势、观察文件夹）
ctest --test-dir linux/build --output-on-failure
```

安装到 `/usr/local`：

```bash
sudo cmake --install linux/build
```

便携版：

```bash
linux/package.sh        # 生成 dist/DropShelf-<版本>-linux-<架构>.tar.gz
```

Arch 包（本地源码目录，版本标签发布后也可以用 makepkg）：

```bash
cd linux && makepkg -si
```

## 用法

- 启动后在后台常驻，托盘图标（StatusNotifierItem）可以新建置物架、打开主窗口或退出。
- 全局快捷键：优先用 xdg-desktop-portal 的 GlobalShortcuts；Hyprland 上如果 portal
  只登记了名字而没有真正绑定，程序会用 `hyprctl` 在运行时绑定 `<组合键>`，
  bind 的说明是 `DropShelf`。重复启动会清掉上一次留下的绑定。
- 鼠标手势：读取 `/dev/input/event*`（用户需要在 `input` 组里），光标位置来自
  Hyprland IPC。没有权限时自动退回快捷键。
- 观察文件夹：设置 → 手势与置物架 → 观察文件夹。文件夹里出现新文件时，
  光标所在的屏幕边缘会探出置物架的一角；点一下展开，右键收起。
- 拖放：从文件管理器拖入；拖出用 GTK 的拖动协议，移动/复制由协商的动作决定。
- 主窗口、引导和“关于”是固定布局的普通窗口（不是层壳），标题栏由程序自己画：
  按住标题栏空白处可以拖动，双击切换最大化，最小化/最大化/关闭按钮在标题栏右侧。
- Hyprland 默认把普通窗口平铺；程序启动时会用 `hyprctl` 给 `<app_id>` 加一条运行时
  `float` 规则（不改配置文件，重载配置后消失），主窗口和引导因此以浮动窗口出现。
  其它平铺式合成器需要自己加规则，例如 Sway：
  `for_window [app_id="io.github.wangmingkun148.DropShelf"] floating enable`。

## 和 Windows 版的差异

- **Mica/Acrylic**：Wayland 没有对应的系统材质；主窗口是半透明的纯色背景。
  想要模糊可以在 Hyprland 里加 `layerrule = blur, dropshelf`（只影响 layer 层）。
- **窗口位置**：普通 Wayland 客户端不能指定主窗口的位置，由合成器决定；
  设置里记住的位置只对层壳窗口（置物架、探角）有用。
- **拖到桌面空白处**：纯 Wayland 桌面没有接收拖放的桌面 surface；请拖进文件管理器窗口。
- **拖出时没有跟手的缩略图**：Wayland 没有 drag-icon 协议。
- **前台程序屏蔽**：只能拿到 app_id/class，Windows 上的进程名匹配不适用。
- **快速操作**：内置的压缩、复制路径、移动到、在文件管理器中显示和属性，
  以及自定义的复制/移动到文件夹、压缩图片、转换图片、用程序打开都可以用。
- **属性窗口**：Linux 没有系统提供的文件属性对话框，属性窗口是程序自绘的，
  显示名称、类型、位置、大小和修改时间；文件夹大小在后台统计，没有 Windows 那些扩展属性页。
- **电源**：窗口显示或有拖动操作时，用 `systemd-inhibit` 阻止系统因为空闲而锁屏、休眠；
  没有 systemd-inhibit（或不在 systemd 会话里）时自动跳过，显示器休眠仍按桌面设置。
- **输入法**：自绘输入框有焦点时会在上面盖一个真正的 `GtkEntry`，中文输入可用。
- **卸载**：用发行版的包管理器；没有 `unins000.exe` 那样的卸载程序。

## 字体和图标

界面里的图标不是按码位找系统字体，而是随程序带一份 `DropShelfIcons.ttf`：
它是 WinSymbols3 的子集（只保留 DropShelf 用到的 53 个字形，族名改成 “DropShelf Icons”），
启动时通过 fontconfig 注册，所以不需要安装任何字体。找不到字体文件时，图标会显示成小方块或圆点，
文字部分不受影响。字体是 MIT 许可，许可证随包附带。

## 配置和数据

- 设置：`$XDG_CONFIG_HOME/dropshelf/settings.json`
- 历史：`$XDG_DATA_HOME/dropshelf/history.json`
- 日志：`$XDG_STATE_HOME/dropshelf/logs/`
- 开机自启：`$XDG_CONFIG_HOME/autostart/io.github.wangmingkun148.DropShelf.desktop`

调试用环境变量：

- `DROPSHELF_LOG_DIRECTORY`：改日志目录
- `DROPSHELF_TRACE=1`：更详细的日志
- `DROPSHELF_POINTER=x,y`：假装光标在指定位置（测试不同显示器时用）
- `DROPSHELF_DEBUG_SIZE=1`：每帧打印窗口和排版大小