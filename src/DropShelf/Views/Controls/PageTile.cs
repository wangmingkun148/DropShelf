using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;

namespace DropShelf.Views.Controls;

/// <summary>
/// 概览页上进入设置页的卡片。窗口较矮时是一行图标和文字；高度足够时卡片拉高，
/// 换成图标在左上、设置项标签在下的样式，背景图案随高度渐显。
/// 两种样式按 <see cref="IsTall"/> 切换并交叉淡入，不随高度连续变化，否则高度停在中间时两套内容会叠在一起。
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

    public static readonly DependencyProperty GlanceProperty =
        DependencyProperty.Register(nameof(Glance), typeof(object), typeof(PageTile), new PropertyMetadata(null));

    private static readonly DependencyPropertyKey ItemsPropertyKey =
        DependencyProperty.RegisterReadOnly(nameof(Items), typeof(string[]), typeof(PageTile), new PropertyMetadata(Array.Empty<string>()));

    public static readonly DependencyProperty ItemsProperty = ItemsPropertyKey.DependencyProperty;

    private static readonly DependencyPropertyKey IsTallPropertyKey =
        DependencyProperty.RegisterReadOnly(nameof(IsTall), typeof(bool), typeof(PageTile), new PropertyMetadata(false));

    public static readonly DependencyProperty IsTallProperty = IsTallPropertyKey.DependencyProperty;

    private const double TallHeight = 138;
    // 回差：高度停在分界附近时不会来回切换。
    private const double Hysteresis = 6;

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

    /// <summary>卡片右侧画出的当前设置，如快捷键的键帽、底部按钮的排法。</summary>
    public object? Glance
    {
        get => GetValue(GlanceProperty);
        set => SetValue(GlanceProperty, value);
    }

    /// <summary>摘要拆成的各项，大卡片上显示为标签。</summary>
    public string[] Items => (string[])GetValue(ItemsProperty);

    /// <summary>卡片够高时显示大卡片样式。</summary>
    public bool IsTall => (bool)GetValue(IsTallProperty);

    protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
    {
        base.OnRenderSizeChanged(sizeInfo);
        var height = sizeInfo.NewSize.Height;
        if (IsTall ? height < TallHeight - Hysteresis : height >= TallHeight + Hysteresis) SetValue(IsTallPropertyKey, !IsTall);
    }

    private void UpdateItems() => SetValue(ItemsPropertyKey,
        Summary?.Split([" · ", "、", ", "], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries) ?? []);

    // 卡片底下的牌背伸出卡片几像素。摘要被截断、内容刚好占满宽度时，像素取整会让测量结果比分到的宽度多出不到一像素，
    // WPF 就按布局区域裁剪，牌背只剩圆角处露出一点。卡片内容已在模板里自行裁剪，这里不再按布局区域裁。
    protected override Geometry? GetLayoutClip(Size layoutSlotSize) => null;
}

/// <summary><see cref="PageTile"/> 模板的根：同样不按布局区域裁剪，伸出卡片的牌背和悬停时的位移始终完整显示。</summary>
public sealed class UnclippedGrid : Grid
{
    protected override Geometry? GetLayoutClip(Size layoutSlotSize) => null;
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
