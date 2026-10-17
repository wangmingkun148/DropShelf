using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.Localization;

/// <summary>
/// 界面文字。每一项同时写出中文和英文，按 <see cref="LanguageService"/> 的当前语言返回。
/// 代码直接读取属性；XAML 用 {l:Loc 属性名} 引用，切换语言后自动刷新。
/// </summary>
public static class Strings
{
    private static string T(string chinese, string english) => LanguageService.IsChinese ? chinese : english;

    private static string Count(int count, string one, string many) => count == 1 ? $"1 {one}" : $"{count} {many}";

    // ───────────── 通用 ─────────────

    public static string Shelf => T("置物架", "Shelf");
    public static string OpenShelf => T("打开置物架", "Open shelf");
    public static string CloseShelf => T("关闭置物架", "Close shelf");
    public static string NewShelf => T("新建置物架", "New shelf");
    public static string QuitApp => T("退出 DropShelf", "Quit DropShelf");
    public static string OpenSettings => T("打开设置", "Open settings");
    public static string OpenFolder => T("打开文件夹", "Open folder");
    public static string FollowSystem => T("跟随系统", "System default");
    public static string ToggleOn => T("开", "On");
    public static string ToggleOff => T("关", "Off");
    public static string UnexpectedError => T("操作失败，错误已写入日志。请重试。", "Something went wrong. The error was written to the log. Please try again.");

    public static string ItemCount(int count) => T($"{count} 个项目", Count(count, "item", "items"));
    public static string FileCount(int count) => T($"{count} 个文件", Count(count, "file", "files"));
    public static string FolderCount(int count) => T($"{count} 个文件夹", Count(count, "folder", "folders"));
    public static string TextCount(int count) => T($"{count} 段文本", Count(count, "snippet", "snippets"));
    public static string LinkCount(int count) => T($"{count} 个链接", Count(count, "link", "links"));

    // ───────────── 设置窗口 ─────────────

    public static string Overview => T("概览", "Overview");
    public static string GesturesAndShelf => T("手势与置物架", "Gestures & shelf");
    public static string QuickActionsTitle => T("快速操作", "Quick actions");
    public static string Appearance => T("外观", "Appearance");
    public static string About => T("关于", "About");
    public static string QuitTooltip => T("停止手势识别并退出，关闭所有置物架", "Stop gesture detection, close all shelves and quit");

    public static string StatusPaused => T("晃动呼出已暂停", "Shake to open is paused");
    public static string StatusRunning => T("正在后台运行", "Running in the background");
    public static string StatusGestureFailed => T("手势识别未能启动", "Gesture detection couldn't start");
    public static string StatusGestureFailedDetail => T("请重新启动 DropShelf；置物架和快捷键仍可使用", "Restart DropShelf. Shelves and the shortcut still work");

    public static string StatusPausedDetail(string? hotkey) => T(
        $"可在“{GesturesAndShelf}”中重新开启{(hotkey is null ? null : $"，或按 {hotkey} 打开置物架")}",
        $"Turn it back on in {GesturesAndShelf}{(hotkey is null ? null : $", or press {hotkey} to open a shelf")}");

    public static string StatusRunningDetail(string? hotkey) => T(
        $"拖住文件并快速晃动鼠标{(hotkey is null ? null : $"，或按 {hotkey} 打开置物架")}",
        $"Drag a file and shake the mouse quickly{(hotkey is null ? null : $", or press {hotkey} to open a shelf")}");

    public static string GettingStarted => T("快速上手", "Get started");
    public static string TipShakeTitle => T("拖住并晃动", "Drag and shake");
    public static string TipShakeBody => T("拖住文件后左右快速晃动鼠标，置物架会出现在光标处。",
        "While dragging a file, shake the mouse quickly left and right and a shelf appears at the cursor.");
    public static string TipStackTitle => T("整摞拖出", "Drag out the stack");
    public static string TipStackBody => T("拖动缩略图堆叠一次带走全部文件；点开后也能单独或多选拖出。",
        "Drag the thumbnail stack to take every file at once, or click it to drag out one or several.");
    public static string TipActionsBody => T("把文件拖到置物架底部的按钮上，直接压缩、复制路径或移动。",
        "Drop files on the buttons at the bottom of the shelf to compress, copy paths or move them right away.");

    public static string General => T("常规", "General");
    public static string Language => T("语言", "Language");
    public static string LanguageDescription => T("选择界面显示的语言，切换后立即生效", "Choose the display language. Changes apply immediately");
    public static string StartWithWindows => T("开机自动启动", "Start with Windows");
    public static string StartupDescription => T("登录 Windows 后在后台运行，不弹出窗口", "Runs in the background after you sign in, without opening a window");
    public static string StartupFailed => T("Windows 没有允许修改开机启动，设置保持不变", "Windows didn't allow the startup change, so the setting is unchanged");
    public static string GlobalHotkey => T("全局快捷键", "Global shortcut");
    public static string HotkeyNotSet => T("未设置快捷键", "No shortcut set");
    public static string HotkeyRegistered => T("在光标处打开空置物架，可直接按 Ctrl+V 粘贴", "Opens an empty shelf at the cursor. Press Ctrl+V to paste into it");
    public static string HotkeyTaken => T("这个快捷键已被其他程序占用，请换一个", "Another app is already using this shortcut. Please choose a different one");
    public static string Tutorial => T("使用教程", "Tutorial");
    public static string TutorialDescription => T("用动画快速了解晃动呼出、整摞拖出和快速操作", "Short animations of shake to open, dragging out the stack and quick actions");
    public static string ViewTutorial => T("查看教程", "View tutorial");

    public static string ShakeToOpen => T("晃动呼出", "Shake to open");
    public static string ShakeToOpenHeader => T("晃动鼠标呼出置物架", "Shake the mouse to open a shelf");
    public static string ShakeToOpenDescription => T("拖住文件、附件、图片或文本时，快速来回晃动鼠标",
        "While dragging files, attachments, images or text, shake the mouse quickly back and forth");
    public static string Sensitivity => T("灵敏度", "Sensitivity");
    public static string SensitivityDescription => T("灵敏只需小幅晃动；稳健需要更明确的晃动，误触更少",
        "Responsive needs only a small shake. Steady needs a clearer shake and triggers by accident less often");
    public static string SensitivityResponsive => T("灵敏", "Responsive");
    public static string SensitivityBalanced => T("均衡", "Balanced");
    public static string SensitivitySteady => T("稳健", "Steady");
    public static string Size => T("尺寸", "Size");
    public static string SizeDescription => T("三档尺寸保持相同的比例和布局", "All three sizes keep the same proportions and layout");
    public static string SizeSmall => T("小", "Small");
    public static string SizeMedium => T("中", "Medium");
    public static string SizeLarge => T("大", "Large");
    public static string RemoveAfterDragOut => T("拖出后从置物架移除", "Remove from the shelf after dragging out");
    public static string RemoveAfterDragOutDescription => T("关闭后可以把同一批文件依次拖到多个地方", "Turn off to drag the same files to several places in turn");
    public static string CloseWhenEmpty => T("清空后自动关闭", "Close when empty");
    public static string CloseWhenEmptyDescription => T("项目全部拖出或移除后关闭置物架；钉住的置物架会保持打开",
        "Closes the shelf once every item is dragged out or removed. Pinned shelves stay open");

    public static string ActionsPageDescription => T(
        "显示在置物架底部。点击按钮处理置物架中的项目（展开后只处理选中项）；拖动文件时，也可以直接放到对应按钮上。",
        "Shown at the bottom of the shelf. Click a button to apply it to the items on the shelf (only the selected ones when the list is expanded), or drop files straight onto a button while dragging.");
    public static string ActionsOff => T("已关闭快速操作，置物架底部不显示按钮", "Quick actions are off, so the shelf shows no buttons at the bottom");
    public static string ActionsSelected(int count, int maximum) => T($"已选择 {count} 个，最多 {maximum} 个", $"{count} selected, up to {maximum}");

    public static string Theme => T("主题", "Theme");
    public static string ThemeLight => T("浅色", "Light");
    public static string ThemeDark => T("深色", "Dark");
    public static string Color => T("颜色", "Color");
    public static string AccentColor => T("强调色", "Accent color");
    public static string AccentDescription => T("按钮、开关和选中状态使用 Windows 强调色", "Buttons, switches and selections use your Windows accent color");
    public static string Materials => T("材质效果", "Window materials");
    public static string MaterialsDescription => T("Windows 11 上，主窗口使用云母背景，置物架使用亚克力半透明效果",
        "On Windows 11, the main window uses Mica and shelves use translucent Acrylic");

    public static string Version(string? version) => T($"版本 {version}", $"Version {version}");
    public static string AboutTagline => T("通过鼠标手势呼出的临时文件置物架", "A temporary file shelf you open with a mouse gesture");
    public static string DataLocations => T("数据位置", "Data locations");
    public static string SettingsFile => T("设置文件", "Settings file");
    public static string Logs => T("运行日志", "Logs");

    // ───────────── 快捷键输入框 ─────────────

    public static string HotkeyNone => T("未设置", "Not set");
    public static string HotkeyPrompt => T("请按下快捷键…", "Press a shortcut…");
    public static string HotkeyNeedsModifier => T("需要搭配 Ctrl、Alt 或 Win", "Include Ctrl, Alt or Win");
    public static string ClearHotkey => T("清除快捷键", "Clear shortcut");

    // ───────────── 教程 ─────────────

    public static string OnboardingWelcomeTitle => T("欢迎使用 DropShelf", "Welcome to DropShelf");
    public static string OnboardingWelcomeBody => T("一个随叫随到的临时置物架：把文件先放进来，换个窗口、换个文件夹，再从这里拖出去。",
        "A temporary shelf that's there whenever you need it: put files on it, switch to another window or folder, then drag them back out.");
    public static string OnboardingShakeTitle => T("拖住文件，晃一晃", "Drag a file and shake");
    public static string OnboardingShakeBody => T("在资源管理器、桌面、浏览器或聊天软件里拖住文件，快速左右晃动鼠标，置物架就会出现在光标处。",
        "While dragging a file in File Explorer, on the desktop, in a browser or in a chat app, shake the mouse quickly left and right and a shelf appears at the cursor.");
    public static string OnboardingStackTitle => T("放进来，再拖出去", "Drop it in, drag it out");
    public static string OnboardingStackBody => T("松开即可暂存。拖动缩略图堆叠可一次带走全部文件；点开堆叠后也能多选，只拖走需要的部分。",
        "Release to hold the files. Drag the thumbnail stack to take everything at once, or click the stack to pick only the files you need.");
    public static string OnboardingActionsTitle => T("快速操作与快捷键", "Quick actions and shortcut");

    public static string OnboardingActionsBody(string? hotkey) => hotkey is null
        ? T("把文件拖到底部按钮上，可以直接压缩、复制路径、移动或查看属性。也可以在设置中添加全局快捷键，随时打开空置物架。",
            "Drop files on the buttons at the bottom to compress, copy paths, move them or view their properties. You can also add a global shortcut in Settings to open an empty shelf at any time.")
        : T($"把文件拖到底部按钮上，可以直接压缩、复制路径、移动或查看属性。按 {hotkey} 也能随时打开空置物架。",
            $"Drop files on the buttons at the bottom to compress, copy paths, move them or view their properties. Press {hotkey} to open an empty shelf at any time.");

    public static string OnboardingSampleCount => ItemCount(3);
    public static string OnboardingSampleFolder => T("文档", "Documents");
    public static string OnboardingSampleToast => Created(T("资料.zip", "Files.zip"));
    public static string Skip => T("跳过", "Skip");
    public static string Back => T("上一步", "Back");
    public static string Next => T("下一步", "Next");
    public static string GetStarted => T("开始使用", "Get started");

    // ───────────── 托盘 ─────────────

    public static string TrayTip => T("DropShelf · 拖住文件并晃动鼠标", "DropShelf · Drag a file and shake the mouse");
    public static string StillRunningTitle => T("DropShelf 仍在运行", "DropShelf is still running");
    public static string StillRunningBody => T("拖住文件并晃动鼠标即可呼出置物架。可从任务栏通知区域重新打开设置。",
        "Drag a file and shake the mouse to open a shelf. You can reopen Settings from the notification area.");
    public static string PauseShake => T("暂停晃动呼出", "Pause shake to open");

    // ───────────── 置物架 ─────────────

    public static string ShelfWindowTitle(string title) => T($"DropShelf 置物架 - {title}", $"DropShelf shelf - {title}");
    public static string PinTooltip => T("钉住：清空后也保持打开", "Pin: stay open even when empty");
    public static string CollapseTooltip => T("收起为堆叠", "Collapse to stack");
    public static string MoreTooltip => T("更多", "More");
    public static string CloseShelfTooltip => T("关闭置物架 (Esc)", "Close shelf (Esc)");
    public static string DropHere => T("拖到这里暂存", "Drop here to hold");
    public static string OrPaste => T("也可以按 Ctrl+V 粘贴", "or press Ctrl+V to paste");
    public static string RemoveItemTooltip => T("从置物架移除（不会删除文件）", "Remove from shelf (doesn't delete the file)");
    public static string ReleaseToAdd => T("松开以添加", "Release to add");
    public static string ReleaseToHold => T("松开以暂存", "Release to hold");

    public static string PleaseWait => T("请稍候", "Please wait");
    public static string UnsupportedContent => T("不支持此内容", "Not supported");
    public static string FilesOnlyAction => T("此操作只支持文件", "Files only");
    public static string AddToShelf => T("添加到置物架", "Add to shelf");
    public static string PutOnShelf => T("放入置物架", "Put on shelf");
    public static string DropFailed => T("拖入没有完成，来源内容未受影响", "The drop didn't finish. The source content is unaffected");
    public static string ChooseMoveFolder => T("选择要移动到的文件夹", "Choose a folder to move to");
    public static string ClipboardEmpty => T("剪贴板是空的", "The clipboard is empty");
    public static string PasteFailed => T("粘贴没有完成", "Paste didn't finish");

    public static string WithFailedAttachments(string message, int failed) => T(
        $"{message}，{failed} 个附件接收失败",
        $"{message}; {Count(failed, "attachment", "attachments")} couldn't be received");

    public static string MenuExpand => T("展开列表", "Expand list");
    public static string MenuSelectAll => T("全选", "Select all");
    public static string MenuCopy => T("复制", "Copy");
    public static string MenuPaste => T("粘贴", "Paste");
    public static string MenuCopyPath => T("复制路径", "Copy path");
    public static string MenuReveal => T("在资源管理器中显示", "Show in File Explorer");
    public static string MenuClear => T("清空置物架", "Clear shelf");
    public static string MenuSettings => T("DropShelf 设置", "DropShelf settings");
    public static string MenuOpen => T("打开", "Open");
    public static string MenuCopyText => T("复制文本", "Copy text");
    public static string MenuRemove => T("从置物架移除", "Remove from shelf");

    public static string AlreadyOnShelf => T("这些项目已在置物架中", "These items are already on the shelf");
    public static string ItemsUnavailable => T("项目无法访问，未能添加", "The items couldn't be accessed and weren't added");
    public static string NothingToAdd => T("没有可添加的内容", "Nothing to add");
    public static string Added(int count) => T($"已添加 {count} 项", $"Added {Count(count, "item", "items")}");
    public static string SkippedDuplicates(int count) => T($"，忽略 {count} 个重复项", $", skipped {Count(count, "duplicate", "duplicates")}");
    public static string UnavailableItems(int count) => T($"，{count} 项无法访问", $", {count} couldn't be accessed");

    public static string AddFilesFirst => T("先把文件放进置物架", "Put some files on the shelf first");
    public static string FilesAndFoldersOnly => T("这个操作只支持文件或文件夹", "This action only works with files and folders");
    public static string ActionFailed => T("操作没有完成，原文件没有改变", "The action didn't finish. The original files are unchanged");
    public static string CopiedFiles(int count) => T($"已复制 {count} 个文件", $"Copied {Count(count, "file", "files")}");
    public static string CopiedText => T("已复制文本", "Copied text");
    public static string ClipboardBusy => T("剪贴板正被占用，请重试", "The clipboard is in use. Please try again");
    public static string NoPathsToCopy => T("没有可复制的路径", "No paths to copy");
    public static string CopiedPath => T("已复制路径", "Copied path");
    public static string CopiedPaths(int count) => T($"已复制 {count} 个路径", $"Copied {count} paths");
    public static string ItemMissing => T("项目已被移动或删除", "The item was moved or deleted");
    public static string CannotOpenItem => T("无法打开这个项目", "Couldn't open this item");
    public static string CannotOpenFolder => T("无法打开所在文件夹", "Couldn't open the containing folder");

    // ───────────── 项目说明 ─────────────

    public static string MovedOrDeleted => T("已移动或删除", "Moved or deleted");
    public static string Folder => T("文件夹", "Folder");
    public static string Link => T("链接", "Link");
    public static string TextDetail(int characters) => T($"文本 · {characters} 个字符", $"Text · {Count(characters, "character", "characters")}");
    public static string GenericFileType => T("文件", "File");
    public static string FileType(string extension) => T($"{extension} 文件", $"{extension} File");
    public static string DroppedImageName => T("图片", "Image");

    // ───────────── 快速操作 ─────────────

    public static string ActionCompress => T("压缩", "Compress");
    public static string ActionCopyPath => T("复制路径", "Copy path");
    public static string ActionMoveTo => T("移动到", "Move to");
    public static string ActionProperties => T("属性", "Properties");
    public static string ActionReveal => T("显示位置", "Locate");

    public static string QuickActionLabel(QuickActionType type) => type switch
    {
        QuickActionType.CompressZip => ActionCompress,
        QuickActionType.CopyFullPath => ActionCopyPath,
        QuickActionType.MoveTo => ActionMoveTo,
        QuickActionType.Reveal => ActionReveal,
        _ => ActionProperties
    };

    public static string QuickActionDescription(QuickActionType type) => type switch
    {
        QuickActionType.CompressZip => T("压缩为 ZIP；单独放入 ZIP 或 7z 时解压到原位置旁。",
            "Compresses into a ZIP. A single ZIP or 7z file is extracted next to the original instead."),
        QuickActionType.CopyFullPath => T("把完整路径复制到剪贴板，多个项目按行分隔。",
            "Copies full paths to the clipboard, one item per line."),
        QuickActionType.MoveTo => T("选择文件夹后移动项目，同名时自动保留两份。",
            "Moves items to a folder you choose, keeping both if the names clash."),
        QuickActionType.Reveal => T("在文件资源管理器中选中这些项目。", "Selects these items in File Explorer."),
        _ => T("打开 Windows 文件属性窗口。", "Opens the Windows file properties window.")
    };

    public static string QuickActionBusyText(QuickActionType type) => type switch
    {
        QuickActionType.CompressZip => T("正在压缩…", "Compressing…"),
        QuickActionType.CopyFullPath => T("正在复制路径…", "Copying paths…"),
        QuickActionType.MoveTo => T("正在移动…", "Moving…"),
        QuickActionType.Reveal => T("正在打开文件夹…", "Opening folder…"),
        _ => T("正在打开属性…", "Opening properties…")
    };

    public static string QuickActionTooltip(string label, string description) => T($"{label}：{description}", $"{label}: {description}");

    public static string NoFilesToProcess => T("没有可操作的文件", "No files to work with");
    public static string OutputFolderUnavailable => T("无法使用输出文件夹，原文件没有改变", "Couldn't use the output folder. The original files are unchanged");
    public static string ClipboardBusyOtherApp => T("剪贴板正被其他程序占用，请重试", "Another app is using the clipboard. Please try again");
    public static string MoveCancelled => T("已取消移动", "Move cancelled");
    public static string MovedTo(int count, string folder) => T($"已移动 {count} 项到 {folder}", $"Moved {Count(count, "item", "items")} to {folder}");
    public static string MoveFailed => T("移动失败，原项目保持不变", "Move failed. The items are unchanged");
    public static string MovedPartially(int moved, int failed) => T($"已移动 {moved} 项，{failed} 项未能移动",
        $"Moved {Count(moved, "item", "items")}; {failed} couldn't be moved");
    public static string ShownInExplorer => T("已在文件资源管理器中显示", "Shown in File Explorer");
    public static string NoFilesForProperties => T("没有可查看属性的文件", "No files to show properties for");
    public static string OpenedPropertiesFor(int count) => T($"已打开 {count} 个项目的属性", $"Opened properties for {count} items");
    public static string OpenedProperties => T("已打开属性", "Opened properties");
    public static string PropertiesUnavailable => T("Windows 未能打开属性窗口", "Windows couldn't open the properties window");
    public static string PropertiesFailed => T("未能打开属性窗口，原文件没有改变", "Couldn't open the properties window. The original files are unchanged");
    public static string NothingToArchive => T("没有可压缩或解压的文件", "No files to compress or extract");
    public static string NothingToCompress => T("没有可压缩的文件", "No files to compress");
    public static string DefaultArchiveName => T("DropShelf 压缩文件", "DropShelf Archive");
    public static string Created(string name) => T($"已创建 {name}", $"Created {name}");
    public static string CompressFailed => T("压缩失败，原文件没有改变", "Compression failed. The original files are unchanged");
    public static string SavedToDownloads(int count) => T($"已保存 {count} 项到下载文件夹", $"Saved {Count(count, "item", "items")} to Downloads");
    public static string AttachmentSaveFailed => T("附件保存失败，无法复制可用路径", "Couldn't save the attachments, so there's no path to copy");
    public static string ExtractedTo(string name) => T($"已解压到 {name}", $"Extracted to {name}");
    public static string ExtractFailed => T("解压失败，压缩包没有改变", "Extraction failed. The archive is unchanged");
    public static string SevenZipUnsupported => T("当前 Windows 版本不支持内置 7z 解压", "This version of Windows can't extract 7z files");
    public static string SevenZipFailed => T("7z 解压失败，压缩包没有改变", "7z extraction failed. The archive is unchanged");

    // ───────────── 拖入与附件 ─────────────

    public static string SourceProvidedNothing => T("来源程序没有提供可读取的内容", "The source app didn't provide any readable content");
    public static string NoLinkRead => T("没有读取到链接地址", "Couldn't read the link address");
    public static string NoTextToAdd => T("没有可添加的文本", "No text to add");
    public static string NoFilesRead => T("没有读取到文件", "Couldn't read any files");
    public static string NoImageRead => T("没有读取到图片内容", "Couldn't read the image");
    public static string ImageSaveFailed => T("图片保存失败", "Couldn't save the image");
    public static string NoVirtualAttachments => T("没有可接收的虚拟附件", "No attachments to receive");
    public static string NonStandardAttachments => T("来源程序未提供标准 Windows 附件内容", "The source app didn't provide standard Windows attachment content");
    public static string AttachmentInfoUnreadable => T("无法读取附件信息", "Couldn't read the attachment details");
    public static string NoFileAttachments => T("没有可接收的文件附件", "No file attachments to receive");
    public static string AttachmentUnavailable => T("此附件暂时无法从来源程序读取", "This attachment can't be read from the source app right now");
    public static string AttachmentReceiveFailed => T("附件接收失败，来源文件未受影响", "Couldn't receive the attachments. The source files are unaffected");

    // ───────────── 文件与系统 ─────────────

    public static string AccessDenied => T("没有访问权限。", "Access denied.");
    public static string ItemNotFound => T("项目已经不存在或暂时无法访问。", "The item no longer exists or can't be accessed right now.");
    public static string FolderNotFound => T("文件夹已经不存在或暂时无法访问。", "The folder no longer exists or can't be accessed right now.");
    public static string PathTooLong => T("路径过长，Windows 无法处理。", "The path is too long for Windows to handle.");
    public static string FolderIntoItself => T("不能把文件夹复制或移动到它自己的内部。", "A folder can't be copied or moved into itself.");
    public static string ItemBusy => T("项目正被占用、目标空间不足或设备暂时不可用。",
        "The item is in use, the destination is out of space, or the device is unavailable.");
    public static string OperationIncomplete => T("操作没有完成。", "The operation didn't finish.");
    public static string StartupSettingsUnavailable => T("Windows 启动设置不可用", "Windows startup settings are unavailable");
    public static string StartupRefused => T("Windows 拒绝了这项设置", "Windows refused this setting");
}
