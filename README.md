# DropShelf for Windows

DropShelf 是一个 Windows 临时文件置物架。拖住文件并晃动鼠标，置物架就会出现在光标处；先把文件放进来，切换窗口或文件夹后，再从置物架拖到目标位置、聊天软件或浏览器。

## 使用方法

1. 运行 `artifacts\installer\DropShelf-Setup-2.0.0.exe` 安装；也可以直接运行便携版 `artifacts\win-x64\DropShelf.exe`。
2. 关闭主窗口后，程序继续在任务栏通知区域运行。
3. 在资源管理器、桌面、浏览器或聊天软件里拖住文件，快速来回晃动鼠标，置物架会出现在光标处。
4. 把文件放进置物架。之后拖动缩略图堆叠，可一次带走全部文件；点击堆叠展开列表，可以单独或多选拖出。
5. 也可以按 `Ctrl+Shift+Space`（可在设置中修改）随时打开空置物架，再按 `Ctrl+V` 粘贴文件、图片或文本。

## 功能

### 呼出置物架
- 由独立线程上的低级鼠标钩子识别晃动，不再定时轮询，空闲时几乎不占用资源。
- 识别到晃动后，在光标下放置一个肉眼不可见的探针窗口，由系统拖放机制确认确实在拖动文件、附件、图片或文本后才弹出置物架。按住鼠标晃动但没有拖东西时不会误弹。
- 支持左右、上下和斜向晃动，提供灵敏、均衡、稳健三档；可随时关闭晃动呼出，只用快捷键。
- 对高回报率鼠标自动重新取样，掉头时的细小侧移不会打断识别。

### 置物架
- 收起时显示缩略图堆叠、数量角标和总大小；拖动堆叠即可整摞拖出。
- 展开列表显示文件名、大小和 Windows 文件类型，支持 Ctrl/Shift 多选、拖出所选、双击打开、右键菜单。
- 拖出使用与资源管理器相同的数据格式和拖动图像（缩略图堆叠 + 数量）。能正确识别资源管理器在同一磁盘“优化移动”的情况，被移走的项目会自动从置物架移除。
- 拖入时显示来源程序的拖动图像和“添加到置物架”等提示；不支持的内容会直接提示。
- 支持文件、文件夹、尚未落盘的附件（浏览器、邮件、聊天软件）、网页链接、图片和文本。
- 可同时打开多个置物架；可以钉住，清空后也不会关闭。
- 快捷键：`Esc` 收起或关闭、`Delete` 移除所选、`Ctrl+A` 全选、`Ctrl+C` 以文件形式复制、`Ctrl+V` 粘贴、`Enter` 打开。
- 小、中、大三档尺寸各自重新排版，文字不会被缩小。

### 快速操作
- 置物架底部最多显示四个快速操作：压缩/解压、复制路径、移动到、属性、显示位置。
- 点击按钮处理置物架中的项目（展开后只处理选中项）；拖动文件时也可以直接放到按钮上。
- 压缩生成的 ZIP 会放回置物架，方便直接拖走。压缩、解压、复制都不会覆盖已有的同名项目。
- 单独放入 ZIP 或 7z 时解压；7z 使用 Windows 自带的 tar，旧版 Windows 不支持时会提示。

### 设置与外观
- Windows 11 风格的设置窗口：云母背景、侧边导航、设置卡片、开关和分段选择。
- 置物架在 Windows 11 上使用亚克力半透明材质和系统圆角；按钮、开关和选中状态使用 Windows 强调色，并自动保证文字对比度。
- 支持跟随系统、浅色和深色三种主题。所有设置修改后立即生效并自动保存。
- 首次运行显示带动画演示的四步教程，之后可在设置中重新查看。

### 系统集成
- 托盘图标：左键打开设置，右键可新建置物架、暂停晃动呼出、退出。
- 可选开机自动启动，启动时只在后台运行。
- 安装时可选添加到右键“发送到”菜单；把文件拖到程序图标上或用命令行 `DropShelf.exe --shelf 文件...` 也会打开置物架。程序已运行时会交给现有实例处理。
- 重复启动时唤醒已有窗口，不会出现第二个实例。

## 隐私与数据安全

- 置物架只记录文件路径，不读取也不复制文件内容；放入、移除、清空都不会改动磁盘文件。
- 文件拖出后是复制还是移动，由接收方按 Windows 规则决定；DropShelf 自己不会删除任何文件。
- 尚未落盘的附件和拖入的图片会暂存在系统临时目录，关闭对应置物架时清理；意外退出留下的旧缓存会在下次启动时清理。
- 关闭置物架后不保留文件记录。鼠标位置只在内存中用于识别手势，不会保存或上传。
- 程序以普通用户权限运行，不需要管理员权限。

## 安装与卸载

安装包默认安装到当前用户的本地程序目录（`%LOCALAPPDATA%\Programs\DropShelf`），不需要管理员权限，可直接覆盖升级 1.0 版，原有设置会自动迁移。卸载可通过“设置 > 应用 > 已安装的应用”、开始菜单，或程序目录中的 `卸载 DropShelf.exe`；卸载会关闭正在运行的 DropShelf，并移除程序文件、快捷方式和开机启动项，个人设置与日志默认保留。便携版没有安装记录，完全退出后删除文件夹即可。

## 数据位置

- 设置：`%LOCALAPPDATA%\DropShelf\settings.json`
- 运行日志：`%LOCALAPPDATA%\DropShelf\Logs`（自动删除 14 天前的日志）

## 系统要求

- Windows 10 或 Windows 11，64 位（云母、亚克力和系统圆角需要 Windows 11）
- 发布版 EXE 已包含运行环境，无需另行安装 .NET

## 从源码构建

项目使用 .NET 10 和 WPF，不依赖第三方 NuGet 包。

```powershell
dotnet build .\DropShelf.sln -c Release
dotnet run --project .\src\DropShelf\DropShelf.csproj
```

运行测试：

```powershell
dotnet run --project .\tests\DropShelf.Tests\DropShelf.Tests.csproj
```

生成便携版和安装包（需要 Inno Setup 6 或 7）：

```powershell
.\installer\Build-Installer.ps1
```

开发调试时可设置环境变量 `DROPSHELF_INSTANCE`（独立实例，可与已安装版本同时运行）、`DROPSHELF_SETTINGS_PATH` 和 `DROPSHELF_LOG_DIRECTORY`。

## 项目结构

- `src/DropShelf/AppController.cs`：应用总控，协调设置、手势、置物架、托盘和窗口。
- `src/DropShelf/Services/Gestures`：低级鼠标钩子与晃动识别。
- `src/DropShelf/Views/ProbeWindow.cs`：确认拖放内容的探针窗口。
- `src/DropShelf/Shelves/ShelfManager.cs`：多个置物架的创建、复用与关闭。
- `src/DropShelf/Views/ShelfWindow.xaml`：置物架界面；`MainWindow.xaml`：设置；`OnboardingWindow.xaml`：教程。
- `src/DropShelf/Services`：拖入数据解析、原生拖出、快速操作、剪贴板、缩略图、主题、托盘、快捷键等。
- `src/DropShelf/Themes`：颜色、图标与 Fluent 风格控件样式。
- `src/DropShelf/Interop`：Win32、DWM 与 Shell 接口。
- `tests/DropShelf.Tests`：自动化测试。
