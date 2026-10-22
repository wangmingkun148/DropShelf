using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace DropShelf.Views.Controls;

/// <summary>
/// 把子元素排成一手牌：中间高、两侧低，略微倾斜，底部可以伸出面板外由外层裁掉。
/// 鼠标在牌堆上横向移动时选出离光标最近的一张，抽出、回正并放大，相邻的牌向两侧让开。
/// 只改变每张牌的变换，不重新排版；新动画从当前位置接着走，切换时不会跳。
/// </summary>
public sealed class FanPanel : Panel
{
    /// <summary>被选出的牌；会继承给牌内的元素，模板可据此显示边框和按钮。</summary>
    public static readonly DependencyProperty IsHotProperty = DependencyProperty.RegisterAttached(
        "IsHot", typeof(bool), typeof(FanPanel), new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.Inherits));

    public static bool GetIsHot(DependencyObject element) => (bool)element.GetValue(IsHotProperty);
    public static void SetIsHot(DependencyObject element, bool value) => element.SetValue(IsHotProperty, value);

    private const double MaxSpacing = 98;
    private const double Tilt = 2.2;
    private const double EdgeDrop = 34;
    private const double Lift = 92;
    private const double HotScale = 1.14;
    private const double Push = 42;
    private static readonly Duration MoveDuration = TimeSpan.FromMilliseconds(300);
    private static readonly IEasingFunction MoveEase = new CubicEase { EasingMode = EasingMode.EaseOut };
    private static readonly IEasingFunction LiftEase = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.3 };

    private int _hot = -1;
    private int _lastHot = -1;
    private bool _childrenChanged = true;
    private int _pressed = -1;
    private double _center;
    private double _spacing;
    private double _width;
    private UIElement? _dragged;
    private int _gap = -1;
    private Size _card;

    public FanPanel()
    {
        Background = Brushes.Transparent;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        foreach (UIElement child in InternalChildren) child.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return default;
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var count = InternalChildren.Count;
        if (count == 0) return finalSize;
        _card = InternalChildren[0].DesiredSize;
        _spacing = count > 1 ? Math.Clamp((finalSize.Width - _card.Width) / (count - 1), 0, MaxSpacing) : 0;
        _center = finalSize.Width / 2;
        _width = finalSize.Width;
        for (var index = 0; index < count; index++)
        {
            var child = InternalChildren[index];
            child.Arrange(new Rect(CenterOf(index) - _card.Width / 2, RestY(index, count), _card.Width, _card.Height));
        }

        // 牌换过时（含数量不变、整组重新生成，如切换语言）重新摆好，不播放动画；其余重新排版（如窗口变化）不打断正在播放的动画。
        if (_childrenChanged)
        {
            _childrenChanged = false;
            _hot = _lastHot = -1;
            _dragged = null;
            _gap = -1;
            ApplyState(animate: false);
        }
        return finalSize;
    }

    protected override void OnVisualChildrenChanged(DependencyObject visualAdded, DependencyObject visualRemoved)
    {
        base.OnVisualChildrenChanged(visualAdded, visualRemoved);
        _childrenChanged = true;
    }

    private double CenterOf(int index) => _center + (index - (InternalChildren.Count - 1) / 2.0) * _spacing;

    /// <summary>牌堆排成 slots 张牌时第 slot 张的中心。</summary>
    private double SlotCenter(int slot, int slots) =>
        _center + (slot - (slots - 1) / 2.0) * (slots > 1 ? Math.Clamp((_width - _card.Width) / (slots - 1), 0, MaxSpacing) : 0);

    private static double RestY(int index, int count)
    {
        var mid = (count - 1) / 2.0;
        return mid > 0 ? Math.Pow(Math.Abs(index - mid) / mid, 2) * EdgeDrop : 0;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        SetHot(IndexAt(e.GetPosition(this)));
    }

    protected override void OnMouseLeave(MouseEventArgs e)
    {
        base.OnMouseLeave(e);
        SetHot(-1);
    }

    /// <summary>点在选出的牌下方的空白处，也算点了这张牌。</summary>
    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonDown(e);
        _pressed = _hot;
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonUp(e);
        if (_pressed < 0 || _pressed != _hot) return;
        _pressed = -1;
        if (FindButton(InternalChildren[_hot]) is { Command: { } command } button && command.CanExecute(button.CommandParameter))
        {
            command.Execute(button.CommandParameter);
            e.Handled = true;
        }
    }

    protected override void OnGotKeyboardFocus(KeyboardFocusChangedEventArgs e)
    {
        base.OnGotKeyboardFocus(e);
        for (var index = 0; index < InternalChildren.Count; index++)
            if (InternalChildren[index].IsKeyboardFocusWithin) SetHot(index);
    }

    protected override void OnLostKeyboardFocus(KeyboardFocusChangedEventArgs e)
    {
        base.OnLostKeyboardFocus(e);
        if (!IsKeyboardFocusWithin && !IsMouseOver) SetHot(-1);
    }

    /// <summary>选出的牌抽出后超过面板上沿，这块区域也要接收鼠标，牌升起的过程中光标才不会落到下面的元素上。</summary>
    protected override HitTestResult? HitTestCore(PointHitTestParameters hitTestParameters)
    {
        var point = hitTestParameters.HitPoint;
        if (_hot >= 0 && _hot < InternalChildren.Count)
        {
            var half = _card.Width * HotScale / 2;
            var top = -Lift - _card.Height * (HotScale - 1);
            if (point.Y >= top && point.Y <= ActualHeight && Math.Abs(point.X - CenterOf(_hot)) <= half)
                return new PointHitTestResult(this, point);
        }
        return base.HitTestCore(hitTestParameters);
    }

    private int IndexAt(Point point)
    {
        var count = InternalChildren.Count;
        if (count == 0) return -1;
        if (_hot >= 0 && _hot < count && HotContains(point)) return _hot;
        var mid = (count - 1) / 2.0;
        var half = _card.Width / 2;
        if (point.X < CenterOf(0) - half || point.X > CenterOf(count - 1) + half) return -1;
        return _spacing > 0 ? Math.Clamp((int)Math.Round((point.X - _center) / _spacing + mid), 0, count - 1) : 0;
    }

    /// <summary>
    /// 光标在选出的牌抽出后的位置上时保持选中，否则移到牌靠边处会离相邻的牌更近、牌被换掉，点不到右上角的按钮。
    /// 按动画终点计算，牌升起的过程中不会因位置还在变化而来回切换。
    /// </summary>
    private bool HotContains(Point point)
    {
        var bottom = _card.Height - Lift;
        return Math.Abs(point.X - CenterOf(_hot)) <= _card.Width * HotScale / 2 && point.Y >= bottom - _card.Height * HotScale && point.Y <= bottom;
    }

    /// <summary>选出的牌被拖走时，所有牌回到原位。</summary>
    public void ClearHot() => SetHot(-1);

    /// <summary>拖着的牌要放回的位置；-1 表示牌已离开牌堆，其余的牌并拢。</summary>
    public int Gap => _gap;

    /// <summary>
    /// 拖动 dragged 时其余的牌在 gap 处让出空位，gap 为 -1 时并拢；dragged 为 null 时结束拖动。
    /// 拖着的牌（已隐藏）摆在空位上，放回时飞向它即可。
    /// </summary>
    public void SetGap(UIElement? dragged, int gap)
    {
        if (dragged is null) gap = -1;
        if (dragged == _dragged && gap == _gap) return;
        _dragged = dragged;
        _gap = gap;
        _hot = -1;
        ApplyState(animate: true);
    }

    /// <summary>面板上横坐标 x 处对应的空位。</summary>
    public int SlotAt(double x)
    {
        var count = InternalChildren.Count;
        if (count <= 1 || _spacing <= 0) return 0;
        return Math.Clamp((int)Math.Round((x - _center) / _spacing + (count - 1) / 2.0), 0, count - 1);
    }

    private void SetHot(int index)
    {
        if (index == _hot) return;
        if (_hot >= 0) _lastHot = _hot;
        _hot = index;
        ApplyState(animate: true);
    }

    private void ApplyState(bool animate)
    {
        var count = InternalChildren.Count;
        var dragged = _dragged is null ? -1 : InternalChildren.IndexOf(_dragged);
        var slots = dragged >= 0 && _gap < 0 ? count - 1 : count;
        var mid = (slots - 1) / 2.0;
        for (var index = 0; index < count; index++)
        {
            var child = InternalChildren[index];
            var (scale, rotate, move) = Transforms(child);
            // 拖动时其余的牌依次补上拖走的位置，再在空位处让开。
            var slot = index;
            if (index == dragged && _gap >= 0) slot = _gap;
            else if (dragged >= 0 && index != dragged)
            {
                if (index > dragged) slot--;
                if (_gap >= 0 && slot >= _gap) slot++;
            }
            var rest = (slot - mid) * Tilt;
            var isHot = index == _hot;
            var offset = SlotCenter(slot, slots) - CenterOf(index) +
                         (_hot < 0 || isHot ? 0 : Math.Sign(index - _hot) * Math.Max(0, 3 - Math.Abs(index - _hot)) / 3.0 * Push);

            var ease = isHot ? LiftEase : MoveEase;
            // 拖着的牌是隐藏的，直接放到空位上，拖回来的卡牌被吸向空位的最终位置，而不是一路滑过去的中间位置。
            var smooth = animate && index != dragged;
            Set(scale, ScaleTransform.ScaleXProperty, isHot ? HotScale : 1, ease, smooth);
            Set(scale, ScaleTransform.ScaleYProperty, isHot ? HotScale : 1, ease, smooth);
            Set(rotate, RotateTransform.AngleProperty, isHot ? 0 : rest, MoveEase, smooth);
            Set(move, TranslateTransform.XProperty, offset, MoveEase, smooth);
            Set(move, TranslateTransform.YProperty, isHot ? -Lift - RestY(index, count) : RestY(slot, slots) - RestY(index, count), ease, smooth);

            // 中间的牌在上面；刚放回去的牌在落下过程中仍压在邻居上面。
            SetZIndex(child, isHot ? 1000 : index == _lastHot ? 999 : slots - (int)Math.Ceiling(Math.Abs(slot - mid)));
            SetIsHot(child, isHot);
            child.IsHitTestVisible = isHot;
        }
    }

    private static (ScaleTransform Scale, RotateTransform Rotate, TranslateTransform Move) Transforms(UIElement child)
    {
        if (child.RenderTransform is TransformGroup { Children: [ScaleTransform scale, RotateTransform rotate, TranslateTransform move] })
            return (scale, rotate, move);
        scale = new ScaleTransform();
        rotate = new RotateTransform();
        move = new TranslateTransform();
        child.RenderTransformOrigin = new Point(0.5, 1);
        child.RenderTransform = new TransformGroup { Children = { scale, rotate, move } };
        return (scale, rotate, move);
    }

    private static void Set(Animatable target, DependencyProperty property, double value, IEasingFunction ease, bool animate)
    {
        if (animate)
        {
            target.BeginAnimation(property, new DoubleAnimation(value, MoveDuration) { EasingFunction = ease });
            return;
        }
        target.BeginAnimation(property, null);
        target.SetValue(property, value);
    }

    private static ButtonBase? FindButton(DependencyObject element)
    {
        if (element is ButtonBase button) return button;
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(element); index++)
            if (FindButton(VisualTreeHelper.GetChild(element, index)) is { } found) return found;
        return null;
    }
}
