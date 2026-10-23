using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;

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
        // 调整顺序或删掉一张时牌沿用原来的：记下换位前的位置，换位后从看到的位置接着滑过去，正在播放的动画不会跳到终点。
        var before = _childrenChanged && InternalChildren.Cast<UIElement>().All(child => child is FrameworkElement element && LayoutInformation.GetLayoutSlot(element).Width > 0)
            ? InternalChildren.Cast<FrameworkElement>().Select(element => LayoutInformation.GetLayoutSlot(element).TopLeft).ToArray()
            : null;
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
            if (_dragged is not null) UseCache(false);
            _dragged = null;
            _gap = -1;
            // 放回的牌调整顺序后整组重新生成，光标下的牌直接保持选中，不会先落下再抽出。
            if (IsMouseOver && Mouse.Captured is null) _hot = IndexAt(Mouse.GetPosition(this));
            if (before is not null)
                for (var index = 0; index < count; index++)
                    Shift(InternalChildren[index], LayoutInformation.GetLayoutSlot((FrameworkElement)InternalChildren[index]).TopLeft - before[index]);
            ApplyState(animate: before is not null);
        }
        return finalSize;
    }

    protected override void OnVisualChildrenChanged(DependencyObject visualAdded, DependencyObject visualRemoved)
    {
        base.OnVisualChildrenChanged(visualAdded, visualRemoved);
        _childrenChanged = true;
    }

    private double CenterOf(int index) => _center + (index - (InternalChildren.Count - 1) / 2.0) * _spacing;

    /// <summary>牌堆排成 slots 张牌时相邻两张的间距。</summary>
    private double SlotSpacing(int slots) => slots > 1 ? Math.Clamp((_width - _card.Width) / (slots - 1), 0, MaxSpacing) : 0;

    /// <summary>牌堆排成 slots 张牌时第 slot 张的中心。</summary>
    private double SlotCenter(int slot, int slots) => _center + (slot - (slots - 1) / 2.0) * SlotSpacing(slots);

    private int DraggedIndex => _dragged is null ? -1 : InternalChildren.IndexOf(_dragged);

    /// <summary>现在排出的位置数：拖着的牌离开牌堆时少一个。</summary>
    private int Slots => _dragged is not null && _gap < 0 ? InternalChildren.Count - 1 : InternalChildren.Count;

    /// <summary>第 index 张牌现在排在第几个位置：拖动时其余的牌依次补上拖走的位置，再在空位处让开，拖着的牌排在空位上。</summary>
    private int SlotOf(int index)
    {
        var dragged = DraggedIndex;
        if (dragged < 0) return index;
        if (index == dragged) return _gap >= 0 ? _gap : index;
        var slot = index > dragged ? index - 1 : index;
        return _gap >= 0 && slot >= _gap ? slot + 1 : slot;
    }

    /// <summary>排在第 slot 个位置的牌；拖着的牌离开牌堆时不算。</summary>
    private int IndexOfSlot(int slot)
    {
        var dragged = DraggedIndex;
        for (var index = 0; index < InternalChildren.Count; index++)
            if (SlotOf(index) == slot && (index != dragged || _gap >= 0)) return index;
        return -1;
    }

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
            if (point.Y >= top && point.Y <= ActualHeight && Math.Abs(point.X - SlotCenter(SlotOf(_hot), Slots)) <= half)
                return new PointHitTestResult(this, point);
        }
        return base.HitTestCore(hitTestParameters);
    }

    private int IndexAt(Point point)
    {
        var slots = Slots;
        if (slots <= 0) return -1;
        if (_hot >= 0 && _hot < InternalChildren.Count && HotContains(point)) return _hot;
        var half = _card.Width / 2;
        if (point.X < SlotCenter(0, slots) - half || point.X > SlotCenter(slots - 1, slots) + half) return -1;
        var spacing = SlotSpacing(slots);
        return IndexOfSlot(spacing > 0 ? Math.Clamp((int)Math.Round((point.X - _center) / spacing + (slots - 1) / 2.0), 0, slots - 1) : 0);
    }

    /// <summary>
    /// 光标在选出的牌抽出后的位置上时保持选中，否则移到牌靠边处会离相邻的牌更近、牌被换掉，点不到右上角的按钮。
    /// 按动画终点计算，牌升起的过程中不会因位置还在变化而来回切换。
    /// </summary>
    private bool HotContains(Point point)
    {
        var bottom = _card.Height - Lift;
        return Math.Abs(point.X - SlotCenter(SlotOf(_hot), Slots)) <= _card.Width * HotScale / 2 && point.Y >= bottom - _card.Height * HotScale && point.Y <= bottom;
    }

    /// <summary>选出的牌被拖走时，所有牌回到原位。</summary>
    public void ClearHot() => SetHot(-1);

    /// <summary>选出光标下的牌，不必等鼠标再移动（如拖动结束、释放捕获后）。</summary>
    public void UpdateHot()
    {
        if (IsMouseOver) SetHot(IndexAt(Mouse.GetPosition(this)));
    }

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
        if (dragged is null != _dragged is null) UseCache(dragged is not null);
        _dragged = dragged;
        _gap = gap;
        ApplyState(animate: true);
    }

    /// <summary>
    /// 拖动期间每张牌缓存为位图：让位、并拢时只移动这些图，不必每帧重画牌面上的文字和缩略图。
    /// 按选出时的放大倍数缓存，选出的牌放大后也清晰。
    /// </summary>
    private void UseCache(bool cached)
    {
        foreach (UIElement child in InternalChildren)
            child.CacheMode = cached ? new BitmapCache(CacheScale.For(this) * HotScale) : null;
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
        var dragged = DraggedIndex;
        var slots = Slots;
        var mid = (slots - 1) / 2.0;
        var hotSlot = _hot >= 0 && _hot < count ? SlotOf(_hot) : -1;
        for (var index = 0; index < count; index++)
        {
            var child = InternalChildren[index];
            var (scale, rotate, move) = Transforms(child);
            var slot = SlotOf(index);
            var rest = (slot - mid) * Tilt;
            var isHot = index == _hot;
            var offset = SlotCenter(slot, slots) - CenterOf(index) +
                         (hotSlot < 0 || isHot ? 0 : Math.Sign(slot - hotSlot) * Math.Max(0, 3 - Math.Abs(slot - hotSlot)) / 3.0 * Push);

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

    /// <summary>牌的排版位置挪了 delta：位移减去这段，画面停在原处，再从这里滑向新的终点。</summary>
    private static void Shift(UIElement child, Vector delta)
    {
        var (_, _, move) = Transforms(child);
        Hold(move, TranslateTransform.XProperty, move.X - delta.X);
        Hold(move, TranslateTransform.YProperty, move.Y - delta.Y);
    }

    private static void Hold(Animatable target, DependencyProperty property, double value)
    {
        target.BeginAnimation(property, null);
        target.SetValue(property, value);
        Goals.GetOrCreateValue(target).Remove(property);
    }

    /// <summary>每个变换各属性最后一次要去的值。</summary>
    private static readonly ConditionalWeakTable<Animatable, Dictionary<DependencyProperty, double>> Goals = new();

    /// <summary>选出一张牌时只有几张牌的位置会变：目标没变的属性不重新开始动画，正在播放的照常播完。</summary>
    private static void Set(Animatable target, DependencyProperty property, double value, IEasingFunction ease, bool animate)
    {
        var goals = Goals.GetOrCreateValue(target);
        if (animate && goals.TryGetValue(property, out var goal) && goal == value) return;
        goals[property] = value;
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
