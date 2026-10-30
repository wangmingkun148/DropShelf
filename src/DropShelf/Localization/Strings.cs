using System.Globalization;
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
    public static string BackTooltip => T("返回（Esc）", "Back (Esc)");
    public static string AboutTooltip => T("关于 DropShelf", "About DropShelf");
    public static string SettingsSection => T("设置", "Settings");
    public static string ClickToRestore => T("点击恢复", "Click to restore");
    public static string ProgramLocation => T("程序位置", "Program location");
    public static string Uninstall => T("卸载", "Uninstall");
    public static string UninstallHeader => T("卸载 DropShelf", "Uninstall DropShelf");
    public static string UninstallDescription => T("打开卸载程序并退出 DropShelf，设置和日志会保留", "Opens the uninstaller and quits DropShelf. Settings and logs are kept");
    public static string UninstallPortable => T("便携版或开发版无需卸载：退出后删除程序所在的文件夹即可",
        "The portable or development version doesn't need uninstalling. Quit and delete its folder");
    public static string UninstallFailed => T("无法启动卸载程序，请从 Windows 设置中卸载", "Couldn't start the uninstaller. Uninstall DropShelf from Windows Settings instead");
    public static string QuitDescription => T("关闭所有置物架，不再响应拖动", "Closes every shelf and stops responding to drags");
    public static string Quit => T("退出", "Quit");

    public static string GeneralSummary(AppLanguage language, bool startWithWindows, HotkeyGesture hotkey) => string.Join(" · ",
        language switch { AppLanguage.Chinese => "中文", AppLanguage.English => "English", _ => T("语言跟随系统", "System language") },
        startWithWindows ? T("开机启动", "Starts with Windows") : T("不开机启动", "Doesn't start with Windows"),
        hotkey.IsEmpty ? HotkeyNotSet : hotkey.ToString());

    public static string ShelfSummary(AutoOpenTrigger trigger, TransferMode mode) => string.Join(" · ",
        trigger switch
        {
            AutoOpenTrigger.Shake => T("晃动呼出", "Shake to open"),
            AutoOpenTrigger.Edge => T("屏幕边缘呼出", "Pause at edge to open"),
            _ => T("晃动或屏幕边缘呼出", "Shake or pause at edge")
        },
        mode == TransferMode.Copy ? T("拖出时复制", "Drag out copies") : T("拖出时移动", "Drag out moves"));

    public static string ActionsSummary(IEnumerable<string> labels) =>
        string.Join(T("、", ", "), labels) is { Length: > 0 } text ? text : T("不显示快速操作", "Quick actions are off");

    public static string AppearanceSummary(AppTheme theme, bool customAccent) => string.Join(" · ",
        theme switch { AppTheme.Light => ThemeLight, AppTheme.Dark => ThemeDark, _ => T("主题跟随系统", "System theme") },
        customAccent ? T("自选强调色", "Custom accent color") : T("Windows 强调色", "Windows accent color"));

    public static string StatusRunning => T("正在运行", "Running");
    public static string StatusGestureFailed => T("手势识别未能启动", "Gesture detection couldn't start");
    public static string StatusGestureFailedDetail => T("请重新启动 DropShelf；置物架和快捷键仍可使用", "Restart DropShelf. Shelves and the shortcut still work");

    // 宽窗口左侧的状态栏
    public static string PanelReady => T("置物架已就绪", "Your shelf is ready");
    public static string PanelTagline => T("拖动文件时随时呼出，临时放一放，再拖到任何地方。",
        "Call it up while dragging files, park them for a moment, then drop them anywhere.");
    public static string PanelWays => T("呼出方式", "Ways to open");
    public static string WayShake => T("拖住文件晃一晃", "Drag a file and shake");
    public static string WayEdge => T("拖到屏幕边缘停一下", "Pause at the screen edge");

    public static string StatusRunningDetail(AutoOpenTrigger trigger, string? hotkey) => trigger switch
    {
        AutoOpenTrigger.Shake => T($"拖住文件并快速晃动鼠标{(hotkey is null ? null : $"，或按 {hotkey} 打开置物架")}",
            $"Drag a file and shake the mouse{(hotkey is null ? null : $", or press {hotkey} to open a shelf")}"),
        AutoOpenTrigger.Edge => T($"把文件拖到屏幕边缘停一下{(hotkey is null ? null : $"，或按 {hotkey} 打开置物架")}",
            $"Drag a file to the edge of the screen and pause{(hotkey is null ? null : $", or press {hotkey} to open a shelf")}"),
        _ => T($"拖住文件晃一晃，或拖到屏幕边缘停一下{(hotkey is null ? null : $"；也可以按 {hotkey}")}",
            $"Drag a file and shake it, or pause at the screen edge{(hotkey is null ? null : $". You can also press {hotkey}")}")
    };

    public static string GettingStarted => T("快速上手", "Get started");
    public static string DismissTooltip => T("不再显示", "Don't show again");
    public static string TipShakeTitle => T("拖住并晃动", "Drag and shake");
    public static string TipShakeBody => T("拖住文件后快速晃动鼠标，或拖到屏幕边缘停一下，置物架就会出现。",
        "While dragging a file, shake the mouse or pause at the edge of the screen, and a shelf appears.");
    public static string TipStackTitle => T("整摞拖出", "Drag out the stack");
    public static string TipStackBody => T("拖动缩略图堆叠一次带走全部文件；点顶部的列表按钮展开列表，也能单独或多选拖出。",
        "Drag the thumbnail stack to take every file at once, or open the list with the list button at the top to drag out one or several.");
    public static string TipActionsBody => T("把文件拖到置物架底部，在展开的操作上松开，就能直接压缩、复制路径或移动。",
        "Drag files to the bottom of a shelf and release them on an action to compress them, copy their paths or move them.");
    public static string RestoreShelf => T("恢复", "Reopen");
    public static string RemoveHistoryTooltip => T("删除这条记录", "Remove from history");

    public static string General => T("常规", "General");
    public static string Language => T("语言", "Language");
    public static string LanguageDescription => T("切换后立即生效，无需重启", "Applies right away, no restart needed");
    public static string StartWithWindows => T("开机自动启动", "Start with Windows");
    public static string StartupDescription => T("登录 Windows 后在后台运行，不弹出窗口", "Runs in the background after you sign in, without opening a window");
    public static string StartupFailed => T("Windows 没有允许修改开机启动，设置保持不变", "Windows didn't allow the startup change, so the setting is unchanged");
    public static string GlobalHotkey => T("快捷键", "Keyboard shortcut");
    public static string HotkeyNotSet => T("未设置快捷键", "No shortcut set");
    public static string HotkeyNotSetDescription => T("设置后，在任何程序中按下都能在光标处打开空置物架", "Set one to open an empty shelf at the cursor from any app");
    public static string HotkeyRegistered => T("在光标处打开空置物架，可直接按 Ctrl+V 粘贴", "Opens an empty shelf at the cursor. Press Ctrl+V to paste into it");
    public static string HotkeyTaken => T("这个快捷键已被其他程序占用，请换一个", "Another app is already using this shortcut. Please choose a different one");
    public static string Tutorial => T("使用教程", "Tutorial");
    public static string TutorialDescription => T("用几段动画了解如何呼出置物架、拖出文件和使用快速操作", "Short animations showing how to open a shelf, drag files out and use quick actions");
    public static string ViewTutorial => T("查看教程", "View tutorial");

    public static string AutoOpen => T("自动弹出", "Auto open");
    public static string TriggerHeader => T("呼出方式", "How to open");
    public static string TriggerDescription => T("拖动文件时晃动鼠标，或拖到屏幕边缘停一下", "While dragging files, shake the mouse or pause at the edge of the screen");
    public static string TriggerShake => T("晃动", "Shake");
    public static string TriggerEdge => T("屏幕边缘", "Screen edge");
    public static string TriggerBoth => T("两种都用", "Both");
    public static string ShakeSensitivity => T("晃动灵敏度", "Shake sensitivity");
    public static string SensitivityResponsive => T("灵敏", "Responsive");
    public static string SensitivitySteady => T("稳健", "Steady");
    public static string SensitivityBalanced => T("均衡", "Balanced");

    public static string SensitivityDetail(int reversals, int distance) => T(
        $"来回晃动约 {distance} 像素、掉头 {reversals} 次即可弹出",
        $"Opens after shaking about {distance} px and turning back {reversals} times");
    public static string EdgeDwellHeader => T("边缘停留时间", "Edge delay");
    public static string EdgeDwellDescription => T("拖到屏幕左、右或上边缘后，停留多久弹出置物架", "How long to pause at the left, right or top edge before a shelf opens");
    public static string DwellShort => T("0.3 秒", "0.3 s");
    public static string DwellMedium => T("0.6 秒", "0.6 s");
    public static string DwellLong => T("1 秒", "1 s");
    public static string BlockedAppsHeader => T("屏蔽的应用", "Excluded apps");
    public static string BlockedAppsDescription => T("这些应用在前台、或拖动经过它们的窗口时不弹出置物架，快捷键也交给应用自己使用", "No shelf opens while these apps are in front or you drag over their windows, and the hotkey goes to the app");
    public static string AddApp => T("添加应用", "Add app");
    public static string BrowseApp => T("浏览…", "Browse…");
    public static string AppFilter => T("程序|*.exe", "Programs|*.exe");
    public static string RemoveBlockedApp => T("移除", "Remove");
    public static string Size => T("尺寸", "Size");
    public static string SizeDescription => T("置物架窗口的大小", "How large shelves appear");
    public static string SizeSmall => T("小", "Small");
    public static string SizeMedium => T("中", "Medium");
    public static string SizeLarge => T("大", "Large");
    public static string DragOutModeHeader => T("拖动目的", "Drag-out action");
    public static string DragOutModeDescription => T("把文件拖出置物架时移动还是复制，也可以在置物架顶部随时切换",
        "Whether files dragged off a shelf are moved or copied. You can also switch this at the top of a shelf");
    public static string TransferMove => T("移动", "Move");
    public static string TransferCopy => T("复制", "Copy");
    public static string CloseWhenEmpty => T("自动关闭空置物架", "Close empty shelves");
    public static string CloseWhenEmptyDescription => T("拖出的文件会离开置物架，空了就自动关闭。关闭此项后，复制出去的文件仍留在架上，可以再拖到别处",
        "Dragged-out files leave the shelf, and it closes once empty. Turn off to keep copied files on the shelf so you can drag them elsewhere too");
    public static string RecentShelves => T("最近关闭的置物架", "Recently closed shelves");
    public static string ClearShelfHistory => T("清空置物架历史", "Clear shelf history");
    public static string HistoryEmpty => T("暂无最近关闭的置物架", "No recently closed shelves");
    public static string HistoryCount(int count) => T($"已保留 {count} 个最近关闭的置物架", $"{Count(count, "recently closed shelf", "recently closed shelves")} kept");
    public static string ClearHistory => T("清除记录", "Clear history");
    public static string Clear => T("清空", "Clear");

    public static string ActionsPageDescription => T("显示在置物架底部。点按可处理架上的项目，也可以把文件直接拖到按钮上。",
        "Shown at the bottom of each shelf. Click one to use it on the shelf's items, or drop files straight onto it.");
    public static string ActionLayoutHeader => T("按钮数量", "Number of buttons");
    public static string ActionLayoutDescription => T("两个时按钮更大，并显示名称", "With two, buttons are larger and show their names");
    public static string ActionLayoutOffDescription => T("不显示按钮，整个置物架都可以放入文件", "No buttons, so you can drop files anywhere on the shelf");
    public static string LayoutOff => T("关闭", "Off");
    public static string LayoutTwo => T("2 个", "2");
    public static string LayoutFour => T("4 个", "4");
    public static string CurrentActions => T("当前操作", "Current actions");
    public static string MoreActions => T("更多操作", "More actions");
    public static string DragToSwapHint => T("拖动卡片调换位置，或从下方“更多操作”拖上来替换；自定义操作拖到窗口底部可删除",
        "Drag to rearrange, or drag one up from More actions to replace an action. Drag a custom action to the bottom of the window to delete it");
    public static string SlotLeft => T("左", "Left");
    public static string SlotRight => T("右", "Right");
    public static string SlotFarLeft => T("左一", "Far left");
    public static string SlotCenterLeft => T("左二", "Center left");
    public static string SlotCenterRight => T("右二", "Center right");
    public static string SlotFarRight => T("右一", "Far right");
    public static string DropHintReplace => T("替换", "Replace");
    public static string DropHintSwap => T("交换", "Swap");
    public static string AddCustomAction => T("添加自定义操作", "Add custom action");
    public static string EditCustomAction => T("编辑自定义操作", "Edit custom action");
    public static string EditTooltip => T("编辑", "Edit");
    public static string DeleteTooltip => T("删除", "Delete");
    public static string ReleaseToDelete => T("松开以删除", "Release to delete");
    public static string BuiltInCannotDelete => T("内置操作不能删除", "Built-in actions can't be deleted");
    public static string ConfirmDeleteTitle => T("删除快速操作", "Delete quick action");
    public static string ConfirmDeleteAction(string title) => T($"“{title}”将从快速操作中移除，此操作无法撤销。",
        $"“{title}” will be removed from your quick actions. This can't be undone.");

    public static string Theme => T("主题", "Theme");
    public static string ThemeLight => T("浅色", "Light");
    public static string ThemeDark => T("深色", "Dark");
    public static string AccentColor => T("强调色", "Accent color");
    public static string AccentDescription => T("按钮、开关和选中状态的颜色", "Used for buttons, switches and selections");
    public static string AccentFollowSystem => T("跟随 Windows", "Match Windows");
    public static string CaptionMinimize => T("最小化", "Minimize");
    public static string CaptionMaximize => T("最大化", "Maximize");
    public static string CaptionClose => T("关闭", "Close");

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
    public static string OnboardingShakeBody => T("在资源管理器、桌面、浏览器或聊天软件里拖住文件，快速左右晃动鼠标，置物架就会出现在光标处。也可以拖到屏幕边缘停一下。",
        "While dragging a file in File Explorer, on the desktop, in a browser or in a chat app, shake the mouse quickly left and right and a shelf appears at the cursor. You can also pause at the edge of the screen.");
    public static string OnboardingStackTitle => T("放进来，再拖出去", "Drop it in, drag it out");
    public static string OnboardingStackBody => T("松开即可暂存。拖动缩略图堆叠可一次带走全部文件；点顶部的列表按钮展开列表后也能多选，只拖走需要的部分。拖出时默认移动文件，点左上角的“移动”可切换为复制。",
        "Release to hold the files. Drag the thumbnail stack to take everything at once, or open the list with the list button at the top to pick only the files you need. Files are moved when you drag them out; click Move at the top left to copy them instead.");
    public static string OnboardingActionsTitle => T("快速操作与快捷键", "Quick actions and shortcut");

    public static string OnboardingActionsBody(string? hotkey) => hotkey is null
        ? T("把文件拖到置物架底部，会展开快速操作，可以直接压缩、复制路径、移动或查看属性。这些操作可以在设置中更换，也能添加自己的操作。在设置中设定快捷键后，还能随时打开空置物架。",
            "Drag files to the bottom of the shelf to reveal quick actions: compress, copy paths, move them or view their properties. You can change these actions or add your own in Settings, and set a keyboard shortcut there to open an empty shelf at any time.")
        : T($"把文件拖到置物架底部，会展开快速操作，可以直接压缩、复制路径、移动或查看属性。这些操作可以在设置中更换，也能添加自己的操作。按 {hotkey} 能随时打开空置物架。",
            $"Drag files to the bottom of the shelf to reveal quick actions: compress, copy paths, move them or view their properties. You can change these actions or add your own in Settings. Press {hotkey} to open an empty shelf at any time.");

    public static string OnboardingSampleCount => ItemCount(3);
    public static string OnboardingSampleFolder => T("文档", "Documents");
    public static string OnboardingSampleArchive => T("资料.zip", "Files.zip");
    public static string OnboardingSampleToast => Created(OnboardingSampleArchive);
    public static string Skip => T("跳过", "Skip");
    public static string Back => T("上一步", "Back");
    public static string Next => T("下一步", "Next");
    public static string GetStarted => T("开始使用", "Get started");

    // ───────────── 托盘 ─────────────

    public static string TrayTip => T("DropShelf · 拖动文件时呼出置物架", "DropShelf · Open a shelf while dragging files");
    public static string StillRunningTitle => T("DropShelf 仍在运行", "DropShelf is still running");
    public static string StillRunningBody => T("拖动文件时晃一晃或拖到屏幕边缘，即可呼出置物架。可从通知区域重新打开设置。",
        "While dragging files, shake or pause at the screen edge to open a shelf. You can reopen Settings from the notification area.");

    // ───────────── 置物架 ─────────────

    public static string ShelfWindowTitle(string title) => T($"DropShelf 置物架 - {title}", $"DropShelf shelf - {title}");
    public static string PinTooltip => T("置顶：保持在其他窗口上方", "Pin: keep on top of other windows");
    public static string MoreTooltip => T("更多", "More");
    public static string CloseShelfTooltip => T("关闭置物架 (Esc)", "Close shelf (Esc)");
    public static string DropHere => T("拖到这里暂存", "Drop here to hold");
    public static string OrPaste => T("也可以按 Ctrl+V 粘贴", "or press Ctrl+V to paste");
    public static string RemoveItemTooltip => T("从置物架移除（不会删除文件）", "Remove from shelf (doesn't delete the file)");
    public static string ReleaseToAdd => T("松开以添加", "Release to add");
    public static string ReleaseToHold => T("松开以暂存", "Release to hold");
    public static string RestoreLastShelf => T("恢复上次的置物架", "Reopen last shelf");

    public static string DragOutModeTooltip(TransferMode mode) => mode == TransferMode.Copy
        ? T("拖出时复制文件，点按改为移动", "Dragging out copies files. Click to move instead")
        : T("拖出时移动文件，点按改为复制", "Dragging out moves files. Click to copy instead");

    public static string DragOutModeChanged(TransferMode mode) => mode == TransferMode.Copy
        ? T("拖出时将复制文件", "Dragging out now copies files")
        : T("拖出时将移动文件", "Dragging out now moves files");

    public static string Restored(int added, int unavailable) => T(
        $"已恢复 {added} 项{(unavailable > 0 ? $"，{unavailable} 项已删除或移动" : null)}",
        $"Reopened {Count(added, "item", "items")}{(unavailable > 0 ? $", {unavailable} moved or deleted" : null)}");

    public static string RestoreUnavailable => T("这些项目已删除或移动", "These items were moved or deleted");

    public static string HistoryEntryTitle(ShelfHistoryEntry entry)
    {
        var first = entry.Items.Count > 0 ? entry.Items[0].Name : Shelf;
        return entry.Items.Count <= 1 ? first : T($"{first} 等 {entry.Items.Count} 项", $"{first} and {entry.Items.Count - 1} more");
    }

    public static string HistoryEntryTime(DateTime closedAt)
    {
        var time = closedAt.ToString("HH:mm", CultureInfo.InvariantCulture);
        if (closedAt.Date == DateTime.Today) return time;
        if (closedAt.Date == DateTime.Today.AddDays(-1)) return T($"昨天 {time}", $"Yesterday {time}");
        return T($"{closedAt.Month}月{closedAt.Day}日", closedAt.ToString("MMM d", CultureInfo.GetCultureInfo("en-US")));
    }

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
    public static string MenuCollapse => T("收起列表", "Collapse list");
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
    public static string ItemMissing => T("项目已删除或移动", "The item was moved or deleted");
    public static string CannotOpenItem => T("无法打开这个项目", "Couldn't open this item");
    public static string CannotOpenFolder => T("无法打开所在文件夹", "Couldn't open the containing folder");

    // ───────────── 项目说明 ─────────────

    public static string MovedOrDeleted => T("已删除或移动", "Moved or deleted");
    public static string Folder => T("文件夹", "Folder");
    public static string Link => T("链接", "Link");
    public static string TextDetail(int characters) => T($"文本 · {characters} 个字符", $"Text · {Count(characters, "character", "characters")}");
    public static string GenericFileType => T("文件", "File");
    public static string FileType(string extension) => T($"{extension} 文件", $"{extension} File");
    public static string DroppedImageName => T("图片", "Image");

    // ───────────── 快速操作 ─────────────

    public static string ActionCompress => T("压缩", "Compress");
    public static string ActionCopyPath => T("路径", "Copy path");
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

    /// <summary>设置页和提示中的完整名称；按钮上的名称较短时在这里写全。</summary>
    public static string QuickActionTitle(QuickActionType type) => type switch
    {
        QuickActionType.CopyFullPath => MenuCopyPath,
        QuickActionType.Reveal => MenuReveal,
        _ => QuickActionLabel(type)
    };

    public static string QuickActionDescription(QuickActionType type) => type switch
    {
        QuickActionType.CompressZip => T("压缩为 ZIP；只有一个 ZIP 或 7z 时改为解压到它旁边。",
            "Compresses into a ZIP. A single ZIP or 7z file is extracted next to it instead."),
        QuickActionType.CopyFullPath => T("把完整路径复制到剪贴板，多个项目按行分隔。",
            "Copies full paths to the clipboard, one item per line."),
        QuickActionType.MoveTo => T("移动到你选择的文件夹，重名时两份都保留。",
            "Moves items to a folder you choose, keeping both if names clash."),
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

    // ───────────── 自定义操作 ─────────────

    public static string KindFolder => T("发送到文件夹", "Send to folder");
    public static string KindCompressImage => T("压缩图片", "Compress image");
    public static string KindOpenWith => T("用程序打开", "Open with app");
    public static string ActionKind => T("类型", "Type");
    public static string FolderLabel => T("目标文件夹", "Destination folder");
    public static string FolderModeLabel => T("移动还是复制", "Move or copy");
    public static string ImageSizeLabel => T("最大文件大小", "Maximum file size");
    public static string ImageSizeHint => T("保存为尽量清晰的 JPG，原图保持不变", "Saves a JPG as sharp as the limit allows. The original stays unchanged");
    public static string ProgramLabel => T("程序", "Program");
    public static string ArgumentsLabel => T("参数（可选）", "Arguments (optional)");
    public static string ArgumentsHint => T("用 {files} 指定文件在参数中的位置；不写时文件放在最后", "Use {files} to mark where the files go. Without it, they're added at the end");
    public static string NameLabel => T("按钮名称（可选）", "Button name (optional)");
    public static string Browse => T("浏览…", "Browse…");
    public static string Save => T("保存", "Save");
    public static string Cancel => T("取消", "Cancel");
    public static string ChooseTargetFolder => T("选择文件夹", "Choose a folder");
    public static string ChooseProgram => T("选择程序", "Choose a program");
    public static string ProgramFilter => T("程序|*.exe;*.bat;*.cmd;*.lnk|所有文件|*.*", "Programs|*.exe;*.bat;*.cmd;*.lnk|All files|*.*");
    public static string ChooseFolderFirst => T("请选择文件夹", "Choose a folder");
    public static string FolderNotFoundShort => T("找不到这个文件夹", "Folder not found");
    public static string ChooseProgramFirst => T("请选择程序", "Choose a program");
    public static string ProgramNotFoundShort => T("找不到这个程序", "Program not found");
    public static string ImageSizeInvalid(int minimum, int maximum) => T($"请输入 {minimum} 到 {maximum} 之间的整数", $"Enter a whole number from {minimum} to {maximum}");

    /// <summary>500 KB、2 MB 等大小文字。</summary>
    public static string ImageSizeText(int kilobytes) =>
        kilobytes >= 1024 && kilobytes % 1024 == 0 ? $"{kilobytes / 1024} MB" : $"{kilobytes} KB";

    /// <summary>置物架按钮上的名称：用户填写的名称，或按内容生成的简短名称。</summary>
    public static string CustomActionLabel(CustomQuickAction action) => action.Name ?? CustomActionAutoName(action);

    public static string CustomActionAutoName(CustomQuickAction action) => action.Kind switch
    {
        CustomActionKind.Folder => string.IsNullOrWhiteSpace(action.Folder) ? KindFolder : QuickActionService.GetFolderName(action.Folder),
        CustomActionKind.CompressImage => $"≤ {ImageSizeText(action.ImageSizeKb)}",
        _ => string.IsNullOrWhiteSpace(action.ProgramPath) ? KindOpenWith : ShellFileInfo.GetProgramName(action.ProgramPath)
    };

    /// <summary>设置页和菜单中的完整名称。</summary>
    public static string CustomActionTitle(CustomQuickAction action)
    {
        if (action.Name is { } name) return name;
        var label = CustomActionAutoName(action);
        return action.Kind switch
        {
            CustomActionKind.Folder => action.FolderMode == TransferMode.Copy
                ? T($"复制到“{label}”", $"Copy to {label}")
                : T($"移动到“{label}”", $"Move to {label}"),
            CustomActionKind.CompressImage => T($"压缩图片到 {ImageSizeText(action.ImageSizeKb)}", $"Compress images to {ImageSizeText(action.ImageSizeKb)}"),
            _ => T($"用 {label} 打开", $"Open with {label}")
        };
    }

    public static string CustomActionDescription(CustomQuickAction action) => action.Kind switch
    {
        CustomActionKind.Folder => action.FolderMode == TransferMode.Copy
            ? T($"复制到 {action.Folder}", $"Copies items to {action.Folder}")
            : T($"移动到 {action.Folder}", $"Moves items to {action.Folder}"),
        CustomActionKind.CompressImage => T($"压缩为不超过 {ImageSizeText(action.ImageSizeKb)} 的 JPG，原图保持不变",
            $"Saves a JPG under {ImageSizeText(action.ImageSizeKb)}. The original stays unchanged"),
        _ => T($"用 {action.ProgramPath} 打开", $"Opens with {action.ProgramPath}")
    };

    public static string CustomActionBusyText(CustomQuickAction action) => action.Kind switch
    {
        CustomActionKind.Folder => action.FolderMode == TransferMode.Copy ? T("正在复制…", "Copying…") : T("正在移动…", "Moving…"),
        CustomActionKind.CompressImage => T("正在压缩图片…", "Compressing images…"),
        _ => T("正在打开…", "Opening…")
    };

    public static string FolderMissing(string name) => T($"找不到文件夹“{name}”", $"Couldn't find the folder \"{name}\"");
    public static string CopiedTo(int count, string folder) => T($"已复制 {count} 项到 {folder}", $"Copied {Count(count, "item", "items")} to {folder}");
    public static string CopyFailed => T("复制失败，原项目保持不变", "Copy failed. The items are unchanged");
    public static string CopiedPartially(int copied, int failed) => T($"已复制 {copied} 项，{failed} 项未能复制",
        $"Copied {Count(copied, "item", "items")}; {failed} couldn't be copied");
    public static string ImageCompressFailed => T("图片压缩失败，原图没有改变", "Couldn't compress the images. The originals are unchanged");
    public static string NoImagesToCompress => T("没有可压缩的图片", "No images to compress");
    public static string ImagesAlreadySmall(string size) => T($"图片已小于 {size}，无需压缩", $"The images are already under {size}");
    public static string ImagesCompressed(int count, string size) => T($"已压缩 {count} 张图片到 {size} 以内",
        $"Compressed {Count(count, "image", "images")} to under {size}");
    public static string ImagesSkippedSmall(int count) => T($"，{count} 张本来就符合", $", {count} already {(count == 1 ? "fits" : "fit")}");
    public static string ImagesFailed(int count) => T($"，{count} 张无法读取", $", {count} couldn't be read");
    public static string ProgramMissing(string name) => T($"找不到程序“{name}”", $"Couldn't find {name}");
    public static string OpenedWith(int count, string name) => T($"已用 {name} 打开 {count} 项", $"Opened {Count(count, "item", "items")} with {name}");
    public static string OpenWithFailed(string name) => T($"{name} 没能打开这些文件", $"{name} couldn't open these files");

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
    public static string ItemNotFound => T("项目已删除或移动，或暂时无法访问。", "The item was moved or deleted, or can't be accessed right now.");
    public static string FolderNotFound => T("文件夹已删除或移动，或暂时无法访问。", "The folder was moved or deleted, or can't be accessed right now.");
    public static string PathTooLong => T("路径过长，Windows 无法处理。", "The path is too long for Windows to handle.");
    public static string FolderIntoItself => T("不能把文件夹复制或移动到它自己的内部。", "A folder can't be copied or moved into itself.");
    public static string FolderContainsLinks => T("文件夹中包含联接点或符号链接，无法完整复制，原文件夹未改动。", "The folder contains junctions or symbolic links and can't be copied completely. The original folder was left unchanged.");
    public static string ItemBusy => T("项目正被占用、目标空间不足或设备暂时不可用。",
        "The item is in use, the destination is out of space, or the device is unavailable.");
    public static string OperationIncomplete => T("操作没有完成。", "The operation didn't finish.");
    public static string StartupSettingsUnavailable => T("Windows 启动设置不可用", "Windows startup settings are unavailable");
    public static string StartupRefused => T("Windows 拒绝了这项设置", "Windows refused this setting");
}
