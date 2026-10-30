using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using System.Windows.Media;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

/// <summary>
/// 强调色选项；Value 为空字符串表示默认的钴蓝，为 <see cref="ThemeService.SystemAccent"/> 表示跟随 Windows，
/// 后者的色块使用主题资源中的系统强调色。
/// </summary>
public sealed record AccentOption(string Value, SolidColorBrush? Brush)
{
    /// <summary>选项列表保持不变：替换列表会让选择框把选中值写回为空，误把强调色改回默认。</summary>
    public static IReadOnlyList<AccentOption> All { get; } =
    [
        new(string.Empty, Frozen(ThemeService.Cobalt)),
        new(ThemeService.SystemAccent, null),
        .. ThemeService.AccentPresets.Select(hex => new AccentOption(hex, Frozen(ThemeService.ParseAccent(hex)!.Value)))
    ];

    private static SolidColorBrush Frozen(System.Windows.Media.Color color)
    {
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        return brush;
    }
}
