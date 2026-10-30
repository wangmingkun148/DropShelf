using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using System.Windows.Media;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

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
