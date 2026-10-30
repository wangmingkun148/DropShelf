using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Views.Controls;
using WpfPoint = System.Windows.Point;
using WpfSize = System.Windows.Size;

namespace DropShelf.Views;

/// <summary>
/// 置物架内容的动画：拖进来的文件从松手处飞进堆叠或平铺的格子；堆叠和平铺之间切换时，牌从堆叠里一张张发到各自的格子（或收回来）。
/// 只改变换和透明度。窗口大小平滑过渡，但不和牌同时动：逐帧改窗口大小时界面线程要等每一帧画完，
/// 和别的动画一起播会掉帧，所以展开时窗口先变大再发牌，收起时牌收回去之后窗口再变小。动画途中再有变化时直接跳到终点。
/// </summary>
public partial class ShelfWindow
{
    private static readonly TimeSpan LandDuration = TimeSpan.FromMilliseconds(380);
    private static readonly TimeSpan LandStagger = TimeSpan.FromMilliseconds(80);
    private static readonly TimeSpan PushDelay = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan PushDuration = TimeSpan.FromMilliseconds(300);
    private static readonly TimeSpan FlyDuration = TimeSpan.FromMilliseconds(300);
    private static readonly TimeSpan DealStagger = TimeSpan.FromMilliseconds(40);
    private static readonly TimeSpan NameFadeDuration = TimeSpan.FromMilliseconds(220);
    private static readonly TimeSpan FadeOutDuration = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan ResizeDuration = TimeSpan.FromMilliseconds(200);
    private static readonly IEasingFunction MotionEase = new CubicEase { EasingMode = EasingMode.EaseOut };

    private readonly List<MotionTween> _tweens = [];
    private bool _motionHooked;
    private TimeSpan _motionFrameTime;
    private Action? _motionCleanup;
    private StackCard[]? _stackCards;

    /// <summary>一段逐帧动画：从 Start 起等 Delay，再在 Duration 内推进。</summary>
    private sealed record MotionTween(long Start, TimeSpan Delay, TimeSpan Duration, Action<double> Apply, Action? Completed);

    /// <summary>牌在堆叠画布里的姿态：中心位置、角度、缩放和透明度。</summary>
    private readonly record struct CardPose(double X, double Y, double Angle, double Scale = 1, double Opacity = 1)
    {
        public static CardPose Lerp(CardPose from, CardPose to, double t) => new(
            from.X + (to.X - from.X) * t, from.Y + (to.Y - from.Y) * t, from.Angle + (to.Angle - from.Angle) * t,
            from.Scale + (to.Scale - from.Scale) * t, from.Opacity + (to.Opacity - from.Opacity) * t);
    }

    /// <summary>堆叠里的一张牌和它在 XAML 中的原位。</summary>
    private sealed class StackCard(ContentControl card, ScaleTransform scale, RotateTransform turn, TranslateTransform shift, CardPose home)
    {
        public ContentControl Card { get; } = card;
        public CardPose Home { get; } = home;

        /// <summary>按中心摆放：牌在画布上的位置不变，平移量把中心移到目标点，旋转和缩放都绕中心。</summary>
        public void Pose(CardPose pose)
        {
            shift.X = pose.X - (Canvas.GetLeft(Card) + Card.Width / 2);
            shift.Y = pose.Y - (Canvas.GetTop(Card) + Card.Height / 2);
            turn.Angle = pose.Angle;
            scale.ScaleX = scale.ScaleY = pose.Scale;
            Card.Opacity = pose.Opacity;
        }

        public static StackCard From(ContentControl card)
        {
            var group = card.RenderTransform as TransformGroup;
            var angle = group?.Children.OfType<RotateTransform>().FirstOrDefault()?.Angle ?? 0;
            var offset = group?.Children.OfType<TranslateTransform>().FirstOrDefault();
            var (scale, turn, shift) = (new ScaleTransform(), new RotateTransform(angle), new TranslateTransform(offset?.X ?? 0, offset?.Y ?? 0));
            card.RenderTransform = new TransformGroup { Children = { scale, turn, shift } };
            var home = new CardPose(Canvas.GetLeft(card) + card.Width / 2 + shift.X, Canvas.GetTop(card) + card.Height / 2 + shift.Y, angle);
            return new StackCard(card, scale, turn, shift, home);
        }
    }

    /// <summary>从上到下：最上面一张、第二张、最下面一张。</summary>
    private StackCard[] StackCards => _stackCards ??= [StackCard.From(Card0), StackCard.From(Card1), StackCard.From(Card2)];

    /// <summary>
    /// 松手后，拖进来的文件从松手处飞进堆叠：从拖动图像大小放大，划一道弧落到自己的位置，原来的牌往下退一层。
    /// 一次拖入多个时最多 3 张依次落下，最上面那张最后落下，数量随每张落下增加。
    /// </summary>
    private void PlayLanding(WpfPoint drop, int added)
    {
        FinishMotion();
        var cards = StackCards;
        var shown = cards.Count(card => card.Card.DataContext is not null);
        var incoming = Math.Min(added, shown);
        if (incoming == 0) return;

        StackView.UpdateLayout();
        var start = TranslatePoint(drop, StackCanvas);
        var count = ViewModel.Count;
        BeginLandingMark(count - incoming);
        var end = TimeSpan.Zero;
        for (var i = 0; i < shown; i++)
        {
            var card = cards[i];
            if (i < incoming)
            {
                var delay = LandStagger * (incoming - 1 - i);
                var from = new CardPose(start.X, start.Y, -8, 0.5, 0);
                var control = new WpfPoint((from.X + card.Home.X) / 2, Math.Min(from.Y, card.Home.Y) - 20);
                var landed = count - i;
                var top = i == 0;
                Tween(delay, LandDuration, t => card.Pose(Arc(from, card.Home, control, t)), () => CardLanded(landed, top));
                end = Max(end, delay + LandDuration);
            }
            else
            {
                var from = cards[i - incoming].Home;
                Tween(PushDelay, PushDuration, t => card.Pose(CardPose.Lerp(from, card.Home, t)));
                end = Max(end, PushDelay + PushDuration);
            }
        }

        _motionCleanup = () =>
        {
            foreach (var card in cards) card.Pose(card.Home);
            RestoreStackMark();
        };
        Tween(TimeSpan.Zero, end, _ => { }, FinishMotion);
    }

    /// <summary>落下的路径：位置沿二次贝塞尔曲线划一道弧，缩放途中略微超过原大小，开头很快变得不透明。</summary>
    private static CardPose Arc(CardPose from, CardPose to, WpfPoint control, double t)
    {
        var u = 1 - t;
        return new CardPose(
            u * u * from.X + 2 * u * t * control.X + t * t * to.X,
            u * u * from.Y + 2 * u * t * control.Y + t * t * to.Y,
            from.Angle + (to.Angle - from.Angle) * t,
            from.Scale + (to.Scale - from.Scale) * t + 0.06 * Math.Sin(Math.PI * t),
            Math.Min(1, t / 0.15));
    }

    /// <summary>
    /// 平铺状态下拖入：新文件的牌从松手处划一道弧落到自己的格子，快落下时名字淡入。
    /// 一次拖入多个时最多 3 张依次落下，最后一张最后落下；新项目在末尾，看不到时先滚过去。
    /// </summary>
    private void PlayGridLanding(WpfPoint drop, int added)
    {
        FinishMotion();
        var items = ViewModel.Items;
        var incoming = Math.Min(Math.Min(added, items.Count), 3);
        if (incoming == 0) return;

        ItemsList.UpdateLayout();
        ItemsList.ScrollIntoView(items[^1]);
        ItemsList.UpdateLayout();
        var tiles = new List<ListBoxItem>();
        var end = TimeSpan.Zero;
        for (var i = 0; i < incoming; i++)
        {
            if (ItemsList.ItemContainerGenerator.ContainerFromIndex(items.Count - 1 - i) is not ListBoxItem tile ||
                FindNamed(tile, "TileName") is not { } name) continue;
            // 位置用相对格子的平移表示：从松手处出发，从拖动图像大小放大，落到原位。
            var center = tile.TranslatePoint(TileCardCenter, this);
            var from = new CardPose(drop.X - center.X, drop.Y - center.Y, -8, 0.5, 0);
            var home = new CardPose(0, 0, 0);
            var control = new WpfPoint(from.X / 2, Math.Min(from.Y, 0) - 20);
            var pose = PoseTile(tile);
            tiles.Add(tile);

            var delay = LandStagger * (incoming - 1 - i);
            Tween(delay, LandDuration, t => pose(Arc(from, home, control, t)));
            var nameDelay = delay + LandDuration / 2;
            Tween(nameDelay, NameFadeDuration, t => name.Opacity = t);
            end = Max(end, nameDelay + NameFadeDuration);
        }

        _motionCleanup = () =>
        {
            foreach (var tile in tiles) ResetTile(tile);
        };
        Tween(TimeSpan.Zero, end, _ => { }, FinishMotion);
    }

    /// <summary>
    /// 堆叠和平铺之间切换。展开时窗口先平滑变大，堆叠里的牌藏起来，平铺里看得见的牌从堆叠的位置出发，一张张发到各自的格子，落下时略微抬起再放平，名字随后淡入；
    /// 最后几张占住堆叠顶上几张的位置，其余的从最下面那张出发、压在它们下面，起手时那一摞和堆叠一模一样。收起时名字先淡出，牌倒着一张张收回堆叠，之后窗口再平滑变小。
    /// 只有看得见的牌参与，其余的直接出现或消失。
    /// </summary>
    private void AnimateViewSwitch()
    {
        FinishMotion();
        // 上一段动画刚跳到终点时窗口大小还没重新布局，先布局一次再量尺寸。
        UpdateLayout();
        var toGrid = ViewModel.IsExpanded;
        var (width, height) = TargetSize();
        var bodySize = new WpfSize(Body.ActualWidth + width - ActualWidth, Body.ActualHeight + height - ActualHeight);
        var stackSize = toGrid ? new WpfSize(StackView.ActualWidth, StackView.ActualHeight) : bodySize;
        var gridSize = toGrid ? bodySize : new WpfSize(ItemsList.ActualWidth, ItemsList.ActualHeight);

        UpdateViews();
        StackView.Visibility = Visibility.Visible;
        ItemsList.Visibility = Visibility.Visible;
        // 堆叠和平铺都固定为各自显示时的大小，窗口比它们大或小时只是露出或裁掉一部分；平铺压在堆叠上面，牌飞过堆叠时不会被挡住。
        Pin(StackView, stackSize);
        Pin(ItemsList, gridSize);
        StackView.IsHitTestVisible = false;
        ItemsList.IsHitTestVisible = false;
        Body.UpdateLayout();

        var cards = StackCards;
        var stackScale = StackCanvas.TransformToVisual(Body).TransformBounds(new Rect(0, 0, 1, 1)).Width;
        CardPose InBody(StackCard card)
        {
            var center = StackCanvas.TranslatePoint(new WpfPoint(card.Home.X, card.Home.Y), Body);
            return new CardPose(center.X, center.Y, card.Home.Angle, stackScale);
        }
        var starts = cards.Where(card => card.Card.DataContext is not null).Select(InBody).ToList();
        if (starts.Count == 0) starts.Add(InBody(cards[0]));
        foreach (var card in cards) card.Card.Opacity = 0;

        var tiles = new List<(ListBoxItem Tile, FrameworkElement Name)>();
        for (var index = 0; index < ViewModel.Items.Count; index++)
        {
            if (ItemsList.ItemContainerGenerator.ContainerFromIndex(index) is not ListBoxItem tile ||
                FindNamed(tile, "TileName") is not { } name) continue;
            var box = tile.TransformToVisual(ItemsList).TransformBounds(new Rect(tile.RenderSize));
            if (box.Bottom > 0 && box.Top < gridSize.Height) tiles.Add((tile, name));
        }

        var lead = toGrid ? ResizeDuration : FadeOutDuration / 2;
        var end = TimeSpan.Zero;
        for (var i = 0; i < tiles.Count; i++)
        {
            var (tile, name) = tiles[i];
            var center = tile.TranslatePoint(TileCardCenter, Body);
            // 从后往前数：最后一张对着堆叠最上面那张，依次往下，其余的都从最下面那张出发。
            var stack = starts[Math.Min(tiles.Count - 1 - i, starts.Count - 1)];
            var away = stack with { X = stack.X - center.X, Y = stack.Y - center.Y };
            var home = new CardPose(0, 0, 0);
            var (from, to) = toGrid ? (away, home) : (home, away);
            var pose = PoseTile(tile);
            var delay = lead + DealStagger * (toGrid ? i : tiles.Count - 1 - i);
            Tween(delay, FlyDuration, t =>
            {
                var current = CardPose.Lerp(from, to, t);
                pose(current with { Scale = current.Scale + 0.04 * Math.Sin(Math.PI * t) });
            });
            end = Max(end, delay + FlyDuration);
            if (toGrid)
            {
                var nameDelay = delay + FlyDuration * 0.6;
                Tween(nameDelay, NameFadeDuration, t => name.Opacity = t);
                end = Max(end, nameDelay + NameFadeDuration);
            }
            else
            {
                Tween(TimeSpan.Zero, FadeOutDuration, t => name.Opacity = 1 - t);
            }
        }

        // 堆叠下方的说明和角标：展开时随牌发出淡出，收起时在最后一张收回前淡入。
        var captionDelay = toGrid ? lead : Max(TimeSpan.Zero, end - NameFadeDuration);
        var captionDuration = toGrid ? FadeOutDuration : NameFadeDuration;
        Tween(captionDelay, captionDuration, t =>
        {
            StackCaptionPanel.Opacity = StackMark.Opacity = toGrid ? 1 - t : t;
        });
        end = Max(end, captionDelay + captionDuration);

        // 展开时上面的动画都推迟到窗口变大之后；收起时牌是从各个格子飞回来的，等它们都回到堆叠里再把窗口变小。
        TweenWindowSize(toGrid ? TimeSpan.Zero : end);
        if (!toGrid) end += ResizeDuration;

        _motionCleanup = () =>
        {
            foreach (var card in cards) card.Pose(card.Home);
            foreach (var (tile, _) in tiles) ResetTile(tile);
            StackCaptionPanel.ClearValue(OpacityProperty);
            RestoreStackMark();
            Unpin(StackView);
            Unpin(ItemsList);
            StackView.ClearValue(IsHitTestVisibleProperty);
            ItemsList.ClearValue(IsHitTestVisibleProperty);
            UpdateViews();
            UpdateWindowSize();
        };
        Tween(TimeSpan.Zero, end, _ => { }, FinishMotion);
    }

    /// <summary>牌面 64×86 在格子顶部居中，这是它的中心。</summary>
    private static readonly WpfPoint TileCardCenter = new(CardGridPanel.SlotWidth / 2, 43);

    /// <summary>平铺里的一张牌：以牌面中心为原点缩放、转动，再平移；名字跟着一起动。</summary>
    private static Action<CardPose> PoseTile(ListBoxItem tile)
    {
        var (scale, turn, shift) = (new ScaleTransform(), new RotateTransform(), new TranslateTransform());
        tile.RenderTransformOrigin = new WpfPoint(TileCardCenter.X / CardGridPanel.SlotWidth, TileCardCenter.Y / CardGridPanel.SlotHeight);
        tile.RenderTransform = new TransformGroup { Children = { scale, turn, shift } };
        return pose =>
        {
            shift.X = pose.X;
            shift.Y = pose.Y;
            turn.Angle = pose.Angle;
            scale.ScaleX = scale.ScaleY = pose.Scale;
            tile.Opacity = pose.Opacity;
        };
    }

    private static void ResetTile(ListBoxItem tile)
    {
        foreach (var property in new[] { RenderTransformProperty, RenderTransformOriginProperty, OpacityProperty }) tile.ClearValue(property);
        if (FindNamed(tile, "TileName") is { } name) name.ClearValue(OpacityProperty);
    }

    /// <summary>逐帧调用 apply，参数是缓动后的进度 0–1；delay 之前停在起点。</summary>
    private void Tween(TimeSpan delay, TimeSpan duration, Action<double> apply, Action? completed = null)
    {
        apply(0);
        _tweens.Add(new MotionTween(Stopwatch.GetTimestamp(), delay, duration, apply, completed));
        if (_motionHooked) return;
        _motionHooked = true;
        CompositionTarget.Rendering += OnMotionFrame;
    }

    /// <summary>所有动画共用一个逐帧回调：同一帧重复触发时跳过，还在等待的动画停在起点不更新。</summary>
    private void OnMotionFrame(object? sender, EventArgs e)
    {
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (time == _motionFrameTime) return;
        _motionFrameTime = time;
        for (var i = 0; i < _tweens.Count; i++)
        {
            var tween = _tweens[i];
            var elapsed = Stopwatch.GetElapsedTime(tween.Start) - tween.Delay;
            if (elapsed < TimeSpan.Zero) continue;
            var progress = tween.Duration <= TimeSpan.Zero ? 1 : Math.Min(elapsed / tween.Duration, 1);
            tween.Apply(MotionEase.Ease(progress));
            if (progress < 1) continue;
            _tweens.RemoveAt(i--);
            // 收尾可能直接结束全部动画并清空这个列表，循环随之结束。
            tween.Completed?.Invoke();
        }
        if (_tweens.Count == 0) StopMotion();
    }

    /// <summary>窗口大小逐帧过渡到目标大小，左上角不动；只改大小不改位置，和滑入动画互不影响。跳到终点时由收尾的 UpdateWindowSize 变到位。</summary>
    private void TweenWindowSize(TimeSpan delay)
    {
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero || !NativeMethods.GetWindowRect(handle, out var rect))
        {
            UpdateWindowSize();
            return;
        }

        var (width, height) = TargetSize();
        var dpi = VisualTreeHelper.GetDpi(this);
        var (toWidth, toHeight) = ((int)Math.Round(width * dpi.DpiScaleX), (int)Math.Round(height * dpi.DpiScaleY));
        var (fromWidth, fromHeight) = (rect.Width, rect.Height);
        var shown = (fromWidth, fromHeight);
        Tween(delay, ResizeDuration, t =>
        {
            var w = (int)Math.Round(fromWidth + (toWidth - fromWidth) * t);
            var h = (int)Math.Round(fromHeight + (toHeight - fromHeight) * t);
            if ((w, h) == shown) return;
            shown = (w, h);
            NativeMethods.SetWindowPos(handle, IntPtr.Zero, 0, 0, w, h, NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);
        }, UpdateWindowSize);
    }

    /// <summary>立即结束进行中的动画：牌、平铺和窗口都回到最终状态。</summary>
    private void FinishMotion()
    {
        StopMotion();
        var cleanup = _motionCleanup;
        _motionCleanup = null;
        cleanup?.Invoke();
    }

    private void StopMotion()
    {
        _tweens.Clear();
        if (!_motionHooked) return;
        _motionHooked = false;
        CompositionTarget.Rendering -= OnMotionFrame;
    }

    private static void Pin(FrameworkElement element, WpfSize size)
    {
        element.Width = size.Width;
        element.Height = size.Height;
        element.HorizontalAlignment = HorizontalAlignment.Left;
        element.VerticalAlignment = VerticalAlignment.Top;
    }

    private static void Unpin(FrameworkElement element)
    {
        foreach (var property in new[] { WidthProperty, HeightProperty, HorizontalAlignmentProperty, VerticalAlignmentProperty })
            element.ClearValue(property);
    }

    private static TimeSpan Max(TimeSpan a, TimeSpan b) => a > b ? a : b;

    private static FrameworkElement? FindNamed(DependencyObject root, string name)
    {
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is FrameworkElement element && element.Name == name) return element;
            if (FindNamed(child, name) is { } found) return found;
        }
        return null;
    }
}
