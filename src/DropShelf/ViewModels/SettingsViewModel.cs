using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

public sealed class QuickActionOption : ObservableObject
{
    private readonly SettingsViewModel _owner;

    public QuickActionOption(SettingsViewModel owner, QuickActionInfo info)
    {
        _owner = owner;
        Info = info;
    }

    public QuickActionInfo Info { get; }
    public string Label => Info.Label;
    public string Glyph => Info.Glyph;
    public string Description => Info.Description;

    public bool IsEnabled
    {
        get => _owner.IsActionEnabled(Info.Type);
        set => _owner.SetActionEnabled(Info.Type, value);
    }

    /// <summary>已选满四个时，其余操作暂时不能再打开。</summary>
    public bool CanToggle => IsEnabled || _owner.EnabledActionCount < QuickActions.MaximumVisible;

    public void Refresh()
    {
        OnPropertyChanged(nameof(IsEnabled));
        OnPropertyChanged(nameof(CanToggle));
    }
}

public sealed class SettingsViewModel : ObservableObject
{
    private readonly AppController _controller;
    private string? _startupError;

    public SettingsViewModel(AppController controller)
    {
        _controller = controller;
        ActionOptions = QuickActions.All.Select(info => new QuickActionOption(this, info)).ToArray();
        controller.SettingsChanged += (_, _) => RefreshAll();
        ThemeService.ThemeChanged += (_, _) => OnPropertyChanged(nameof(AccentDescription));

        OpenShelfCommand = new RelayCommand(_ => controller.OpenShelfNearMainWindow());
        ShowTutorialCommand = new RelayCommand(_ => controller.ShowOnboarding());
        ExitCommand = new RelayCommand(_ => controller.Exit());
        OpenLogFolderCommand = new RelayCommand(_ => OpenFolder(LogService.LogDirectory));
        OpenSettingsFolderCommand = new RelayCommand(_ => OpenFolder(Path.GetDirectoryName(controller.SettingsPath) ?? string.Empty));
    }

    private AppSettings Settings => _controller.Settings;

    public ICommand OpenShelfCommand { get; }
    public ICommand ShowTutorialCommand { get; }
    public ICommand ExitCommand { get; }
    public ICommand OpenLogFolderCommand { get; }
    public ICommand OpenSettingsFolderCommand { get; }

    public IReadOnlyList<QuickActionOption> ActionOptions { get; }

    public string Version => $"版本 {typeof(SettingsViewModel).Assembly.GetName().Version?.ToString(3)}";
    public string SettingsPath => _controller.SettingsPath;
    public string LogDirectory => LogService.LogDirectory;

    public bool IsHealthy => GestureEnabled && _controller.IsGestureRunning;

    public string StatusTitle => !GestureEnabled
        ? "晃动呼出已暂停"
        : _controller.IsGestureRunning ? "正在后台运行" : "手势识别未能启动";

    public string StatusDetail
    {
        get
        {
            var hotkey = Settings.HotkeyGesture;
            var hotkeyText = hotkey.IsEmpty ? string.Empty : $"，或按 {hotkey} 打开置物架";
            return !GestureEnabled
                ? $"可在“手势与置物架”中重新开启{hotkeyText}"
                : _controller.IsGestureRunning
                    ? $"拖住文件并快速晃动鼠标{hotkeyText}"
                    : "请重新启动 DropShelf；置物架和快捷键仍可使用";
        }
    }

    public bool StartWithWindows
    {
        get => Settings.StartWithWindows;
        set
        {
            if (value == Settings.StartWithWindows) return;
            StartupError = _controller.SetStartWithWindows(value) ? null : "Windows 没有允许修改开机启动，设置保持不变";
            OnPropertyChanged();
        }
    }

    public string? StartupError
    {
        get => _startupError;
        private set
        {
            if (SetField(ref _startupError, value)) OnPropertyChanged(nameof(StartupDescription));
        }
    }

    public string StartupDescription => StartupError ?? "登录 Windows 后在后台运行，不弹出窗口";

    public bool GestureEnabled
    {
        get => Settings.GestureEnabled;
        set => Update(settings => settings.GestureEnabled = value);
    }

    public GestureSensitivity Sensitivity
    {
        get => Settings.MouseGestureSensitivity;
        set => Update(settings => settings.MouseGestureSensitivity = value);
    }

    public QuickShelfSize ShelfSize
    {
        get => Settings.QuickShelfSize;
        set => Update(settings => settings.QuickShelfSize = value);
    }

    public bool RemoveAfterDragOut
    {
        get => Settings.RemoveAfterDragOut;
        set => Update(settings => settings.RemoveAfterDragOut = value);
    }

    public bool CloseWhenEmpty
    {
        get => Settings.CloseWhenEmpty;
        set => Update(settings => settings.CloseWhenEmpty = value);
    }

    public AppTheme Theme
    {
        get => Settings.Theme;
        set => Update(settings => settings.Theme = value);
    }

    public HotkeyGesture Hotkey
    {
        get => Settings.HotkeyGesture;
        set => Update(settings => settings.Hotkey = value.ToString());
    }

    public string HotkeyDescription => Settings.HotkeyGesture.IsEmpty
        ? "未设置快捷键"
        : _controller.IsHotkeyRegistered
            ? "在光标处打开空置物架，可直接按 Ctrl+V 粘贴"
            : "这个快捷键已被其他程序占用，请换一个";

    public bool HotkeyHasError => !Settings.HotkeyGesture.IsEmpty && !_controller.IsHotkeyRegistered;

    public string AccentDescription => "按钮、开关和选中状态使用 Windows 强调色";

    public int EnabledActionCount => Settings.QuickActions?.Count ?? 0;

    public string ActionsSummary => EnabledActionCount == 0
        ? "已关闭快速操作，置物架底部不显示按钮"
        : $"已选择 {EnabledActionCount} 个，最多 {QuickActions.MaximumVisible} 个";

    public bool IsActionEnabled(QuickActionType type) => Settings.QuickActions?.Contains(type) == true;

    public void SetActionEnabled(QuickActionType type, bool enabled)
    {
        if (enabled == IsActionEnabled(type)) return;
        if (enabled && EnabledActionCount >= QuickActions.MaximumVisible) return;
        Update(settings =>
        {
            var actions = settings.QuickActions ?? [];
            if (enabled) actions.Add(type);
            else actions.Remove(type);
            settings.QuickActions = actions;
        });
    }

    private void Update(Action<AppSettings> change) => _controller.UpdateSettings(change);

    private void RefreshAll()
    {
        foreach (var name in new[]
                 {
                     nameof(StartWithWindows), nameof(GestureEnabled), nameof(Sensitivity), nameof(ShelfSize),
                     nameof(RemoveAfterDragOut), nameof(CloseWhenEmpty), nameof(Theme), nameof(Hotkey),
                     nameof(HotkeyDescription), nameof(HotkeyHasError), nameof(StatusTitle), nameof(StatusDetail),
                     nameof(IsHealthy), nameof(EnabledActionCount), nameof(ActionsSummary)
                 })
            OnPropertyChanged(name);
        foreach (var option in ActionOptions) option.Refresh();
    }

    private static void OpenFolder(string path)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
        }
        catch (Exception exception)
        {
            LogService.Error("打开文件夹失败", exception);
        }
    }
}
