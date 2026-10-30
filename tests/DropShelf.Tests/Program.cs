using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views;
using WpfApplication = System.Windows.Application;

namespace DropShelf.Tests;

internal static partial class Program
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
