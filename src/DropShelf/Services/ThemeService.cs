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
    private static bool _followSystem;

    /// <summary>强调色设置为这个值时跟随 Windows；为空时用默认的钴蓝。</summary>
    public const string SystemAccent = "system";

    /// <summary>默认强调色：牌背和牌桌的钴蓝。</summary>
    public static WpfColor Cobalt { get; } = WpfColor.FromRgb(0x2B, 0x4E, 0xFF);

    /// <summary>可选的强调色，取自 Windows 的强调色色板。</summary>
    public static IReadOnlyList<string> AccentPresets { get; } =
        ["#8764B8", "#C239B3", "#E74856", "#CA5010", "#498205", "#038387", "#68768A"];

    public static event EventHandler? ThemeChanged;
    public static AppTheme Mode => _mode;
    public static bool IsDark { get; private set; }
    public static AccentPalette Accent { get; private set; } = AccentPalette.Default;

    public static WpfColor AccentFill => Accent.Base;

    public static void Initialize(AppTheme mode, string? accent)
    {
        if (!_initialized)
        {
            SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;
            _initialized = true;
        }
        SetMode(mode, accent);
    }

    /// <summary>切换主题；强调色为 #RRGGBB，为 <see cref="SystemAccent"/> 时跟随 Windows，为空时用钴蓝。</summary>
    public static void SetMode(AppTheme mode, string? accent = null)
    {
        _mode = mode;
        _followSystem = accent == SystemAccent;
        _customAccent = ParseAccent(accent);
        Refresh();
    }

    private static void Refresh()
    {
        var system = ReadAccentPalette() ?? AccentPalette.Default;
        Accent = _followSystem ? system : AccentPalette.FromColor(_customAccent ?? Cobalt);
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
        var fill = accent.Base;
        var background = Parse(dark ? DarkBackground : LightBackground);
        // 牌背和牌桌：浅色主题直接用强调色，深色主题压暗，免得在暗处一大片发亮。
        var back = dark ? Mix(fill, Colors.Black, 0.45) : fill;
        var table = dark ? Mix(fill, Colors.Black, 0.52) : fill;
        var palette = new Dictionary<string, WpfColor>
        {
            ["AccentBrush"] = fill,
            ["AccentHoverBrush"] = WithAlpha(fill, 0xE6),
            ["AccentPressedBrush"] = WithAlpha(fill, 0xCC),
            ["AccentTextBrush"] = EnsureContrast(fill, background, 4.5, dark),
            ["AccentSubtleBrush"] = WithAlpha(fill, dark ? (byte)0x2E : (byte)0x1F),
            ["AccentFaintBrush"] = WithAlpha(fill, dark ? (byte)0x18 : (byte)0x10),
            ["CardBackFillBrush"] = back,
            ["CardBackLineBrush"] = Mix(back, Colors.Black, 0.25),
            ["TableFillBrush"] = table
        };

        foreach (var (key, light, darkValue) in NeutralPalette)
            palette[key] = Parse(dark ? darkValue : light);

        // 强调色由用户自选，可能很暗也可能很亮：按实际亮度选择对比度更高的黑字或白字。
        palette["TextOnAccentBrush"] = PrefersDarkText(fill) ? Parse(NeutralInk) : Colors.White;
        palette["TableTextBrush"] = PrefersDarkText(table) ? Parse(NeutralInk) : dark ? palette["TextPrimaryBrush"] : Colors.White;
        return palette;
    }

    private const string LightBackground = "#FFE8E5DE", DarkBackground = "#FF1C1B19", NeutralInk = "#FF15171C";

    // 浅色是象牙白的牌桌和牌面，深色是暖调的深炭色，两套的冷暖一致。
    private static readonly (string Key, string Light, string Dark)[] NeutralPalette =
    [
        ("TextPrimaryBrush", NeutralInk, "#FFEDEAE3"),
        ("TextSecondaryBrush", "#FF575A63", "#FFA6A39C"),
        ("TextTertiaryBrush", "#FF7A7D85", "#FF86837C"),
        ("TextDisabledBrush", "#FFA3A5AB", "#FF5F5C56"),
        ("WindowBackgroundBrush", LightBackground, DarkBackground),
        ("LayerBrush", "#00000000", "#00000000"),
        ("CardBrush", "#FFFBFAF6", "#FF262522"),
        ("CardHoverBrush", "#FFFFFFFF", "#FF2E2D2A"),
        ("CardBorderBrush", "#FFD2CEC5", "#FF34322E"),
        ("TableCardBorderBrush", "#FFC9C5BB", "#FF3D3B36"),
        ("ControlBrush", "#FFFBFAF6", "#FF2A2926"),
        ("ControlHoverBrush", "#FFFFFFFF", "#FF32312D"),
        ("ControlPressedBrush", "#FFF2EFE8", "#FF262522"),
        ("ControlBorderBrush", "#FFBDB8AE", "#FF4A4843"),
        ("ControlStrongBorderBrush", "#FF5E6066", "#FFA6A39C"),
        ("ControlAltBrush", "#0D15171C", "#14EDEAE3"),
        ("SubtleHoverBrush", "#0F15171C", "#12EDEAE3"),
        ("SubtlePressedBrush", "#0A15171C", "#0AEDEAE3"),
        ("SubtleSelectedBrush", "#1415171C", "#1AEDEAE3"),
        ("DividerBrush", "#FFDDD9D0", "#FF34322E"),
        ("FlyoutBrush", "#FFFBFAF6", "#FF262522"),
        ("FlyoutBorderBrush", "#FFC9C5BB", "#FF3D3B36"),
        ("SettingRowBrush", "#FFF2EFE8", "#FF2E2D2A"),
        ("SettingRowBorderBrush", "#FFE2DED5", "#FF3A3834"),
        ("ShelfSolidBrush", LightBackground, DarkBackground),
        ("ShelfBorderBrush", "#FFB4AFA4", "#FF3A3834"),
        ("ShelfTileBrush", "#FFFFFFFF", "#FF2E2D2A"),
        ("ShelfTileBorderBrush", NeutralInk, "#FFA6A39C"),
        ("ShelfUnderCardBorderBrush", NeutralInk, "#FF6E6B65"),
        ("ArtFrameBrush", "#FFDDD9D0", "#FF3A3834"),
        ("ScrollThumbBrush", "#FF8A8C93", "#FF6E6B65"),
        ("SuccessBrush", "#FF0F7B0F", "#FF6CCB5F"),
        ("CautionBrush", "#FF9D5D00", "#FFFCE100"),
        ("CriticalBrush", "#FFC42B1C", "#FFFF99A4"),
        ("SmokeBrush", "#CCE8E5DE", "#CC1C1B19")
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
        var back = ((SolidColorBrush)resources["CardBackFillBrush"]).Color;
        // 牌背：斜格纹；设置页四周那圈边框和翻牌途中的轮廓都用它。牌背露出的窄边改用斜条纹，几像素宽也看得清。
        resources["SheetBorderBrush"] = Lattice(back, WithAlpha(Colors.White, dark ? (byte)0x1F : (byte)0x29), 8);
        resources["StackEdgeBrush"] = Stripes(back, Mix(back, Colors.White, dark ? 0.15 : 0.3));
        resources["TableBrush"] = Lattice(((SolidColorBrush)resources["TableFillBrush"]).Color, WithAlpha(Colors.White, dark ? (byte)0x0F : (byte)0x12), 12);
        resources["SheetBrush"] = resources["CardBrush"];
        resources["AccentColor"] = AccentFill;
        var systemBrush = new SolidColorBrush(systemAccent);
        systemBrush.Freeze();
        resources["SystemAccentBrush"] = systemBrush;
        ThemeChanged?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>斜格纹：每格画一个叉，平铺后连成菱形格子。格子在缓存里只画一次。</summary>
    private static System.Windows.Media.Brush Lattice(WpfColor fill, WpfColor line, double size)
    {
        var pen = new System.Windows.Media.Pen(new SolidColorBrush(line), 1);
        var drawing = new DrawingGroup
        {
            Children =
            {
                new GeometryDrawing(new SolidColorBrush(fill), null, new RectangleGeometry(new Rect(0, 0, size, size))),
                new GeometryDrawing(null, pen, new LineGeometry(new System.Windows.Point(0, 0), new System.Windows.Point(size, size))),
                new GeometryDrawing(null, pen, new LineGeometry(new System.Windows.Point(size, 0), new System.Windows.Point(0, size)))
            }
        };
        var brush = new DrawingBrush(drawing)
        {
            TileMode = TileMode.Tile, Stretch = Stretch.None,
            Viewport = new Rect(0, 0, size, size), ViewportUnits = BrushMappingMode.Absolute,
            Viewbox = new Rect(0, 0, size, size), ViewboxUnits = BrushMappingMode.Absolute
        };
        RenderOptions.SetCachingHint(brush, CachingHint.Cache);
        brush.Freeze();
        return brush;
    }

    /// <summary>45° 的细条纹，两种颜色各 2 像素宽。</summary>
    private static System.Windows.Media.Brush Stripes(WpfColor first, WpfColor second)
    {
        var brush = new LinearGradientBrush
        {
            MappingMode = BrushMappingMode.Absolute, SpreadMethod = GradientSpreadMethod.Repeat,
            StartPoint = new System.Windows.Point(0, 0), EndPoint = new System.Windows.Point(2.83, 2.83),
            GradientStops = { new GradientStop(first, 0), new GradientStop(first, 0.5), new GradientStop(second, 0.5), new GradientStop(second, 1) }
        };
        brush.Freeze();
        return brush;
    }

    public static WpfColor Mix(WpfColor from, WpfColor to, double amount) => WpfColor.FromRgb(
        (byte)Math.Round(from.R + (to.R - from.R) * amount),
        (byte)Math.Round(from.G + (to.G - from.G) * amount),
        (byte)Math.Round(from.B + (to.B - from.B) * amount));

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
