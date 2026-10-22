using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;

namespace DropShelf.Views.Controls;

/// <summary>
/// 概览页上进入设置页的卡片。窗口较矮时是一行图标和文字；高度足够时卡片拉高，
/// 换成图标在左上、设置项标签在下的样式，背景图案随高度渐显（模板中按卡片高度交叉淡入）。
/// </summary>
public sealed class PageTile : Button
{
    public static readonly DependencyProperty IconProperty =
        DependencyProperty.Register(nameof(Icon), typeof(string), typeof(PageTile), new PropertyMetadata(null));

    public static readonly DependencyProperty HeaderProperty =
        DependencyProperty.Register(nameof(Header), typeof(string), typeof(PageTile), new PropertyMetadata(null));

    public static readonly DependencyProperty SummaryProperty =
        DependencyProperty.Register(nameof(Summary), typeof(string), typeof(PageTile),
            new PropertyMetadata(null, (element, _) => ((PageTile)element).UpdateItems()));

    private static readonly DependencyPropertyKey ItemsPropertyKey =
        DependencyProperty.RegisterReadOnly(nameof(Items), typeof(string[]), typeof(PageTile), new PropertyMetadata(Array.Empty<string>()));

    public static readonly DependencyProperty ItemsProperty = ItemsPropertyKey.DependencyProperty;

    public string? Icon
    {
        get => (string?)GetValue(IconProperty);
        set => SetValue(IconProperty, value);
    }

    public string? Header
    {
        get => (string?)GetValue(HeaderProperty);
        set => SetValue(HeaderProperty, value);
    }

    /// <summary>当前设置的摘要，如“中文 · 开机启动 · Ctrl+Shift+Space”。</summary>
    public string? Summary
    {
        get => (string?)GetValue(SummaryProperty);
        set => SetValue(SummaryProperty, value);
    }

    /// <summary>摘要拆成的各项，大卡片上显示为标签。</summary>
    public string[] Items => (string[])GetValue(ItemsProperty);

    private void UpdateItems() => SetValue(ItemsPropertyKey,
        Summary?.Split([" · ", "、", ", "], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries) ?? []);
}

/// <summary>把数值在参数“起点,终点”之间线性映射到 0–1，用于随高度渐变的透明度；起点大于终点时反向。</summary>
public sealed class RampConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var bounds = ((string)parameter).Split(',');
        var from = double.Parse(bounds[0], CultureInfo.InvariantCulture);
        var to = double.Parse(bounds[1], CultureInfo.InvariantCulture);
        return Math.Clamp(((double)value - from) / (to - from), 0, 1);
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => throw new NotSupportedException();
}
