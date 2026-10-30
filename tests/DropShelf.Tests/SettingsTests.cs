using System.Globalization;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using WpfApplication = System.Windows.Application;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static partial class Program
{
    // ───────────── 设置与模型 ─────────────

    private static void TestSettingsRoundTrip()
    {
        var service = new SettingsService();
        var defaults = service.Load();
        Assert(defaults.CloseWhenEmpty && defaults.ShowGettingStarted, "默认开关不正确");
        Assert(defaults.AccentColor is null, "默认强调色应跟随 Windows");
        Assert(defaults.QuickShelfSize == QuickShelfSize.Medium, "默认尺寸不是中档");
        Assert(defaults.QuickActionLayout == QuickActionLayout.Four &&
               defaults.QuickActionSlots!.SequenceEqual(QuickActions.Defaults.Select(type => type.ToString())), "默认快速操作不正确");
        Assert(defaults.CustomActions!.Single().Kind == CustomActionKind.CompressImage, "没有提供压缩图片的示例操作");
        Assert(defaults.AutoOpenTrigger == AutoOpenTrigger.Both && defaults.EdgeDwell == EdgeDwellTime.Medium, "默认呼出方式不正确");
        Assert(defaults.DragOutMode == TransferMode.Move, "默认拖出方式应为移动");
        Assert(defaults.Hotkey == "Ctrl+Shift+Space", "默认快捷键不正确");
        Assert(defaults.Language == AppLanguage.System, "默认语言应跟随系统");

        var folder = CreateFolder("settings-custom");
        defaults.ShakeSensitivity = 20;
        defaults.AutoOpenTrigger = AutoOpenTrigger.Edge;
        defaults.EdgeDwell = EdgeDwellTime.Long;
        defaults.DragOutMode = TransferMode.Copy;
        defaults.Theme = AppTheme.Dark;
        defaults.Language = AppLanguage.English;
        defaults.QuickShelfSize = QuickShelfSize.Large;
        defaults.QuickActionLayout = QuickActionLayout.Two;
        var custom = new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = folder, FolderMode = TransferMode.Copy, Name = "素材" };
        defaults.CustomActions!.Add(custom);
        defaults.QuickActionSlots = ["Reveal", custom.Id, "CopyFullPath", "MoveTo"];
        defaults.CloseWhenEmpty = false;
        defaults.ShowGettingStarted = false;
        defaults.AccentColor = "#8764B8";
        defaults.Hotkey = "Ctrl+Alt+D";
        defaults.HasCompletedFirstRun = true;
        defaults.BlockedApps = ["Photoshop.exe", "game.exe"];
        service.Save(defaults);

        var loaded = service.Load();
        Assert(loaded.ShakeSensitivity == 20 && loaded.MouseGestureSensitivity is null, "灵敏度未恢复");
        Assert(loaded.AutoOpenTrigger == AutoOpenTrigger.Edge && loaded.EdgeDwell == EdgeDwellTime.Long, "呼出方式未恢复");
        Assert(loaded.DragOutMode == TransferMode.Copy, "拖出方式未恢复");
        Assert(loaded.Theme == AppTheme.Dark, "主题未恢复");
        Assert(loaded.Language == AppLanguage.English, "语言未恢复");
        Assert(loaded.QuickShelfSize == QuickShelfSize.Large, "尺寸未恢复");
        Assert(loaded.QuickActionLayout == QuickActionLayout.Two, "按钮数量未恢复");
        Assert(loaded.QuickActionSlots!.SequenceEqual(["Reveal", custom.Id, "CopyFullPath", "MoveTo"]), "快速操作位置没有按原顺序恢复");
        Assert(loaded.BlockedApps!.SequenceEqual(["Photoshop.exe", "game.exe"]), "屏蔽的应用未恢复");
        var restored = loaded.CustomActions!.Single(action => action.Id == custom.Id);
        Assert(restored.Folder == folder && restored.FolderMode == TransferMode.Copy && restored.Name == "素材", "自定义操作未恢复");
        Assert(QuickActions.Visible(loaded).Select(info => info.Id).SequenceEqual(["Reveal", custom.Id]), "只显示两个时应使用前两个位置");
        Assert(!loaded.CloseWhenEmpty && !loaded.ShowGettingStarted, "自动关闭或快速上手设置未恢复");
        Assert(loaded.AccentColor == "#8764B8", "强调色未恢复");
        Assert(loaded.Hotkey == "Ctrl+Alt+D", "快捷键未恢复");
        Assert(loaded.HasCompletedFirstRun, "首次运行状态未恢复");
        Assert(!File.ReadAllText(service.SettingsPath).Contains("\"QuickActions\""), "已迁移的旧版快速操作仍写入了设置文件");

        var clone = loaded.Clone();
        clone.QuickActionSlots!.Clear();
        clone.CustomActions![0].Name = "changed";
        Assert(loaded.QuickActionSlots!.Count == 4 && loaded.CustomActions![0].Name != "changed", "复制设置时共享了快速操作");
        File.Delete(service.SettingsPath);
    }

    private static void TestSettingsMigration()
    {
        var service = new SettingsService();
        File.WriteAllText(service.SettingsPath, """
            {
              "AlwaysOnTop": false,
              "WindowLeft": 624,
              "WindowTop": 160,
              "MouseGestureSensitivity": "Responsive",
              "Theme": "System",
              "StartWithWindows": false,
              "QuickShelfSize": "Small",
              "QuickAction": "CompressZip",
              "HasCompletedFirstRun": true
            }
            """);
        var migrated = service.Load();
        Assert(migrated.Version == AppSettings.CurrentVersion, "版本号没有更新");
        Assert(migrated.ShakeSensitivity == ShakeSensitivities.Responsive && migrated.MouseGestureSensitivity is null &&
               migrated.QuickShelfSize == QuickShelfSize.Small,
            "旧设置没有保留");
        Assert(migrated.QuickActionLayout == QuickActionLayout.Four &&
               migrated.QuickActionSlots!.SequenceEqual(QuickActions.Defaults.Select(type => type.ToString())), "旧的单个快速操作没有迁移为默认组合");
        Assert(migrated.WindowLeft == 624 && migrated.HasCompletedFirstRun, "窗口位置或首次运行状态丢失");
        Assert(migrated.AutoOpenTrigger == AutoOpenTrigger.Both && migrated.DragOutMode == TransferMode.Move, "新增设置没有使用默认值");

        File.WriteAllText(service.SettingsPath, """{ "QuickAction": "None" }""");
        Assert(service.Load().QuickActionLayout == QuickActionLayout.Off, "关闭快速操作的旧设置没有保持关闭");

        File.WriteAllText(service.SettingsPath, """{ "QuickAction": "ConvertPng" }""");
        Assert(service.Load().QuickActionSlots!.SequenceEqual(QuickActions.Defaults.Select(type => type.ToString())), "已移除的图片转换设置没有恢复默认");

        // 2.0：选了两个操作时改为显示两个大按钮，原来的操作排在前面。
        File.WriteAllText(service.SettingsPath, """{ "Version": 2, "QuickActions": ["Reveal", "CopyFullPath"] }""");
        var two = service.Load();
        Assert(two.QuickActionLayout == QuickActionLayout.Two && two.QuickActionSlots!.Take(2).SequenceEqual(["CopyFullPath", "Reveal"]),
            "2.0 版的快速操作没有正确迁移");
        File.WriteAllText(service.SettingsPath, """{ "Version": 2, "QuickActions": [] }""");
        Assert(service.Load().QuickActionLayout == QuickActionLayout.Off, "2.0 版关闭的快速操作没有保持关闭");
        File.Delete(service.SettingsPath);
    }

    private static void TestSettingsNormalization()
    {
        var invalid = new AppSettings
        {
            MouseGestureSensitivity = (GestureSensitivity)999,
            AutoOpenTrigger = (AutoOpenTrigger)999,
            EdgeDwell = (EdgeDwellTime)999,
            DragOutMode = (TransferMode)999,
            Theme = (AppTheme)999,
            AccentColor = "blue",
            Language = (AppLanguage)999,
            QuickShelfSize = (QuickShelfSize)999,
            QuickActionLayout = (QuickActionLayout)3,
            QuickActionSlots = ["Properties", "Properties", "ConvertJpeg", "nope"],
            CustomActions =
            [
                new CustomQuickAction { Kind = CustomActionKind.Folder },
                new CustomQuickAction { Kind = CustomActionKind.CompressImage, ImageSizeKb = 1, Name = "   " },
                new CustomQuickAction { Kind = (CustomActionKind)999 }
            ],
            Hotkey = "Shift+A",
            BlockedApps = ["  game.exe ", "GAME.EXE", "", @"C:\Tools\paint.exe"],
            WindowWidth = 10,
            WindowHeight = double.NaN
        };
        var normalized = SettingsService.Normalize(invalid.Clone());
        Assert(normalized.ShakeSensitivity == ShakeSensitivities.Balanced, "异常灵敏度没有恢复默认值");
        Assert(SettingsService.Normalize(new AppSettings { ShakeSensitivity = 250 }).ShakeSensitivity == ShakeSensitivities.Responsive &&
               SettingsService.Normalize(new AppSettings { ShakeSensitivity = -5 }).ShakeSensitivity == ShakeSensitivities.Steady, "超出范围的灵敏度没有限制在 0–100");
        Assert(normalized.AutoOpenTrigger == AutoOpenTrigger.Both && normalized.EdgeDwell == EdgeDwellTime.Medium, "异常呼出方式没有恢复默认值");
        Assert(normalized.DragOutMode == TransferMode.Move, "异常拖出方式没有恢复默认值");
        Assert(normalized.Theme == AppTheme.System && normalized.AccentColor is null, "异常主题或强调色没有恢复默认值");
        Assert(normalized.Language == AppLanguage.System, "异常语言没有恢复默认值");
        Assert(normalized.QuickShelfSize == QuickShelfSize.Medium, "异常尺寸没有恢复默认值");
        Assert(normalized.QuickActionLayout == QuickActionLayout.Four, "异常按钮数量没有恢复默认值");
        Assert(normalized.BlockedApps!.SequenceEqual(["game.exe", "paint.exe"]), "屏蔽的应用没有去掉空白、路径和重复");
        Assert(normalized.QuickActionSlots!.SequenceEqual(["Properties", "CompressZip", "CopyFullPath", "MoveTo"]), "快速操作位置没有规范化");
        var image = normalized.CustomActions!.Single();
        Assert(image.Kind == CustomActionKind.CompressImage && image.ImageSizeKb == CustomQuickAction.MinimumImageSizeKb && image.Name is null,
            "自定义操作没有规范化");
        Assert(normalized.Hotkey == HotkeyGesture.Default.ToString(), "无效快捷键没有恢复默认值");
        Assert(normalized.WindowWidth >= 640 && normalized.WindowHeight >= 520, "异常窗口尺寸没有恢复默认值");
        Assert((int)invalid.Theme == 999, "规范化时修改了原设置对象");
    }

    private static void TestHotkeyGesture()
    {
        Assert(HotkeyGesture.TryParse("Ctrl+Shift+Space", out var gesture) && gesture == HotkeyGesture.Default, "默认快捷键无法解析");
        Assert(gesture.ToString() == "Ctrl+Shift+Space", "快捷键显示不正确");
        Assert(HotkeyGesture.TryParse("alt+win+1", out var digit) && digit.Key == Key.D1 && digit.ToString() == "Alt+Win+1", "数字快捷键不正确");
        Assert(HotkeyGesture.TryParse("F9", out var function) && function.IsValid, "功能键应允许不带修饰键");
        Assert(!HotkeyGesture.TryParse("Shift+A", out _), "只有 Shift 的快捷键不应有效");
        Assert(!HotkeyGesture.TryParse("Ctrl+Nope", out _), "未知按键不应有效");
        Assert(HotkeyGesture.TryParse("", out var empty) && empty.IsEmpty, "空快捷键应表示关闭");
    }

    private static void TestQuickActionNormalize()
    {
        var normalized = QuickActions.Normalize([QuickActionType.Reveal, QuickActionType.None, QuickActionType.Properties,
            QuickActionType.Properties, QuickActionType.ConvertPng, QuickActionType.CopyFullPath, QuickActionType.MoveTo, QuickActionType.CompressZip]);
        Assert(normalized.SequenceEqual([QuickActionType.CompressZip, QuickActionType.CopyFullPath, QuickActionType.MoveTo, QuickActionType.Properties]),
            "快速操作顺序或数量不正确");
        Assert(QuickActions.Normalize(null).Count == 0, "空列表处理不正确");
        Assert(QuickActions.All.All(info => info.Glyph.Length == 1 && info.Label.Length > 0), "快速操作缺少图标或名称");
        var copyPath = QuickActions.Get(QuickActionType.CopyFullPath);
        Assert(copyPath.Label == "路径" && copyPath.Title == "复制路径", "复制路径的按钮名称应简写为“路径”");

        var custom = new CustomQuickAction { Kind = CustomActionKind.CompressImage, ImageSizeKb = 200 };
        var slots = QuickActions.NormalizeSlots(["Reveal", "Reveal", "nope", custom.Id], [custom]);
        Assert(slots.SequenceEqual(["Reveal", custom.Id, "CompressZip", "CopyFullPath"]), "快速操作位置没有去重或补齐");
        Assert(QuickActions.NormalizeSlots([custom.Id], []).SequenceEqual(QuickActions.Defaults.Select(type => type.ToString())),
            "已删除的自定义操作没有被替换");
        var info = QuickActions.Find(custom.Id, [custom])!;
        Assert(info.IsCustom && info.Label == "≤ 200 KB" && info.Title == "压缩图片到 200 KB" && info.ShowsProgress, "自定义操作名称不正确");
        Assert(QuickActions.Available([custom]).Count == QuickActions.All.Count + 1, "可选操作列表不完整");
    }

    private static void TestQuickActionSlots()
    {
        var controller = new AppController(Dispatcher.CurrentDispatcher);
        var viewModel = new SettingsViewModel(controller);
        controller.UpdateSettings(settings => settings.QuickActionSlots = ["CompressZip", "CopyFullPath", "MoveTo", "Properties"]);
        viewModel.AssignSlot(0, "Reveal");
        Assert(controller.Settings.QuickActionSlots!.SequenceEqual(["Reveal", "CopyFullPath", "MoveTo", "Properties"]), "替换快速操作不正确");
        viewModel.AssignSlot(3, "Reveal");
        Assert(controller.Settings.QuickActionSlots!.SequenceEqual(["Properties", "CopyFullPath", "MoveTo", "Reveal"]), "已在其他位置的操作没有互换");
        Assert(viewModel.VisibleSlots.Select(slot => slot.Position).SequenceEqual(["左一", "左二", "右二", "右一"]), "四个操作时位置应从左到右排列");
        viewModel.ActionLayout = QuickActionLayout.Two;
        Assert(viewModel.VisibleSlots.Count == 2 && viewModel.VisibleSlots[1].Action.Id == "CopyFullPath", "两个按钮时显示的位置不正确");
        viewModel.ActionLayout = QuickActionLayout.Off;
        Assert(viewModel.VisibleSlots.Count == 0 && !viewModel.HasActionSlots, "关闭后仍显示按钮位置");
        File.Delete(new SettingsService().SettingsPath);
    }

    private static void TestStringTables()
    {
        try
        {
            foreach (var language in new[] { AppLanguage.Chinese, AppLanguage.English })
            {
                LanguageService.SetMode(language);
                foreach (var key in LocalizedText.Keys)
                {
                    var text = LocalizedText.Instance[key];
                    Assert(!string.IsNullOrWhiteSpace(text), $"{language} 缺少界面文字：{key}");
                    Assert(language == AppLanguage.Chinese || !text.Any(c => c is >= '\u4e00' and <= '\u9fff'), $"英文界面文字中含有中文：{key}");
                }
            }
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }
        Assert(LocalizedText.Contains("Overview") && !LocalizedText.Contains("NoSuchText"), "界面文字名称查询不正确");
    }

    private static void TestLanguageSwitch()
    {
        Assert(LanguageService.ResolveIsChinese(AppLanguage.System, new CultureInfo("zh-CN")) &&
               LanguageService.ResolveIsChinese(AppLanguage.System, new CultureInfo("zh-TW")) &&
               !LanguageService.ResolveIsChinese(AppLanguage.System, new CultureInfo("en-US")) &&
               LanguageService.ResolveIsChinese(AppLanguage.Chinese, new CultureInfo("en-US")) &&
               !LanguageService.ResolveIsChinese(AppLanguage.English, new CultureInfo("zh-CN")), "跟随系统时的语言判断不正确");

        var changes = 0;
        var notified = new List<string?>();
        EventHandler onLanguage = (_, _) => changes++;
        System.ComponentModel.PropertyChangedEventHandler onText = (_, e) => notified.Add(e.PropertyName);
        LanguageService.LanguageChanged += onLanguage;
        LocalizedText.Instance.PropertyChanged += onText;
        try
        {
            LanguageService.SetMode(AppLanguage.English);
            LanguageService.SetMode(AppLanguage.English);
            Assert(changes == 1 && !LanguageService.IsChinese, "切换到英文时没有通知或重复通知");
            Assert(LocalizedText.Instance["Overview"] == "Overview" && notified.Contains(System.Windows.Data.Binding.IndexerName),
                "绑定使用的文字没有切换");

            var viewModel = new ShelfViewModel();
            viewModel.AddEntries([new DroppedEntry(ShelfItemKind.Text, "a"), new DroppedEntry(ShelfItemKind.Text, "b")]);
            Assert(viewModel.Title == "2 items" && viewModel.StackDetail == "2 snippets", $"英文汇总不正确：{viewModel.StackDetail}");
            Assert(new ShelfAddSummary(1, 2, 0).Message == "Added 1 item, skipped 2 duplicates", "英文添加结果不正确");
            Assert(QuickActions.Get(QuickActionType.CompressZip).Label == "Compress", "快速操作名称没有切换");
            Assert(ShellFileInfo.GetTypeName("noextension") == "File", "文件类型备用名称没有切换");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
            LanguageService.LanguageChanged -= onLanguage;
            LocalizedText.Instance.PropertyChanged -= onText;
        }
        Assert(changes == 2 && LocalizedText.Instance["Overview"] == "概览" && new ShelfAddSummary(0, 3, 0).Message == "这些项目已在置物架中",
            "切换回中文后文字不正确");
    }

    private static void TestShelfItemText()
    {
        var folder = new ShelfItem { Id = "1", Kind = ShelfItemKind.Folder, Value = @"C:\Users\Test\Documents\" };
        Assert(folder.DisplayName == "Documents", "带结尾分隔符的文件夹名称不正确");
        var text = new ShelfItem { Id = "2", Kind = ShelfItemKind.Text, Value = "  第一行   内容\n第二行" };
        Assert(text.DisplayName == "第一行 内容", "文本摘要不正确");
        Assert(ShelfItem.Summarize("\n\n  a \t b  \r\nc", 10) == "a b" && ShelfItem.Summarize(string.Concat(Enumerable.Repeat("ab ", 100_000)), 5) == "ab ab…",
            "长文本或空白开头的文本摘要不正确");
        var link = new ShelfItem { Id = "3", Kind = ShelfItemKind.Link, Value = "https://example.com/docs/page", Title = "示例文档" };
        Assert(link.DisplayName == "示例文档" && link.Detail == "example.com", "链接显示不正确");
        var untitled = new ShelfItem { Id = "4", Kind = ShelfItemKind.Link, Value = "https://example.com/docs/" };
        Assert(untitled.DisplayName == "example.com/docs", "无标题链接显示不正确");
        Assert(ShelfItem.FormatSize(512) == "512 B" && ShelfItem.FormatSize(1536) == "1.5 KB" && ShelfItem.FormatSize(5 * 1024 * 1024) == "5 MB",
            "文件大小格式不正确");
    }

    private static void TestShelfSizes()
    {
        var widths = new[] { QuickShelfSize.Small, QuickShelfSize.Medium, QuickShelfSize.Large }.Select(QuickShelfSizeOptions.ResolveWidth).ToArray();
        Assert(widths[0] < widths[1] && widths[1] < widths[2], "三档尺寸没有依次增大");
        foreach (var size in Enum.GetValues<QuickShelfSize>())
        {
            var ratio = QuickShelfSizeOptions.ResolveStackHeight(size) / QuickShelfSizeOptions.ResolveWidth(size);
            Assert(Math.Abs(ratio - 0.66) < 0.01, "堆叠区域比例不一致");
            Assert(QuickShelfSizeOptions.ResolveExpandedWidth(size) >= 300, "展开列表过窄");
        }
    }

    private static void TestQuickShelfPositioning()
    {
        var centered = QuickShelfPositioner.Calculate(500, 400, 0, 0, 1000, 800, 300, 300);
        Assert(centered == new QuickShelfPlacement(350, 250), "小窗没有以鼠标位置为中心");
        var topLeft = QuickShelfPositioner.Calculate(10, 10, 0, 0, 1000, 800, 300, 300);
        Assert(topLeft == new QuickShelfPlacement(0, 0), "左上角位置超出屏幕");
        var bottomRight = QuickShelfPositioner.Calculate(990, 790, 0, 0, 1000, 800, 300, 300);
        Assert(bottomRight == new QuickShelfPlacement(700, 500), "右下角位置超出屏幕");
        var secondMonitor = QuickShelfPositioner.Calculate(-500, 300, -1920, 0, 0, 1080, 300, 300);
        Assert(secondMonitor == new QuickShelfPlacement(-650, 150), "左侧副屏位置不正确");
    }

    private static void TestProductVersion()
    {
        var assembly = typeof(AppController).Assembly;
        var version = assembly.GetName().Version;
        var fileVersion = System.Diagnostics.FileVersionInfo.GetVersionInfo(assembly.Location).FileVersion;
        Assert(version is { Major: >= 2 } && fileVersion == version.ToString(), $"程序版本号不正确：{version} / {fileVersion}");
    }
}
