using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using DropShelf.Models;

namespace DropShelf.Services;

public sealed class AppSettings
{
    public const int CurrentVersion = 3;

    public int Version { get; set; } = CurrentVersion;
    public double WindowLeft { get; set; } = double.NaN;
    public double WindowTop { get; set; } = double.NaN;
    public double WindowWidth { get; set; } = 860;
    public double WindowHeight { get; set; } = 640;

    public AutoOpenTrigger AutoOpenTrigger { get; set; } = AutoOpenTrigger.Both;
    public GestureSensitivity MouseGestureSensitivity { get; set; } = GestureSensitivity.Balanced;
    public EdgeDwellTime EdgeDwell { get; set; } = EdgeDwellTime.Medium;

    public AppTheme Theme { get; set; } = AppTheme.System;

    /// <summary>自选的强调色（#RRGGBB）；为空时跟随 Windows。</summary>
    public string? AccentColor { get; set; }

    public AppLanguage Language { get; set; } = AppLanguage.System;
    public bool StartWithWindows { get; set; }
    public QuickShelfSize QuickShelfSize { get; set; } = QuickShelfSize.Medium;

    /// <summary>从置物架拖出文件时默认移动还是复制；目标程序只接受复制时仍会复制。</summary>
    public TransferMode DragOutMode { get; set; } = TransferMode.Move;

    public QuickActionLayout QuickActionLayout { get; set; } = QuickActionLayout.Four;

    /// <summary>四个位置上的快速操作标识，按从左到右的顺序；只显示两个时使用前两个。</summary>
    public List<string>? QuickActionSlots { get; set; }

    public List<CustomQuickAction>? CustomActions { get; set; }

    /// <summary>2.0 版本的快速操作列表，仅用于迁移旧设置。</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public List<QuickActionType>? QuickActions { get; set; }

    /// <summary>1.0 版本只能选择一个快速操作，仅用于迁移旧设置。</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public QuickActionType QuickAction { get; set; } = QuickActionType.Properties;

    /// <summary>自动关闭空置物架：拖出的项目从置物架移走，全部拖出或移除后关闭置物架。</summary>
    public bool CloseWhenEmpty { get; set; } = true;
    public string Hotkey { get; set; } = HotkeyGesture.Default.ToString();
    public bool HasCompletedFirstRun { get; set; }

    /// <summary>概览页的“快速上手”被关闭后不再显示。</summary>
    public bool ShowGettingStarted { get; set; } = true;

    [JsonIgnore]
    public HotkeyGesture HotkeyGesture =>
        HotkeyGesture.TryParse(Hotkey, out var gesture) ? gesture : HotkeyGesture.None;

    public AppSettings Clone()
    {
        var clone = (AppSettings)MemberwiseClone();
        clone.QuickActions = QuickActions is null ? null : [.. QuickActions];
        clone.QuickActionSlots = QuickActionSlots is null ? null : [.. QuickActionSlots];
        clone.CustomActions = CustomActions?.Select(action => action.Clone()).ToList();
        return clone;
    }
}

public sealed class SettingsService
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
        Converters = { new JsonStringEnumConverter() }
    };

    public string SettingsPath =>
        Environment.GetEnvironmentVariable("DROPSHELF_SETTINGS_PATH") is { Length: > 0 } customPath
            ? customPath
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DropShelf", "settings.json");

    /// <summary>设置、最近关闭的置物架等数据所在的文件夹。</summary>
    public string DataDirectory => Path.GetDirectoryName(SettingsPath) ?? string.Empty;

    public AppSettings Load()
    {
        try
        {
            if (!File.Exists(SettingsPath)) return Normalize(new AppSettings());
            var settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(SettingsPath), JsonOptions) ?? new AppSettings();
            return Normalize(settings);
        }
        catch (Exception exception)
        {
            LogService.Error("读取设置失败，已使用默认值", exception);
            return Normalize(new AppSettings());
        }
    }

    public void Save(AppSettings settings)
    {
        try
        {
            var directory = Path.GetDirectoryName(SettingsPath);
            if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
            var tempPath = SettingsPath + ".tmp";
            File.WriteAllText(tempPath, JsonSerializer.Serialize(Normalize(settings.Clone()), JsonOptions));
            File.Move(tempPath, SettingsPath, true);
        }
        catch (Exception exception)
        {
            LogService.Error("保存设置失败", exception);
        }
    }

    public static AppSettings Normalize(AppSettings settings)
    {
        if (!Enum.IsDefined(settings.MouseGestureSensitivity))
            settings.MouseGestureSensitivity = GestureSensitivity.Balanced;
        if (!Enum.IsDefined(settings.AutoOpenTrigger))
            settings.AutoOpenTrigger = AutoOpenTrigger.Both;
        if (!Enum.IsDefined(settings.EdgeDwell))
            settings.EdgeDwell = EdgeDwellTime.Medium;
        if (!Enum.IsDefined(settings.Theme))
            settings.Theme = AppTheme.System;
        if (settings.AccentColor != ThemeService.SystemAccent && ThemeService.ParseAccent(settings.AccentColor) is null)
            settings.AccentColor = null;
        if (!Enum.IsDefined(settings.Language))
            settings.Language = AppLanguage.System;
        if (!Enum.IsDefined(settings.QuickShelfSize))
            settings.QuickShelfSize = QuickShelfSize.Medium;
        if (!Enum.IsDefined(settings.DragOutMode))
            settings.DragOutMode = TransferMode.Move;
        if (!Enum.IsDefined(settings.QuickActionLayout))
            settings.QuickActionLayout = QuickActionLayout.Four;

        settings.CustomActions = NormalizeCustomActions(settings.CustomActions);
        if (settings.QuickActionSlots is null) MigrateQuickActions(settings);
        settings.QuickActionSlots = Models.QuickActions.NormalizeSlots(settings.QuickActionSlots, settings.CustomActions);
        settings.QuickActions = null;
        settings.QuickAction = QuickActionType.None;

        if (!HotkeyGesture.TryParse(settings.Hotkey, out var hotkey))
            hotkey = HotkeyGesture.Default;
        settings.Hotkey = hotkey.ToString();

        if (!double.IsFinite(settings.WindowWidth) || settings.WindowWidth < 640) settings.WindowWidth = 860;
        if (!double.IsFinite(settings.WindowHeight) || settings.WindowHeight < 520) settings.WindowHeight = 640;
        settings.Version = AppSettings.CurrentVersion;
        return settings;
    }

    /// <summary>首次运行时提供一个压缩图片的示例操作，用户可以直接放到置物架上。</summary>
    public static CustomQuickAction CreateDefaultImageAction() => new() { Kind = CustomActionKind.CompressImage };

    /// <summary>
    /// 旧版本的快速操作迁移为四个位置：1.0 只有一个操作，关闭时保持关闭，其余情况使用默认组合；
    /// 2.0 选择的操作按原顺序放在前面，选了一两个时改为显示两个大按钮。
    /// </summary>
    private static void MigrateQuickActions(AppSettings settings)
    {
        var legacy = settings.QuickActions is not null
            ? Models.QuickActions.Normalize(settings.QuickActions)
            : settings.QuickAction == QuickActionType.None ? [] : [.. Models.QuickActions.Defaults];
        settings.QuickActionLayout = legacy.Count switch
        {
            0 => QuickActionLayout.Off,
            <= 2 => QuickActionLayout.Two,
            _ => QuickActionLayout.Four
        };
        settings.QuickActionSlots = legacy.Select(type => type.ToString()).ToList();
    }

    private static List<CustomQuickAction> NormalizeCustomActions(List<CustomQuickAction>? actions)
    {
        if (actions is null) return [CreateDefaultImageAction()];
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var result = new List<CustomQuickAction>();
        foreach (var action in actions)
        {
            if (action is null || !Enum.IsDefined(action.Kind) || !action.IsComplete) continue;
            if (string.IsNullOrWhiteSpace(action.Id) || Models.QuickActions.IsBuiltInId(action.Id) || ids.Contains(action.Id))
                action.Id = CustomQuickAction.NewId();
            ids.Add(action.Id);

            var name = action.Name?.Trim();
            action.Name = string.IsNullOrEmpty(name)
                ? null
                : name.Length > CustomQuickAction.MaximumNameLength ? name[..CustomQuickAction.MaximumNameLength] : name;
            if (!Enum.IsDefined(action.FolderMode)) action.FolderMode = TransferMode.Move;
            action.ImageSizeKb = Math.Clamp(action.ImageSizeKb, CustomQuickAction.MinimumImageSizeKb, CustomQuickAction.MaximumImageSizeKb);
            action.Folder = action.Folder?.Trim();
            action.ProgramPath = action.ProgramPath?.Trim().Trim('"');
            action.Arguments = string.IsNullOrWhiteSpace(action.Arguments) ? null : action.Arguments.Trim();

            result.Add(action);
            if (result.Count == Models.QuickActions.MaximumCustomActions) break;
        }
        return result;
    }
}
