using System.Globalization;
using System.Windows;
using System.Windows.Media;
using DropShelf.Interop;
using DropShelf.Models;
using Colors = System.Windows.Media.Colors;
using Microsoft.Win32;
using WpfColor = System.Windows.Media.Color;

namespace DropShelf.Services;

public readonly record struct AccentPalette(WpfColor Light3, WpfColor Light2, WpfColor Light1, WpfColor Base,
    WpfColor Dark1, WpfColor Dark2, WpfColor Dark3)
{
    public static AccentPalette Default { get; } = new(
        WpfColor.FromRgb(0x99, 0xEB, 0xFF), WpfColor.FromRgb(0x4C, 0xC2, 0xFF), WpfColor.FromRgb(0x00, 0x91, 0xF8),
        WpfColor.FromRgb(0x00, 0x78, 0xD4), WpfColor.FromRgb(0x00, 0x67, 0xC0), WpfColor.FromRgb(0x00, 0x3E, 0x92),
        WpfColor.FromRgb(0x00, 0x1A, 0x68));

    /// <summary>解析注册表 AccentPalette：依次为 Light3、Light2、Light1、强调色、Dark1、Dark2、Dark3，每项 RGBA 四字节。</summary>
    public static AccentPalette? Parse(byte[]? bytes)
    {
        if (bytes is null || bytes.Length < 28) return null;
        WpfColor At(int index) => WpfColor.FromRgb(bytes[index * 4], bytes[index * 4 + 1], bytes[index * 4 + 2]);
        return new AccentPalette(At(0), At(1), At(2), At(3), At(4), At(5), At(6));
    }

    /// <summary>由一个颜色生成整组深浅：向白色或黑色逐级混合。</summary>
    public static AccentPalette FromColor(WpfColor color) => new(
        Mix(color, Colors.White, 0.65), Mix(color, Colors.White, 0.45), Mix(color, Colors.White, 0.25), color,
        Mix(color, Colors.Black, 0.15), Mix(color, Colors.Black, 0.35), Mix(color, Colors.Black, 0.55));

    private static WpfColor Mix(WpfColor from, WpfColor to, double amount) => WpfColor.FromRgb(
        (byte)Math.Round(from.R + (to.R - from.R) * amount),
        (byte)Math.Round(from.G + (to.G - from.G) * amount),
        (byte)Math.Round(from.B + (to.B - from.B) * amount));
}

public static class ThemeService
{
    private const string PersonalizeKey = @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";
    private const string AccentKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\Accent";
    private static bool _initialized;
    private static AppTheme _mode = AppTheme.System;
    private static WpfColor? _customAccent;

    /// <summary>可选的强调色，取自 Windows 的强调色色板。</summary>
    public static IReadOnlyList<string> AccentPresets { get; } =
        ["#0078D4", "#8764B8", "#C239B3", "#E74856", "#CA5010", "#498205", "#038387", "#68768A"];

    public static event EventHandler? ThemeChanged;
    public static AppTheme Mode => _mode;
    public static bool IsDark { get; private set; }
    public static AccentPalette Accent { get; private set; } = AccentPalette.Default;

    public static WpfColor AccentFill => IsDark ? Accent.Light2 : Accent.Dark1;

    public static void Initialize(AppTheme mode, string? accent)
    {
        if (!_initialized)
        {
            SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;
            _initialized = true;
        }
        SetMode(mode, accent);
    }

    /// <summary>切换主题；强调色为 #RRGGBB，为空时跟随 Windows。</summary>
    public static void SetMode(AppTheme mode, string? accent = null)
    {
        _mode = mode;
        _customAccent = ParseAccent(accent);
        Refresh();
    }

    private static void Refresh()
    {
        var system = ReadAccentPalette() ?? AccentPalette.Default;
        Accent = _customAccent is { } custom ? AccentPalette.FromColor(custom) : system;
        ApplyTheme(ResolveIsDark(_mode, ReadSystemDarkMode()), system.Base);
    }

    public static WpfColor? ParseAccent(string? value) =>
        value is { Length: 7 } && value[0] == '#' &&
        uint.TryParse(value.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var rgb)
            ? WpfColor.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb)
            : null;

    public static bool ResolveIsDark(AppTheme mode, bool systemIsDark) => mode switch
    {
        AppTheme.Dark => true,
        AppTheme.Light => false,
        _ => systemIsDark
    };

    public static void Dispose()
    {
        if (!_initialized) return;
        SystemEvents.UserPreferenceChanged -= OnUserPreferenceChanged;
        _initialized = false;
    }

    public static IReadOnlyDictionary<string, WpfColor> BuildPalette(bool dark, AccentPalette accent)
    {
        var fill = dark ? accent.Light2 : accent.Dark1;
        var background = dark ? WpfColor.FromRgb(0x20, 0x20, 0x20) : WpfColor.FromRgb(0xF3, 0xF3, 0xF3);
        var palette = new Dictionary<string, WpfColor>
        {
            ["AccentBrush"] = fill,
            ["AccentHoverBrush"] = WithAlpha(fill, 0xE6),
            ["AccentPressedBrush"] = WithAlpha(fill, 0xCC),
            ["AccentTextBrush"] = EnsureContrast(dark ? accent.Light3 : accent.Dark2, background, 4.5, dark),
            ["AccentSubtleBrush"] = WithAlpha(fill, dark ? (byte)0x2E : (byte)0x1F),
            ["AccentFaintBrush"] = WithAlpha(fill, dark ? (byte)0x18 : (byte)0x10)
        };

        foreach (var (key, light, darkValue) in NeutralPalette)
            palette[key] = Parse(dark ? darkValue : light);

        // 强调色由用户自选，可能很暗也可能很亮：按实际亮度选择对比度更高的黑字或白字。
        palette["TextOnAccentBrush"] = PrefersDarkText(fill) ? WpfColor.FromArgb(0xE4, 0, 0, 0) : Colors.White;
        return palette;
    }

    private static readonly (string Key, string Light, string Dark)[] NeutralPalette =
    [
        ("TextPrimaryBrush", "#E4000000", "#FFFFFFFF"),
        ("TextSecondaryBrush", "#9E000000", "#C5FFFFFF"),
        ("TextTertiaryBrush", "#72000000", "#87FFFFFF"),
        ("TextDisabledBrush", "#5C000000", "#5DFFFFFF"),
        ("TextOnAccentBrush", "#FFFFFFFF", "#E4000000"),
        ("WindowBackgroundBrush", "#FFF3F3F3", "#FF202020"),
        ("LayerBrush", "#80FFFFFF", "#4C3A3A3A"),
        ("CardBrush", "#B3FFFFFF", "#0DFFFFFF"),
        ("CardHoverBrush", "#E6FFFFFF", "#14FFFFFF"),
        ("CardBorderBrush", "#14000000", "#33000000"),
        ("ControlBrush", "#B3FFFFFF", "#0FFFFFFF"),
        ("ControlHoverBrush", "#80F9F9F9", "#15FFFFFF"),
        ("ControlPressedBrush", "#4DF9F9F9", "#08FFFFFF"),
        ("ControlBorderBrush", "#1F000000", "#17FFFFFF"),
        ("ControlStrongBorderBrush", "#72000000", "#8BFFFFFF"),
        ("ControlAltBrush", "#06000000", "#19000000"),
        ("SubtleHoverBrush", "#09000000", "#0FFFFFFF"),
        ("SubtlePressedBrush", "#06000000", "#0AFFFFFF"),
        ("SubtleSelectedBrush", "#0D000000", "#12FFFFFF"),
        ("DividerBrush", "#0F000000", "#15FFFFFF"),
        ("FlyoutBrush", "#FFF9F9F9", "#FF2C2C2C"),
        ("FlyoutBorderBrush", "#1A000000", "#26FFFFFF"),
        ("ShelfSolidBrush", "#FFF6F6F6", "#FF2B2B2B"),
        ("ShelfAcrylicTintBrush", "#B3F7F7F7", "#B32A2A2A"),
        ("ShelfTileBrush", "#FFFFFFFF", "#FF4D4D4D"),
        ("ShelfTileBorderBrush", "#17000000", "#2EFFFFFF"),
        ("ScrollThumbBrush", "#72000000", "#8BFFFFFF"),
        ("SuccessBrush", "#FF0F7B0F", "#FF6CCB5F"),
        ("CautionBrush", "#FF9D5D00", "#FFFCE100"),
        ("CriticalBrush", "#FFC42B1C", "#FFFF99A4"),
        ("SmokeBrush", "#CCF6F6F6", "#CC2B2B2B")
    ];

    /// <param name="systemAccent">Windows 当前的强调色，用于设置页“跟随 Windows”色块的预览。</param>
    private static void ApplyTheme(bool dark, WpfColor systemAccent)
    {
        IsDark = dark;
        if (System.Windows.Application.Current is not { } application) return;
        var resources = application.Resources;
        foreach (var (key, color) in BuildPalette(dark, Accent))
        {
            var brush = new SolidColorBrush(color);
            brush.Freeze();
            resources[key] = brush;
        }
        resources["AccentColor"] = AccentFill;
        var systemBrush = new SolidColorBrush(systemAccent);
        systemBrush.Freeze();
        resources["SystemAccentBrush"] = systemBrush;
        ThemeChanged?.Invoke(null, EventArgs.Empty);
    }

    public static double RelativeLuminance(WpfColor color)
    {
        static double Channel(byte value)
        {
            var c = value / 255.0;
            return c <= 0.03928 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4);
        }
        return 0.2126 * Channel(color.R) + 0.7152 * Channel(color.G) + 0.0722 * Channel(color.B);
    }

    public static double ContrastRatio(WpfColor first, WpfColor second)
    {
        var a = RelativeLuminance(first) + 0.05;
        var b = RelativeLuminance(second) + 0.05;
        return a > b ? a / b : b / a;
    }

    public static bool PrefersDarkText(WpfColor fill) =>
        ContrastRatio(fill, Colors.Black) > ContrastRatio(fill, Colors.White);

    /// <summary>把颜色逐步向白色（深色主题）或黑色（浅色主题）调整，直到与背景达到所需对比度。</summary>
    public static WpfColor EnsureContrast(WpfColor color, WpfColor background, double ratio, bool lighten)
    {
        var target = lighten ? Colors.White : Colors.Black;
        var current = color;
        for (var step = 1; step <= 20 && ContrastRatio(current, background) < ratio; step++)
        {
            var amount = step / 20.0;
            current = WpfColor.FromRgb(
                (byte)Math.Round(color.R + (target.R - color.R) * amount),
                (byte)Math.Round(color.G + (target.G - color.G) * amount),
                (byte)Math.Round(color.B + (target.B - color.B) * amount));
        }
        return current;
    }

    private static WpfColor WithAlpha(WpfColor color, byte alpha) => WpfColor.FromArgb(alpha, color.R, color.G, color.B);

    private static WpfColor Parse(string value) => (WpfColor)System.Windows.Media.ColorConverter.ConvertFromString(value);

    private static bool ReadSystemDarkMode()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(PersonalizeKey);
            return key?.GetValue("AppsUseLightTheme") is int value && value == 0;
        }
        catch
        {
            return false;
        }
    }

    private static AccentPalette? ReadAccentPalette()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(AccentKey);
            return AccentPalette.Parse(key?.GetValue("AccentPalette") as byte[]);
        }
        catch
        {
            return null;
        }
    }

    private static void OnUserPreferenceChanged(object sender, UserPreferenceChangedEventArgs e)
    {
        if (System.Windows.Application.Current is not { } application) return;
        application.Dispatcher.BeginInvoke(Refresh);
    }
}
