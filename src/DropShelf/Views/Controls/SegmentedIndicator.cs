using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace DropShelf.Views.Controls;

/// <summary>
/// 分段选择的选中块，垫在选项下面：切换时从原来的选项滑到新的选项，宽度跟着变，两个选项的文字颜色同时过渡。
/// 第一次显示、选项大小变化、看不见或关闭了动画效果时直接到位。
/// </summary>
public static class SegmentedIndicator
{
    private static readonly TimeSpan Duration = TimeSpan.FromMilliseconds(220);
    private static readonly IEasingFunction Ease = new BezierEase(0.2, 0, 0, 1);

    /// <summary>选中块所属的分段选择，设在模板里的选中块上。</summary>
    public static readonly DependencyProperty OwnerProperty = DependencyProperty.RegisterAttached(
        "Owner", typeof(ListBox), typeof(SegmentedIndicator), new PropertyMetadata(null, OnOwnerChanged));

    public static ListBox? GetOwner(DependencyObject element) => (ListBox?)element.GetValue(OwnerProperty);
    public static void SetOwner(DependencyObject element, ListBox? value) => element.SetValue(OwnerProperty, value);

    /// <summary>
    /// 选中块铺满整条分段选择，只露出裁剪范围里的一块；切换时只移动、伸缩裁剪的矩形，不改选中块的大小和位置，每帧都不用重新排版。
    /// </summary>
    private static void OnOwnerChanged(DependencyObject element, DependencyPropertyChangedEventArgs e)
    {
        if (element is not FrameworkElement block || e.NewValue is not ListBox owner) return;
        block.Width = double.NaN;
        block.HorizontalAlignment = HorizontalAlignment.Stretch;
        var clip = new RectangleGeometry();
        block.Clip = clip;
        var placed = false;

        void Place(bool animate)
        {
            if (owner.SelectedItem is not { } selected || owner.ItemContainerGenerator.ContainerFromItem(selected) is not FrameworkElement item ||
                item.ActualWidth <= 0 || VisualTreeHelper.GetParent(block) is not UIElement area)
            {
                block.Visibility = Visibility.Hidden;
                placed = false;
                return;
            }
            var bounds = new Rect(item.TranslatePoint(default, area), item.RenderSize);
            clip.RadiusX = clip.RadiusY = block is Border border ? border.CornerRadius.TopLeft : 0;
            block.Visibility = Visibility.Visible;
            if (animate && placed && owner.IsVisible && ShelfMotion.Enabled)
            {
                clip.BeginAnimation(RectangleGeometry.RectProperty, new RectAnimation(bounds, Duration) { EasingFunction = Ease });
                return;
            }
            clip.BeginAnimation(RectangleGeometry.RectProperty, null);
            (clip.Rect, placed) = (bounds, true);
        }

        owner.SizeChanged += (_, _) => Place(false);
        owner.SelectionChanged += (_, args) =>
        {
            var animate = placed;
            Place(true);
            if (animate && block.Visibility == Visibility.Visible && args.AddedItems is [Control added] && args.RemovedItems is [Control removed])
                Recolor(added, removed, owner.IsVisible && ShelfMotion.Enabled);
        };
    }

    /// <summary>新选中和取消选中的两个选项的文字颜色跟着选中块过渡；正在过渡的接着当前颜色变，否则从对方现在的颜色（也就是自己原来的颜色）开始。</summary>
    private static void Recolor(Control added, Control removed, bool animate)
    {
        var (addedNow, removedNow) = (Fading(added), Fading(removed));
        added.ClearValue(Control.ForegroundProperty);
        removed.ClearValue(Control.ForegroundProperty);
        if (!animate || added.Foreground is not SolidColorBrush { Color: var selected } ||
            removed.Foreground is not SolidColorBrush { Color: var unselected }) return;
        Fade(added, addedNow ?? unselected, selected);
        Fade(removed, removedNow ?? selected, unselected);
    }

    private static Color? Fading(Control item) =>
        item.ReadLocalValue(Control.ForegroundProperty) is SolidColorBrush brush ? brush.Color : null;

    /// <summary>暂时换上一支单独的画刷过渡颜色，过渡完交还给样式，主题切换照常生效。</summary>
    private static void Fade(Control item, Color from, Color to)
    {
        if (from == to) return;
        var brush = new SolidColorBrush(from);
        item.Foreground = brush;
        var fade = new ColorAnimation(to, Duration) { EasingFunction = Ease };
        fade.Completed += (_, _) =>
        {
            if (ReferenceEquals(item.ReadLocalValue(Control.ForegroundProperty), brush)) item.ClearValue(Control.ForegroundProperty);
        };
        brush.BeginAnimation(SolidColorBrush.ColorProperty, fade);
    }
}
