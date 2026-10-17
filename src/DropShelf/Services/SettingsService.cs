using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using DropShelf.Models;

namespace DropShelf.Services;

public sealed class AppSettings
{
    public const int CurrentVersion = 2;

    public int Version { get; set; } = CurrentVersion;
    public double WindowLeft { get; set; } = double.NaN;
    public double WindowTop { get; set; } = double.NaN;
    public double WindowWidth { get; set; } = 900;
    public double WindowHeight { get; set; } = 640;
    public bool GestureEnabled { get; set; } = true;
    public GestureSensitivity MouseGestureSensitivity { get; set; } = GestureSensitivity.Balanced;
    public AppTheme Theme { get; set; } = AppTheme.System;
    public bool StartWithWindows { get; set; }
    public QuickShelfSize QuickShelfSize { get; set; } = QuickShelfSize.Medium;

    /// <summary>置物架底部显示的快速操作（按固定顺序，最多四个）。</summary>
    public List<QuickActionType>? QuickActions { get; set; }

    /// <summary>1.0 版本只能选择一个快速操作，仅用于迁移旧设置。</summary>
    public QuickActionType QuickAction { get; set; } = QuickActionType.Properties;

    public bool RemoveAfterDragOut { get; set; } = true;
    public bool CloseWhenEmpty { get; set; } = true;
    public string Hotkey { get; set; } = HotkeyGesture.Default.ToString();
    public bool HasCompletedFirstRun { get; set; }

    [JsonIgnore]
    public HotkeyGesture HotkeyGesture =>
        HotkeyGesture.TryParse(Hotkey, out var gesture) ? gesture : HotkeyGesture.None;

    public AppSettings Clone()
    {
        var clone = (AppSettings)MemberwiseClone();
        clone.QuickActions = QuickActions is null ? null : [.. QuickActions];
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
        if (!Enum.IsDefined(settings.Theme))
            settings.Theme = AppTheme.System;
        if (!Enum.IsDefined(settings.QuickShelfSize))
            settings.QuickShelfSize = QuickShelfSize.Medium;

        if (settings.QuickActions is null)
        {
            // 1.0 只有一个快速操作：关闭时保持关闭，其余情况启用新的默认组合。
            settings.QuickActions = settings.QuickAction == QuickActionType.None ? [] : [.. QuickActions.Defaults];
        }
        settings.QuickActions = QuickActions.Normalize(settings.QuickActions);
        settings.QuickAction = settings.QuickActions.FirstOrDefault();

        if (!HotkeyGesture.TryParse(settings.Hotkey, out var hotkey))
            hotkey = HotkeyGesture.Default;
        settings.Hotkey = hotkey.ToString();

        if (!double.IsFinite(settings.WindowWidth) || settings.WindowWidth < 760) settings.WindowWidth = 900;
        if (!double.IsFinite(settings.WindowHeight) || settings.WindowHeight < 540) settings.WindowHeight = 640;
        settings.Version = AppSettings.CurrentVersion;
        return settings;
    }
}
