using System.Threading;
using System.Windows;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Services;

namespace DropShelf;

public partial class App : System.Windows.Application
{
    private Mutex? _singleInstanceMutex;
    private bool _ownsSingleInstanceMutex;
    private EventWaitHandle? _showWindowEvent;
    private CancellationTokenSource? _listenerCancellation;
    private Task? _listenerTask;
    private AppController? _controller;

    private const long ErrorQuietMilliseconds = 10_000;
    private static bool _showingError;
    private static string? _lastError;
    private static long _lastErrorTick;

    private static string MutexName => @"Local\DropShelf.SingleInstance.v1" + AppController.InstanceSuffix;
    private static string ShowWindowEventName => @"Local\DropShelf.ShowMainWindow.v1" + AppController.InstanceSuffix;

    protected override void OnStartup(StartupEventArgs e)
    {
        _singleInstanceMutex = new Mutex(true, MutexName, out _ownsSingleInstanceMutex);
        if (!_ownsSingleInstanceMutex)
        {
            // 已在运行：“发送到”或拖到图标上的文件交给现有实例，否则唤醒主窗口。
            // 先允许现有实例切到前台，否则它的窗口可能打开在其他窗口后面，看起来像没有反应。
            NativeMethods.AllowSetForegroundWindow(-1);
            var forwarded = AppController.IsShelfRequest(e.Args) &&
                            MessageWindow.SendArguments(AppController.MessageWindowTitle, e.Args);
            if (!forwarded) SignalExistingInstance();
            Shutdown();
            return;
        }

        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnUnhandledException;
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            LogService.Error("未处理的后台任务异常", args.Exception);
            args.SetObserved();
        };
        base.OnStartup(e);

        var startInBackground = AppController.IsShelfRequest(e.Args) || e.Args.Any(argument =>
            string.Equals(argument, "--background", StringComparison.OrdinalIgnoreCase));
        _controller = new AppController(Dispatcher);
        StartShowWindowListener();
        _controller.Start(startInBackground);
        if (AppController.IsShelfRequest(e.Args)) _controller.HandleArguments(e.Args);
    }

    private void StartShowWindowListener()
    {
        try
        {
            _showWindowEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowWindowEventName);
            _listenerCancellation = new CancellationTokenSource();
            var cancellationToken = _listenerCancellation.Token;
            var eventHandle = _showWindowEvent;

            _listenerTask = Task.Run(() =>
            {
                try
                {
                    var handles = new WaitHandle[] { eventHandle, cancellationToken.WaitHandle };
                    while (!cancellationToken.IsCancellationRequested)
                    {
                        if (WaitHandle.WaitAny(handles) != 0 || cancellationToken.IsCancellationRequested) break;
                        Dispatcher.BeginInvoke(() => _controller?.ShowMainWindow());
                    }
                }
                catch (ObjectDisposedException) when (cancellationToken.IsCancellationRequested)
                {
                    // 程序退出时等待句柄可能已经释放。
                }
                catch (Exception exception)
                {
                    LogService.Error("重复启动唤醒监听意外停止", exception);
                }
            }, cancellationToken);
        }
        catch (Exception exception)
        {
            LogService.Error("重复启动唤醒功能初始化失败", exception);
        }
    }

    private static void SignalExistingInstance()
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            try
            {
                using var showEvent = EventWaitHandle.OpenExisting(ShowWindowEventName);
                showEvent.Set();
                return;
            }
            catch (WaitHandleCannotBeOpenedException)
            {
                Thread.Sleep(80);
            }
            catch (Exception exception)
            {
                LogService.Error("无法唤醒已经运行的 DropShelf", exception);
                return;
            }
        }
    }

    /// <summary>
    /// 同一个错误常在每次布局、渲染或计时器触发时重复出现。提示框打开期间仍在处理消息，
    /// 再出错时不能叠加新的提示框，否则会层层嵌套直到程序崩溃；短时间内重复的错误只记录和提示一次。
    /// </summary>
    private static void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        e.Handled = true;
        var now = Environment.TickCount64;
        var error = $"{e.Exception.GetType().FullName}: {e.Exception.Message}";
        var repeated = error == _lastError && now - _lastErrorTick < ErrorQuietMilliseconds;
        _lastError = error;
        _lastErrorTick = now;
        if (repeated) return;

        LogService.Error("未处理的界面异常", e.Exception);
        if (_showingError) return;
        _showingError = true;
        try
        {
            System.Windows.MessageBox.Show(Strings.UnexpectedError, "DropShelf", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            _showingError = false;
        }
    }

    private static void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
    {
        if (e.ExceptionObject is Exception exception)
            LogService.Error("未处理的程序异常", exception);
    }

    protected override void OnSessionEnding(SessionEndingCancelEventArgs e)
    {
        _controller?.PrepareForShutdown();
        base.OnSessionEnding(e);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _controller?.PrepareForShutdown();
        _listenerCancellation?.Cancel();
        try { _showWindowEvent?.Set(); } catch { }
        try { _listenerTask?.Wait(TimeSpan.FromMilliseconds(500)); } catch { }
        _showWindowEvent?.Dispose();
        _listenerCancellation?.Dispose();
        _listenerTask = null;
        ThemeService.Dispose();

        if (_ownsSingleInstanceMutex)
            _singleInstanceMutex?.ReleaseMutex();
        _singleInstanceMutex?.Dispose();
        base.OnExit(e);
    }
}
