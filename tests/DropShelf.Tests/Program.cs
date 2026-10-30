using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.Services.Gestures;
using DropShelf.ViewModels;
using DropShelf.Views;
using DropShelf.Views.Controls;
using WpfApplication = System.Windows.Application;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static class Program
{
    private static int _passed;
    private static int _failed;
    private static string _root = string.Empty;

    /// <summary>临时文件夹以外的测试目录：临时文件夹中拖入的文件会被当作聊天软件的临时文件另存一份。</summary>
    private static readonly string _stableRoot = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DropShelf.Tests", Guid.NewGuid().ToString("N"));

    [STAThread]
    private static int Main()
    {
        Console.OutputEncoding = Encoding.UTF8;
        _root = Path.Combine(Path.GetTempPath(), "DropShelf.Tests", DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        Directory.CreateDirectory(_root);
        Environment.SetEnvironmentVariable("DROPSHELF_LOG_DIRECTORY", Path.Combine(_root, "logs"));
        Environment.SetEnvironmentVariable("DROPSHELF_SETTINGS_PATH", Path.Combine(_root, "settings.json"));
        Environment.SetEnvironmentVariable("DROPSHELF_INSTANCE", "tests");

        // 测试在 STA 主线程上运行，await 之后回到同一线程，剪贴板和界面测试才能正常工作。
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext());
        var application = new WpfApplication { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        application.Resources.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri("pack://application:,,,/DropShelf;component/Themes/Tokens.xaml") });
        application.Resources.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri("pack://application:,,,/DropShelf;component/Themes/Controls.xaml") });
        ThemeService.Initialize(AppTheme.Light, null);
        // 断言按中文界面文字编写，固定语言，避免受运行测试的电脑的显示语言影响。
        LanguageService.SetMode(AppLanguage.Chinese);

        // 设置与模型
        Run("设置默认值与保存恢复", TestSettingsRoundTrip);
        Run("1.0 版设置自动迁移", TestSettingsMigration);
        Run("异常设置恢复为默认值", TestSettingsNormalization);
        Run("快捷键解析与显示", TestHotkeyGesture);
        Run("快速操作去重、排序且最多四个", TestQuickActionNormalize);
        Run("快速操作位置可替换并互换", TestQuickActionSlots);
        Run("置物架项目名称与说明", TestShelfItemText);
        Run("三档尺寸保持比例", TestShelfSizes);
        Run("小窗以鼠标为中心并保持在屏幕内", TestQuickShelfPositioning);
        Run("正式版本号已写入程序", TestProductVersion);
        Run("界面文字中英文齐全", TestStringTables);
        Run("切换语言后文字随之更新", TestLanguageSwitch);

        // 手势识别
        Run("均衡手势可识别快速左右晃动", TestBalancedGestureTriggers);
        Run("三档灵敏度都能触发", TestSensitivityProfilesTrigger);
        Run("过快抖动和缓慢往返不会触发", TestGestureDurationBounds);
        Run("不规则采样和自然晃动不会漏识别", TestGestureToleratesIrregularSamples);
        Run("纵向和斜向晃动可以触发", TestOtherDirectionGesturesTrigger);
        Run("普通直线和转弯移动不会触发", TestNormalMovementDoesNotTrigger);
        Run("屏幕边缘移动不会触发", TestScreenEdgeDoesNotTrigger);
        Run("高回报率鼠标的晃动可以识别", TestHighRateShakeTriggers);
        Run("低精度计时下的高回报率晃动可以识别", TestCoarseTimerShakeTriggers);
        Run("高回报率鼠标的普通移动不会触发", TestHighRateNormalMovement);
        Run("掉头时的细小侧移不会打断手势", TestTurnaroundJitter);
        Run("鼠标输入监听可以启动和停止", TestGestureServiceLifecycle);
        Run("拖到屏幕边缘停留后触发", TestEdgeDwellDetector);

        // 置物架逻辑
        Run("100 个文件与重复路径", TestOneHundredFilesAndDuplicates);
        Run("拖入文件只读取路径属性", TestAddingLockedFileReadsNoContent);
        Run("清空置物架不影响磁盘文件", TestClearKeepsFiles);
        Run("文本与链接可以暂存并去重", TestTextAndLinkEntries);
        Run("堆叠显示最新项目和汇总", TestStackSummary);
        Run("拖出结果按设置移除或保留", TestApplyDragOut);
        Run("拖出结果判定（含资源管理器优化移动）", TestResolveDragOut);
        Run("移动后更新置物架中的路径", TestApplyMoves);
        Run("最近关闭的置物架只记录路径和名称", TestShelfHistory);
        Run("拖出时按设置移动或复制", TestDragOutEffects);

        // 拖放与剪贴板数据
        Run("识别拖入内容类型", TestClassifyDropData);
        Run("读取文件、文本、链接和图片", TestReadDropData);
        Run("识别来自置物架自身的拖动", TestSourceShelfId);
        Run("拖出数据包含不同文件夹中的全部文件", TestShellDataObject);
        Run("未落盘附件可接收为实际文件", TestVirtualAttachmentImport);
        Run("临时附件拖到另一个置物架后共同持有缓存", TestAttachmentSharedBetweenShelves);
        Run("聊天软件的临时文件会立即保存一份", TestVolatileFileSnapshot);
        Run("聊天软件提供的图片格式与富文本图片", TestChatAppImageFormats);
        Run("富文本中的网络路径不会被访问", TestRichTextRejectsNetworkPaths);
        Run("非网页链接按文本保存，不交给系统打开", TestNonWebLinksBecomeText);

        // 文件与快速操作
        Run("同名复制不覆盖", TestCopyDoesNotOverwrite);
        Run("源文件消失时返回失败", TestMissingSourceIsReported);
        Run("文件被占用时返回失败", TestLockedFileIsReported);
        Run("阻止复制到文件夹自身或其内部", TestNestedDestinationIsRejected);
        Run("复制保留下载来源标记和修改时间", TestCopyKeepsFileMetadata);
        Run("含联接点的文件夹明确失败且不改动", TestFolderLinksAreRejected);
        Run("快速压缩与安全解压", TestArchiveActions);
        Run("复制路径写入剪贴板", TestCopyPathAction);
        Run("移动到所选文件夹并可取消", TestMoveToAction);
        Run("压缩结果放进新的置物架", TestArchiveAddsOutputToShelf);
        Run("复制文件到剪贴板", TestCopyFilesToClipboard);
        Run("自定义操作：复制或移动到文件夹", TestCustomFolderAction);
        Run("自定义操作：压缩图片到指定大小", TestImageCompression);
        Run("压缩后的图片替换置物架中的原图", TestCompressImageReplacesItems);
        Run("自定义操作：用程序打开的参数", TestOpenWithArguments);

        // 系统集成
        Run("文件使用 Windows 系统缩略图和类型名称", TestShellThumbnail);
        Run("后台图片处理集中在同一个常驻线程", TestImagingThread);
        Run("开机启动命令正确处理空格路径", TestStartupCommand);
        Run("主题解析与强调色对比度", TestThemeAndAccent);
        Run("牌角花色按文件类型", TestFileSuits);
        Run("出场离场的牌落点、版本轮换和平铺行列", TestShelfMotion);
        Run("托盘图标可从图标资源创建", TestTrayIconLoader);
        Run("全局快捷键可注册和注销", TestHotkeyRegistration);
        Run("命令行文件可转交给运行中的实例", TestArgumentForwarding);
        Run("错误日志已生成", TestFailureLogExists);

        // 界面
        Run("置物架窗口按设置生成快速操作", TestShelfWindowActions);
        Run("主窗口整合设置并自动保存", TestMainWindowSettings);
        Run("教程可以逐步切换", TestOnboardingSteps);
        Run("探针窗口可以显示和隐藏", TestProbeWindow);

        try { if (Directory.Exists(_stableRoot)) Directory.Delete(_stableRoot, true); }
        catch { }

        Console.WriteLine();
        Console.WriteLine($"结果：通过 {_passed}，失败 {_failed}");
        Console.WriteLine($"测试样本保留在：{_root}");
        return _failed == 0 ? 0 : 1;
    }

    // ───────────── 设置与模型 ─────────────

    private static void TestSettingsRoundTrip()
    {
        var service = new SettingsService();
        var defaults = service.Load();
        Assert(defaults.CloseWhenEmpty && defaults.ShowGettingStarted, "默认开关不正确");
        Assert(defaults.AccentColor is null, "默认强调色应为钴蓝");
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
        Assert(SettingsService.Normalize(new AppSettings { AccentColor = ThemeService.SystemAccent }).AccentColor == ThemeService.SystemAccent,
            "跟随 Windows 的强调色不应被清除");
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

    // ───────────── 手势识别 ─────────────

    private static void TestBalancedGestureTriggers()
    {
        var detector = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var samples = new[] { (100, 100, 0L), (150, 101, 60L), (95, 100, 120L), (155, 101, 180L), (90, 100, 240L) };
        Assert(samples.Any(sample => detector.AddSample(sample.Item1, sample.Item2, sample.Item3, false)), "快速左右晃动未被识别");
    }

    private static void TestSensitivityProfilesTrigger()
    {
        Assert(Shake(ShakeSensitivities.Responsive, [100, 134, 100, 134], 70), "灵敏档无法通过短距离两次来回触发");
        Assert(Shake(ShakeSensitivities.Balanced, [100, 143, 100, 143, 100], 70), "均衡档无法通过正常幅度晃动触发");
        Assert(Shake(ShakeSensitivities.Steady, [100, 185, 100, 185, 100, 185, 100, 185], 100), "稳健档无法通过明确晃动触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 165, 100, 165, 100, 165, 100], 70), "最稳健时被 2.5.7 的稳健档晃动触发");

        // 三档的差距：灵敏档轻晃即可，均衡档不行；均衡档的正常晃动在稳健档不够。
        Assert(Shake(ShakeSensitivities.Responsive, [100, 125, 100, 125], 70), "灵敏档无法通过轻晃触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 150, 100, 150, 100, 150, 100], 70), "稳健档被中等幅度晃动触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 165, 100, 165, 100], 70), "稳健档只来回两次就被触发");

        // 拖动时手部的小幅来回不应触发。
        Assert(!Shake(ShakeSensitivities.Responsive, [100, 120, 100, 120], 70), "灵敏档被小幅来回误触发");
        Assert(!Shake(ShakeSensitivities.Balanced, [100, 125, 100, 125, 100], 70), "均衡档被小幅晃动误触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 135, 100, 135, 100, 135], 70), "稳健档被小幅晃动误触发");

        // 无级调节：同样的晃动，灵敏度越高只会越容易触发，中途不会突然变难。
        for (var amplitude = 10; amplitude <= 80; amplitude += 2)
        {
            var xs = Enumerable.Range(0, 8).Select(index => index % 2 == 0 ? 100 : 100 + amplitude).ToArray();
            var previous = false;
            for (var level = ShakeSensitivities.Steady; level <= ShakeSensitivities.Responsive; level++)
            {
                var triggered = Shake(level, xs, 70);
                Assert(triggered || !previous, $"灵敏度 {level} 反而比 {level - 1} 更难触发（幅度 {amplitude}）");
                previous = triggered;
            }
        }

        // 设置页的说明：两端和中间与原来的三档一致，拖向灵敏时幅度和次数只减不增，且照说明晃动一定能触发。
        var descriptions = Enumerable.Range(0, 101).Select(MouseGestureDetector.Describe).ToArray();
        Assert(descriptions[0] == (5, 80) && descriptions[50] == (3, 38) && descriptions[100] == (2, 23), "灵敏度两端和中间的说明不正确");
        Assert(descriptions.Zip(descriptions.Skip(1)).All(pair => pair.Second.Distance <= pair.First.Distance && pair.Second.Reversals <= pair.First.Reversals),
            "灵敏度说明没有随滑块单调变化");
        Assert(descriptions.Select((description, level) => (description, level)).All(item =>
                Shake(item.level, Enumerable.Range(0, item.description.Reversals + 2)
                    .Select(index => index % 2 == 0 ? 100 : 101 + item.description.Distance).ToArray(), 70)),
            "按设置页说明的幅度和次数晃动没有触发");

        static bool Shake(int sensitivity, int[] xs, long interval)
        {
            var detector = new MouseGestureDetector(sensitivity);
            return xs.Select((x, index) => (x, index)).Any(sample => detector.AddSample(sample.x, 100, sample.index * interval, false));
        }
    }

    private static void TestGestureDurationBounds()
    {
        var tooFast = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var fastSamples = new[] { (100, 0L), (143, 30L), (100, 60L), (143, 90L), (100, 120L), (143, 150L) };
        Assert(!fastSamples.Any(sample => tooFast.AddSample(sample.Item1, 100, sample.Item2, false)), "瞬间抖动被错误识别为晃动手势");

        var tooSlow = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var slowSamples = new[] { (100, 0L), (143, 300L), (100, 600L), (143, 900L), (100, 1200L), (143, 1500L) };
        Assert(!slowSamples.Any(sample => tooSlow.AddSample(sample.Item1, 100, sample.Item2, false)), "缓慢来回移动被错误识别为晃动手势");
    }

    private static void TestGestureToleratesIrregularSamples()
    {
        var delayed = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var delayedSamples = new[] { (100, 100, 0L), (143, 103, 60L), (100, 99, 380L), (144, 105, 445L), (99, 98, 510L) };
        Assert(delayedSamples.Any(sample => delayed.AddSample(sample.Item1, sample.Item2, sample.Item3, false)),
            "系统短暂忙碌导致采样间隔变长后未能识别手势");

        var compact = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var compactSamples = new[] { 100, 138, 100, 138, 100, 138 };
        Assert(compactSamples.Select((x, index) => (x, index))
                .Any(sample => compact.AddSample(sample.x, 100 + sample.index % 2 * 2, sample.index * 55L, false)),
            "合理幅度和时长的晃动未被识别");
    }

    private static void TestOtherDirectionGesturesTrigger()
    {
        var vertical = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var verticalSamples = new[] { (100, 100), (101, 150), (100, 95), (101, 155), (100, 90) };
        Assert(verticalSamples.Select((point, index) => (point, index))
            .Any(sample => vertical.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 65L, false)), "快速上下晃动未被识别");

        var diagonal = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var diagonalSamples = new[] { (100, 100), (140, 140), (90, 90), (145, 145), (85, 85) };
        Assert(diagonalSamples.Select((point, index) => (point, index))
            .Any(sample => diagonal.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 65L, false)), "快速斜向晃动未被识别");
    }

    private static void TestNormalMovementDoesNotTrigger()
    {
        var straight = new MouseGestureDetector(ShakeSensitivities.Responsive);
        Assert(!Enumerable.Range(0, 12).Any(index => straight.AddSample(100 + index * 12, 100 + index, index * 40L, false)), "普通直线移动被误识别");

        var turning = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var turningSamples = new[] { (100, 100), (140, 100), (140, 140), (100, 140), (100, 100), (140, 100) };
        Assert(!turningSamples.Select((point, index) => (point, index))
            .Any(sample => turning.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 45L, false)), "普通转弯移动被误识别");
    }

    private static void TestScreenEdgeDoesNotTrigger()
    {
        var detector = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var samples = new[] { (100, 0L), (150, 50L), (95, 100L), (155, 150L), (90, 200L) };
        Assert(!samples.Any(sample => detector.AddSample(sample.Item1, 4, sample.Item2, true)), "屏幕边缘移动被误识别");
    }

    private static void TestHighRateShakeTriggers()
    {
        foreach (var sensitivity in new[] { ShakeSensitivities.Steady, 25, ShakeSensitivities.Balanced, 75, ShakeSensitivities.Responsive })
        {
            var detector = new MouseGestureDetector(sensitivity);
            var random = new Random(7);
            var triggered = false;
            // 1000 Hz 回报率，约 4 次/秒、幅度 ±60 像素的左右晃动，叠加 ±1 像素的抖动。
            for (var ms = 0; ms < 1200 && !triggered; ms++)
            {
                var x = 500 + (int)Math.Round(60 * Math.Sin(ms / 1000.0 * 2 * Math.PI * 4)) + random.Next(-1, 2);
                var y = 300 + random.Next(-1, 2);
                triggered = detector.AddRawSample(x, y, ms, false);
            }
            Assert(triggered, $"{sensitivity} 档没有识别高回报率鼠标的晃动");
        }
    }

    private static void TestCoarseTimerShakeTriggers()
    {
        // 复现：系统计时每 15.6 毫秒才跳一次，1000 Hz 鼠标的多次采样时间完全相同。
        foreach (var sensitivity in new[] { ShakeSensitivities.Steady, 25, ShakeSensitivities.Balanced, 75, ShakeSensitivities.Responsive })
        {
            var detector = new MouseGestureDetector(sensitivity);
            var triggered = false;
            for (var ms = 0; ms < 1200 && !triggered; ms++)
            {
                var x = 500 + (int)Math.Round(55 * Math.Sin(ms / 1000.0 * 2 * Math.PI * 4.5));
                var coarse = (long)(Math.Floor(ms / 15.625) * 15.625);
                triggered = detector.AddRawSample(x, 300 + ms % 3, coarse, false);
            }
            Assert(triggered, $"{sensitivity} 档在低精度计时下没有识别晃动");
        }
    }

    private static void TestHighRateNormalMovement()
    {
        var straight = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var straightTriggered = Enumerable.Range(0, 1500).Any(ms => straight.AddRawSample(100 + ms, 300 + ms / 10, ms, false));
        Assert(!straightTriggered, "高回报率的直线拖动被误识别");

        var circle = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var circleTriggered = Enumerable.Range(0, 2000).Any(ms =>
            circle.AddRawSample(500 + (int)(80 * Math.Cos(ms / 300.0)), 400 + (int)(80 * Math.Sin(ms / 300.0)), ms, false));
        Assert(!circleTriggered, "绕圈拖动被误识别");
    }

    private static void TestTurnaroundJitter()
    {
        // 每次掉头时先出现一次几乎垂直的细小位移，再继续反向移动；旧算法会因此清空已记录的往返。
        var samples = new List<(int X, int Y)> { (100, 100) };
        var x = 100;
        for (var cycle = 0; cycle < 5; cycle++)
        {
            var direction = cycle % 2 == 0 ? 1 : -1;
            for (var step = 0; step < 4; step++)
            {
                x += direction * 12;
                samples.Add((x, 100));
            }
            samples.Add((x + direction, 105));
        }
        var detector = new MouseGestureDetector(ShakeSensitivities.Balanced);
        Assert(samples.Select((point, index) => (point, index))
            .Any(sample => detector.AddSample(sample.point.X, sample.point.Y, sample.index * 14L, false)), "掉头时的侧向位移打断了手势");

        var sideways = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var zigzag = new[] { (100, 100), (130, 100), (131, 106), (131, 112), (131, 118), (101, 118), (100, 124), (100, 130) };
        Assert(!zigzag.Select((point, index) => (point, index))
            .Any(sample => sideways.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 40L, false)), "连续侧向移动被误识别");
    }

    private static void TestGestureServiceLifecycle()
    {
        var service = new GestureService(Dispatcher.CurrentDispatcher, ShakeSensitivities.Balanced);
        service.Start();
        Assert(service.IsRunning, "鼠标原始输入没有注册");
        service.SetSensitivity(ShakeSensitivities.Steady);
        service.SetTrigger(AutoOpenTrigger.Edge);
        service.SetEdgeDwell(EdgeDwellTime.Short);
        service.Dispose();
        Thread.Sleep(100);
        Assert(!service.IsRunning, "停止后仍在监听鼠标输入");
    }

    private static void TestEdgeDwellDetector()
    {
        var detector = new EdgeDwellDetector(AutoOpenTriggers.DwellMilliseconds(EdgeDwellTime.Medium));
        Assert(!detector.Update(ScreenEdge.Left, 0, 300, 0) && !detector.Update(ScreenEdge.Left, 0, 310, 300) &&
               detector.Update(ScreenEdge.Left, 0, 320, 600), "在左边缘停留 0.6 秒没有触发");
        Assert(!detector.Update(ScreenEdge.Left, 0, 300, 1000) && !detector.Update(ScreenEdge.None, 40, 300, 1300) &&
               !detector.Update(ScreenEdge.Left, 0, 300, 1400) && !detector.Update(ScreenEdge.Left, 0, 300, 1900), "离开边缘后没有重新计时");
        Assert(!detector.Update(ScreenEdge.Right, 1919, 300, 3000) && !detector.Update(ScreenEdge.Right, 1919, 300 + EdgeDwellDetector.MaximumDrift + 1, 3500) &&
               !detector.Update(ScreenEdge.Right, 1919, 400, 4000), "沿边缘大幅移动后没有重新计时");
        Assert(!detector.Update(ScreenEdge.Top, 100, 0, 5000) && !detector.Update(ScreenEdge.Left, 0, 0, 5400), "换到另一条边缘时没有重新计时");
        detector.DwellMilliseconds = AutoOpenTriggers.DwellMilliseconds(EdgeDwellTime.Short);
        Assert(detector.Update(ScreenEdge.Left, 0, 0, 5700), "停留时间调整后没有生效");
        Assert(AutoOpenTriggers.UsesShake(AutoOpenTrigger.Both) && AutoOpenTriggers.UsesEdge(AutoOpenTrigger.Both) &&
               !AutoOpenTriggers.UsesShake(AutoOpenTrigger.Edge) && !AutoOpenTriggers.UsesEdge(AutoOpenTrigger.Shake), "呼出方式判断不正确");
    }

    // ───────────── 置物架逻辑 ─────────────

    private static void TestOneHundredFilesAndDuplicates()
    {
        var folder = CreateFolder("hundred-files");
        var paths = Enumerable.Range(1, 100).Select(index =>
        {
            var path = Path.Combine(folder, $"sample-{index:000}.txt");
            File.WriteAllText(path, index.ToString());
            return path;
        }).ToArray();

        var viewModel = new ShelfViewModel();
        var countNotifications = 0;
        viewModel.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(ShelfViewModel.Count)) countNotifications++; };
        var first = viewModel.AddFiles(paths);
        Assert(countNotifications == 1 && viewModel.StackDetail.StartsWith(Strings.FileCount(100)), "批量添加时数量通知没有合并或汇总不正确");
        var second = viewModel.AddFiles(paths);
        Assert(viewModel.Items.Count == 100, $"预期 100 项，实际 {viewModel.Items.Count} 项");
        Assert(first.Added == 100 && second.Duplicates == 100 && second.Message == "这些项目已在置物架中", "添加结果统计不正确");
        var missing = viewModel.AddFiles([Path.Combine(folder, "missing.txt")]);
        Assert(missing.Unavailable == 1 && viewModel.Items.Count == 100, "不存在的文件被添加");
        countNotifications = 0;
        viewModel.Remove(viewModel.Items.Take(60));
        Assert(countNotifications == 1 && viewModel.Items.Count == 40 && viewModel.StackDetail.StartsWith(Strings.FileCount(40)), "批量移除时数量通知没有合并或汇总不正确");
    }

    private static void TestAddingLockedFileReadsNoContent()
    {
        var folder = CreateFolder("path-only-drop");
        var path = Path.Combine(folder, "large-locked.bin");
        File.WriteAllBytes(path, [1, 2, 3]);
        using var lockStream = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([path]);
        Assert(viewModel.Items.Count == 1, "拖入时尝试读取了文件内容");
        Assert(viewModel.Items[0].SizeBytes == 3, "没有记录文件大小");
    }

    private static void TestClearKeepsFiles()
    {
        var folder = CreateFolder("clear-keeps-files");
        var path = Path.Combine(folder, "keep.txt");
        File.WriteAllText(path, "keep me");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([path, folder]);
        Assert(viewModel.Items.Any(item => item.Kind == ShelfItemKind.Folder), "文件夹没有识别为文件夹");
        viewModel.Clear();
        Assert(viewModel.Items.Count == 0, "置物架没有清空");
        Assert(File.ReadAllText(path) == "keep me", "清空置物架改变了磁盘文件");
    }

    private static void TestTextAndLinkEntries()
    {
        var viewModel = new ShelfViewModel();
        var summary = viewModel.AddEntries([
            new DroppedEntry(ShelfItemKind.Text, "一段文本"),
            new DroppedEntry(ShelfItemKind.Link, "https://example.com", "示例"),
            new DroppedEntry(ShelfItemKind.Text, "一段文本")
        ]);
        Assert(summary.Added == 2 && summary.Duplicates == 1, "文本或链接去重不正确");
        Assert(viewModel.Items[1].Title == "示例", "链接标题丢失");
    }

    private static void TestStackSummary()
    {
        var folder = CreateFolder("stack-summary");
        var a = Path.Combine(folder, "a.txt");
        var b = Path.Combine(folder, "b.txt");
        File.WriteAllBytes(a, new byte[1024]);
        File.WriteAllBytes(b, new byte[2048]);
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([a]);
        Assert(viewModel.StackCaption == "a.txt" && viewModel.StackDetail.StartsWith("1 KB"), "单个项目的堆叠说明不正确");
        viewModel.AddFiles([b, folder]);
        Assert(ReferenceEquals(viewModel.Stack0, viewModel.Items[^1]), "堆叠最上面不是最新项目");
        Assert(viewModel.Stack2 is not null && viewModel.HasMultiple, "三张堆叠卡片不完整");
        Assert(viewModel.StackCaption == "3 个项目" && viewModel.StackDetail == "2 个文件 · 1 个文件夹 · 3 KB", $"汇总不正确：{viewModel.StackDetail}");
        Assert(viewModel.Title == "3 个项目", "标题不正确");
    }

    private static void TestApplyDragOut()
    {
        var folder = CreateFolder("drag-out");
        var paths = Enumerable.Range(1, 3).Select(index =>
        {
            var path = Path.Combine(folder, $"item-{index}.txt");
            File.WriteAllText(path, "content");
            return path;
        }).ToArray();

        var viewModel = new ShelfViewModel();
        viewModel.AddFiles(paths);
        Assert(!viewModel.ApplyDragOut([viewModel.Items[0]], DragOutResult.Cancelled, true) && viewModel.Items.Count == 3, "取消拖动时移除了项目");
        Assert(!viewModel.ApplyDragOut([viewModel.Items[0]], DragOutResult.Copied, false) && viewModel.Items.Count == 3, "设置为保留时仍移除了项目");
        Assert(viewModel.ApplyDragOut([viewModel.Items[0]], DragOutResult.Copied, true) && viewModel.Items.Count == 2, "复制拖出后没有移除项目");
        Assert(File.Exists(paths[0]), "移除置物架引用时删除了磁盘文件");

        var moved = viewModel.Items[0];
        File.Move(moved.Value, moved.Value + ".moved");
        Assert(viewModel.ApplyDragOut([moved], DragOutResult.Moved, false) && !viewModel.Items.Contains(moved), "源文件已移动但项目仍留在置物架");
    }

    private static void TestResolveDragOut()
    {
        const int dropped = NativeMethods.DRAGDROP_S_DROP;
        const int cancelled = NativeMethods.DRAGDROP_S_CANCEL;
        Assert(ShelfDragService.Resolve(cancelled, 0, null, null, false) == DragOutResult.Cancelled, "取消没有识别");
        Assert(ShelfDragService.Resolve(dropped, 1, null, null, false) == DragOutResult.Copied, "复制没有识别");
        Assert(ShelfDragService.Resolve(dropped, 2, null, null, false) == DragOutResult.Moved, "移动没有识别");
        Assert(ShelfDragService.Resolve(dropped, 4, null, null, false) == DragOutResult.Linked, "创建快捷方式没有识别");
        Assert(ShelfDragService.Resolve(dropped, 0, 0, 2, false) == DragOutResult.Moved, "资源管理器的优化移动没有识别");
        Assert(ShelfDragService.Resolve(dropped, 0, null, null, true) == DragOutResult.Moved, "源文件消失时没有判定为移动");
        Assert(ShelfDragService.Resolve(dropped, 0, null, null, false) == DragOutResult.Cancelled, "目标拒绝放下时被误判为成功");
    }

    private static void TestApplyMoves()
    {
        var folder = CreateFolder("apply-moves");
        var source = Path.Combine(folder, "before.txt");
        File.WriteAllText(source, "x");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([source]);
        var id = viewModel.Items[0].Id;
        viewModel.ApplyMoves([(source, Path.Combine(folder, "after.txt"))]);
        Assert(viewModel.Items.Count == 1 && viewModel.Items[0].Value.EndsWith("after.txt") && viewModel.Items[0].Id == id, "移动后路径没有更新");
    }

    private static void TestShelfHistory()
    {
        var folder = CreateFolder("history");
        var files = Enumerable.Range(1, 3).Select(index =>
        {
            var path = Path.Combine(folder, $"h{index}.txt");
            File.WriteAllText(path, "x");
            return path;
        }).ToArray();
        var historyPath = Path.Combine(_root, "history-test.json");
        var history = new ShelfHistoryService(historyPath);

        var viewModel = new ShelfViewModel();
        viewModel.AddFiles(files);
        viewModel.AddEntries([new DroppedEntry(ShelfItemKind.Text, "secret text"), new DroppedEntry(ShelfItemKind.Link, "https://example.com")]);
        viewModel.AddFiles([files[0] + ".tmp-copy"]);
        var temporary = Path.Combine(folder, "attachment.txt");
        File.WriteAllText(temporary, "x");
        viewModel.AddFiles([temporary], temporary: true);

        Assert(history.Record(viewModel.Items), "没有记录关闭的置物架");
        var entry = history.Latest!;
        Assert(entry.Items.Select(item => item.Path).SequenceEqual(files) && entry.Items[0].Name == "h1.txt", "记录的路径或名称不正确");
        history.Flush();
        var json = File.ReadAllText(historyPath);
        Assert(!json.Contains("secret text") && !json.Contains("example.com") && !json.Contains("attachment.txt"), "记录中包含了文本、链接或临时附件");
        Assert(!history.Record([]), "空置物架不应记录");

        history.Record(viewModel.Items.Reverse());
        Assert(history.Entries.Count == 1, "相同内容的置物架重复记录");
        for (var index = 0; index < 12; index++)
        {
            var path = Path.Combine(folder, $"single-{index}.txt");
            File.WriteAllText(path, "x");
            var single = new ShelfViewModel();
            single.AddFiles([path]);
            history.Record(single.Items);
        }
        Assert(history.Entries.Count == ShelfHistoryService.Capacity && history.Latest!.Items[0].Name == "single-11.txt", "最多保留 10 个");
        history.Flush();

        var reloaded = new ShelfHistoryService(historyPath);
        Assert(reloaded.Entries.Count == ShelfHistoryService.Capacity && reloaded.Latest!.Items[0].Path.EndsWith("single-11.txt"), "重新启动后记录丢失");
        var moved = reloaded.Entries[0];
        reloaded.Move(moved, 3);
        reloaded.Flush();
        Assert(reloaded.Entries[3] == moved && new ShelfHistoryService(historyPath).Entries[3].Items[0].Path == moved.Items[0].Path, "调整记录顺序没有生效或没有保存");
        reloaded.Move(moved, 0);
        reloaded.Remove(reloaded.Latest!);
        Assert(reloaded.Entries.Count == ShelfHistoryService.Capacity - 1, "恢复后记录没有移除");
        reloaded.Clear();
        reloaded.Flush();
        Assert(new ShelfHistoryService(historyPath).Entries.Count == 0, "清除记录没有保存");
        // 拖动卡片收尾时列表可能已被清空，延迟执行的换序不应出错。
        reloaded.Move(moved, 3);
        Assert(reloaded.Entries.Count == 0, "对已移除的记录换序不应改动列表");
    }

    private static void TestDragOutEffects()
    {
        Assert(ShelfDragService.GetAllowedEffects(TransferMode.Move, false) == 7, "移动模式应同时允许复制，便于拖到聊天软件");
        Assert(ShelfDragService.GetAllowedEffects(TransferMode.Copy, false) == 5, "复制模式不应允许移动");
        Assert(ShelfDragService.GetAllowedEffects(TransferMode.Move, true) == 3 && ShelfDragService.GetAllowedEffects(TransferMode.Copy, true) == 1,
            "临时附件不应提供快捷方式");
    }

    // ───────────── 拖放与剪贴板数据 ─────────────

    private static void TestClassifyDropData()
    {
        var folder = CreateFolder("classify");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");

        var files = new WpfDataObject();
        files.SetData(System.Windows.DataFormats.FileDrop, new[] { file });
        Assert(DropDataReader.Classify(files) == DropContentKind.Files, "文件没有识别");

        var text = new WpfDataObject();
        text.SetData(System.Windows.DataFormats.UnicodeText, "hello");
        Assert(DropDataReader.Classify(text) == DropContentKind.Text, "文本没有识别");

        var link = new WpfDataObject();
        link.SetData("UniformResourceLocatorW", new MemoryStream(Encoding.Unicode.GetBytes("https://example.com\0")));
        Assert(DropDataReader.Classify(link) == DropContentKind.Link, "链接没有识别");

        var image = new WpfDataObject();
        image.SetData("PNG", new MemoryStream(CreatePng()));
        Assert(DropDataReader.Classify(image) == DropContentKind.Image, "图片没有识别");
        Assert(DropDataReader.Classify(new WpfDataObject()) == DropContentKind.None, "空数据被误识别");
        Assert(DropDataReader.CanProvideFiles(DropContentKind.Image) && !DropDataReader.CanProvideFiles(DropContentKind.Text),
            "快速操作可用内容判断不正确");
    }

    private static async Task TestReadDropData()
    {
        var folder = CreateFolder("read-drop");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");
        var files = new WpfDataObject();
        files.SetData(System.Windows.DataFormats.FileDrop, new[] { file, folder });
        var read = await DropDataReader.ReadAsync(files);
        Assert(read.Entries.Count == 2 && read.Entries[1].Kind == ShelfItemKind.Folder && read.FilePaths.Count == 2, "文件读取不正确");

        var url = new WpfDataObject();
        url.SetData(System.Windows.DataFormats.UnicodeText, "  https://example.com/page  ");
        var link = await DropDataReader.ReadAsync(url);
        Assert(link.Entries.Single().Kind == ShelfItemKind.Link && link.Entries[0].Value == "https://example.com/page", "网址文本没有识别为链接");

        var moz = new WpfDataObject();
        moz.SetData("text/x-moz-url", new MemoryStream(Encoding.Unicode.GetBytes("https://example.org\n示例网站")));
        var mozLink = await DropDataReader.ReadAsync(moz);
        Assert(mozLink.Entries.Single().Title == "示例网站", "链接标题没有读取");

        var text = new WpfDataObject();
        text.SetData(System.Windows.DataFormats.UnicodeText, "第一行\n第二行");
        Assert((await DropDataReader.ReadAsync(text)).Entries.Single().Kind == ShelfItemKind.Text, "多行文本识别不正确");
        Assert((await DropDataReader.ReadAsync(text, filesOnly: true)).Entries.Count == 0, "快速操作错误地接受了文本");

        var image = new WpfDataObject();
        image.SetData("PNG", new MemoryStream(CreatePng()));
        var picture = await DropDataReader.ReadAsync(image, filesOnly: true);
        Assert(picture.Entries.Single().IsTemporary && File.Exists(picture.Entries[0].Value), "图片没有保存为临时文件");
        Assert(File.ReadAllBytes(picture.Entries[0].Value).Take(4).SequenceEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47 }), "保存的图片不是 PNG");
        foreach (var cache in picture.CacheDirectories) await VirtualFileDropService.ReleaseCacheAsync(cache);
        Assert(!File.Exists(picture.Entries[0].Value), "图片临时目录没有清理");
    }

    private static void TestSourceShelfId()
    {
        var data = new WpfDataObject();
        data.SetData(DropDataReader.ShelfIdFormat, new MemoryStream(Encoding.Unicode.GetBytes("abc123\0")));
        Assert(DropDataReader.GetSourceShelfId(data) == "abc123", "没有读取到来源置物架");
        Assert(DropDataReader.GetSourceShelfId(new WpfDataObject()) is null, "普通拖动被误认为来自置物架");
    }

    private static void TestShellDataObject()
    {
        var first = CreateFolder("shell-data-a");
        var second = CreateFolder("shell-data-b");
        var a = Path.Combine(first, "a.txt");
        var b = Path.Combine(second, "b 空格.txt");
        File.WriteAllText(a, "a");
        File.WriteAllText(b, "b");

        foreach (var data in new[] { ShellInterop.CreateFileDataObject([a, b, second]), CreateFallback([a, b, second]) })
        {
            Assert(data is not null, "没有创建拖出数据");
            var files = new WpfDataObject(data!).GetData(System.Windows.DataFormats.FileDrop) as string[];
            Assert(files is { Length: 3 } && files.Contains(a) && files.Contains(b) && files.Contains(second), "拖出数据中的文件不完整");
            Assert(ShellInterop.TrySetUnicodeString(data!, DropDataReader.ShelfIdFormat, "shelf-1") &&
                   DropDataReader.GetSourceShelfId(new WpfDataObject(data!)) == "shelf-1", "拖出数据无法标记来源置物架");
            System.Runtime.InteropServices.Marshal.ReleaseComObject(data!);
        }

        static System.Runtime.InteropServices.ComTypes.IDataObject? CreateFallback(IReadOnlyList<string> paths)
        {
            var data = ShellInterop.CreateEmptyDataObject();
            return data is not null && ShellInterop.TrySetBytes(data, System.Windows.DataFormats.FileDrop, ShellInterop.BuildDropFiles(paths)) ? data : null;
        }
    }

    private static async Task TestVirtualAttachmentImport()
    {
        var content = Encoding.UTF8.GetBytes("virtual attachment content");
        var descriptor = new byte[4 + 592];
        BitConverter.GetBytes(1u).CopyTo(descriptor, 0);
        Encoding.Unicode.GetBytes("chat-attachment.txt\0").CopyTo(descriptor, 4 + 72);

        var data = new WpfDataObject();
        data.SetData(VirtualFileDropService.FileDescriptorUnicodeFormat, new MemoryStream(descriptor));
        data.SetData(VirtualFileDropService.FileContentsFormat, new MemoryStream(content));
        Assert(DropDataReader.Classify(data) == DropContentKind.VirtualFiles, "标准 Windows 虚拟附件未被识别");
        Assert(VirtualFileDropService.GetDescriptorNames(data).Single() == "chat-attachment.txt", "附件名称读取不正确");

        var result = await DropDataReader.ReadAsync(data);
        Assert(result.Entries.Count == 1 && result.FailedCount == 0, result.ErrorMessage ?? "虚拟附件没有成功接收");
        var path = result.Entries[0].Value;
        Assert(Path.GetFileName(path) == "chat-attachment.txt" && File.ReadAllBytes(path).SequenceEqual(content), "虚拟附件内容不正确");

        var viewModel = new ShelfViewModel();
        viewModel.Add(result);
        Assert(viewModel.Items.Single().IsTemporaryAttachment && viewModel.CacheDirectories.Count == 1, "临时附件没有登记");
        viewModel.ReleaseResources();
        await Task.Delay(300);
        Assert(!Directory.Exists(result.CacheDirectories[0]), "关闭置物架后临时附件没有清理");
    }

    private static async Task TestAttachmentSharedBetweenShelves()
    {
        var cache = VirtualFileDropService.CreateCacheDirectory();
        var file = Path.Combine(cache, "附件.txt");
        File.WriteAllText(file, "shared");
        var first = new ShelfViewModel();
        first.Add(new DropReadResult([new DroppedEntry(ShelfItemKind.File, file, IsTemporary: true)], [cache], 0, null));

        var data = new WpfDataObject();
        data.SetData(System.Windows.DataFormats.FileDrop, new[] { file });
        var read = await DropDataReader.ReadAsync(data);
        // 读取完成、还没放进新置物架时原置物架就关闭了：读取结果已持有缓存。
        first.ReleaseResources();
        await Task.Delay(300);
        Assert(File.Exists(file), "读取后原置物架关闭，临时附件被删除");
        var second = new ShelfViewModel();
        second.Add(read);
        Assert(second.Items.Single().IsTemporaryAttachment && second.Items[0].Value == file, "从其他置物架拖来的临时附件没有标记为临时附件");
        second.ReleaseResources();
        await Task.Delay(300);
        Assert(!Directory.Exists(cache), "所有置物架关闭后临时附件没有清理");

        // 最后一个持有者释放后、后台删除执行前又被持有：不能删除。
        var raced = VirtualFileDropService.CreateCacheDirectory();
        File.WriteAllText(Path.Combine(raced, "keep.txt"), "keep");
        var deletion = VirtualFileDropService.ReleaseCacheAsync(raced);
        VirtualFileDropService.RetainCache(raced);
        await deletion;
        Assert(File.Exists(Path.Combine(raced, "keep.txt")), "重新持有后缓存仍被删除");
        await VirtualFileDropService.ReleaseCacheAsync(raced);
        Assert(!Directory.Exists(raced), "释放后缓存没有清理");

        // 操作进行中关闭置物架：操作自己持有缓存，文件仍能处理完。
        var busyCache = VirtualFileDropService.CreateCacheDirectory();
        var attachment = Path.Combine(busyCache, "处理中.txt");
        File.WriteAllText(attachment, "busy");
        var shelf = new ShelfViewModel();
        shelf.Add(new DropReadResult([new DroppedEntry(ShelfItemKind.File, attachment, IsTemporary: true)], [busyCache], 0, null));
        var target = CreateFolder("close-during-action");
        var moveTo = QuickActions.Get(QuickActionType.MoveTo);
        var moved = await shelf.RunActionAsync(moveTo, shelf.Items.ToArray(), IntPtr.Zero, () =>
        {
            shelf.ReleaseResources();
            Thread.Sleep(300);
            return target;
        });
        Assert(moved.Succeeded && File.ReadAllText(Path.Combine(target, "处理中.txt")) == "busy", "处理途中关闭置物架，正在使用的临时附件被提前删除");
        await Task.Delay(300);
        Assert(!Directory.Exists(busyCache), "操作结束后临时附件没有清理");
    }

    private static async Task TestVolatileFileSnapshot()
    {
        var temporary = Path.Combine(Path.GetTempPath(), "DropShelf.Tests.Chat", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        var source = Path.Combine(temporary, "聊天图片.png");
        File.WriteAllBytes(source, CreatePng());
        File.WriteAllText(source + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        var stable = Path.Combine(CreateStableFolder("volatile-stable"), "keep.txt");
        File.WriteAllText(stable, "keep");
        Assert(ChatAppCompatibility.IsVolatilePath(source) && !ChatAppCompatibility.IsVolatilePath(stable), "临时文件判断不正确");
        Assert(ChatAppCompatibility.IsVolatilePath(@"C:\Users\me\Documents\WeChat Files\wxid\FileStorage\Temp\a.jpg") &&
               !ChatAppCompatibility.IsVolatilePath(@"C:\Users\me\Documents\WeChat Files\wxid\FileStorage\File\a.pdf"), "聊天软件临时目录判断不正确");

        var data = new WpfDataObject();
        data.SetData(System.Windows.DataFormats.FileDrop, new[] { source, stable });
        var read = await DropDataReader.ReadAsync(data);
        Directory.Delete(temporary, true);
        Assert(read.Entries.Count == 2 && read.Entries[0].IsTemporary && read.Entries[0].Value != source, "临时文件没有保存到置物架缓存");
        Assert(File.Exists(read.Entries[0].Value) && Path.GetFileName(read.Entries[0].Value) == "聊天图片.png", "来源程序删除临时文件后内容丢失");
        Assert(File.ReadAllText(read.Entries[0].Value + ":Zone.Identifier").Contains("ZoneId=3"), "临时文件的副本丢失了下载来源标记");
        Assert(!read.Entries[1].IsTemporary && read.Entries[1].Value == stable, "普通文件不应复制");
        foreach (var cache in read.CacheDirectories) await VirtualFileDropService.ReleaseCacheAsync(cache);
    }

    private static void TestRichTextRejectsNetworkPaths()
    {
        var image = Path.Combine(CreateStableFolder("rich-text-local"), "local.png");
        File.WriteAllBytes(image, CreatePng());
        Assert(ChatAppCompatibility.ParseHtmlLocalImages($"<img src=\"{new Uri(image).AbsoluteUri}\">").SequenceEqual([image]), "本机图片路径没有识别");
        Assert(ChatAppCompatibility.ParseHtmlLocalImages("<img src=\"file://server/share/a.png\">").Count == 0, "网页片段中的网络共享路径没有被拒绝");
        Assert(ChatAppCompatibility.ParseQqRichEdit(
            "<QQRichEditFormat><EditElement type=\"1\" filepath=\"\\\\server\\share\\a.png\"></EditElement></QQRichEditFormat>").Count == 0,
            "QQ 富文本中的网络共享路径没有被拒绝");
    }

    private static async Task TestNonWebLinksBecomeText()
    {
        var data = new WpfDataObject();
        data.SetData("UniformResourceLocatorW", new MemoryStream(Encoding.Unicode.GetBytes("file:///C:/Windows/System32/calc.exe\0")));
        var read = await DropDataReader.ReadAsync(data);
        Assert(read.Entries.Single().Kind == ShelfItemKind.Text, "非网页链接没有按文本保存");

        // 旧版本保存的链接项目打开时也要检查协议：只复制文字，不启动程序。
        var viewModel = new ShelfViewModel();
        viewModel.AddEntries([new DroppedEntry(ShelfItemKind.Link, "calc.exe")]);
        viewModel.Open(viewModel.Items[0]);
        Assert(System.Windows.Clipboard.GetText() == "calc.exe", "非网页链接被交给系统打开");
    }

    private static async Task TestChatAppImageFormats()
    {
        var mime = new WpfDataObject();
        mime.SetData("image/png", new MemoryStream(CreatePng()));
        mime.SetData("UniformResourceLocatorW", new MemoryStream(Encoding.Unicode.GetBytes("https://example.com/a.png\0")));
        Assert(DropDataReader.Classify(mime) == DropContentKind.Image, "带有图片数据的拖动被当成了链接");
        var picture = await DropDataReader.ReadAsync(mime);
        Assert(picture.Entries.Single().Value.EndsWith(".png") && File.Exists(picture.Entries[0].Value), "image/png 图片没有保存");
        foreach (var cache in picture.CacheDirectories) await VirtualFileDropService.ReleaseCacheAsync(cache);

        var folder = CreateStableFolder("chat-images");
        var image = Path.Combine(folder, "qq-image.png");
        File.WriteAllBytes(image, CreatePng());
        var qq = new WpfDataObject();
        qq.SetData("QQ_Unicode_RichEdit_Format", new MemoryStream(Encoding.UTF8.GetBytes(
            $"<QQRichEditFormat><Info version=\"1001\"></Info><EditElement type=\"1\" filepath=\"{image}\" shortcut=\"\"></EditElement></QQRichEditFormat>")));
        qq.SetData(System.Windows.DataFormats.UnicodeText, "[图片]");
        Assert(DropDataReader.Classify(qq) == DropContentKind.Files, "旧版 QQ 拖出的图片没有识别");
        var fileName = new WpfDataObject();
        fileName.SetData("FileNameW", new[] { image });
        Assert(DropDataReader.Classify(fileName) == DropContentKind.Files &&
               (await DropDataReader.ReadAsync(fileName)).Entries.Single().Value == image, "只提供 FileNameW 的来源没有识别");
        Assert((await DropDataReader.ReadAsync(qq)).Entries.Single().Value == image, "旧版 QQ 拖出的图片路径不正确");

        var html = new WpfDataObject();
        html.SetData(System.Windows.DataFormats.Html, $"<html><body><!--StartFragment--><img src=\"{new Uri(image).AbsoluteUri}\"><!--EndFragment--></body></html>");
        Assert(DropDataReader.Classify(html) == DropContentKind.Files, "只含本地图片的网页片段没有识别为文件");
        var withText = new WpfDataObject();
        withText.SetData(System.Windows.DataFormats.Html, $"<!--StartFragment-->说明<img src=\"{new Uri(image).AbsoluteUri}\"><!--EndFragment-->");
        withText.SetData(System.Windows.DataFormats.UnicodeText, "说明");
        Assert(DropDataReader.Classify(withText) == DropContentKind.Text, "带文字的网页片段不应只取图片");
    }

    // ───────────── 文件与快速操作 ─────────────

    private static async Task TestCopyKeepsFileMetadata()
    {
        var source = Path.Combine(CreateFolder("metadata-source"), "download.txt");
        File.WriteAllText(source, "downloaded");
        File.WriteAllText(source + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        var modified = new DateTime(2020, 1, 2, 3, 4, 5, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(source, modified);

        var results = await new FileOperationService().CopyAsync([source], CreateFolder("metadata-destination"));
        var copy = results.Single().Destination!;
        Assert(results[0].Succeeded && File.ReadAllText(copy + ":Zone.Identifier").Contains("ZoneId=3"), "复制后丢失了下载来源标记");
        Assert(File.GetLastWriteTimeUtc(copy) == modified, "复制后修改时间被改成了当前时间");
    }

    private static async Task TestFolderLinksAreRejected()
    {
        var source = CreateFolder("link-source");
        var linked = CreateFolder("link-target");
        File.WriteAllText(Path.Combine(linked, "inside.txt"), "linked");
        var junction = Path.Combine(source, "junction");
        using (var mklink = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{junction}\" \"{linked}\"")
               { CreateNoWindow = true, UseShellExecute = false })!)
            mklink.WaitForExit();
        Assert(Directory.Exists(junction), "无法创建测试用的联接点");

        var destination = CreateFolder("link-destination");
        var results = await new FileOperationService().CopyAsync([source], destination);
        Assert(results.Count == 1 && !results[0].Succeeded && results[0].ErrorMessage == Strings.FolderContainsLinks, "含联接点的文件夹没有明确报告失败");
        Assert(!Directory.EnumerateFileSystemEntries(destination).Any(), "失败时留下了不完整的副本");
        Assert(Directory.Exists(junction) && File.Exists(Path.Combine(linked, "inside.txt")), "源文件夹被改动");
    }

    private static async Task TestCopyDoesNotOverwrite()
    {
        var sourceFolder = CreateFolder("copy-source");
        var destinationFolder = CreateFolder("copy-destination");
        var source = Path.Combine(sourceFolder, "same-name.txt");
        var existing = Path.Combine(destinationFolder, "same-name.txt");
        File.WriteAllText(source, "new content");
        File.WriteAllText(existing, "existing content");

        var results = await new FileOperationService().CopyAsync([source], destinationFolder);
        Assert(results.Count == 1 && results[0].Succeeded, "复制没有成功");
        Assert(File.ReadAllText(existing) == "existing content", "已有同名文件被覆盖");
        Assert(File.ReadAllText(Path.Combine(destinationFolder, "same-name (2).txt")) == "new content", "未生成正确的重名副本");
    }

    private static async Task TestMissingSourceIsReported()
    {
        var destination = CreateFolder("missing-destination");
        var results = await new FileOperationService().CopyAsync([Path.Combine(_root, "not-created.txt")], destination);
        Assert(results.Count == 1 && !results[0].Succeeded && !string.IsNullOrWhiteSpace(results[0].ErrorMessage), "不存在的源文件没有被报告为失败");
    }

    private static async Task TestLockedFileIsReported()
    {
        var sourceFolder = CreateFolder("locked-source");
        var destination = CreateFolder("locked-destination");
        var source = Path.Combine(sourceFolder, "locked.txt");
        File.WriteAllText(source, "locked content");
        await using var lockStream = new FileStream(source, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        var results = await new FileOperationService().CopyAsync([source], destination);
        Assert(results.Count == 1 && !results[0].Succeeded && File.Exists(source), "被占用文件没有被报告为失败");
    }

    private static async Task TestNestedDestinationIsRejected()
    {
        var source = CreateFolder("nested-source");
        File.WriteAllText(Path.Combine(source, "file.txt"), "content");
        var results = await new FileOperationService().CopyAsync([source], Path.Combine(source, "child"));
        Assert(results.Count == 1 && !results[0].Succeeded, "复制到文件夹自身内部未被阻止");

        var same = await new FileOperationService().CopyAsync([source], source);
        Assert(same.Count == 1 && !same[0].Succeeded && !Directory.Exists(Path.Combine(source, "nested-source")),
            "复制到文件夹自身未被阻止");

        // 目标路径本身或它的上级是指向源文件夹的联接点。
        var junction = Path.Combine(CreateFolder("nested-links"), "to-source");
        CreateJunction(junction, source);
        var viaLink = await new FileOperationService().CopyAsync([source], Path.Combine(junction, "inner"));
        Assert(viaLink.Count == 1 && !viaLink[0].Succeeded && !Directory.Exists(Path.Combine(source, "inner", "nested-source")),
            "通过联接点复制到文件夹自身内部未被阻止");
        var parentJunction = Path.Combine(CreateFolder("nested-parent-links"), "root");
        CreateJunction(parentJunction, Path.GetDirectoryName(source)!);
        var viaParent = await new FileOperationService().CopyAsync([source], Path.Combine(parentJunction, "nested-source", "deep"));
        Assert(viaParent.Count == 1 && !viaParent[0].Succeeded, "通过上级目录的联接点复制到文件夹自身内部未被阻止");
    }

    private static void CreateJunction(string link, string target)
    {
        using (var mklink = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"")
               { CreateNoWindow = true, UseShellExecute = false })!)
            mklink.WaitForExit();
        Assert(Directory.Exists(link), "无法创建测试用的联接点");
    }

    private static void TestArchiveActions()
    {
        var folder = CreateFolder("quick-action-zip");
        var source = Path.Combine(folder, "quick-action.txt");
        File.WriteAllText(source, "quick action content");

        var compressed = QuickActionService.Archive([source]);
        Assert(compressed.Succeeded && File.Exists(compressed.OutputPath), compressed.Message);
        using (var archive = ZipFile.OpenRead(compressed.OutputPath!))
        {
            var entry = archive.Entries.SingleOrDefault(item => item.FullName == "quick-action.txt");
            Assert(entry is not null, "ZIP 中没有原文件");
            using var reader = new StreamReader(entry!.Open());
            Assert(reader.ReadToEnd() == "quick action content", "ZIP 中的内容不正确");
        }

        var extracted = QuickActionService.Archive([compressed.OutputPath!]);
        Assert(extracted.Succeeded && Directory.Exists(extracted.OutputPath), extracted.Message);
        Assert(File.ReadAllText(Path.Combine(extracted.OutputPath!, "quick-action.txt")) == "quick action content", "ZIP 解压后的文件内容不正确");

        // 来自网络的压缩包：解出的文件带上同样的来源区域。
        File.WriteAllText(compressed.OutputPath! + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        var downloaded = QuickActionService.Archive([compressed.OutputPath!]);
        Assert(downloaded.Succeeded && File.ReadAllText(Path.Combine(downloaded.OutputPath!, "quick-action.txt") + ":Zone.Identifier").Contains("ZoneId=3"),
            "解压来自网络的 ZIP 后没有传递下载来源标记");

        var unsafeZip = Path.Combine(folder, "unsafe.zip");
        using (var archive = ZipFile.Open(unsafeZip, ZipArchiveMode.Create)) archive.CreateEntry("../escaped.txt");
        Assert(!QuickActionService.Archive([unsafeZip]).Succeeded && !File.Exists(Path.Combine(folder, "escaped.txt")), "含有不安全路径的 ZIP 被错误解压");

        var rootEntryZip = Path.Combine(folder, "root-entry.zip");
        using (var archive = ZipFile.Open(rootEntryZip, ZipArchiveMode.Create))
        {
            archive.CreateEntry("./");
            using var writer = new StreamWriter(archive.CreateEntry("inside.txt").Open());
            writer.Write("inside root entry archive");
        }
        var rootEntryResult = QuickActionService.Archive([rootEntryZip]);
        Assert(rootEntryResult.Succeeded && File.ReadAllText(Path.Combine(rootEntryResult.OutputPath!, "inside.txt")) == "inside root entry archive",
            "包含根目录项的 ZIP 解压不正确");

        if (QuickActionService.IsNative7ZipAvailable())
        {
            var sevenZipSource = Path.Combine(folder, "native-seven.txt");
            var sevenZipPath = Path.Combine(folder, "native-seven.7z");
            File.WriteAllText(sevenZipSource, "native 7z content");
            var tar = new System.Diagnostics.ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"))
            {
                UseShellExecute = false,
                CreateNoWindow = true
            };
            foreach (var argument in new[] { "-caf", sevenZipPath, "-C", folder, Path.GetFileName(sevenZipSource) }) tar.ArgumentList.Add(argument);
            using (var process = System.Diagnostics.Process.Start(tar)!) process.WaitForExit();
            var extracted7Zip = QuickActionService.Archive([sevenZipPath]);
            Assert(extracted7Zip.Succeeded && File.ReadAllText(Path.Combine(extracted7Zip.OutputPath!, "native-seven.txt")) == "native 7z content",
                "Windows 内置 7z 解压结果不正确");
        }
    }

    private static async Task TestCopyPathAction()
    {
        var folder = CreateFolder("copy-path");
        var a = Path.Combine(folder, "a.txt");
        var b = Path.Combine(folder, "b.txt");
        File.WriteAllText(a, "a");
        File.WriteAllText(b, "b");
        var result = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActions.Get(QuickActionType.CopyFullPath), [a, b], false, IntPtr.Zero));
        Assert(result.Succeeded, result.Message);
        Assert(System.Windows.Clipboard.GetText() == a + Environment.NewLine + b, "剪贴板中的路径不正确");
    }

    private static async Task TestMoveToAction()
    {
        var folder = CreateFolder("move-to");
        var destination = CreateFolder("move-to-destination");
        var source = Path.Combine(folder, "move-me.txt");
        File.WriteAllText(source, "move");

        var cancelled = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActions.Get(QuickActionType.MoveTo), [source], false, IntPtr.Zero, () => null));
        Assert(cancelled.Cancelled && File.Exists(source), "取消选择文件夹后仍然移动了文件");

        var moved = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActions.Get(QuickActionType.MoveTo), [source], false, IntPtr.Zero, () => destination));
        Assert(moved.Succeeded && moved.Moves!.Single().Destination == Path.Combine(destination, "move-me.txt"), moved.Message);
        Assert(!File.Exists(source) && File.ReadAllText(Path.Combine(destination, "move-me.txt")) == "move", "文件没有移动到目标位置");
    }

    private static async Task TestArchiveAddsOutputToShelf()
    {
        var folder = CreateFolder("archive-shelf");
        var source = Path.Combine(folder, "report.txt");
        File.WriteAllText(source, "report");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([source]);
        var toasts = new List<ShelfToast>();
        viewModel.ToastRequested += (_, toast) => toasts.Add(toast);
        IReadOnlyList<string>? produced = null;
        viewModel.NewItemsProduced += (_, outputs) => produced = outputs;
        var result = await viewModel.RunActionAsync(QuickActions.Get(QuickActionType.CompressZip), viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(result.Succeeded && viewModel.Items.Count == 1 && produced?.Single().EndsWith("report.zip") == true,
            "压缩结果没有放进新的置物架");
        Assert(toasts.Single().Success && !viewModel.IsBusy, "操作结果没有提示");

        var empty = await new ShelfViewModel().RunActionAsync(QuickActions.Get(QuickActionType.CompressZip), [], IntPtr.Zero, null);
        Assert(!empty.Succeeded, "空置物架执行操作没有提示");
    }

    private static void TestCopyFilesToClipboard()
    {
        var folder = CreateFolder("clipboard-files");
        var file = Path.Combine(folder, "copy.txt");
        File.WriteAllText(file, "copy");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([file]);
        Assert(viewModel.CopyToClipboard(viewModel.Items.ToArray()), "复制文件失败");
        var list = System.Windows.Clipboard.GetFileDropList();
        Assert(list.Count == 1 && list[0] == file, "剪贴板中没有文件");
    }

    private static async Task TestCustomFolderAction()
    {
        var folder = CreateFolder("custom-folder-source");
        var target = CreateFolder("custom-folder-target");
        var copySource = Path.Combine(folder, "copy.txt");
        var moveSource = Path.Combine(folder, "move.txt");
        File.WriteAllText(copySource, "copy");
        File.WriteAllText(moveSource, "move");

        var copy = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = target, FolderMode = TransferMode.Copy });
        var copied = await QuickActionService.ExecuteAsync(new QuickActionRequest(copy, [copySource], false, IntPtr.Zero));
        Assert(copied.Succeeded && File.Exists(copySource) && File.ReadAllText(Path.Combine(target, "copy.txt")) == "copy", copied.Message);

        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([moveSource]);
        var move = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = target });
        var moved = await viewModel.RunActionAsync(move, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(moved.Succeeded && !File.Exists(moveSource) && viewModel.Items.Single().Value == Path.Combine(target, "move.txt"), moved.Message);

        var again = await viewModel.RunActionAsync(move, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(again.Succeeded && !File.Exists(Path.Combine(target, "move (2).txt")), "已在目标文件夹中的项目被重命名");

        var missing = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = Path.Combine(target, "gone") });
        Assert(!(await QuickActionService.ExecuteAsync(new QuickActionRequest(missing, [copySource], false, IntPtr.Zero))).Succeeded,
            "目标文件夹不存在时没有提示");
    }

    private static void TestImageCompression()
    {
        var folder = CreateFolder("compress-image");
        var large = Path.Combine(folder, "photo.png");
        File.WriteAllBytes(large, CreateNoisyPng(900, 700));
        const long limit = 80 * 1024;
        Assert(new FileInfo(large).Length > limit, "测试图片不够大");

        var result = ImageCompressor.Compress(large, limit);
        Assert(result.Status == ImageCompressionStatus.Compressed && result.Output is not null, "图片没有压缩");
        Assert(Path.GetFileName(result.Output!) == "photo-80KB.jpg" && new FileInfo(result.Output!).Length <= limit, "压缩后的图片超过了限制");
        var decoder = System.Windows.Media.Imaging.BitmapDecoder.Create(new Uri(result.Output!), System.Windows.Media.Imaging.BitmapCreateOptions.None,
            System.Windows.Media.Imaging.BitmapCacheOption.OnLoad);
        Assert(decoder is System.Windows.Media.Imaging.JpegBitmapDecoder && decoder.Frames[0].PixelWidth <= 900, "输出不是 JPG");
        Assert(File.Exists(large), "原图被改动");

        Assert(ImageCompressor.Compress(result.Output!, limit).Status == ImageCompressionStatus.AlreadySmall, "已经足够小的图片不应再压缩");
        var text = Path.Combine(folder, "note.txt");
        File.WriteAllText(text, "x");
        Assert(ImageCompressor.Compress(text, limit).Status == ImageCompressionStatus.NotImage, "非图片文件没有跳过");
        var broken = Path.Combine(folder, "broken.jpg");
        File.WriteAllText(broken, new string('x', 200_000));
        Assert(ImageCompressor.Compress(broken, limit).Status == ImageCompressionStatus.Failed, "损坏的图片没有报告失败");
        Assert(ImageCompressor.FormatLimit(2 * 1024 * 1024) == "2MB" && Strings.ImageSizeText(2048) == "2 MB", "大小文字不正确");

        var banner = Path.Combine(folder, "banner.png");
        File.WriteAllBytes(banner, CreateNoisyPng(3000, 40));
        var bannerResult = ImageCompressor.Compress(banner, 10 * 1024);
        Assert(bannerResult.Status == ImageCompressionStatus.Failed ||
               (bannerResult.Status == ImageCompressionStatus.Compressed && new FileInfo(bannerResult.Output!).Length <= 10 * 1024),
            "细长图片压缩后超过了限制却报告成功");

        // 分成多个条带复制时，每一行都要与原图一致。
        var source = System.Windows.Media.Imaging.BitmapFrame.Create(new MemoryStream(CreateNoisyPng(1200, 1500)));
        var bgr = new System.Windows.Media.Imaging.FormatConvertedBitmap(source, System.Windows.Media.PixelFormats.Bgr24, null, 0);
        var copy = ImageCompressor.Materialize(bgr);
        var stride = (1200 * 3 + 3) & ~3;
        var expected = new byte[stride * 1500];
        var actual = new byte[stride * 1500];
        bgr.CopyPixels(expected, stride, 0);
        copy.CopyPixels(actual, stride, 0);
        Assert(copy.PixelWidth == 1200 && copy.PixelHeight == 1500 && expected.AsSpan().SequenceEqual(actual), "分段复制的像素与原图不一致");
    }

    private static async Task TestCompressImageReplacesItems()
    {
        var folder = CreateFolder("compress-replace");
        var large = Path.Combine(folder, "scan.png");
        File.WriteAllBytes(large, CreateNoisyPng(800, 600));
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([large]);
        var id = viewModel.Items[0].Id;
        var action = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.CompressImage, ImageSizeKb = 60 });
        var result = await viewModel.RunActionAsync(action, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(result.Succeeded && viewModel.Items.Count == 1 && viewModel.Items[0].Value.EndsWith("scan-60KB.jpg") && viewModel.Items[0].Id == id,
            $"压缩后的图片没有替换原图：{result.Message}");
        Assert(File.Exists(large), "原图被删除");
    }

    private static void TestOpenWithArguments()
    {
        Assert(QuickActionService.BuildArguments(null, [@"C:\a b\x.png", @"D:\"]) == "\"C:\\a b\\x.png\" \"D:\\\\\"", "路径引号不正确");
        Assert(QuickActionService.BuildArguments("-q 80 {files} --out", [@"C:\x.png"]) == "-q 80 \"C:\\x.png\" --out", "{files} 没有替换");
        Assert(QuickActionService.BuildArguments("--edit", [@"C:\x.png"]) == "--edit \"C:\\x.png\"", "没有 {files} 时文件应放在最后");
        var program = new CustomQuickAction { Kind = CustomActionKind.OpenWith, ProgramPath = @"C:\Windows\System32\notepad.exe" };
        Assert(QuickActionInfo.ForCustom(program).Label.Length > 0, "程序名称为空");
    }

    // ───────────── 系统集成 ─────────────

    private static async Task TestShellThumbnail()
    {
        var folder = CreateFolder("shell-thumbnail");
        var path = Path.Combine(folder, "preview.txt");
        File.WriteAllText(path, "thumbnail preview");
        var thumbnail = await ShellThumbnailService.LoadAsync(path, 96);
        Assert(thumbnail is System.Windows.Media.Imaging.BitmapSource { PixelWidth: > 0 } && thumbnail.IsFrozen, "没有获取到可跨线程使用的缩略图");
        Assert(thumbnail is System.Windows.Media.Imaging.BitmapSource { PixelWidth: <= 96, PixelHeight: <= 96 }, "缩略图大于需要的尺寸");
        Assert(!string.IsNullOrWhiteSpace(ShellFileInfo.GetTypeName(path)), "没有获取到文件类型名称");
        Assert(ShellFileInfo.GetTypeName("noextension") == "文件", "无扩展名文件类型不正确");
    }

    private static async Task TestImagingThread()
    {
        var first = await ImagingThread.RunAsync(() => (Environment.CurrentManagedThreadId, Thread.CurrentThread.GetApartmentState()));
        var second = await ImagingThread.RunAsync(() => Environment.CurrentManagedThreadId);
        Assert(first.Item1 == second && first.Item2 == ApartmentState.STA, "图片处理没有在同一个单线程套间线程上运行");
        Assert(Environment.CurrentManagedThreadId != second, "图片处理占用了界面线程");
        var longWork = await ImagingThread.RunLongAsync(() => (Environment.CurrentManagedThreadId, Thread.CurrentThread.GetApartmentState()));
        Assert(longWork.Item1 != second && longWork.Item2 == ApartmentState.STA, "整批压缩图片和缩略图共用了同一个线程");
        var cancelled = false;
        try { await ImagingThread.RunAsync(() => 1, new CancellationToken(canceled: true)); }
        catch (OperationCanceledException) { cancelled = true; }
        Assert(cancelled, "已取消的图片处理仍然执行");
        var failed = false;
        try { await ImagingThread.RunAsync(() => throw new InvalidOperationException()); }
        catch (InvalidOperationException) { failed = true; }
        Assert(failed, "图片处理中的异常没有传回调用方");
    }

    private static void TestStartupCommand()
    {
        var executablePath = @"C:\Program Files\Drop Shelf\DropShelf.exe";
        Assert(StartupService.BuildCommand(executablePath) == $"\"{executablePath}\" --background", "开机启动命令没有正确处理空格路径");
    }

    private static void TestThemeAndAccent()
    {
        Assert(ThemeService.ResolveIsDark(AppTheme.System, true) && !ThemeService.ResolveIsDark(AppTheme.System, false), "跟随系统不正确");
        Assert(ThemeService.ResolveIsDark(AppTheme.Dark, false) && !ThemeService.ResolveIsDark(AppTheme.Light, true), "手动主题被系统覆盖");

        var bytes = Convert.FromHexString("7B7171FF736969FF6D6464FF685F5FFF625959FF5C5454FF544C4CFF88179800");
        var palette = AccentPalette.Parse(bytes)!.Value;
        Assert(palette.Base == System.Windows.Media.Color.FromRgb(0x68, 0x5F, 0x5F) && palette.Light2 == System.Windows.Media.Color.FromRgb(0x73, 0x69, 0x69),
            "强调色调色板解析错误");
        Assert(AccentPalette.Parse(new byte[4]) is null, "不完整的调色板应被忽略");

        var dark = ThemeService.BuildPalette(true, palette);
        Assert(dark["TextOnAccentBrush"] == System.Windows.Media.Colors.White, "深色强调色上应使用白字");
        var darkBackground = System.Windows.Media.Color.FromRgb(0x1C, 0x1B, 0x19);
        Assert(ThemeService.ContrastRatio(dark["AccentTextBrush"], darkBackground) >= 4.5, "强调色文字对比度不足");
        var light = ThemeService.BuildPalette(false, AccentPalette.Default);
        Assert(light["AccentBrush"] == AccentPalette.Default.Base && light["TextOnAccentBrush"] == System.Windows.Media.Colors.White, "默认浅色强调色不正确");
        var cobalt = ThemeService.BuildPalette(false, AccentPalette.FromColor(ThemeService.Cobalt));
        Assert(cobalt["CardBackFillBrush"] == ThemeService.Cobalt && cobalt["TableTextBrush"] == System.Windows.Media.Colors.White, "钴蓝牌背或牌桌文字不正确");
        var darkCobalt = ThemeService.BuildPalette(true, AccentPalette.FromColor(ThemeService.Cobalt));
        Assert(ThemeService.ContrastRatio(darkCobalt["AccentTextBrush"], darkBackground) >= 4.5, "深色主题下钴蓝文字对比度不足");
        Assert(ThemeService.PrefersDarkText(System.Windows.Media.Color.FromRgb(0x99, 0xEB, 0xFF)), "浅色强调色上应使用黑字");
    }

    private static void TestFileSuits()
    {
        Assert(FileSuits.Of(@"C:\a\photo.JPG") == FileSuit.Image && FileSuits.Of(@"C:\a\report.pdf") == FileSuit.Document, "图片或文档的花色不正确");
        Assert(FileSuits.Of(@"C:\a\Downloads") == FileSuit.Folder && FileSuits.Of(@"C:\a\Downloads\") == FileSuit.Folder, "没有扩展名的应按文件夹算");
        Assert(FileSuits.Of(@"C:\a\setup.exe") == FileSuit.Other, "其他文件的花色不正确");
        Assert(FileSuits.Dominant([FileSuit.Image, FileSuit.Document, FileSuit.Image]) == FileSuit.Image, "应取数量最多的花色");
        Assert(FileSuits.Dominant([FileSuit.Image, FileSuit.Document]) == FileSuit.Document, "数量相同时应取最后放入的");
        Assert(FileSuits.Dominant([]) == FileSuit.Other, "没有项目时应为其他");
        var link = new ShelfItem { Id = "1", Kind = ShelfItemKind.Link, Value = "https://example.com" };
        Assert(FileSuits.Of(link) == FileSuit.Other, "链接的花色不正确");
        var photo = new ShelfItem { Id = "2", Kind = ShelfItemKind.File, Value = @"C:\a\photo.jpeg" };
        var folder = new ShelfItem { Id = "3", Kind = ShelfItemKind.Folder, Value = @"C:\a\Downloads" };
        var plain = new ShelfItem { Id = "4", Kind = ShelfItemKind.File, Value = @"C:\a\README" };
        Assert(photo.CardMark == "JPEG" && photo.Suit == FileSuit.Image && link.CardMark == "URL", "牌角应印扩展名或链接");
        Assert(folder.CardMark.Length == 0 && plain.CardMark.Length == 0 && folder.Suit == FileSuit.Folder, "文件夹和没有扩展名的文件牌角不印文字");
    }

    private static void TestShelfMotion()
    {
        var target = new RECT { Left = 1500, Top = 300, Right = 1720, Bottom = 573 };
        var work = new RECT { Left = 0, Top = 0, Right = 1920, Bottom = 1040 };
        double? lastShake = null, lastEdge = null;
        for (var i = 0; i < 20; i++)
        {
            var shake = ShelfMotion.ShakeIn(new Vector(30, 40));
            var right = ShelfMotion.EdgeIn(ScreenEdge.Right, target, work, 1);
            Assert(shake.Keys[0].Angle != lastShake && right.Keys[0].Angle != lastEdge, "相邻两次呼出不应用同一个版本");
            (lastShake, lastEdge) = (shake.Keys[0].Angle, right.Keys[0].Angle);
            Assert(shake.At(0).Turn == 180 && shake.At(0).Opacity == 0, "晃动呼出应从还看不见的牌背开始");
            Assert(target.Left + target.Width / 2.0 + right.At(0).X - target.Width / 2.0 > work.Right, "从右边缘呼出时牌应从屏幕外开始");
            foreach (var motion in new[] { shake, right })
                Assert(motion.At(motion.Duration + 1) is { X: 0, Y: 0, Scale: 1, Angle: 0, Turn: 0, Opacity: 1 }, "出场的牌最后应正好落在置物架的位置");
        }
        for (var i = 0; i < 8; i++)
            foreach (var motion in new[] { ShelfMotion.EdgeIn(ScreenEdge.Left, target, work, 1), ShelfMotion.EdgeIn(ScreenEdge.Top, target, work, 1) })
                Assert(motion.At(motion.Duration + 1) is { X: 0, Y: 0, Scale: 1, Angle: 0, Opacity: 1 }, "从左边和顶边呼出的牌最后应正好落在置物架的位置");

        var toss = ShelfMotion.Exit(ShelfEntrance.FromEdge(ScreenEdge.Left), false, target, work, 1);
        var flip = ShelfMotion.Exit(ShelfEntrance.Shake, true, target, work, 1);
        Assert(toss.At(0) is { X: 0, Scale: 1 } && toss.At(toss.Duration).X < -target.Left, "边缘呼出的置物架关闭时应甩回那条边");
        Assert(flip.HorizontalAxis && flip.At(flip.Duration) is { Turn: 180, Opacity: 0 }, "其余的置物架关闭时应按出场的方向翻回背面淡出");

        var line = new CardMotion([new CardKey(0), new CardKey(100, X: 10)], FadeInMs: 50);
        Assert(Math.Abs(line.At(50).X - 5) < 1e-9 && Math.Abs(line.At(25).Opacity - 0.5) < 1e-9, "关键帧之间应按时间插值，开头按时间淡入");

        Assert(CardGridPanel.ColumnsFor(280) == 4 && CardGridPanel.RowsFor(5, 280) == 2 && CardGridPanel.RowsFor(4, 280) == 1, "平铺应一行四张");
        Assert(Math.Abs(CardGridPanel.HeightFor(2) - 266) < 1e-9 && CardGridPanel.HeightFor(0) == 0, "平铺的高度不正确");
    }

    private static void TestTrayIconLoader()
    {
        var resource = WpfApplication.GetResourceStream(new Uri("pack://application:,,,/DropShelf;component/Assets/DropShelf.ico"))!;
        using var memory = new MemoryStream();
        resource.Stream.CopyTo(memory);
        var icon = IconLoader.CreateFromIco(memory.ToArray(), 16);
        Assert(icon != IntPtr.Zero, "没有从图标资源创建托盘图标");
        NativeMethods.DestroyIcon(icon);
        Assert(IconLoader.CreateFromIco([0, 1], 16) == IntPtr.Zero, "无效图标数据没有安全处理");
    }

    private static void TestHotkeyRegistration()
    {
        using var window = new MessageWindow("DropShelf.Tests.Hotkey");
        using var hotkey = new HotkeyService(window);
        Assert(hotkey.Register(new HotkeyGesture(ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Shift, Key.F23)), "快捷键注册失败");
        hotkey.Unregister();
        Assert(hotkey.Register(HotkeyGesture.None), "关闭快捷键应视为成功");
    }

    private static void TestArgumentForwarding()
    {
        var folder = CreateFolder("forward");
        var file = Path.Combine(folder, "send to.txt");
        File.WriteAllText(file, "x");
        Assert(AppController.IsShelfRequest([file]) && AppController.IsShelfRequest(["--shelf"]), "没有识别置物架请求");
        Assert(!AppController.IsShelfRequest(["--background"]) && !AppController.IsShelfRequest([Path.Combine(folder, "missing")]), "错误识别了置物架请求");
        Assert(AppController.GetPathArguments(["--shelf", file]).Single() == file, "没有取出文件参数");

        using var window = new MessageWindow("DropShelf.Tests.Forward");
        string[]? received = null;
        window.ArgumentsReceived += arguments => received = arguments;
        Assert(MessageWindow.SendArguments("DropShelf.Tests.Forward", ["--shelf", file]), "没有找到正在运行的实例");
        Pump(200);
        Assert(received is ["--shelf", var path] && path == file, "转交的参数不正确");
        Assert(!MessageWindow.SendArguments("DropShelf.Tests.NoSuchWindow", ["x"]), "不存在的实例被误认为已转交");
    }

    private static void TestFailureLogExists()
    {
        var log = Path.Combine(_root, "logs", $"dropshelf-{DateTime.Now:yyyy-MM-dd}.log");
        Assert(File.Exists(log) && new FileInfo(log).Length > 0, "错误日志没有生成");
    }

    // ───────────── 界面 ─────────────

    private static void TestShelfWindowActions()
    {
        var controller = new AppController(Dispatcher.CurrentDispatcher);
        var window = new ShelfWindow(new ShelfViewModel(), controller.Shelves) { Left = -20000, Top = -20000 };
        window.Show();
        Pump(150);
        var bar = (System.Windows.Controls.Primitives.UniformGrid)window.FindName("ActionBar")!;
        Assert(bar.Children.Count == (int)controller.Settings.QuickActionLayout, "快速操作按钮数量不正确");
        Assert(window.IsReusable, "空置物架应可复用");
        Assert(window.Title == "DropShelf 置物架 - 置物架", "置物架窗口标题不正确");
        var modeText = (System.Windows.Controls.TextBlock)window.FindName("TransferModeText")!;
        Assert(modeText.Text == "移动", "默认拖出方式应显示为移动");

        LanguageService.SetMode(AppLanguage.English);
        try
        {
            Assert(((System.Windows.Controls.Button)bar.Children[0]).ToolTip is string tip && tip.StartsWith("Compress: "),
                "切换语言后快速操作提示没有更新");
            Assert(window.Title == "DropShelf shelf - Shelf" && modeText.Text == "Move", "切换语言后置物架文字没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }

        controller.Settings.DragOutMode = TransferMode.Copy;
        controller.Settings.QuickActionLayout = QuickActionLayout.Two;
        window.ApplySettings(controller.Settings);
        var tiles = (System.Windows.Controls.Primitives.UniformGrid)window.FindName("ActionTiles")!;
        Assert(modeText.Text == "复制", "拖出方式没有更新");
        Assert(bar.Children.Count == 2 && ((System.Windows.Controls.Button)bar.Children[0]).Content is DockPanel && tiles.Rows == 1,
            "两个按钮时应显示名称并使用更大的拖放区域");
        var twoHeight = window.Height;
        controller.Settings.QuickActionLayout = QuickActionLayout.Four;
        window.ApplySettings(controller.Settings);
        Assert(Math.Abs(window.Height - twoHeight) < 0.5, "两个和四个按钮的置物架高度应相同");
        Assert(tiles.Rows == 1 && tiles.Children.Count == 4, "四个操作时拖放区域应与按钮排成一行");

        controller.Settings.QuickActionLayout = QuickActionLayout.Off;
        window.ApplySettings(controller.Settings);
        Assert(bar.Children.Count == 0 && ((FrameworkElement)window.FindName("ActionBarHost")!).Visibility == Visibility.Collapsed,
            "关闭快速操作后仍显示按钮");

        var folder = CreateFolder("shelf-window");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");
        var toggle = (System.Windows.Controls.Button)window.FindName("ViewToggleButton")!;
        Assert(toggle.Visibility == Visibility.Collapsed, "空置物架不应显示文件堆/列表切换按钮");
        window.ViewModel.AddFiles([file]);
        Pump(100);
        Assert(toggle.Visibility == Visibility.Visible, "只有一项时也应显示文件堆/列表切换按钮");
        window.ViewModel.AddFiles([folder]);
        Pump(100);
        Assert(!window.IsReusable, "有内容的置物架不应被复用");
        Assert(window.ViewModel.IsPinned && window.Topmost, "置物架默认应置顶");
        window.ViewModel.IsPinned = false;
        Assert(!window.Topmost, "取消钉子后置物架不应再置顶");
        window.ViewModel.IsPinned = true;
        toggle.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        // 切换时窗口先平滑变大、牌再一张张发到格子，等动画播完再检查。
        Pump(900);
        Assert(window.ViewModel.IsExpanded, "点击切换按钮没有平铺开");
        Assert(window.Width >= QuickShelfSizeOptions.ResolveExpandedWidth(controller.Settings.QuickShelfSize), "展开后宽度没有变化");

        // 平铺时继续放入项目：一行放四张，放到第五张时窗口变高；变高要等列表收到新增项目之后，否则布局会抛出异常。
        var oneRowHeight = window.Height;
        var extras = new[] { "b.txt", "c.txt", "d.txt" }.Select(name => Path.Combine(folder, name)).ToArray();
        foreach (var extra in extras) File.WriteAllText(extra, "y");
        window.ViewModel.AddFiles(extras);
        Pump(100);
        Assert(((System.Windows.Controls.ListBox)window.FindName("ItemsList")!).Items.Count == 5 && window.Height > oneRowHeight, "平铺时放入项目后牌或高度没有更新");
        window.ViewModel.Remove(window.ViewModel.Items.Skip(2).ToArray());
        Pump(100);
        toggle.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(900);
        Assert(!window.ViewModel.IsExpanded &&
               Math.Abs(window.Width - QuickShelfSizeOptions.ResolveWidth(controller.Settings.QuickShelfSize)) < 0.5 &&
               ((FrameworkElement)window.FindName("StackView")!).Visibility == Visibility.Visible &&
               ((FrameworkElement)window.FindName("ItemsList")!).Visibility == Visibility.Collapsed &&
               ((FrameworkElement)window.FindName("Card0")!).Opacity == 1,
            "叠起来的动画结束后应回到堆叠，窗口恢复收起时的宽度");

        // 压缩生成的压缩包放进旁边新开的置物架，不混进原来的项目。
        controller.Settings.QuickActionLayout = QuickActionLayout.Four;
        window.ApplySettings(controller.Settings);
        var openBefore = controller.Shelves.OpenShelves.Count;
        var compressed = window.ViewModel.RunActionAsync(QuickActions.Get(QuickActionType.CompressZip), [window.ViewModel.Items[0]], IntPtr.Zero, null);
        while (!compressed.IsCompleted) Pump(20);
        Assert(compressed.Result.Succeeded && window.ViewModel.Items.Count == 2, "压缩包不应放进原来的置物架");
        var archiveShelf = controller.Shelves.OpenShelves.Skip(openBefore).Single();
        Assert(archiveShelf.ViewModel.Items.Single().Value.EndsWith(".zip", StringComparison.OrdinalIgnoreCase), "压缩包没有放进新的置物架");
        archiveShelf.Close();
        window.Close();
        Assert(window.ViewModel.Items.Count == 0, "关闭后没有释放项目");

        var latest = controller.History.Latest;
        Assert(latest is not null && latest.Items.Select(item => item.Path).SequenceEqual([file, folder]), "关闭的置物架没有记入最近关闭");
        controller.Shelves.Restore(latest!);
        Pump(150);
        var reopened = controller.Shelves.OpenShelves.Last();
        Assert(reopened.ViewModel.Items.Count == 2 && controller.History.Entries.All(entry => entry != latest), "最近关闭的置物架没有恢复");
        reopened.Close();
        controller.History.Clear();
        File.Delete(new SettingsService().SettingsPath);
    }

    private static void TestMainWindowSettings()
    {
        var service = new SettingsService();
        service.Save(new AppSettings { HasCompletedFirstRun = true });
        var controller = new AppController(Dispatcher.CurrentDispatcher);
        var window = new MainWindow(controller) { Left = -20000, Top = -20000, WindowStartupLocation = WindowStartupLocation.Manual };
        window.Show();
        Pump(200);

        foreach (var name in new[] { "PageCards", "VersionButton", "StatusIndicator", "GettingStartedSection", "HistorySection", "HistoryList",
                     "LanguageSelector", "StartupEnabledBox", "HotkeyEditor", "TriggerSelector", "SensitivitySlider", "EdgeDwellSelector",
                     "ShelfSizeSelector", "DragOutModeSelector", "CloseWhenEmptyBox", "ClearHistoryButton", "ThemeSelector",
                     "AccentSelector", "ActionLayoutSelector", "ActionSlotsList", "MoreActionsList", "AddCustomActionButton", "EditorOverlay" })
            Assert(window.FindName(name) is not null, $"主窗口缺少 {name}");

        var sidePanel = (FrameworkElement)window.FindName("SidePanel")!;
        var overviewHeader = (FrameworkElement)window.FindName("OverviewHeader")!;
        var panelTitle = (FrameworkElement)window.FindName("PanelPageTitle")!;
        Assert(sidePanel.Visibility == Visibility.Collapsed && overviewHeader.Visibility == Visibility.Visible, "窗口较窄时不应显示左侧状态栏");
        window.Width = 1500;
        Pump(700);
        Assert(sidePanel.Visibility == Visibility.Visible && sidePanel.Opacity == 1 && overviewHeader.Visibility == Visibility.Collapsed &&
               panelTitle.Visibility == Visibility.Visible, "窗口足够宽时应显示左侧状态栏，概览标题移到状态栏");
        window.Width = 900;
        Pump(700);
        Assert(sidePanel.Visibility == Visibility.Collapsed && overviewHeader.Visibility == Visibility.Visible &&
               panelTitle.Visibility == Visibility.Collapsed, "窗口变窄后应收起左侧状态栏，标题回到页面");

        var shortCards = window.SettingsCardHeight;
        window.Height = 1100;
        Pump(700);
        Assert(window.SettingsCardHeight > shortCards + 40, "窗口变高后设置卡片应拉长");
        var firstTile = (DropShelf.Views.Controls.PageTile)((System.Windows.Controls.Panel)window.FindName("PageCards")!).Children[0];
        Assert(firstTile.ActualHeight < 144 || firstTile.IsTall, "卡片够高时应切换成大卡片样式");
        window.Height = 640;
        Pump(700);
        Assert(Math.Abs(window.SettingsCardHeight - shortCards) < 1, "窗口变矮后设置卡片应恢复");
        Assert(!firstTile.IsTall || firstTile.ActualHeight >= 132, "窗口变矮后卡片应回到单行样式");

        controller.History.Clear();
        var pageCards = (System.Windows.Controls.Primitives.UniformGrid)window.FindName("PageCards")!;
        Assert(pageCards.Children.Count == 4, "概览页应有四张设置卡片");
        ((System.Windows.Controls.Button)pageCards.Children[1]).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        var shelfHost = (System.Windows.Controls.Grid)window.FindName("ShelfHost")!;
        var shelfScroll = (System.Windows.Controls.ScrollViewer)window.FindName("ShelfScroll")!;
        var overviewHost = (FrameworkElement)window.FindName("OverviewHost")!;
        Pump(900);
        Assert(shelfHost.Visibility == Visibility.Visible && overviewHost.Visibility == Visibility.Collapsed, "点击设置卡片没有切换页面");
        Assert(shelfHost is { Clip: null, IsHitTestVisible: true } && shelfHost.RenderTransform.Value.IsIdentity && shelfHost.Children.Count == 1 &&
               shelfScroll is { CacheMode: null, Opacity: 1 } && overviewHost.Clip is null, "翻牌结束后页面应回到静止状态：没有变换、裁剪、缓存和背面的边框");
        var stackLayer = (System.Windows.Controls.Canvas)window.FindName("StackLayer")!;
        var shelfSheet = (FrameworkElement)window.FindName("ShelfSheet")!;
        var edges = stackLayer.Children.OfType<System.Windows.Controls.Button>().ToList();
        var sheetBottom = shelfSheet.TranslatePoint(new System.Windows.Point(0, shelfSheet.ActualHeight), stackLayer).Y;
        Assert(stackLayer is { Visibility: Visibility.Visible, Clip: null, IsHitTestVisible: true } && edges.Count == 3 &&
               edges.All(edge => edge.Visibility == Visibility.Visible && System.Windows.Controls.Canvas.GetTop(edge) >= sheetBottom - 0.5 && edge.ActualWidth < shelfSheet.ActualWidth),
            "设置页底下应露出其余三张牌的边");
        Assert(pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1), "翻牌结束后概览页的卡片应恢复显示");
        var pageName = (System.Windows.Controls.TextBlock)window.FindName("PanelPageName")!;
        try
        {
            LanguageService.SetMode(AppLanguage.English);
            Pump(50);
            Assert(pageName.Text == ((DropShelf.Views.Controls.PageTile)pageCards.Children[1]).Header && !pageName.Text.Any(c => c > 0x2E80),
                "切换语言后侧栏页面标题没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
            Pump(50);
        }
        var backLayer = (System.Windows.Controls.Canvas)window.FindName("StackBackLayer")!;
        var frontLayer = (System.Windows.Controls.Canvas)window.FindName("StackFrontLayer")!;
        var stage = (FrameworkElement)window.FindName("PageStage")!;
        // 露出的牌边是真按钮；鼠标点击实际命中按钮，键盘也可激活同一个跳转。
        foreach (var edge in edges)
        {
            var point = edge.TranslatePoint(new System.Windows.Point(edge.ActualWidth / 2, edge.ActualHeight / 2), window);
            var hit = window.InputHitTest(point) as DependencyObject;
            while (hit is System.Windows.Media.Visual && hit != edge) hit = System.Windows.Media.VisualTreeHelper.GetParent(hit);
            Assert(hit == edge, "露出的牌边必须能被鼠标命中，不能被设置页的透明区域挡住");
        }
        edges[^1].RaiseEvent(new MouseEventArgs(Mouse.PrimaryDevice, 0) { RoutedEvent = Mouse.MouseEnterEvent });
        Pump(240);
        var lowest = edges.Max(edge => edge.TranslatePoint(new System.Windows.Point(0, edge.ActualHeight), stage).Y);
        Assert(stage.ActualHeight - lowest >= 20, "牌边悬停展开后距离窗口底部仍应至少留 20 DIP");
        var savedOffset = Math.Min(80, shelfScroll.ScrollableHeight);
        shelfScroll.ScrollToVerticalOffset(savedOffset);
        Pump(50);
        foreach (var page in new[] { "General", "Appearance", "Actions", "Shelf" })
        {
            edges.Single(edge => Equals(edge.Tag, page)).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            if (frontLayer.Visibility == Visibility.Visible)
            {
                var image = frontLayer.Children.OfType<System.Windows.Controls.Image>().Single();
                var bitmap = (System.Windows.Media.Imaging.BitmapSource)image.Source;
                var pixels = new byte[bitmap.PixelWidth * bitmap.PixelHeight * 4];
                bitmap.CopyPixels(pixels, bitmap.PixelWidth * 4, 0);
                Assert(Enumerable.Range(0, pixels.Length / 4).Count(i => pixels[i * 4 + 3] > 127) > 50,
                    "抽牌开始前卡面上就必须有设置 UI，不能是一张透明位图");
                Pump(190);
                Assert(frontLayer.Clip is System.Windows.Media.RectangleGeometry geometry && geometry.Rect.Height > 40 &&
                       backLayer.Visibility == Visibility.Visible, "动画中途应已抽出带设置内容的新卡，旧卡同时退回");
                if (page == "General")
                {
                    window.Width += 1;
                    Pump(60);
                    window.Width -= 1;
                }
            }
            Pump(650);
            Assert(((FrameworkElement)window.FindName(page + "Host")!).Visibility == Visibility.Visible &&
                   overviewHost.Visibility == Visibility.Collapsed &&
                   new[] { backLayer, frontLayer }.All(layer => layer.Visibility == Visibility.Collapsed && layer.Children.Count == 0 && layer.Clip is null) &&
                   stackLayer is { Clip: null, IsHitTestVisible: true },
                "抽牌完成或被窗口变化打断后应显示目标页，并释放两张临时卡面");
            Assert(edges.Select(edge => (string)edge.Tag).Distinct().Count() == 3 && edges.All(edge => !Equals(edge.Tag, page)),
                "切换后牌堆应恰好包含另外三页，旧页也能再点回来");
        }
        Assert(Math.Abs(shelfScroll.VerticalOffset - savedOffset) < 1, "设置页之间切换不应重置各页的滚动位置");
        Assert(((FrameworkElement)window.FindName("HistorySection")!).Visibility == Visibility.Collapsed, "没有记录时不应显示最近关闭");

        var viewModel = (SettingsViewModel)window.DataContext;
        var sensitivitySlider = (DropShelf.Views.Controls.DeckSlider)window.FindName("SensitivitySlider")!;
        sensitivitySlider.Value = 20;
        Assert(viewModel.SensitivityDescription == Strings.SensitivityDetail(5, 63), "拖动滑块时说明没有立即更新");
        Assert(sensitivitySlider.FaceDownCount == 2, "切牌卡越过前两张牌的中线，应正好翻过两张");
        Pump(400);
        var labelScales = new[] { "SteadyLabel", "BalancedLabel", "ResponsiveLabel" }
            .Select(name => ((System.Windows.Media.ScaleTransform)((FrameworkElement)window.FindName(name)!).RenderTransform).ScaleX).ToArray();
        Assert(labelScales[0] > labelScales[1] && labelScales[1] > labelScales[2] && Math.Abs(labelScales[2] - 0.85) < 0.001,
            "滑块偏向稳健时，稳健应最大、均衡其次、灵敏保持最小");
        ((System.Windows.Controls.ListBox)window.FindName("TriggerSelector")!).SelectedValue = AutoOpenTrigger.Edge;
        Assert(!viewModel.UsesShake && viewModel.UsesEdge, "只用屏幕边缘时不应显示晃动灵敏度");
        ((System.Windows.Controls.ListBox)window.FindName("EdgeDwellSelector")!).SelectedValue = EdgeDwellTime.Long;
        ((System.Windows.Controls.ListBox)window.FindName("DragOutModeSelector")!).SelectedValue = TransferMode.Copy;
        ((System.Windows.Controls.ListBox)window.FindName("ShelfSizeSelector")!).SelectedValue = QuickShelfSize.Small;
        ((System.Windows.Controls.ListBox)window.FindName("ThemeSelector")!).SelectedValue = AppTheme.Dark;
        ((System.Windows.Controls.ListBox)window.FindName("AccentSelector")!).SelectedValue = "#038387";
        ((System.Windows.Controls.ListBox)window.FindName("ActionLayoutSelector")!).SelectedValue = QuickActionLayout.Two;
        ((System.Windows.Controls.CheckBox)window.FindName("CloseWhenEmptyBox")!).IsChecked = false;
        viewModel.AssignSlot(1, "Reveal");
        Assert(viewModel.ShelfSummary == "屏幕边缘呼出 · 拖出时复制" && viewModel.ActionsSummary == "压缩、显示位置" &&
               viewModel.AppearanceSummary == "深色 · 自选强调色", "设置卡片上的摘要不正确");
        Assert(viewModel.VisibleSlots.Select(slot => slot.Position).SequenceEqual(["左", "右"]), "两个操作时位置应为左、右");
        Assert(viewModel.MoreActions.All(card => viewModel.VisibleSlots.All(slot => slot.Action.Id != card.Action.Id)) &&
               viewModel.MoreActions.Count == QuickActions.Available(controller.Settings.CustomActions).Count - 2, "更多操作应列出其余全部操作");
        viewModel.Hotkey = new HotkeyGesture(ModifierKeys.Control | ModifierKeys.Alt, Key.D);
        viewModel.DismissGettingStartedCommand.Execute(null);
        Pump(50);
        Assert(((FrameworkElement)window.FindName("GettingStartedSection")!).Visibility == Visibility.Collapsed, "快速上手关闭后仍显示");

        var historyFolder = CreateFolder("main-history");
        for (var index = 0; index < 5; index++)
        {
            var path = Path.Combine(historyFolder, $"{index}.txt");
            File.WriteAllText(path, "x");
            controller.History.Record([new ShelfItem { Id = $"h{index}", Kind = ShelfItemKind.File, Value = path }]);
        }
        Pump(50);
        Assert(((FrameworkElement)window.FindName("HistorySection")!).Visibility == Visibility.Visible &&
               viewModel.HistoryEntries.Count == 5, "最近关闭应全部显示在牌堆中");
        Assert(viewModel.HistoryEntries[0].Stack0 is { Glyph.Length: 1 } && viewModel.HistoryEntries[0].Stack1 is null, "历史卡片的缩略图堆叠不正确");
        // 牌堆在概览页上，回到概览页排版后才会排开。
        var shelfPage = (System.Windows.Controls.Panel)window.FindName("ShelfPage")!;
        var backButton = (System.Windows.Controls.Button)((System.Windows.Controls.Panel)shelfPage.Children[0]).Children[0];
        backButton.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(900);
        Assert(((FrameworkElement)window.FindName("OverviewPage")!) is { Opacity: 1, CacheMode: null }, "切换页面的淡入动画结束后页面应完全显示并去掉位图缓存");
        Assert(shelfHost.Visibility == Visibility.Collapsed && shelfHost.Children.Count == 1 && overviewHost.Clip is null &&
               pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1 && tile.RenderTransform.Value.IsIdentity), "返回概览页后卡片应落回原位，设置页收起");
        // 翻牌途中窗口大小变了，算好的位置失效，应直接摆到终点；返回时概览页不能停在透明状态。
        var generalHost = (FrameworkElement)window.FindName("GeneralHost")!;
        ((System.Windows.Controls.Button)pageCards.Children[0]).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(250);
        window.Width += 10;
        Pump(100);
        Assert(generalHost.Visibility == Visibility.Visible && overviewHost.Visibility == Visibility.Collapsed && generalHost.Clip is null &&
               generalHost.RenderTransform.Value.IsIdentity, "打开途中被打断后应直接显示设置页");
        var generalBack = (System.Windows.Controls.Button)((System.Windows.Controls.Panel)((System.Windows.Controls.Panel)window.FindName("GeneralPage")!).Children[0]).Children[0];
        generalBack.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(30);
        window.Width -= 10;
        Pump(100);
        Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed && overviewHost.Clip is null &&
               ((FrameworkElement)window.FindName("OverviewPage")!) is { Opacity: 1, CacheMode: null } &&
               pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1 && tile.RenderTransform.Value.IsIdentity), "返回途中被打断后概览页应完全显示，卡片回到原位");
        Pump(400);
        // 设置页里按 Esc 或鼠标后退键也能回到概览。
        foreach (var (name, goBack) in new (string, Action)[]
                 {
                     ("Esc", () => window.RaiseEvent(new System.Windows.Input.KeyEventArgs(Keyboard.PrimaryDevice, PresentationSource.FromVisual(window)!, 0, Key.Escape) { RoutedEvent = Keyboard.KeyDownEvent })),
                     ("鼠标后退键", () => window.RaiseEvent(new MouseButtonEventArgs(Mouse.PrimaryDevice, 0, MouseButton.XButton1) { RoutedEvent = Mouse.MouseDownEvent })),
                 })
        {
            ((System.Windows.Controls.Button)pageCards.Children[0]).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Pump(900);
            goBack();
            Pump(900);
            Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed, $"设置页里按{name}应回到概览");
        }
        // 拖动翻牌：转过一半前只转动卡片，过了侧立换成页面，往回拖又换回卡片；松手时没过侧立就退回，过了或甩得够快就翻过去。
        // 软件渲染时背面空白，拖动直接播完一次翻牌。
        var generalTile = (DropShelf.Views.Controls.PageTile)pageCards.Children[0];
        var overviewPage = (FrameworkElement)window.FindName("OverviewPage")!;
        var generalScroll = (System.Windows.Controls.ScrollViewer)window.FindName("GeneralScroll")!;
        if (window.BeginFlipDrag(generalTile))
        {
            void AssertCardFacesVisible(string when)
            {
                stackLayer.UpdateLayout();
                var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap((int)Math.Ceiling(stackLayer.ActualWidth),
                    (int)Math.Ceiling(stackLayer.ActualHeight), 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
                bitmap.Render(stackLayer);
                foreach (var edge in edges)
                {
                    var point = edge.TranslatePoint(new System.Windows.Point(edge.ActualWidth / 2, edge.ActualHeight / 2), stackLayer);
                    var pixel = new byte[4];
                    bitmap.CopyPixels(new Int32Rect((int)point.X, (int)point.Y, 1, 1), pixel, 4, 0);
                    Assert(pixel[3] > 0, $"{when}其余卡牌的卡面必须连续可见，不能只在结束时恢复真实按钮");
                }
            }
            AssertCardFacesVisible("打开的第一帧");
            var tileFace = (UIElement)stackLayer.Children[^1];
            window.DragFlip(0.3);
            Pump(50);
            Assert(generalTile.Opacity == 0 && tileFace.Visibility == Visibility.Visible && !tileFace.RenderTransform.Value.IsIdentity &&
                   generalHost.Visibility == Visibility.Hidden, "拖到侧立前应只转动卡片的截图");
            window.DragFlip(0.7);
            Pump(50);
            Assert(tileFace.Visibility == Visibility.Hidden && generalHost.Visibility == Visibility.Visible && overviewHost.Clip is not null && stackLayer.Clip is not null,
                "拖过侧立后应换成页面");
            window.DragFlip(0.2);
            Pump(50);
            Assert(tileFace.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Hidden && overviewHost.Clip is null, "往回拖过侧立后应换回卡片");
            window.ReleaseFlip(0);
            Pump(700);
            Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed && overviewPage is { Opacity: 1, CacheMode: null } &&
                   generalTile is { Opacity: 1 } && generalTile.RenderTransform.Value.IsIdentity, "没拖过侧立就松手应退回概览页");
            Assert(window.BeginFlipDrag(generalTile), "退回后应能再次拖动");
            window.DragFlip(0.6);
            window.ReleaseFlip(0);
            Pump(700);
            Assert(generalHost is { Visibility: Visibility.Visible, Clip: null, IsHitTestVisible: true } && generalHost.RenderTransform.Value.IsIdentity &&
                   generalScroll is { CacheMode: null, Opacity: 1 } && overviewHost.Visibility == Visibility.Collapsed, "拖过侧立后松手应翻开页面");
            Assert(window.BeginFlipDrag(null), "设置页应能拖回概览页");
            window.DragFlip(0.3);
            window.ReleaseFlip(0);
            Pump(700);
            Assert(generalHost is { Visibility: Visibility.Visible, Clip: null } && overviewHost.Visibility == Visibility.Collapsed, "返回时没拖过侧立就松手应留在设置页");
            Assert(window.BeginFlipDrag(null), "退回后应能再次拖回概览页");
            window.DragFlip(1);
            Pump(50);
            AssertCardFacesVisible("返回即将落位时");
            window.DragFlip(0.2);
            window.ReleaseFlip(0.01);
            Pump(700);
        }
        else
        {
            Pump(900);
            generalBack.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Pump(900);
        }
        Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed && overviewPage is { Opacity: 1, CacheMode: null } &&
               pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1 && tile.RenderTransform.Value.IsIdentity) &&
               stackLayer.Visibility == Visibility.Collapsed, "甩动应翻回概览页，卡片落回原位，牌的边收起");
        var historyList = (System.Windows.Controls.ItemsControl)window.FindName("HistoryList")!;
        void AssertFanArranged(string when)
        {
            var cards = Enumerable.Range(0, 5).Select(index => (UIElement)historyList.ItemContainerGenerator.ContainerFromIndex(index)).ToArray();
            Assert(cards.All(card => card.RenderTransform is System.Windows.Media.TransformGroup) &&
                   System.Windows.Controls.Panel.GetZIndex(cards[2]) > System.Windows.Controls.Panel.GetZIndex(cards[0]) &&
                   cards.All(card => !DropShelf.Views.Controls.FanPanel.GetIsHot(card)), $"{when}牌堆应以中间的牌在最上面排开，默认不选出任何一张");
        }
        AssertFanArranged("");
        // 调整顺序时卡片沿用原来的，只换位置。
        var firstEntry = viewModel.HistoryEntries[0];
        var firstCard = historyList.ItemContainerGenerator.ContainerFromItem(firstEntry);
        controller.History.Move(firstEntry.Entry, 3);
        Pump(50);
        Assert(viewModel.HistoryEntries[3] == firstEntry && historyList.ItemContainerGenerator.ContainerFromItem(firstEntry) == firstCard,
            "调整顺序后卡片应沿用原来的，不重新生成");
        AssertFanArranged("调整顺序后");
        controller.History.Move(firstEntry.Entry, 0);
        Pump(50);
        // 切换语言会重新生成同样数量的牌，也要重新排开。
        viewModel.Language = AppLanguage.English;
        Pump(50);
        viewModel.Language = AppLanguage.Chinese;
        Pump(50);
        AssertFanArranged("切换语言后");
        var pageScroll = (System.Windows.Controls.ScrollViewer)window.FindName("OverviewScroll")!;
        foreach (var height in new[] { 640.0, 1100.0 })
        {
            window.Height = height;
            Pump(700);
            var fanBottom = historyList.TranslatePoint(new Point(0, historyList.ActualHeight), pageScroll).Y;
            Assert(Math.Abs(fanBottom - pageScroll.ViewportHeight) < 1, $"窗口高 {height} 时牌堆应贴着窗口底边");
        }
        Assert(window.SettingsCardHeight > shortCards + 40, "有牌堆时窗口变高后设置卡片也应拉长");
        window.Height = 640;
        controller.History.Clear();

        viewModel.BeginNewCustomAction();
        Assert(viewModel.IsEditorOpen && ((FrameworkElement)window.FindName("EditorOverlay")!).Visibility == Visibility.Visible, "自定义操作对话框没有打开");
        var editor = viewModel.Editor!;
        Assert(!editor.TrySave() && editor.Error is not null, "没有选择文件夹时不应保存");
        editor.Kind = CustomActionKind.CompressImage;
        editor.SizeText = "abc";
        Assert(!editor.TrySave(), "无效大小不应保存");
        editor.SizeText = "200";
        Assert(editor.NamePlaceholder == "≤ 200 KB" && editor.TrySave() && !viewModel.IsEditorOpen, "自定义操作没有保存");

        var saved = service.Load();
        Assert(saved.ShakeSensitivity == 20, "灵敏度没有在停下后保存");
        Assert(saved.AutoOpenTrigger == AutoOpenTrigger.Edge && saved.EdgeDwell == EdgeDwellTime.Long, "呼出方式没有保存");
        Assert(saved.DragOutMode == TransferMode.Copy, "拖出方式没有保存");
        Assert(saved.QuickShelfSize == QuickShelfSize.Small, "尺寸没有立即保存");
        Assert(saved.Theme == AppTheme.Dark && ThemeService.IsDark, "主题没有保存或生效");
        Assert(!saved.CloseWhenEmpty && !saved.ShowGettingStarted, "开关设置没有保存");
        Assert(saved.AccentColor == "#038387" && ThemeService.Accent.Base == ThemeService.ParseAccent("#038387"), "强调色没有保存或生效");
        Assert(saved.QuickActionLayout == QuickActionLayout.Two && viewModel.VisibleSlots.Count == 2, "按钮数量没有保存");
        var created = saved.CustomActions!.Single(action => action.ImageSizeKb == 200);
        Assert(viewModel.MoreActions.Any(card => card.Action.Id == created.Id), "新建的操作应出现在更多操作中");
        viewModel.AssignSlot(0, created.Id);
        Assert(service.Load().QuickActionSlots![0] == created.Id && service.Load().QuickActionSlots![1] == "Reveal", "拖到位置上的操作没有保存");
        Assert(saved.Hotkey == "Ctrl+Alt+D", "快捷键没有保存");

        viewModel.EditCustomAction(created);
        viewModel.Editor!.Name = "压缩证件照";
        viewModel.Editor.TrySave();
        Assert(service.Load().CustomActions!.Single(action => action.Id == created.Id).Name == "压缩证件照", "编辑自定义操作没有保存");
        viewModel.DeleteCustomAction(created);
        Assert(service.Load().CustomActions!.All(action => action.Id != created.Id) && !service.Load().QuickActionSlots!.Contains(created.Id),
            "删除的自定义操作仍在置物架上");

        Assert(generalTile.Header == "常规", "设置卡片文字不正确");
        try
        {
            ((System.Windows.Controls.ListBox)window.FindName("LanguageSelector")!).SelectedValue = AppLanguage.English;
            Pump(50);
            Assert(service.Load().Language == AppLanguage.English && !LanguageService.IsChinese, "语言设置没有保存或生效");
            Assert(generalTile.Header == "General" && viewModel.GeneralSummary.StartsWith("English"), "切换语言后主窗口文字没有更新");
            viewModel.Language = AppLanguage.Chinese;
            Pump(50);
            Assert(generalTile.Header == "常规" && LanguageService.IsChinese, "切换回中文后主窗口文字没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }

        ThemeService.SetMode(AppTheme.Light);
        window.Close();
        File.Delete(service.SettingsPath);
    }

    private static void TestOnboardingSteps()
    {
        var window = new OnboardingWindow { Left = -20000, Top = -20000, WindowStartupLocation = WindowStartupLocation.Manual };
        window.Show();
        Pump(150);
        var next = (System.Windows.Controls.Button)window.FindName("NextButton")!;
        Assert(window.CurrentStep == 0 && (string)next.Content == "下一步", "教程起始状态不正确");
        for (var step = 1; step < 4; step++) window.ShowStep(step);
        Assert(window.CurrentStep == 3 && (string)next.Content == "开始使用", "教程最后一步不正确");
        var hotkey = AppController.Current?.Settings.HotkeyGesture ?? HotkeyGesture.Default;
        Assert(((System.Windows.Controls.TextBlock)window.FindName("StepBodyText")!).Text.Contains(hotkey.ToString()), "教程没有说明当前的快捷键");
        LanguageService.SetMode(AppLanguage.English);
        try
        {
            Assert((string)next.Content == "Get started" &&
                   ((System.Windows.Controls.TextBlock)window.FindName("StepTitleText")!).Text == "Quick actions and shortcut",
                "切换语言后教程文字没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }
        window.ShowStep(99);
        Assert(window.CurrentStep == 3, "越界步骤没有限制");
        window.Close();
    }

    private static void TestProbeWindow()
    {
        var probe = new ProbeWindow();
        probe.ShowAt(new POINT(-10000, -10000));
        Pump(100);
        Assert(probe.IsVisible, "探针窗口没有显示");
        probe.HideProbe();
        Assert(!probe.IsVisible, "探针窗口没有隐藏");
        probe.Close();
    }

    // ───────────── 工具 ─────────────

    private static byte[] CreatePng()
    {
        var bitmap = System.Windows.Media.Imaging.BitmapSource.Create(2, 2, 96, 96, System.Windows.Media.PixelFormats.Bgra32, null,
            new byte[16], 8);
        var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
        encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
        using var stream = new MemoryStream();
        encoder.Save(stream);
        return stream.ToArray();
    }

    /// <summary>随机噪点图片几乎无法压缩，用来确认压缩到指定大小的逻辑。带有透明度，也能检查透明部分的处理。</summary>
    private static byte[] CreateNoisyPng(int width, int height)
    {
        var random = new Random(42);
        var pixels = new byte[width * height * 4];
        random.NextBytes(pixels);
        var bitmap = System.Windows.Media.Imaging.BitmapSource.Create(width, height, 96, 96, System.Windows.Media.PixelFormats.Bgra32, null,
            pixels, width * 4);
        var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
        encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
        using var stream = new MemoryStream();
        encoder.Save(stream);
        return stream.ToArray();
    }

    private static void Pump(int milliseconds)
    {
        var end = Environment.TickCount64 + milliseconds;
        while (Environment.TickCount64 < end)
        {
            var frame = new DispatcherFrame();
            Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() => frame.Continue = false));
            Dispatcher.PushFrame(frame);
            Thread.Sleep(10);
        }
    }

    private static void Run(string name, Action test) => Run(name, () =>
    {
        test();
        return Task.CompletedTask;
    });

    private static void Run(string name, Func<Task> test)
    {
        try
        {
            var task = test();
            if (!task.IsCompleted)
            {
                var frame = new DispatcherFrame();
                task.ContinueWith(_ => frame.Continue = false, TaskScheduler.Default);
                Dispatcher.PushFrame(frame);
            }
            task.GetAwaiter().GetResult();
            _passed++;
            Console.WriteLine($"通过  {name}");
        }
        catch (Exception exception)
        {
            _failed++;
            Console.WriteLine($"失败  {name}：{exception.Message}");
            // 断言失败的消息已说明原因；其他异常附上调用栈，才能找到出错的位置。
            if (exception is not InvalidOperationException) Console.WriteLine(exception.StackTrace);
        }
    }

    private static string CreateFolder(string name)
    {
        var path = Path.Combine(_root, name);
        Directory.CreateDirectory(path);
        return path;
    }

    private static string CreateStableFolder(string name)
    {
        var path = Path.Combine(_stableRoot, name);
        Directory.CreateDirectory(path);
        return path;
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
