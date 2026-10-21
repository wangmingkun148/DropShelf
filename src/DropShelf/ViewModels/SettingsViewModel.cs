using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

/// <summary>置物架底部的一个位置及其当前的快速操作。</summary>
public sealed class ActionSlotViewModel(int index, QuickActionInfo action)
{
    public int Index { get; } = index;
    public QuickActionInfo Action { get; } = action;
    public string Label => Action.Label;
    public string Description => Action.Description;
    public string Glyph => Action.Glyph;
}

/// <summary>设置页中列出的一个自定义操作。</summary>
public sealed class CustomActionItemViewModel(CustomQuickAction action, SettingsViewModel owner)
{
    public CustomQuickAction Action { get; } = action;
    public QuickActionInfo Info { get; } = QuickActionInfo.ForCustom(action);
    public string Label => Info.Label;
    public string Description => Info.Description;
    public string Glyph => Info.Glyph;
    public ICommand EditCommand { get; } = new RelayCommand(_ => owner.EditCustomAction(action));
    public ICommand DeleteCommand { get; } = new RelayCommand(_ => owner.DeleteCustomAction(action));
}

public sealed class SettingsViewModel : ObservableObject
{
    private readonly AppController _controller;
    private bool _startupFailed;
    private CustomActionEditorViewModel? _editor;
    private IReadOnlyList<ActionSlotViewModel> _visibleSlots = [];
    private IReadOnlyList<CustomActionItemViewModel> _customActions = [];

    public SettingsViewModel(AppController controller)
    {
        _controller = controller;
        controller.SettingsChanged += (_, _) => RefreshAll();
        controller.History.Changed += (_, _) => RefreshHistory();
        ThemeService.ThemeChanged += (_, _) => OnPropertyChanged(nameof(AccentDescription));
        LanguageService.LanguageChanged += (_, _) => RefreshActions();

        OpenShelfCommand = new RelayCommand(_ => controller.OpenShelfNearMainWindow());
        ShowTutorialCommand = new RelayCommand(_ => controller.ShowOnboarding());
        ExitCommand = new RelayCommand(_ => controller.Exit());
        OpenLogFolderCommand = new RelayCommand(_ => OpenFolder(LogService.LogDirectory));
        OpenSettingsFolderCommand = new RelayCommand(_ => OpenFolder(Path.GetDirectoryName(controller.SettingsPath) ?? string.Empty));
        AddCustomActionCommand = new RelayCommand(_ => BeginNewCustomAction());
        ClearHistoryCommand = new RelayCommand(_ => controller.History.Clear(), _ => HasHistory);
        RefreshActions();
    }

    private AppSettings Settings => _controller.Settings;

    public ICommand OpenShelfCommand { get; }
    public ICommand ShowTutorialCommand { get; }
    public ICommand ExitCommand { get; }
    public ICommand OpenLogFolderCommand { get; }
    public ICommand OpenSettingsFolderCommand { get; }
    public ICommand AddCustomActionCommand { get; }
    public ICommand ClearHistoryCommand { get; }

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
                    ? Strings.StatusRunningDetail(Settings.AutoOpenTrigger, hotkeyText)
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

    // ───────────── 自动弹出 ─────────────

    public bool GestureEnabled
    {
        get => Settings.GestureEnabled;
        set => Update(settings => settings.GestureEnabled = value);
    }

    public AutoOpenTrigger Trigger
    {
        get => Settings.AutoOpenTrigger;
        set => Update(settings => settings.AutoOpenTrigger = value);
    }

    public bool UsesShake => AutoOpenTriggers.UsesShake(Settings.AutoOpenTrigger);
    public bool UsesEdge => AutoOpenTriggers.UsesEdge(Settings.AutoOpenTrigger);

    public GestureSensitivity Sensitivity
    {
        get => Settings.MouseGestureSensitivity;
        set => Update(settings => settings.MouseGestureSensitivity = value);
    }

    public EdgeDwellTime EdgeDwell
    {
        get => Settings.EdgeDwell;
        set => Update(settings => settings.EdgeDwell = value);
    }

    // ───────────── 置物架 ─────────────

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

    public bool HasHistory => _controller.History.Entries.Count > 0;

    public string HistoryDescription => HasHistory
        ? Strings.HistoryCount(_controller.History.Entries.Count)
        : Strings.HistoryEmpty;

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

    // ───────────── 快速操作 ─────────────

    public QuickActionLayout ActionLayout
    {
        get => Settings.QuickActionLayout;
        set => Update(settings => settings.QuickActionLayout = value);
    }

    public bool HasActionSlots => Settings.QuickActionLayout != QuickActionLayout.Off;

    /// <summary>置物架上显示的位置，从左到右。</summary>
    public IReadOnlyList<ActionSlotViewModel> VisibleSlots
    {
        get => _visibleSlots;
        private set => SetField(ref _visibleSlots, value);
    }

    public IReadOnlyList<CustomActionItemViewModel> CustomActions
    {
        get => _customActions;
        private set => SetField(ref _customActions, value);
    }

    public bool CanAddCustomAction => (Settings.CustomActions?.Count ?? 0) < QuickActions.MaximumCustomActions;

    /// <summary>可以放到某个位置上的全部操作。</summary>
    public IReadOnlyList<QuickActionInfo> AvailableActions => QuickActions.Available(Settings.CustomActions);

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

    /// <summary>新建自定义操作；从某个位置的“更换”菜单发起时，保存后直接放到该位置。</summary>
    public void BeginNewCustomAction(int? targetSlot = null)
    {
        if (!CanAddCustomAction) return;
        Editor = new CustomActionEditorViewModel(new CustomQuickAction { Kind = CustomActionKind.Folder }, isNew: true, targetSlot,
            SaveCustomAction, () => Editor = null);
    }

    public void EditCustomAction(CustomQuickAction action) =>
        Editor = new CustomActionEditorViewModel(action.Clone(), isNew: false, null, SaveCustomAction, () => Editor = null);

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

            if (editor.TargetSlot is { } slot && slot is >= 0 and < QuickActions.SlotCount)
            {
                var slots = QuickActions.NormalizeSlots(settings.QuickActionSlots, actions);
                var existing = slots.IndexOf(action.Id);
                if (existing >= 0) slots[existing] = slots[slot];
                slots[slot] = action.Id;
                settings.QuickActionSlots = slots;
            }
        });
        Editor = null;
    }

    private void Update(Action<AppSettings> change) => _controller.UpdateSettings(change);

    private void RefreshActions()
    {
        VisibleSlots = (Settings.QuickActionSlots ?? [])
            .Take((int)Settings.QuickActionLayout)
            .Select((id, index) => QuickActions.Find(id, Settings.CustomActions) is { } info ? new ActionSlotViewModel(index, info) : null)
            .OfType<ActionSlotViewModel>()
            .ToArray();
        CustomActions = (Settings.CustomActions ?? []).Select(action => new CustomActionItemViewModel(action, this)).ToArray();
        OnPropertyChanged(nameof(HasActionSlots));
        OnPropertyChanged(nameof(CanAddCustomAction));
        OnPropertyChanged(nameof(AvailableActions));
        _editor?.RefreshText();
    }

    private void RefreshHistory()
    {
        OnPropertyChanged(nameof(HasHistory));
        OnPropertyChanged(nameof(HistoryDescription));
        CommandManager.InvalidateRequerySuggested();
    }

    private void RefreshAll()
    {
        foreach (var name in new[]
                 {
                     nameof(StartWithWindows), nameof(GestureEnabled), nameof(Trigger), nameof(UsesShake), nameof(UsesEdge),
                     nameof(Sensitivity), nameof(EdgeDwell), nameof(ShelfSize), nameof(DragOutMode), nameof(RemoveAfterDragOut),
                     nameof(CloseWhenEmpty), nameof(Theme), nameof(Language), nameof(Hotkey), nameof(HotkeyDescription),
                     nameof(HotkeyHasError), nameof(StatusTitle), nameof(StatusDetail), nameof(IsHealthy), nameof(ActionLayout),
                     nameof(StartupDescription), nameof(AccentDescription), nameof(Version), nameof(HistoryDescription)
                 })
            OnPropertyChanged(name);
        RefreshActions();
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

    public CustomActionEditorViewModel(CustomQuickAction action, bool isNew, int? targetSlot,
        Action<CustomActionEditorViewModel> save, Action cancel)
    {
        _original = action;
        _save = save;
        IsNew = isNew;
        TargetSlot = targetSlot;
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
    public int? TargetSlot { get; }
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
