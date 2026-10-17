using System.Windows;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.Services.Gestures;
using DropShelf.Shelves;
using DropShelf.Views;

namespace DropShelf;

/// <summary>应用总控：持有设置并协调手势、置物架、托盘、快捷键和各个窗口。</summary>
public sealed class AppController : IDisposable
{
    private readonly SettingsService _settingsService = new();
    private readonly StartupService _startupService = new();
    private readonly Dispatcher _dispatcher;
    private AppSettings _settings;
    private MessageWindow? _messageWindow;
    private TrayIconService? _tray;
    private HotkeyService? _hotkey;
    private GestureService? _gestures;
    private ProbeWindow? _probe;
    private MainWindow? _mainWindow;
    private OnboardingWindow? _onboarding;
    private bool _hasShownTrayHint;
    private bool _exiting;

    public AppController(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        _settings = _settingsService.Load();
        Shelves = new ShelfManager(() => _settings, ShowMainWindow, dispatcher);
        Current = this;
    }

    public static AppController? Current { get; private set; }

    /// <summary>开发调试时可通过环境变量使用独立实例，避免与已安装的版本冲突。</summary>
    public static string InstanceSuffix =>
        Environment.GetEnvironmentVariable("DROPSHELF_INSTANCE") is { Length: > 0 } suffix ? "." + suffix : string.Empty;

    public static string MessageWindowTitle => "DropShelfMessageWindow" + InstanceSuffix;

    /// <summary>命令行包含 --shelf 或直接带有文件路径（“发送到”菜单、拖到程序图标上）。</summary>
    public static bool IsShelfRequest(IReadOnlyList<string> arguments) =>
        arguments.Any(argument => string.Equals(argument, "--shelf", StringComparison.OrdinalIgnoreCase)) ||
        GetPathArguments(arguments).Count > 0;

    public static IReadOnlyList<string> GetPathArguments(IReadOnlyList<string> arguments) => arguments
        .Where(argument => !argument.StartsWith("--", StringComparison.Ordinal))
        .Where(argument => System.IO.File.Exists(argument) || System.IO.Directory.Exists(argument))
        .ToArray();

    public AppSettings Settings => _settings;
    public ShelfManager Shelves { get; }
    public bool IsGestureRunning => _gestures?.IsRunning == true;
    public bool IsHotkeyRegistered { get; private set; }
    public string SettingsPath => _settingsService.SettingsPath;

    public event EventHandler? SettingsChanged;

    public void Start(bool startInBackground)
    {
        ThemeService.Initialize(_settings.Theme);
        LogService.CleanupOldLogs();
        _ = VirtualFileDropService.CleanupStaleCacheAsync();
        RestoreStartupRegistration();

        _messageWindow = new MessageWindow(MessageWindowTitle);
        _messageWindow.ArgumentsReceived += HandleArguments;
        _tray = new TrayIconService(_messageWindow,
            () => new TrayMenuState(_settings.GestureEnabled, _settings.HotkeyGesture.ToString()),
            ShowMainWindow, () => Shelves.OpenAtCursor(activate: true),
            enabled => UpdateSettings(settings => settings.GestureEnabled = enabled), Exit);
        _hotkey = new HotkeyService(_messageWindow);
        _hotkey.Pressed += (_, _) => Shelves.OpenAtCursor(activate: true);
        IsHotkeyRegistered = _hotkey.Register(_settings.HotkeyGesture);

        _probe = new ProbeWindow();
        _probe.DragDetected += (_, _) => Shelves.OpenAt(WindowPlacement.GetCursor(), openedByGesture: true, activate: false);
        _gestures = new GestureService(_dispatcher, _settings.MouseGestureSensitivity, _settings.GestureEnabled);
        _gestures.ShakeDetected += OnShakeDetected;
        _gestures.DragReleased += OnDragReleased;
        _gestures.Start();

        Shelves.Prewarm();

        if (!startInBackground)
        {
            ShowMainWindow();
            if (!_settings.HasCompletedFirstRun) _dispatcher.BeginInvoke(ShowOnboarding, DispatcherPriority.Loaded);
        }
    }

    /// <summary>打开置物架并放入命令行中的文件。</summary>
    public void HandleArguments(IReadOnlyList<string> arguments)
    {
        if (_exiting) return;
        if (!IsShelfRequest(arguments))
        {
            ShowMainWindow();
            return;
        }
        var shelf = Shelves.OpenAtCursor(activate: true);
        var paths = GetPathArguments(arguments);
        if (paths.Count == 0) return;
        var summary = shelf.ViewModel.AddFiles(paths);
        if (summary.Duplicates > 0 || summary.Unavailable > 0) shelf.ViewModel.Notify(summary.Message, summary.Added > 0);
    }

    /// <summary>修改设置并立即生效、自动保存。</summary>
    public void UpdateSettings(Action<AppSettings> change)
    {
        var previous = _settings.Clone();
        var next = _settings.Clone();
        change(next);
        _settings = SettingsService.Normalize(next);
        Apply(previous, _settings);
        _settingsService.Save(_settings);
        SettingsChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>修改开机启动；写入注册表失败时保持原值。</summary>
    public bool SetStartWithWindows(bool enabled)
    {
        var result = _startupService.SetEnabled(enabled);
        if (!result.Succeeded)
        {
            SettingsChanged?.Invoke(this, EventArgs.Empty);
            return false;
        }
        UpdateSettings(settings => settings.StartWithWindows = enabled);
        return true;
    }

    public void SaveWindowBounds(Rect bounds)
    {
        _settings.WindowLeft = bounds.Left;
        _settings.WindowTop = bounds.Top;
        _settings.WindowWidth = bounds.Width;
        _settings.WindowHeight = bounds.Height;
        _settingsService.Save(_settings);
    }

    public void ShowMainWindow()
    {
        if (_exiting) return;
        if (!_dispatcher.CheckAccess())
        {
            _dispatcher.BeginInvoke(ShowMainWindow);
            return;
        }
        _mainWindow ??= new MainWindow(this);
        if (!_mainWindow.IsVisible) _mainWindow.Show();
        if (_mainWindow.WindowState == WindowState.Minimized) _mainWindow.WindowState = WindowState.Normal;
        _mainWindow.Activate();
    }

    public void OnMainWindowHidden()
    {
        if (_hasShownTrayHint) return;
        _hasShownTrayHint = true;
        _tray?.ShowStillRunningHint();
    }

    public void ShowOnboarding()
    {
        if (_exiting) return;
        if (_onboarding is { IsVisible: true })
        {
            _onboarding.Activate();
            return;
        }
        _onboarding = new OnboardingWindow();
        if (_mainWindow is { IsVisible: true }) _onboarding.Owner = _mainWindow;
        else _onboarding.WindowStartupLocation = WindowStartupLocation.CenterScreen;
        _onboarding.Closed += (_, _) =>
        {
            _onboarding = null;
            if (!_settings.HasCompletedFirstRun) UpdateSettings(settings => settings.HasCompletedFirstRun = true);
        };
        _onboarding.Show();
    }

    public void OpenShelfNearMainWindow()
    {
        if (_mainWindow is { IsVisible: true } window && new System.Windows.Interop.WindowInteropHelper(window).Handle is var handle &&
            NativeMethods.GetWindowRect(handle, out var rect))
        {
            Shelves.OpenAt(new POINT(rect.Right - 150, rect.Top + rect.Height / 2), openedByGesture: false, activate: true);
            return;
        }
        Shelves.OpenAtCursor(activate: true);
    }

    public void Exit()
    {
        if (!_dispatcher.CheckAccess())
        {
            _dispatcher.BeginInvoke(Exit);
            return;
        }
        PrepareForShutdown();
        System.Windows.Application.Current.Shutdown();
    }

    public void PrepareForShutdown()
    {
        if (_exiting) return;
        _exiting = true;
        _settingsService.Save(_settings);
        _gestures?.Dispose();
        _probe?.Close();
        Shelves.CloseAll();
        _hotkey?.Dispose();
        _tray?.Dispose();
        _onboarding?.Close();
        _mainWindow?.CloseForShutdown();
        _messageWindow?.Dispose();
    }

    public void Dispose() => PrepareForShutdown();

    private void Apply(AppSettings previous, AppSettings current)
    {
        if (previous.Theme != current.Theme) ThemeService.SetMode(current.Theme);
        if (_gestures is not null)
        {
            _gestures.Enabled = current.GestureEnabled;
            _gestures.SetSensitivity(current.MouseGestureSensitivity);
        }
        if (previous.Hotkey != current.Hotkey && _hotkey is not null)
            IsHotkeyRegistered = _hotkey.Register(current.HotkeyGesture);
        if (previous.QuickShelfSize != current.QuickShelfSize ||
            !previous.QuickActions!.SequenceEqual(current.QuickActions!))
            Shelves.ApplySettings(current);
    }

    private void OnShakeDetected(object? sender, GestureTriggeredEventArgs e)
    {
        if (_exiting || !_settings.GestureEnabled) return;
        LogService.Trace($"识别到晃动 ({e.X}, {e.Y})，显示探针窗口");
        _probe?.ShowAt(new POINT(e.X, e.Y));
    }

    private void OnDragReleased(object? sender, EventArgs e)
    {
        LogService.Trace("晃动后松开鼠标");
        _probe?.HideProbe();
        Shelves.OnDragReleased();
    }

    private void RestoreStartupRegistration()
    {
        if (!_settings.StartWithWindows) return;
        var result = _startupService.SetEnabled(true);
        if (result.Succeeded) return;
        _settings.StartWithWindows = false;
        _settingsService.Save(_settings);
    }
}
