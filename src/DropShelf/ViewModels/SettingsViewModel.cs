using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using System.Windows.Media;
using DropShelf.Infrastructure;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.Services.Gestures;

namespace DropShelf.ViewModels;

/// <summary>快速操作页中的一张操作卡片，可以拖到某个位置上。自定义操作可以编辑和删除。</summary>
public class ActionCardViewModel(QuickActionInfo action, SettingsViewModel owner) : ObservableObject
{
    private bool _isDragSource;

    public QuickActionInfo Action { get; } = action;
    public string Title => Action.Title;
    public string Description => Action.Description;
    public string Glyph => Action.Glyph;
    public bool IsCustom => Action.Custom is not null;
    public ICommand EditCommand { get; } = new RelayCommand(_ => owner.EditCustomAction(action.Custom!));
    public ICommand DeleteCommand { get; } = new RelayCommand(_ => owner.DeleteCustomAction(action.Custom!));

    /// <summary>正在被拖动，卡片变淡。</summary>
    public bool IsDragSource
    {
        get => _isDragSource;
        set => SetField(ref _isDragSource, value);
    }
}

/// <summary>置物架底部的一个位置及其当前的快速操作。</summary>
public sealed class ActionSlotViewModel(int index, string position, QuickActionInfo action, SettingsViewModel owner)
    : ActionCardViewModel(action, owner)
{
    private string? _dropHint;

    public int Index { get; } = index;

    /// <summary>在置物架上的位置，如“左”“右下”。</summary>
    public string Position { get; } = position;

    /// <summary>拖动的操作悬停在这里时显示“替换”或“交换”，否则为空。</summary>
    public string? DropHint
    {
        get => _dropHint;
        set
        {
            if (SetField(ref _dropHint, value)) OnPropertyChanged(nameof(IsDropTarget));
        }
    }

    public bool IsDropTarget => _dropHint is not null;
}

/// <summary>历史卡片上的一张缩略图；读取不到时显示文件或文件夹图标。</summary>
public sealed class HistoryThumbnailViewModel : ObservableObject
{
    private ImageSource? _image;

    public HistoryThumbnailViewModel(string path)
    {
        Glyph = Directory.Exists(path) ? "" : "";
        _ = LoadAsync(path);
    }

    public string Glyph { get; }

    public ImageSource? Image
    {
        get => _image;
        private set
        {
            if (SetField(ref _image, value)) OnPropertyChanged(nameof(HasImage));
        }
    }

    public bool HasImage => _image is not null;

    private async Task LoadAsync(string path) => Image = await ShellThumbnailService.LoadAsync(path, 96);
}

/// <summary>概览页中的一个最近关闭的置物架，用缩略图堆叠表示，与置物架本身的样子一致。</summary>
public sealed class HistoryEntryViewModel
{
    public HistoryEntryViewModel(ShelfHistoryEntry entry, AppController controller)
    {
        Entry = entry;
        Title = Strings.HistoryEntryTitle(entry);
        Detail = $"{Strings.ItemCount(entry.Items.Count)} · {Strings.HistoryEntryTime(entry.ClosedAt)}";
        // 最后放入的项目在最上面。
        var top = entry.Items.TakeLast(3).Reverse().Select(item => new HistoryThumbnailViewModel(item.Path)).ToArray();
        Stack0 = top.ElementAtOrDefault(0);
        Stack1 = top.ElementAtOrDefault(1);
        Stack2 = top.ElementAtOrDefault(2);
        RestoreCommand = new RelayCommand(_ => controller.RestoreNearMainWindow(entry));
        RemoveCommand = new RelayCommand(_ => controller.History.Remove(entry));
    }

    public ShelfHistoryEntry Entry { get; }
    public string Title { get; }
    public string Detail { get; }
    public HistoryThumbnailViewModel? Stack0 { get; }
    public HistoryThumbnailViewModel? Stack1 { get; }
    public HistoryThumbnailViewModel? Stack2 { get; }
    public ICommand RestoreCommand { get; }
    public ICommand RemoveCommand { get; }
}

/// <summary>强调色选项；Value 为空字符串表示跟随 Windows，色块使用主题资源中的系统强调色。</summary>
public sealed record AccentOption(string Value, SolidColorBrush? Brush)
{
    /// <summary>选项列表保持不变：替换列表会让选择框把选中值写回为空，误把强调色改回跟随 Windows。</summary>
    public static IReadOnlyList<AccentOption> All { get; } =
    [
        new(string.Empty, null),
        .. ThemeService.AccentPresets.Select(hex =>
        {
            var brush = new SolidColorBrush(ThemeService.ParseAccent(hex)!.Value);
            brush.Freeze();
            return new AccentOption(hex, brush);
        })
    ];
}

public sealed class SettingsViewModel : ObservableObject
{
    private readonly AppController _controller;
    private bool _startupFailed;
    private CustomActionEditorViewModel? _editor;
    private string? _actionsKey;
    private IReadOnlyList<ActionSlotViewModel> _visibleSlots = [];
    private IReadOnlyList<ActionCardViewModel> _moreActions = [];
    private ObservableCollection<HistoryEntryViewModel> _historyEntries = [];
    private readonly System.Windows.Threading.DispatcherTimer _sensitivitySave = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private int? _pendingSensitivity;

    public SettingsViewModel(AppController controller)
    {
        _controller = controller;
        controller.SettingsChanged += OnSettingsChanged;
        _sensitivitySave.Tick += (_, _) => SaveSensitivity();
        controller.History.Changed += OnHistoryChanged;
        LanguageService.LanguageChanged += OnLanguageChanged;

        OpenShelfCommand = new RelayCommand(_ => controller.OpenShelfNearMainWindow());
        ShowTutorialCommand = new RelayCommand(_ => controller.ShowOnboarding());
        DismissGettingStartedCommand = new RelayCommand(_ => Update(settings => settings.ShowGettingStarted = false));
        ExitCommand = new RelayCommand(_ => controller.Exit());
        UninstallCommand = new RelayCommand(_ => Uninstall(), _ => CanUninstall);
        OpenLogFolderCommand = new RelayCommand(_ => OpenFolder(LogService.LogDirectory));
        OpenProgramFolderCommand = new RelayCommand(_ => OpenFolder(ProgramDirectory));
        OpenSettingsFolderCommand = new RelayCommand(_ => OpenFolder(Path.GetDirectoryName(controller.SettingsPath) ?? string.Empty));
        AddCustomActionCommand = new RelayCommand(_ => BeginNewCustomAction());
        ClearHistoryCommand = new RelayCommand(_ => controller.History.Clear(), _ => HasHistory);
        RemoveBlockedAppCommand = new RelayCommand(app => Update(settings => settings.BlockedApps!.Remove((string)app!)));
        RefreshActions();
        RefreshHistory();
    }

    private AppSettings Settings => _controller.Settings;

    public ICommand OpenShelfCommand { get; }
    public ICommand ShowTutorialCommand { get; }
    public ICommand DismissGettingStartedCommand { get; }
    public ICommand ExitCommand { get; }
    public ICommand UninstallCommand { get; }
    public ICommand OpenLogFolderCommand { get; }
    public ICommand OpenProgramFolderCommand { get; }
    public ICommand OpenSettingsFolderCommand { get; }
    public ICommand AddCustomActionCommand { get; }
    public ICommand ClearHistoryCommand { get; }
    public ICommand RemoveBlockedAppCommand { get; }

    public string VersionNumber => typeof(SettingsViewModel).Assembly.GetName().Version?.ToString(3) ?? string.Empty;
    public string Version => Strings.Version(VersionNumber);
    public string SettingsPath => _controller.SettingsPath;
    public string LogDirectory => LogService.LogDirectory;
    public string ProgramDirectory => AppContext.BaseDirectory;

    /// <summary>安装版的卸载程序与主程序在同一文件夹；便携版和开发版没有。</summary>
    private static string UninstallerPath => Path.Combine(AppContext.BaseDirectory, "unins000.exe");

    public bool CanUninstall => File.Exists(UninstallerPath);
    public string UninstallDescription => CanUninstall ? Strings.UninstallDescription : Strings.UninstallPortable;

    // ───────────── 运行状态与概览 ─────────────

    public bool IsHealthy => _controller.IsGestureRunning;

    public string StatusTitle => IsHealthy ? Strings.StatusRunning : Strings.StatusGestureFailed;

    public string StatusDetail => IsHealthy
        ? Strings.StatusRunningDetail(Settings.AutoOpenTrigger, Settings.HotkeyGesture is { IsEmpty: false } hotkey ? hotkey.ToString() : null)
        : Strings.StatusGestureFailedDetail;

    public bool ShowGettingStarted => Settings.ShowGettingStarted;

    // 宽窗口左侧的状态栏：手势识别出错时显示原因，正常时列出当前可用的呼出方式。
    public string PanelTitle => IsHealthy ? Strings.PanelReady : Strings.StatusGestureFailed;
    public string PanelBody => IsHealthy ? Strings.PanelTagline : Strings.StatusGestureFailedDetail;
    public bool ShowShakeWay => IsHealthy && UsesShake;
    public bool ShowEdgeWay => IsHealthy && UsesEdge;

    public IReadOnlyList<string> HotkeyKeys => !Settings.HotkeyGesture.IsEmpty && _controller.IsHotkeyRegistered
        ? Settings.HotkeyGesture.ToString().Split('+')
        : [];

    public bool ShowHotkeyWay => HotkeyKeys.Count > 0;

    // 概览页四张设置卡片上显示的当前设置。
    public string GeneralSummary => Strings.GeneralSummary(Settings.Language, Settings.StartWithWindows, Settings.HotkeyGesture);
    public string ShelfSummary => Strings.ShelfSummary(Settings.AutoOpenTrigger, Settings.DragOutMode);
    public string ActionsSummary => Strings.ActionsSummary(QuickActions.Visible(Settings).Select(info => info.Label));
    public string AppearanceSummary => Strings.AppearanceSummary(Settings.Theme, Settings.AccentColor is not null);

    /// <summary>概览页底部牌堆中的最近关闭的置物架，最新的在最左边。</summary>
    public IReadOnlyList<HistoryEntryViewModel> HistoryEntries => _historyEntries;

    public bool HasHistory => _controller.History.Entries.Count > 0;

    public string HistoryDescription => HasHistory
        ? Strings.HistoryCount(_controller.History.Entries.Count)
        : Strings.HistoryEmpty;

    // ───────────── 常规 ─────────────

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

    public HotkeyGesture Hotkey
    {
        get => Settings.HotkeyGesture;
        set => Update(settings => settings.Hotkey = value.ToString());
    }

    public string HotkeyDescription => Settings.HotkeyGesture.IsEmpty
        ? Strings.HotkeyNotSetDescription
        : _controller.IsHotkeyRegistered
            ? Strings.HotkeyRegistered
            : Strings.HotkeyTaken;

    public bool HotkeyHasError => !Settings.HotkeyGesture.IsEmpty && !_controller.IsHotkeyRegistered;

    // ───────────── 手势与置物架 ─────────────

    public AutoOpenTrigger Trigger
    {
        get => Settings.AutoOpenTrigger;
        set => Update(settings => settings.AutoOpenTrigger = value);
    }

    public bool UsesShake => AutoOpenTriggers.UsesShake(Settings.AutoOpenTrigger);
    public bool UsesEdge => AutoOpenTriggers.UsesEdge(Settings.AutoOpenTrigger);

    /// <summary>拖动滑块时说明文字立即更新；停下 0.25 秒后才保存，不会每移动一格就写一次设置文件。</summary>
    public double Sensitivity
    {
        get => _pendingSensitivity ?? Settings.ShakeSensitivity;
        set
        {
            var level = ShakeSensitivities.Clamp((int)Math.Round(value));
            if (level == (int)Sensitivity) return;
            _pendingSensitivity = level;
            OnPropertyChanged(nameof(SensitivityDescription));
            _sensitivitySave.Stop();
            _sensitivitySave.Start();
        }
    }

    public string SensitivityDescription
    {
        get
        {
            var (reversals, distance) = MouseGestureDetector.Describe((int)Sensitivity);
            return Strings.SensitivityDetail(reversals, distance);
        }
    }

    public EdgeDwellTime EdgeDwell
    {
        get => Settings.EdgeDwell;
        set => Update(settings => settings.EdgeDwell = value);
    }

    /// <summary>前台时不弹出置物架的程序文件名。</summary>
    public IReadOnlyList<string> BlockedApps => Settings.BlockedApps!;

    public void AddBlockedApp(string name) => Update(settings => settings.BlockedApps!.Add(name));

    public QuickShelfSize ShelfSize
    {
        get => Settings.QuickShelfSize;
        set => Update(settings => settings.QuickShelfSize = value);
    }

    public TransferMode DragOutMode
    {
        get => Settings.DragOutMode;
        set => Update(settings => settings.DragOutMode = value);
    }

    public bool CloseWhenEmpty
    {
        get => Settings.CloseWhenEmpty;
        set => Update(settings => settings.CloseWhenEmpty = value);
    }

    // ───────────── 外观 ─────────────

    public AppTheme Theme
    {
        get => Settings.Theme;
        set => Update(settings => settings.Theme = value);
    }

    public IReadOnlyList<AccentOption> AccentOptions => AccentOption.All;

    /// <summary>选中的强调色；空字符串表示跟随 Windows。</summary>
    public string Accent
    {
        get => Settings.AccentColor ?? string.Empty;
        set => Update(settings => settings.AccentColor = string.IsNullOrEmpty(value) ? null : value);
    }

    // ───────────── 快速操作 ─────────────

    public QuickActionLayout ActionLayout
    {
        get => Settings.QuickActionLayout;
        set => Update(settings => settings.QuickActionLayout = value);
    }

    public bool HasActionSlots => Settings.QuickActionLayout != QuickActionLayout.Off;

    /// <summary>置物架上显示的位置，按从左到右、从上到下的顺序。</summary>
    public IReadOnlyList<ActionSlotViewModel> VisibleSlots
    {
        get => _visibleSlots;
        private set => SetField(ref _visibleSlots, value);
    }

    /// <summary>没有显示在置物架上的操作，可以拖到上方的位置上。</summary>
    public IReadOnlyList<ActionCardViewModel> MoreActions
    {
        get => _moreActions;
        private set => SetField(ref _moreActions, value);
    }

    public bool CanAddCustomAction => (Settings.CustomActions?.Count ?? 0) < QuickActions.MaximumCustomActions;

    /// <summary>
    /// 把操作放到指定位置。已在其他位置的操作会与原位置的操作互换，避免同一个操作出现两次。
    /// </summary>
    public void AssignSlot(int index, string actionId)
    {
        if (index is < 0 or >= QuickActions.SlotCount) return;
        Update(settings =>
        {
            var slots = QuickActions.NormalizeSlots(settings.QuickActionSlots, settings.CustomActions);
            var existing = slots.IndexOf(actionId);
            if (existing == index) return;
            if (existing >= 0) slots[existing] = slots[index];
            slots[index] = actionId;
            settings.QuickActionSlots = slots;
        });
    }

    public CustomActionEditorViewModel? Editor
    {
        get => _editor;
        private set
        {
            if (SetField(ref _editor, value)) OnPropertyChanged(nameof(IsEditorOpen));
        }
    }

    public bool IsEditorOpen => _editor is not null;

    public void BeginNewCustomAction()
    {
        if (!CanAddCustomAction) return;
        Editor = new CustomActionEditorViewModel(new CustomQuickAction { Kind = CustomActionKind.Folder }, isNew: true,
            SaveCustomAction, () => Editor = null);
    }

    public void EditCustomAction(CustomQuickAction action) =>
        Editor = new CustomActionEditorViewModel(action.Clone(), isNew: false, SaveCustomAction, () => Editor = null);

    /// <summary>删除自定义操作；它所在的位置原地换成一个未使用的内置操作，其他按钮不移动。</summary>
    public void DeleteCustomAction(CustomQuickAction action) => Update(settings =>
    {
        var slots = QuickActions.NormalizeSlots(settings.QuickActionSlots, settings.CustomActions);
        var index = slots.IndexOf(action.Id);
        if (index >= 0)
            slots[index] = QuickActions.Defaults.Select(type => type.ToString()).Concat(QuickActions.All.Select(info => info.Id))
                .First(id => !slots.Contains(id));
        settings.QuickActionSlots = slots;
        settings.CustomActions = (settings.CustomActions ?? []).Where(custom => custom.Id != action.Id).ToList();
    });

    private void SaveCustomAction(CustomActionEditorViewModel editor)
    {
        var action = editor.BuildAction();
        Update(settings =>
        {
            var actions = settings.CustomActions ?? [];
            var index = actions.FindIndex(custom => custom.Id == action.Id);
            if (index >= 0) actions[index] = action;
            else actions.Add(action);
            settings.CustomActions = actions;
        });
        Editor = null;
    }

    private void Update(Action<AppSettings> change) => _controller.UpdateSettings(change);

    private static string SlotPosition(int index, QuickActionLayout layout) => layout == QuickActionLayout.Two
        ? index == 0 ? Strings.SlotLeft : Strings.SlotRight
        : index switch
        {
            0 => Strings.SlotFarLeft,
            1 => Strings.SlotCenterLeft,
            2 => Strings.SlotCenterRight,
            _ => Strings.SlotFarRight
        };

    /// <summary>操作的布局、位置和自定义操作内容，用来判断设置变化后是否需要重建操作卡片。</summary>
    private string ActionsKey() =>
        $"{Settings.QuickActionLayout}|{string.Join(',', Settings.QuickActionSlots ?? [])}|{System.Text.Json.JsonSerializer.Serialize(Settings.CustomActions)}";

    private void RefreshActions()
    {
        _actionsKey = ActionsKey();
        var layout = Settings.QuickActionLayout;
        var visible = (Settings.QuickActionSlots ?? []).Take((int)layout).ToArray();
        VisibleSlots = visible
            .Select((id, index) => QuickActions.Find(id, Settings.CustomActions) is { } info
                ? new ActionSlotViewModel(index, SlotPosition(index, layout), info, this)
                : null)
            .OfType<ActionSlotViewModel>()
            .ToArray();
        MoreActions = QuickActions.Available(Settings.CustomActions)
            .Where(info => !visible.Contains(info.Id))
            .Select(info => new ActionCardViewModel(info, this))
            .ToArray();
        OnPropertyChanged(nameof(HasActionSlots));
        OnPropertyChanged(nameof(CanAddCustomAction));
        _editor?.RefreshText();
    }

    /// <summary>
    /// reuse 时沿用已有的卡片，只在原列表上删除、移动和插入：调整顺序或删除一张后，其余卡片不必重新生成，缩略图也不必重新读取。
    /// 否则（如切换语言）整组换新。
    /// </summary>
    private void RefreshHistory(bool reuse = false)
    {
        if (reuse)
        {
            var existing = _historyEntries.ToDictionary(entry => entry.Entry);
            var wanted = _controller.History.Entries.Select(entry => existing.GetValueOrDefault(entry) ?? new HistoryEntryViewModel(entry, _controller)).ToList();
            for (var index = _historyEntries.Count - 1; index >= 0; index--)
                if (!wanted.Contains(_historyEntries[index])) _historyEntries.RemoveAt(index);
            for (var index = 0; index < wanted.Count; index++)
            {
                var current = _historyEntries.IndexOf(wanted[index]);
                if (current == index) continue;
                if (current >= 0) _historyEntries.Move(current, index);
                else _historyEntries.Insert(index, wanted[index]);
            }
        }
        else
        {
            _historyEntries = new(_controller.History.Entries.Select(entry => new HistoryEntryViewModel(entry, _controller)));
            OnPropertyChanged(nameof(HistoryEntries));
        }
        OnPropertyChanged(nameof(HasHistory));
        OnPropertyChanged(nameof(HistoryDescription));
        CommandManager.InvalidateRequerySuggested();
    }

    /// <summary>启动卸载程序后退出，卸载时文件不会被占用。</summary>
    private void Uninstall()
    {
        try
        {
            Process.Start(new ProcessStartInfo(UninstallerPath) { UseShellExecute = true, WorkingDirectory = ProgramDirectory });
            _controller.Exit();
        }
        catch (Exception exception)
        {
            LogService.Error("启动卸载程序失败", exception);
            System.Windows.MessageBox.Show(Strings.UninstallFailed, "DropShelf", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Warning);
        }
    }

    /// <summary>窗口关闭后停止接收通知，让界面和缩略图可以被回收。</summary>
    public void Detach()
    {
        SaveSensitivity();
        _controller.SettingsChanged -= OnSettingsChanged;
        _controller.History.Changed -= OnHistoryChanged;
        LanguageService.LanguageChanged -= OnLanguageChanged;
    }

    private void OnSettingsChanged(object? sender, EventArgs e) => RefreshAll();

    private void SaveSensitivity()
    {
        _sensitivitySave.Stop();
        if (_pendingSensitivity is not { } level) return;
        _pendingSensitivity = null;
        Update(settings => settings.ShakeSensitivity = level);
    }

    private void OnHistoryChanged(object? sender, EventArgs e) => RefreshHistory(reuse: true);

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        RefreshAll();
        RefreshActions();
        RefreshHistory();
    }

    private void RefreshAll()
    {
        foreach (var name in new[]
                 {
                     nameof(StartWithWindows), nameof(Trigger), nameof(UsesShake), nameof(UsesEdge), nameof(Sensitivity), nameof(SensitivityDescription),
                     nameof(EdgeDwell), nameof(BlockedApps), nameof(ShelfSize), nameof(DragOutMode), nameof(CloseWhenEmpty),
                     nameof(Theme), nameof(Accent), nameof(Language), nameof(Hotkey), nameof(HotkeyDescription), nameof(HotkeyHasError),
                     nameof(StatusTitle), nameof(StatusDetail), nameof(IsHealthy), nameof(PanelTitle), nameof(PanelBody),
                     nameof(ShowShakeWay), nameof(ShowEdgeWay), nameof(HotkeyKeys), nameof(ShowHotkeyWay), nameof(ShowGettingStarted), nameof(ActionLayout),
                     nameof(StartupDescription), nameof(Version), nameof(HistoryDescription), nameof(GeneralSummary),
                     nameof(ShelfSummary), nameof(ActionsSummary), nameof(AppearanceSummary), nameof(UninstallDescription)
                 })
            OnPropertyChanged(name);
        if (ActionsKey() != _actionsKey) RefreshActions();
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

/// <summary>添加或编辑自定义操作的对话框。</summary>
public sealed class CustomActionEditorViewModel : ObservableObject
{
    private readonly CustomQuickAction _original;
    private readonly Action<CustomActionEditorViewModel> _save;
    private CustomActionKind _kind;
    private string _name;
    private string _folder;
    private TransferMode _folderMode;
    private string _sizeText;
    private string _programPath;
    private string _arguments;
    private bool _showErrors;

    public CustomActionEditorViewModel(CustomQuickAction action, bool isNew, Action<CustomActionEditorViewModel> save, Action cancel)
    {
        _original = action;
        _save = save;
        IsNew = isNew;
        _kind = action.Kind;
        _name = action.Name ?? string.Empty;
        _folder = action.Folder ?? string.Empty;
        _folderMode = action.FolderMode;
        _sizeText = action.ImageSizeKb.ToString(System.Globalization.CultureInfo.InvariantCulture);
        _programPath = action.ProgramPath ?? string.Empty;
        _arguments = action.Arguments ?? string.Empty;
        SaveCommand = new RelayCommand(_ => TrySave());
        CancelCommand = new RelayCommand(_ => cancel());
    }

    public bool IsNew { get; }
    public ICommand SaveCommand { get; }
    public ICommand CancelCommand { get; }

    public string Title => IsNew ? Strings.AddCustomAction : Strings.EditCustomAction;

    public CustomActionKind Kind
    {
        get => _kind;
        set
        {
            if (!SetField(ref _kind, value)) return;
            OnPropertyChanged(nameof(IsFolder));
            OnPropertyChanged(nameof(IsCompressImage));
            OnPropertyChanged(nameof(IsOpenWith));
            OnFieldChanged();
        }
    }

    public bool IsFolder => Kind == CustomActionKind.Folder;
    public bool IsCompressImage => Kind == CustomActionKind.CompressImage;
    public bool IsOpenWith => Kind == CustomActionKind.OpenWith;

    public string Name
    {
        get => _name;
        set => SetField(ref _name, value);
    }

    public string Folder
    {
        get => _folder;
        set
        {
            if (SetField(ref _folder, value)) OnFieldChanged();
        }
    }

    public TransferMode FolderMode
    {
        get => _folderMode;
        set
        {
            if (SetField(ref _folderMode, value)) OnFieldChanged();
        }
    }

    public string SizeText
    {
        get => _sizeText;
        set
        {
            if (SetField(ref _sizeText, value)) OnFieldChanged();
        }
    }

    public string ProgramPath
    {
        get => _programPath;
        set
        {
            if (SetField(ref _programPath, value)) OnFieldChanged();
        }
    }

    public string Arguments
    {
        get => _arguments;
        set => SetField(ref _arguments, value);
    }

    /// <summary>名称留空时使用的名称，显示为输入框的提示文字。</summary>
    public string NamePlaceholder => Strings.CustomActionAutoName(BuildAction(includeName: false));

    /// <summary>第一次点保存之后才显示错误，避免刚打开对话框就是一片红字。</summary>
    public string? Error => _showErrors ? Validate() : null;

    public bool HasError => Error is not null;

    public void RefreshText()
    {
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(NamePlaceholder));
        OnPropertyChanged(nameof(Error));
        OnPropertyChanged(nameof(HasError));
    }

    public CustomQuickAction BuildAction(bool includeName = true)
    {
        var action = _original.Clone();
        action.Kind = Kind;
        action.Name = includeName && !string.IsNullOrWhiteSpace(Name) ? Name.Trim() : null;
        action.Folder = Kind == CustomActionKind.Folder ? Folder.Trim() : null;
        action.FolderMode = FolderMode;
        action.ImageSizeKb = TryParseSize(out var size) ? size : CustomQuickAction.DefaultImageSizeKb;
        action.ProgramPath = Kind == CustomActionKind.OpenWith ? ProgramPath.Trim().Trim('"') : null;
        action.Arguments = Kind == CustomActionKind.OpenWith && !string.IsNullOrWhiteSpace(Arguments) ? Arguments.Trim() : null;
        return action;
    }

    public string? Validate() => Kind switch
    {
        CustomActionKind.Folder when string.IsNullOrWhiteSpace(Folder) => Strings.ChooseFolderFirst,
        CustomActionKind.Folder when !Directory.Exists(Folder.Trim()) => Strings.FolderNotFoundShort,
        CustomActionKind.CompressImage when !TryParseSize(out _) => Strings.ImageSizeInvalid(CustomQuickAction.MinimumImageSizeKb,
            CustomQuickAction.MaximumImageSizeKb),
        CustomActionKind.OpenWith when string.IsNullOrWhiteSpace(ProgramPath) => Strings.ChooseProgramFirst,
        CustomActionKind.OpenWith when !File.Exists(ProgramPath.Trim().Trim('"')) => Strings.ProgramNotFoundShort,
        _ => null
    };

    public bool TrySave()
    {
        _showErrors = true;
        OnPropertyChanged(nameof(Error));
        OnPropertyChanged(nameof(HasError));
        if (Validate() is not null) return false;
        _save(this);
        return true;
    }

    private bool TryParseSize(out int size) =>
        int.TryParse(SizeText.Trim(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out size) &&
        size is >= CustomQuickAction.MinimumImageSizeKb and <= CustomQuickAction.MaximumImageSizeKb;

    private void OnFieldChanged()
    {
        OnPropertyChanged(nameof(NamePlaceholder));
        OnPropertyChanged(nameof(Error));
        OnPropertyChanged(nameof(HasError));
    }
}
