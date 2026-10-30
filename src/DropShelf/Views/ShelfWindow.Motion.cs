using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Models;
using WpfPoint = System.Windows.Point;
using WpfSize = System.Windows.Size;

namespace DropShelf.Views;

/// <summary>
/// 置物架内容的动画：拖进来的文件从松手处飞进堆叠；堆叠和列表之间切换时，每张牌飞到列表里自己那一行（或飞回来），
/// 窗口大小同时平滑过渡。只改牌的变换和透明度，窗口每帧调整一次大小；动画途中再有变化时直接跳到终点。
/// </summary>
public partial class ShelfWindow
{
    private static readonly TimeSpan LandDuration = TimeSpan.FromMilliseconds(380);
    private static readonly TimeSpan LandStagger = TimeSpan.FromMilliseconds(80);
    private static readonly TimeSpan PushDelay = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan PushDuration = TimeSpan.FromMilliseconds(300);
    private static readonly TimeSpan FlyDuration = TimeSpan.FromMilliseconds(320);
    private static readonly TimeSpan FlyStagger = TimeSpan.FromMilliseconds(30);
    private static readonly TimeSpan RowFadeDelay = TimeSpan.FromMilliseconds(140);
    private static readonly TimeSpan RowFadeDuration = TimeSpan.FromMilliseconds(220);
    private static readonly TimeSpan FadeOutDuration = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan ResizeDuration = TimeSpan.FromMilliseconds(260);
    private static readonly IEasingFunction MotionEase = new CubicEase { EasingMode = EasingMode.EaseOut };

    private readonly List<EventHandler> _motionFrames = [];
    private Action? _motionCleanup;
    private StackCard[]? _stackCards;
    private EventHandler? _resizeFrame;
    private (double Width, double Height) _resizeTarget;

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
    /// 堆叠和列表之间切换：堆叠保持收起时的大小留在左上角，牌飞到列表里自己那一行并缩成行首图标，各行随后从左边滑出；
    /// 收起时各行淡出，图标飞回去叠成一摞。窗口大小同时过渡到新的大小。
    /// </summary>
    private void AnimateViewSwitch()
    {
        FinishMotion();
        // 上一段动画刚跳到终点时窗口大小还没重新布局，先布局一次再量尺寸。
        UpdateLayout();
        var toList = ViewModel.IsExpanded;
        var (width, height) = TargetSize();
        var bodySize = new WpfSize(Body.ActualWidth + width - ActualWidth, Body.ActualHeight + height - ActualHeight);
        var stackSize = toList ? new WpfSize(StackView.ActualWidth, StackView.ActualHeight) : bodySize;
        var listHeight = toList ? bodySize.Height : ItemsList.ActualHeight;

        UpdateViews();
        StackView.Visibility = Visibility.Visible;
        ItemsList.Visibility = Visibility.Visible;
        StackView.Width = stackSize.Width;
        StackView.Height = stackSize.Height;
        StackView.HorizontalAlignment = HorizontalAlignment.Left;
        StackView.VerticalAlignment = VerticalAlignment.Top;
        StackView.IsHitTestVisible = false;
        Panel.SetZIndex(StackView, 1);
        UpdateWindowSize(animate: true);
        Body.UpdateLayout();

        var cards = StackCards;
        var thumbs = new List<UIElement>();
        var rows = new List<ListBoxItem>();
        var end = TimeSpan.Zero;
        for (var index = 0; index < ViewModel.Items.Count; index++)
            if (ItemsList.ItemContainerGenerator.ContainerFromIndex(index) is ListBoxItem row) rows.Add(row);

        for (var i = 0; i < cards.Length; i++)
        {
            var card = cards[i];
            if (card.Card.DataContext is not ShelfItem item) continue;
            var row = ItemsList.ItemContainerGenerator.ContainerFromItem(item) as ListBoxItem;
            var thumb = row is null ? null : FindNamed(row, "RowThumb");
            CardPose target;
            var landsOnRow = thumb is not null && thumb.TransformToVisual(ItemsList).TransformBounds(new Rect(thumb.RenderSize)) is var bounds &&
                bounds.Top >= 0 && bounds.Bottom <= listHeight;
            if (landsOnRow)
            {
                var box = thumb!.TransformToVisual(StackCanvas).TransformBounds(new Rect(thumb.RenderSize));
                target = new CardPose(box.X + box.Width / 2, box.Y + box.Height / 2, 0, box.Height / Math.Max(card.Card.Width, card.Card.Height));
                thumb.Opacity = 0;
                thumbs.Add(thumb);
            }
            else
            {
                // 这一行在列表可见范围之外：牌朝列表底边飞去并淡出，或从那里淡入。
                var edge = ItemsList.TranslatePoint(new WpfPoint(22, listHeight), StackCanvas);
                target = new CardPose(edge.X, edge.Y, 0, 0.3, 0);
            }

            var delay = FlyStagger * (toList ? i : cards.Length - 1 - i);
            var (from, to) = toList ? (card.Home, target) : (target, card.Home);
            Tween(delay, FlyDuration, t =>
            {
                var pose = CardPose.Lerp(from, to, t);
                // 飞到行首时和行里的图标交叉淡入淡出，两者大小不完全一样也看不出跳动。
                if (toList && landsOnRow)
                {
                    pose = pose with { Opacity = Math.Clamp((1 - t) / 0.25, 0, 1) };
                    thumb!.Opacity = 1 - pose.Opacity;
                }
                card.Pose(pose);
            });
            end = Max(end, delay + FlyDuration);
        }

        for (var i = 0; i < rows.Count; i++)
        {
            var row = rows[i];
            var shift = new TranslateTransform();
            row.RenderTransform = shift;
            var order = Math.Min(i, 6);
            if (toList)
            {
                var delay = RowFadeDelay + FlyStagger * order;
                Tween(delay, RowFadeDuration, t =>
                {
                    row.Opacity = t;
                    shift.X = -10 * (1 - t);
                });
                end = Max(end, delay + RowFadeDuration);
            }
            else
            {
                Tween(TimeSpan.Zero, FadeOutDuration, t =>
                {
                    row.Opacity = 1 - t;
                    shift.X = -10 * t;
                });
            }
        }

        var fadeDelay = toList ? TimeSpan.Zero : FlyDuration - FadeOutDuration;
        Tween(fadeDelay, toList ? FadeOutDuration : RowFadeDuration, t =>
        {
            StackCaptionPanel.Opacity = StackMark.Opacity = toList ? 1 - t : t;
        });
        end = Max(end, fadeDelay + RowFadeDuration);

        _motionCleanup = () =>
        {
            foreach (var card in cards) card.Pose(card.Home);
            foreach (var thumb in thumbs) thumb.ClearValue(OpacityProperty);
            foreach (var row in rows)
            {
                row.ClearValue(OpacityProperty);
                row.ClearValue(RenderTransformProperty);
            }
            StackCaptionPanel.ClearValue(OpacityProperty);
            RestoreStackMark();
            foreach (var property in new[] { WidthProperty, HeightProperty, HorizontalAlignmentProperty, VerticalAlignmentProperty, IsHitTestVisibleProperty, Panel.ZIndexProperty })
                StackView.ClearValue(property);
            UpdateViews();
        };
        Tween(TimeSpan.Zero, end, _ => { }, FinishMotion);
    }

    /// <summary>逐帧调用 apply，参数是缓动后的进度 0–1；delay 之前停在起点。</summary>
    private void Tween(TimeSpan delay, TimeSpan duration, Action<double> apply, Action? completed = null)
    {
        apply(0);
        var clock = Stopwatch.StartNew();
        EventHandler? frame = null;
        frame = (_, _) =>
        {
            var progress = duration <= TimeSpan.Zero ? 1 : Math.Clamp((clock.Elapsed - delay) / duration, 0, 1);
            apply(MotionEase.Ease(progress));
            if (progress < 1) return;
            CompositionTarget.Rendering -= frame;
            _motionFrames.Remove(frame!);
            completed?.Invoke();
        };
        _motionFrames.Add(frame);
        CompositionTarget.Rendering += frame;
    }

    /// <summary>立即结束进行中的动画：牌、列表和窗口都回到最终状态。</summary>
    private void FinishMotion()
    {
        StopMotion();
        var cleanup = _motionCleanup;
        _motionCleanup = null;
        cleanup?.Invoke();
        FinishResize();
    }

    private void StopMotion()
    {
        foreach (var frame in _motionFrames) CompositionTarget.Rendering -= frame;
        _motionFrames.Clear();
    }

    /// <summary>窗口大小逐帧过渡到目标大小，左上角不动；只改大小不改位置，和滑入动画互不影响。</summary>
    private void ResizeWindow(double width, double height)
    {
        StopResize();
        var handle = new WindowInteropHelper(this).Handle;
        if (!ShelfMotion.Enabled || !IsVisible || handle == IntPtr.Zero || !NativeMethods.GetWindowRect(handle, out var rect))
        {
            ApplyWindowSize(width, height);
            return;
        }

        var dpi = VisualTreeHelper.GetDpi(this);
        var (toWidth, toHeight) = ((int)Math.Round(width * dpi.DpiScaleX), (int)Math.Round(height * dpi.DpiScaleY));
        var (fromWidth, fromHeight) = (rect.Width, rect.Height);
        var clock = Stopwatch.StartNew();
        _resizeTarget = (width, height);
        _resizeFrame = (_, _) =>
        {
            var progress = Math.Min(1, clock.Elapsed / ResizeDuration);
            var eased = MotionEase.Ease(progress);
            NativeMethods.SetWindowPos(handle, IntPtr.Zero, 0, 0,
                (int)Math.Round(fromWidth + (toWidth - fromWidth) * eased), (int)Math.Round(fromHeight + (toHeight - fromHeight) * eased),
                NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);
            if (progress >= 1) FinishResize();
        };
        CompositionTarget.Rendering += _resizeFrame;
    }

    private void FinishResize()
    {
        if (_resizeFrame is null) return;
        StopResize();
        ApplyWindowSize(_resizeTarget.Width, _resizeTarget.Height);
    }

    private void StopResize()
    {
        if (_resizeFrame is null) return;
        CompositionTarget.Rendering -= _resizeFrame;
        _resizeFrame = null;
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
