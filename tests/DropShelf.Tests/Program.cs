using System.IO;
using System.IO.Compression;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.Services.Gestures;
using DropShelf.ViewModels;
using DropShelf.Views;
using WpfApplication = System.Windows.Application;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static class Program
{
    private static int _passed;
    private static int _failed;
    private static string _root = string.Empty;

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
        ThemeService.Initialize(AppTheme.Light);

        // 设置与模型
        Run("设置默认值与保存恢复", TestSettingsRoundTrip);
        Run("1.0 版设置自动迁移", TestSettingsMigration);
        Run("异常设置恢复为默认值", TestSettingsNormalization);
        Run("快捷键解析与显示", TestHotkeyGesture);
        Run("快速操作去重、排序且最多四个", TestQuickActionNormalize);
        Run("置物架项目名称与说明", TestShelfItemText);
        Run("三档尺寸保持比例", TestShelfSizes);
        Run("小窗以鼠标为中心并保持在屏幕内", TestQuickShelfPositioning);
        Run("正式版本号已写入程序", TestProductVersion);

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
        Run("低级鼠标钩子可以启动和停止", TestGestureServiceLifecycle);

        // 置物架逻辑
        Run("100 个文件与重复路径", TestOneHundredFilesAndDuplicates);
        Run("拖入文件只读取路径属性", TestAddingLockedFileReadsNoContent);
        Run("清空置物架不影响磁盘文件", TestClearKeepsFiles);
        Run("文本与链接可以暂存并去重", TestTextAndLinkEntries);
        Run("堆叠显示最新项目和汇总", TestStackSummary);
        Run("拖出结果按设置移除或保留", TestApplyDragOut);
        Run("拖出结果判定（含资源管理器优化移动）", TestResolveDragOut);
        Run("移动后更新置物架中的路径", TestApplyMoves);

        // 拖放与剪贴板数据
        Run("识别拖入内容类型", TestClassifyDropData);
        Run("读取文件、文本、链接和图片", TestReadDropData);
        Run("识别来自置物架自身的拖动", TestSourceShelfId);
        Run("拖出数据包含不同文件夹中的全部文件", TestShellDataObject);
        Run("未落盘附件可接收为实际文件", TestVirtualAttachmentImport);

        // 文件与快速操作
        Run("同名复制不覆盖", TestCopyDoesNotOverwrite);
        Run("源文件消失时返回失败", TestMissingSourceIsReported);
        Run("文件被占用时返回失败", TestLockedFileIsReported);
        Run("阻止复制到文件夹自身内部", TestNestedDestinationIsRejected);
        Run("快速压缩与安全解压", TestArchiveActions);
        Run("复制路径写入剪贴板", TestCopyPathAction);
        Run("移动到所选文件夹并可取消", TestMoveToAction);
        Run("压缩结果放回置物架", TestArchiveAddsOutputToShelf);
        Run("复制文件到剪贴板", TestCopyFilesToClipboard);

        // 系统集成
        Run("文件使用 Windows 系统缩略图和类型名称", TestShellThumbnail);
        Run("开机启动命令正确处理空格路径", TestStartupCommand);
        Run("主题解析与强调色对比度", TestThemeAndAccent);
        Run("托盘图标可从图标资源创建", TestTrayIconLoader);
        Run("全局快捷键可注册和注销", TestHotkeyRegistration);
        Run("命令行文件可转交给运行中的实例", TestArgumentForwarding);
        Run("错误日志已生成", TestFailureLogExists);

        // 界面
        Run("置物架窗口按设置生成快速操作", TestShelfWindowActions);
        Run("主窗口整合设置并自动保存", TestMainWindowSettings);
        Run("教程可以逐步切换", TestOnboardingSteps);
        Run("探针窗口可以显示和隐藏", TestProbeWindow);

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
        Assert(defaults.GestureEnabled && defaults.RemoveAfterDragOut && defaults.CloseWhenEmpty, "默认开关不正确");
        Assert(defaults.QuickShelfSize == QuickShelfSize.Medium, "默认尺寸不是中档");
        Assert(defaults.QuickActions!.SequenceEqual(QuickActions.Defaults), "默认快速操作不正确");
        Assert(defaults.Hotkey == "Ctrl+Shift+Space", "默认快捷键不正确");

        defaults.MouseGestureSensitivity = GestureSensitivity.Steady;
        defaults.Theme = AppTheme.Dark;
        defaults.QuickShelfSize = QuickShelfSize.Large;
        defaults.QuickActions = [QuickActionType.Reveal, QuickActionType.CopyFullPath];
        defaults.RemoveAfterDragOut = false;
        defaults.Hotkey = "Ctrl+Alt+D";
        defaults.HasCompletedFirstRun = true;
        service.Save(defaults);

        var loaded = service.Load();
        Assert(loaded.MouseGestureSensitivity == GestureSensitivity.Steady, "灵敏度未恢复");
        Assert(loaded.Theme == AppTheme.Dark, "主题未恢复");
        Assert(loaded.QuickShelfSize == QuickShelfSize.Large, "尺寸未恢复");
        Assert(loaded.QuickActions!.SequenceEqual([QuickActionType.CopyFullPath, QuickActionType.Reveal]), "快速操作未按固定顺序恢复");
        Assert(!loaded.RemoveAfterDragOut, "拖出后移除设置未恢复");
        Assert(loaded.Hotkey == "Ctrl+Alt+D", "快捷键未恢复");
        Assert(loaded.HasCompletedFirstRun, "首次运行状态未恢复");

        var clone = loaded.Clone();
        clone.QuickActions!.Clear();
        Assert(loaded.QuickActions!.Count == 2, "复制设置时共享了快速操作列表");
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
        Assert(migrated.MouseGestureSensitivity == GestureSensitivity.Responsive && migrated.QuickShelfSize == QuickShelfSize.Small,
            "旧设置没有保留");
        Assert(migrated.QuickActions!.SequenceEqual(QuickActions.Defaults), "旧的单个快速操作没有迁移为默认组合");
        Assert(migrated.WindowLeft == 624 && migrated.HasCompletedFirstRun, "窗口位置或首次运行状态丢失");

        File.WriteAllText(service.SettingsPath, """{ "QuickAction": "None" }""");
        Assert(service.Load().QuickActions!.Count == 0, "关闭快速操作的旧设置没有保持关闭");

        File.WriteAllText(service.SettingsPath, """{ "QuickAction": "ConvertPng" }""");
        Assert(service.Load().QuickActions!.SequenceEqual(QuickActions.Defaults), "已移除的图片转换设置没有恢复默认");
        File.Delete(service.SettingsPath);
    }

    private static void TestSettingsNormalization()
    {
        var invalid = new AppSettings
        {
            MouseGestureSensitivity = (GestureSensitivity)999,
            Theme = (AppTheme)999,
            QuickShelfSize = (QuickShelfSize)999,
            QuickActions = [QuickActionType.ConvertJpeg, QuickActionType.Properties, QuickActionType.Properties, QuickActionType.Reveal,
                QuickActionType.MoveTo, QuickActionType.CopyFullPath, QuickActionType.CompressZip],
            Hotkey = "Shift+A",
            WindowWidth = 10,
            WindowHeight = double.NaN
        };
        var normalized = SettingsService.Normalize(invalid.Clone());
        Assert(normalized.MouseGestureSensitivity == GestureSensitivity.Balanced, "异常灵敏度没有恢复默认值");
        Assert(normalized.Theme == AppTheme.System, "异常主题没有恢复默认值");
        Assert(normalized.QuickShelfSize == QuickShelfSize.Medium, "异常尺寸没有恢复默认值");
        Assert(normalized.QuickActions!.Count == 4 && !normalized.QuickActions.Contains(QuickActionType.ConvertJpeg), "快速操作没有规范化");
        Assert(normalized.Hotkey == HotkeyGesture.Default.ToString(), "无效快捷键没有恢复默认值");
        Assert(normalized.WindowWidth >= 760 && normalized.WindowHeight >= 540, "异常窗口尺寸没有恢复默认值");
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
    }

    private static void TestShelfItemText()
    {
        var folder = new ShelfItem { Id = "1", Kind = ShelfItemKind.Folder, Value = @"C:\Users\Test\Documents\" };
        Assert(folder.DisplayName == "Documents", "带结尾分隔符的文件夹名称不正确");
        var text = new ShelfItem { Id = "2", Kind = ShelfItemKind.Text, Value = "  第一行   内容\n第二行" };
        Assert(text.DisplayName == "第一行 内容", "文本摘要不正确");
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
        var version = typeof(AppController).Assembly.GetName().Version;
        Assert(version == new Version(2, 0, 0, 0), $"程序版本号不正确：{version}");
    }

    // ───────────── 手势识别 ─────────────

    private static void TestBalancedGestureTriggers()
    {
        var detector = new MouseGestureDetector(GestureSensitivity.Balanced);
        var samples = new[] { (100, 100, 0L), (150, 101, 60L), (95, 100, 120L), (155, 101, 180L), (90, 100, 240L) };
        Assert(samples.Any(sample => detector.AddSample(sample.Item1, sample.Item2, sample.Item3, false)), "快速左右晃动未被识别");
    }

    private static void TestSensitivityProfilesTrigger()
    {
        Assert(Shake(GestureSensitivity.Responsive, [100, 120, 100, 120], 70), "灵敏档无法通过短距离两次来回触发");
        Assert(Shake(GestureSensitivity.Balanced, [100, 125, 100, 125, 100], 70), "均衡档无法通过正常幅度晃动触发");
        Assert(Shake(GestureSensitivity.Steady, [100, 135, 100, 135, 100, 135], 70), "稳健档无法通过明确晃动触发");

        static bool Shake(GestureSensitivity sensitivity, int[] xs, long interval)
        {
            var detector = new MouseGestureDetector(sensitivity);
            return xs.Select((x, index) => (x, index)).Any(sample => detector.AddSample(sample.x, 100, sample.index * interval, false));
        }
    }

    private static void TestGestureDurationBounds()
    {
        var tooFast = new MouseGestureDetector(GestureSensitivity.Balanced);
        var fastSamples = new[] { (100, 0L), (125, 30L), (100, 60L), (125, 90L), (100, 120L), (125, 150L) };
        Assert(!fastSamples.Any(sample => tooFast.AddSample(sample.Item1, 100, sample.Item2, false)), "瞬间抖动被错误识别为晃动手势");

        var tooSlow = new MouseGestureDetector(GestureSensitivity.Balanced);
        var slowSamples = new[] { (100, 0L), (125, 300L), (100, 600L), (125, 900L), (100, 1200L), (125, 1500L) };
        Assert(!slowSamples.Any(sample => tooSlow.AddSample(sample.Item1, 100, sample.Item2, false)), "缓慢来回移动被错误识别为晃动手势");
    }

    private static void TestGestureToleratesIrregularSamples()
    {
        var delayed = new MouseGestureDetector(GestureSensitivity.Balanced);
        var delayedSamples = new[] { (100, 100, 0L), (125, 103, 60L), (100, 99, 380L), (126, 105, 445L), (99, 98, 510L) };
        Assert(delayedSamples.Any(sample => delayed.AddSample(sample.Item1, sample.Item2, sample.Item3, false)),
            "系统短暂忙碌导致采样间隔变长后未能识别手势");

        var compact = new MouseGestureDetector(GestureSensitivity.Balanced);
        var compactSamples = new[] { 100, 122, 100, 122, 100, 122 };
        Assert(compactSamples.Select((x, index) => (x, index))
                .Any(sample => compact.AddSample(sample.x, 100 + sample.index % 2 * 2, sample.index * 55L, false)),
            "合理幅度和时长的晃动未被识别");
    }

    private static void TestOtherDirectionGesturesTrigger()
    {
        var vertical = new MouseGestureDetector(GestureSensitivity.Balanced);
        var verticalSamples = new[] { (100, 100), (101, 150), (100, 95), (101, 155), (100, 90) };
        Assert(verticalSamples.Select((point, index) => (point, index))
            .Any(sample => vertical.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 65L, false)), "快速上下晃动未被识别");

        var diagonal = new MouseGestureDetector(GestureSensitivity.Balanced);
        var diagonalSamples = new[] { (100, 100), (140, 140), (90, 90), (145, 145), (85, 85) };
        Assert(diagonalSamples.Select((point, index) => (point, index))
            .Any(sample => diagonal.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 65L, false)), "快速斜向晃动未被识别");
    }

    private static void TestNormalMovementDoesNotTrigger()
    {
        var straight = new MouseGestureDetector(GestureSensitivity.Responsive);
        Assert(!Enumerable.Range(0, 12).Any(index => straight.AddSample(100 + index * 12, 100 + index, index * 40L, false)), "普通直线移动被误识别");

        var turning = new MouseGestureDetector(GestureSensitivity.Responsive);
        var turningSamples = new[] { (100, 100), (140, 100), (140, 140), (100, 140), (100, 100), (140, 100) };
        Assert(!turningSamples.Select((point, index) => (point, index))
            .Any(sample => turning.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 45L, false)), "普通转弯移动被误识别");
    }

    private static void TestScreenEdgeDoesNotTrigger()
    {
        var detector = new MouseGestureDetector(GestureSensitivity.Responsive);
        var samples = new[] { (100, 0L), (150, 50L), (95, 100L), (155, 150L), (90, 200L) };
        Assert(!samples.Any(sample => detector.AddSample(sample.Item1, 4, sample.Item2, true)), "屏幕边缘移动被误识别");
    }

    private static void TestHighRateShakeTriggers()
    {
        foreach (var sensitivity in Enum.GetValues<GestureSensitivity>())
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
        foreach (var sensitivity in Enum.GetValues<GestureSensitivity>())
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
        var straight = new MouseGestureDetector(GestureSensitivity.Responsive);
        var straightTriggered = Enumerable.Range(0, 1500).Any(ms => straight.AddRawSample(100 + ms, 300 + ms / 10, ms, false));
        Assert(!straightTriggered, "高回报率的直线拖动被误识别");

        var circle = new MouseGestureDetector(GestureSensitivity.Responsive);
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
        var detector = new MouseGestureDetector(GestureSensitivity.Balanced);
        Assert(samples.Select((point, index) => (point, index))
            .Any(sample => detector.AddSample(sample.point.X, sample.point.Y, sample.index * 14L, false)), "掉头时的侧向位移打断了手势");

        var sideways = new MouseGestureDetector(GestureSensitivity.Balanced);
        var zigzag = new[] { (100, 100), (130, 100), (131, 106), (131, 112), (131, 118), (101, 118), (100, 124), (100, 130) };
        Assert(!zigzag.Select((point, index) => (point, index))
            .Any(sample => sideways.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 40L, false)), "连续侧向移动被误识别");
    }

    private static void TestGestureServiceLifecycle()
    {
        var service = new GestureService(Dispatcher.CurrentDispatcher, GestureSensitivity.Balanced, enabled: true);
        service.Start();
        Assert(service.IsRunning, "低级鼠标钩子没有安装");
        service.SetSensitivity(GestureSensitivity.Steady);
        service.Enabled = false;
        service.Dispose();
        Thread.Sleep(100);
        Assert(!service.IsRunning, "停止后钩子仍然存在");
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
        var first = viewModel.AddFiles(paths);
        var second = viewModel.AddFiles(paths);
        Assert(viewModel.Items.Count == 100, $"预期 100 项，实际 {viewModel.Items.Count} 项");
        Assert(first.Added == 100 && second.Duplicates == 100 && second.Message == "这些项目已在置物架中", "添加结果统计不正确");
        var missing = viewModel.AddFiles([Path.Combine(folder, "missing.txt")]);
        Assert(missing.Unavailable == 1 && viewModel.Items.Count == 100, "不存在的文件被添加");
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
        foreach (var cache in picture.CacheDirectories) await VirtualFileDropService.DeleteCacheAsync(cache);
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

    // ───────────── 文件与快速操作 ─────────────

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
        var result = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActionType.CopyFullPath, [a, b], false, IntPtr.Zero));
        Assert(result.Succeeded, result.Message);
        Assert(System.Windows.Clipboard.GetText() == a + Environment.NewLine + b, "剪贴板中的路径不正确");
    }

    private static async Task TestMoveToAction()
    {
        var folder = CreateFolder("move-to");
        var destination = CreateFolder("move-to-destination");
        var source = Path.Combine(folder, "move-me.txt");
        File.WriteAllText(source, "move");

        var cancelled = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActionType.MoveTo, [source], false, IntPtr.Zero, () => null));
        Assert(cancelled.Cancelled && File.Exists(source), "取消选择文件夹后仍然移动了文件");

        var moved = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActionType.MoveTo, [source], false, IntPtr.Zero, () => destination));
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
        var result = await viewModel.RunActionAsync(QuickActionType.CompressZip, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(result.Succeeded && viewModel.Items.Count == 2 && viewModel.Items[1].Value.EndsWith("report.zip"), "压缩结果没有放回置物架");
        Assert(toasts.Single().Success && !viewModel.IsBusy, "操作结果没有提示");

        var empty = await new ShelfViewModel().RunActionAsync(QuickActionType.CompressZip, [], IntPtr.Zero, null);
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

    // ───────────── 系统集成 ─────────────

    private static async Task TestShellThumbnail()
    {
        var folder = CreateFolder("shell-thumbnail");
        var path = Path.Combine(folder, "preview.txt");
        File.WriteAllText(path, "thumbnail preview");
        var thumbnail = await ShellThumbnailService.LoadAsync(path, 96);
        Assert(thumbnail is System.Windows.Media.Imaging.BitmapSource { PixelWidth: > 0 } && thumbnail.IsFrozen, "没有获取到可跨线程使用的缩略图");
        Assert(!string.IsNullOrWhiteSpace(ShellFileInfo.GetTypeName(path)), "没有获取到文件类型名称");
        Assert(ShellFileInfo.GetTypeName("noextension") == "文件", "无扩展名文件类型不正确");
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
        var darkBackground = System.Windows.Media.Color.FromRgb(0x20, 0x20, 0x20);
        Assert(ThemeService.ContrastRatio(dark["AccentTextBrush"], darkBackground) >= 4.5, "强调色文字对比度不足");
        var light = ThemeService.BuildPalette(false, AccentPalette.Default);
        Assert(light["AccentBrush"] == AccentPalette.Default.Dark1 && light["TextOnAccentBrush"] == System.Windows.Media.Colors.White, "默认浅色强调色不正确");
        Assert(ThemeService.PrefersDarkText(System.Windows.Media.Color.FromRgb(0x99, 0xEB, 0xFF)), "浅色强调色上应使用黑字");
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
        Assert(bar.Children.Count == controller.Settings.QuickActions!.Count, "快速操作按钮数量不正确");
        Assert(window.IsReusable, "空置物架应可复用");

        controller.Settings.QuickActions = [];
        window.ApplySettings(controller.Settings);
        Assert(bar.Children.Count == 0 && ((FrameworkElement)window.FindName("ActionBarHost")!).Visibility == Visibility.Collapsed,
            "关闭快速操作后仍显示按钮");

        var folder = CreateFolder("shelf-window");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");
        window.ViewModel.AddFiles([file, folder]);
        Pump(100);
        Assert(!window.IsReusable, "有内容的置物架不应被复用");
        window.ViewModel.IsExpanded = true;
        Pump(100);
        Assert(window.Width >= QuickShelfSizeOptions.ResolveExpandedWidth(controller.Settings.QuickShelfSize), "展开后宽度没有变化");
        window.Close();
        Assert(window.ViewModel.Items.Count == 0, "关闭后没有释放项目");
    }

    private static void TestMainWindowSettings()
    {
        var service = new SettingsService();
        service.Save(new AppSettings { HasCompletedFirstRun = true });
        var controller = new AppController(Dispatcher.CurrentDispatcher);
        var window = new MainWindow(controller) { Left = -20000, Top = -20000, WindowStartupLocation = WindowStartupLocation.Manual };
        window.Show();
        Pump(200);

        foreach (var name in new[] { "NavList", "ExitApplicationButton", "StartupEnabledBox", "HotkeyEditor", "GestureEnabledBox",
                     "SensitivitySelector", "ShelfSizeSelector", "ThemeSelector", "ActionOptionsList" })
            Assert(window.FindName(name) is not null, $"主窗口缺少 {name}");

        ((System.Windows.Controls.ListBox)window.FindName("NavList")!).SelectedIndex = 1;
        Pump(100);
        Assert(((FrameworkElement)window.FindName("ShelfPage")!).Visibility == Visibility.Visible, "导航没有切换页面");

        ((System.Windows.Controls.ListBox)window.FindName("SensitivitySelector")!).SelectedValue = GestureSensitivity.Steady;
        ((System.Windows.Controls.ListBox)window.FindName("ShelfSizeSelector")!).SelectedValue = QuickShelfSize.Small;
        ((System.Windows.Controls.ListBox)window.FindName("ThemeSelector")!).SelectedValue = AppTheme.Dark;
        ((System.Windows.Controls.CheckBox)window.FindName("GestureEnabledBox")!).IsChecked = false;
        var viewModel = (SettingsViewModel)window.DataContext;
        var reveal = viewModel.ActionOptions.Single(option => option.Info.Type == QuickActionType.Reveal);
        Assert(!reveal.CanToggle, "已选满四个时不应再开启新的快速操作");
        reveal.IsEnabled = true;
        Assert(!reveal.IsEnabled, "已选满四个时仍开启了新的快速操作");
        viewModel.ActionOptions.Single(option => option.Info.Type == QuickActionType.Properties).IsEnabled = false;
        Assert(reveal.CanToggle, "关闭一个后应可以开启其他快速操作");
        reveal.IsEnabled = true;
        viewModel.Hotkey = new HotkeyGesture(ModifierKeys.Control | ModifierKeys.Alt, Key.D);

        var saved = service.Load();
        Assert(saved.MouseGestureSensitivity == GestureSensitivity.Steady, "灵敏度没有立即保存");
        Assert(saved.QuickShelfSize == QuickShelfSize.Small, "尺寸没有立即保存");
        Assert(saved.Theme == AppTheme.Dark && ThemeService.IsDark, "主题没有保存或生效");
        Assert(!saved.GestureEnabled, "手势开关没有保存");
        Assert(saved.QuickActions!.Contains(QuickActionType.Reveal) && !saved.QuickActions.Contains(QuickActionType.Properties), "快速操作选择没有保存");
        Assert(saved.Hotkey == "Ctrl+Alt+D", "快捷键没有保存");
        Assert(viewModel.ActionOptions.Count(option => !option.CanToggle) == 1, "选满四个后其余操作应暂时不可开启");
        Assert(saved.QuickActions.SequenceEqual([QuickActionType.CompressZip, QuickActionType.CopyFullPath, QuickActionType.MoveTo, QuickActionType.Reveal]),
            "快速操作顺序不正确");
        Assert(viewModel.StatusTitle == "晃动呼出已暂停", "状态没有反映暂停");

        ThemeService.SetMode(AppTheme.Light);
        window.CloseForShutdown();
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
        }
    }

    private static string CreateFolder(string name)
    {
        var path = Path.Combine(_root, name);
        Directory.CreateDirectory(path);
        return path;
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
