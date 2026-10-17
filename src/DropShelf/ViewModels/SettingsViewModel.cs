using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Localization;
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
        OnPropertyChanged(nameof(Label));
        OnPropertyChanged(nameof(Description));
        OnPropertyChanged(nameof(IsEnabled));
        OnPropertyChanged(nameof(CanToggle));
    }
}

public sealed class SettingsViewModel : ObservableObject
{
    private readonly AppController _controller;
    private bool _startupFailed;

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

    public string Version => Strings.Version(typeof(SettingsViewModel).Assembly.GetName().Version?.ToString(3));
    public string SettingsPath => _controller.SettingsPath;
    public string LogDirectory => LogService.LogDirectory;

    public bool IsHealthy => GestureEnabled && _controller.IsGestureRunning;

    public string StatusTitle => !GestureEnabled
        ? Strings.StatusPaused
        : _controller.IsGestureRunning ? Strings.StatusRunning : Strings.StatusGestureFailed;

    public string StatusDetail
    {
        get
        {
            var hotkey = Settings.HotkeyGesture;
            var hotkeyText = hotkey.IsEmpty ? null : hotkey.ToString();
            return !GestureEnabled
                ? Strings.StatusPausedDetail(hotkeyText)
                : _controller.IsGestureRunning
                    ? Strings.StatusRunningDetail(hotkeyText)
                    : Strings.StatusGestureFailedDetail;
        }
    }

    public AppLanguage Language
    {
        get => Settings.Language;
        set => Update(settings => settings.Language = value);
    }

    public bool StartWithWindows
    {
        get => Settings.StartWithWindows;
        set
        {
            if (value == Settings.StartWithWindows) return;
            StartupFailed = !_controller.SetStartWithWindows(value);
            OnPropertyChanged();
        }
    }

    public bool StartupFailed
    {
        get => _startupFailed;
        private set
        {
            if (SetField(ref _startupFailed, value)) OnPropertyChanged(nameof(StartupDescription));
        }
    }

    public string StartupDescription => StartupFailed ? Strings.StartupFailed : Strings.StartupDescription;

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
        ? Strings.HotkeyNotSet
        : _controller.IsHotkeyRegistered
            ? Strings.HotkeyRegistered
            : Strings.HotkeyTaken;

    public bool HotkeyHasError => !Settings.HotkeyGesture.IsEmpty && !_controller.IsHotkeyRegistered;

    public string AccentDescription => Strings.AccentDescription;

    public int EnabledActionCount => Settings.QuickActions?.Count ?? 0;

    public string ActionsSummary => EnabledActionCount == 0
        ? Strings.ActionsOff
        : Strings.ActionsSelected(EnabledActionCount, QuickActions.MaximumVisible);

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
                     nameof(RemoveAfterDragOut), nameof(CloseWhenEmpty), nameof(Theme), nameof(Language), nameof(Hotkey),
                     nameof(HotkeyDescription), nameof(HotkeyHasError), nameof(StatusTitle), nameof(StatusDetail),
                     nameof(IsHealthy), nameof(EnabledActionCount), nameof(ActionsSummary), nameof(StartupDescription),
                     nameof(AccentDescription), nameof(Version)
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
