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
/// 置物架内容的动画：拖进来的文件从松手处飞进堆叠或列表；堆叠和列表之间切换时，每张牌飞到列表里自己那一行（或飞回来）。
/// 只改变换和透明度。窗口大小平滑过渡，但不和牌同时动：逐帧改窗口大小时界面线程要等每一帧画完，
/// 和别的动画一起播会掉帧，所以展开时窗口先变大再飞牌，收起时牌飞回去之后窗口再变小。动画途中再有变化时直接跳到终点。
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
    private static readonly TimeSpan ResizeDuration = TimeSpan.FromMilliseconds(200);
    private static readonly TimeSpan RemoveDuration = TimeSpan.FromMilliseconds(160);
    private static readonly TimeSpan CloseUpDuration = TimeSpan.FromMilliseconds(220);
    private static readonly IEasingFunction MotionEase = new CubicEase { EasingMode = EasingMode.EaseOut };

    private readonly List<MotionTween> _tweens = [];
    private bool _motionHooked;
    private FrameClock _motionClock = new();
    private Action? _motionCleanup;
    private StackCard[]? _stackCards;
    /// <summary>列表里移除之后、下面的行补位之前，数量变化引起的刷新不改窗口大小；补位后窗口再平滑变小。</summary>
    private bool _holdSize;

    /// <summary>
    /// 正在滑入、退出，内容里有动画，或正在读取拖入的内容（读完紧接着就是落下的动画）：
    /// 这时界面线程不适合再做别的重活，比如创建备用窗口、回收内存。
    /// </summary>
    public bool IsAnimating => IsSliding || _motionHooked || _receiving > 0 || _closing;

    /// <summary>一段逐帧动画：从 Start 起等 Delay，再在 Duration 内推进。</summary>
    private sealed record MotionTween(TimeSpan Start, TimeSpan Delay, TimeSpan Duration, Action<double> Apply, Action? Completed);

    /// <summary>牌在置物架里的姿态（DIP，相对所在的画布）：中心位置、角度、缩放和透明度。与拖动历史卡片用的 ScreenCardPose 不同。</summary>
    private readonly record struct LocalCardPose(double X, double Y, double Angle, double Scale = 1, double Opacity = 1)
    {
        public static LocalCardPose Lerp(LocalCardPose from, LocalCardPose to, double t) => new(
            from.X + (to.X - from.X) * t, from.Y + (to.Y - from.Y) * t, from.Angle + (to.Angle - from.Angle) * t,
            from.Scale + (to.Scale - from.Scale) * t, from.Opacity + (to.Opacity - from.Opacity) * t);
    }

    /// <summary>堆叠里的一张牌和它在 XAML 中的原位。</summary>
    private sealed class StackCard(ContentControl card, ScaleTransform scale, RotateTransform turn, TranslateTransform shift, LocalCardPose home)
    {
        // 牌在画布上的位置和大小是固定的，中心只算一次，不用每帧再读。
        private readonly double _centerX = Canvas.GetLeft(card) + card.Width / 2, _centerY = Canvas.GetTop(card) + card.Height / 2;

        public ContentControl Card { get; } = card;
        public LocalCardPose Home { get; } = home;

        /// <summary>按中心摆放：牌在画布上的位置不变，平移量把中心移到目标点，旋转和缩放都绕中心。</summary>
        public void Pose(LocalCardPose pose)
        {
            shift.X = pose.X - _centerX;
            shift.Y = pose.Y - _centerY;
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
            var home = new LocalCardPose(Canvas.GetLeft(card) + card.Width / 2 + shift.X, Canvas.GetTop(card) + card.Height / 2 + shift.Y, angle);
            return new StackCard(card, scale, turn, shift, home);
        }
    }

    /// <summary>从上到下：最上面一张、第二张、最下面一张。</summary>
    private StackCard[] StackCards => _stackCards ??= [StackCard.From(Card0), StackCard.From(Card1), StackCard.From(Card2)];

    /// <summary>堆叠里的牌都回到原位。</summary>
    private void RestCards()
    {
        foreach (var card in StackCards) card.Pose(card.Home);
    }

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
                var from = new LocalCardPose(start.X, start.Y, -8, 0.5, 0);
                var control = new WpfPoint((from.X + card.Home.X) / 2, Math.Min(from.Y, card.Home.Y) - 20);
                var landed = count - i;
                var top = i == 0;
                Tween(delay, LandDuration, t => card.Pose(Arc(from, card.Home, control, t)), () => CardLanded(landed, top));
                end = Max(end, delay + LandDuration);
            }
            else
            {
                var from = cards[i - incoming].Home;
                Tween(PushDelay, PushDuration, t => card.Pose(LocalCardPose.Lerp(from, card.Home, t)));
                end = Max(end, PushDelay + PushDuration);
            }
        }

        _motionCleanup = () =>
        {
            RestCards();
            RestoreStackMark();
        };
        FinishAfter(end);
    }

    /// <summary>落下的路径：位置沿二次贝塞尔曲线划一道弧，缩放途中略微超过原大小，开头很快变得不透明。</summary>
    private static LocalCardPose Arc(LocalCardPose from, LocalCardPose to, WpfPoint control, double t)
    {
        var u = 1 - t;
        return new LocalCardPose(
            u * u * from.X + 2 * u * t * control.X + t * t * to.X,
            u * u * from.Y + 2 * u * t * control.Y + t * t * to.Y,
            from.Angle + (to.Angle - from.Angle) * t,
            from.Scale + (to.Scale - from.Scale) * t + 0.06 * Math.Sin(Math.PI * t),
            Math.Min(1, t / 0.15));
    }

    /// <summary>
    /// 列表状态下拖入：新文件的图标从松手处划一道弧飞到自己那一行的行首，快落下时这一行的文字从左边滑出。
    /// 一次拖入多个时最多 3 个依次落下，最后一个最后落下；新项目在列表末尾，看不到时先滚过去。
    /// </summary>
    private void PlayListLanding(WpfPoint drop, int added)
    {
        FinishMotion();
        var items = ViewModel.Items;
        var incoming = Math.Min(Math.Min(added, items.Count), 3);
        if (incoming == 0) return;

        ItemsList.UpdateLayout();
        ItemsList.ScrollIntoView(items[^1]);
        ItemsList.UpdateLayout();
        var rows = new List<(FrameworkElement Thumb, FrameworkElement Text)>();
        var end = TimeSpan.Zero;
        for (var i = 0; i < incoming; i++)
        {
            if (ItemsList.ItemContainerGenerator.ContainerFromIndex(items.Count - 1 - i) is not ListBoxItem row ||
                FindNamed(row, "RowThumb") is not { } thumb || FindNamed(row, "RowText") is not { } text) continue;
            // 位置用相对行首的平移表示：从松手处出发，略大于行首图标，落到原位。
            var center = thumb.TranslatePoint(new WpfPoint(thumb.ActualWidth / 2, thumb.ActualHeight / 2), this);
            var from = new LocalCardPose(drop.X - center.X, drop.Y - center.Y, -8, 1.5, 0);
            var home = new LocalCardPose(0, 0, 0);
            var control = new WpfPoint(from.X / 2, Math.Min(from.Y, 0) - 20);
            var (scale, turn, shift) = (new ScaleTransform(), new RotateTransform(), new TranslateTransform());
            var slide = new TranslateTransform();
            thumb.RenderTransformOrigin = new WpfPoint(0.5, 0.5);
            thumb.RenderTransform = new TransformGroup { Children = { scale, turn, shift } };
            text.RenderTransform = slide;
            rows.Add((thumb, text));

            var delay = LandStagger * (incoming - 1 - i);
            Tween(delay, LandDuration, t =>
            {
                var pose = Arc(from, home, control, t);
                shift.X = pose.X;
                shift.Y = pose.Y;
                turn.Angle = pose.Angle;
                scale.ScaleX = scale.ScaleY = pose.Scale;
                thumb.Opacity = pose.Opacity;
            });
            var textDelay = delay + LandDuration / 2;
            Tween(textDelay, RowFadeDuration, t =>
            {
                text.Opacity = t;
                slide.X = -10 * (1 - t);
            });
            end = Max(end, textDelay + RowFadeDuration);
        }

        _motionCleanup = () =>
        {
            foreach (var (thumb, text) in rows)
            {
                foreach (var property in new[] { RenderTransformProperty, RenderTransformOriginProperty, OpacityProperty })
                    thumb.ClearValue(property);
                text.ClearValue(RenderTransformProperty);
                text.ClearValue(OpacityProperty);
            }
        };
        FinishAfter(end);
    }

    /// <summary>
    /// 移除项目：列表里要移除的行向左滑出并淡出，堆叠里要移除的牌缩小、往下沉一点、略微转开并淡出，然后才真正移除。
    /// 之后列表里下面的行滑上来补位，窗口再平滑变小；堆叠里下面的牌升上来补位，新露出来的从最下面淡入。
    /// 动画途中被打断（再次移除、拖入、切换视图、关闭等）时立即移除。关闭了动画效果、看不见或 animate 为 false 时直接移除。
    /// </summary>
    private void RemoveWithMotion(IReadOnlyList<ShelfItem> items, Action remove, bool animate = true)
    {
        FinishMotion();
        var leaving = items.Where(ViewModel.Items.Contains).ToHashSet();
        if (!animate || !ShelfMotion.Enabled || !IsVisible || leaving.Count == 0)
        {
            remove();
            return;
        }
        var remains = leaving.Count < ViewModel.Count;
        if (ViewModel.IsExpanded) RemoveRows(leaving, remove, remains);
        else RemoveFromStack(leaving, remove, remains);
    }

    private void RemoveRows(HashSet<ShelfItem> leaving, Action remove, bool remains)
    {
        ItemsList.UpdateLayout();
        var rows = leaving.Select(item => ItemsList.ItemContainerGenerator.ContainerFromItem(item)).OfType<ListBoxItem>().ToList();
        // 剩下的行移除前的位置，移除后按它补位。
        var before = new Dictionary<ShelfItem, double>();
        foreach (var item in ViewModel.Items)
            if (!leaving.Contains(item) && ItemsList.ItemContainerGenerator.ContainerFromItem(item) is ListBoxItem row)
                before[item] = row.TranslatePoint(default, ItemsList).Y;

        _motionCleanup = () =>
        {
            foreach (var row in rows)
                foreach (var property in new[] { OpacityProperty, RenderTransformProperty, IsHitTestVisibleProperty }) row.ClearValue(property);
            _holdSize = remains;
            remove();
            // 数量变化后的刷新（RefreshViews）已经排在前面，等它按新的数量排好列表再补位。
            if (remains) Dispatcher.BeginInvoke(() => CloseUpRows(before));
        };
        foreach (var row in rows)
        {
            TakeDimmed(row);
            var (start, shift) = (row.Opacity, new TranslateTransform());
            row.RenderTransform = shift;
            row.IsHitTestVisible = false;
            Tween(TimeSpan.Zero, RemoveDuration, t =>
            {
                row.Opacity = start * (1 - t);
                shift.X = -12 * t;
            });
        }
        FinishAfter(RemoveDuration);
    }

    /// <summary>列表移除之后补位：每一行从移除前的位置滑到现在的位置，之前没有生成的行从下面一点淡入；补完窗口再平滑变小。</summary>
    private void CloseUpRows(Dictionary<ShelfItem, double> before)
    {
        _holdSize = false;
        if (IsClosing || !IsVisible || !ViewModel.IsExpanded)
        {
            if (!IsClosing) UpdateWindowSize();
            return;
        }
        FinishMotion();
        ItemsList.UpdateLayout();
        var moved = new List<ListBoxItem>();
        foreach (var item in ViewModel.Items)
        {
            if (ItemsList.ItemContainerGenerator.ContainerFromItem(item) is not ListBoxItem row) continue;
            var appears = !before.TryGetValue(item, out var then);
            var from = appears ? 8 : then - row.TranslatePoint(default, ItemsList).Y;
            if (Math.Abs(from) < 0.5) continue;
            var shift = new TranslateTransform(0, from);
            row.RenderTransform = shift;
            moved.Add(row);
            Tween(TimeSpan.Zero, CloseUpDuration, t =>
            {
                shift.Y = from * (1 - t);
                if (appears) row.Opacity = t;
            });
        }
        var lead = moved.Count > 0 ? CloseUpDuration : TimeSpan.Zero;
        TweenWindowSize(lead);
        _motionCleanup = () =>
        {
            foreach (var row in moved)
            {
                row.ClearValue(RenderTransformProperty);
                row.ClearValue(OpacityProperty);
            }
            UpdateWindowSize();
        };
        FinishAfter(lead + ResizeDuration);
    }

    private void RemoveFromStack(HashSet<ShelfItem> leaving, Action remove, bool remains)
    {
        var cards = StackCards;
        var shown = cards.Select(card => card.Card.DataContext as ShelfItem).ToArray();
        var going = cards.Where(card => card.Card.DataContext is ShelfItem item && leaving.Contains(item)).ToList();
        if (going.Count == 0)
        {
            // 移除的都不在最上面三张里，堆叠看起来不变。
            remove();
            return;
        }

        _motionCleanup = () =>
        {
            RestCards();
            remove();
            if (remains) Dispatcher.BeginInvoke(() => RiseStack(shown));
        };
        for (var i = 0; i < going.Count; i++)
        {
            var card = going[i];
            TakeDimmed(card.Card);
            var (start, side, home) = (card.Card.Opacity, i % 2 == 0 ? -1 : 1, card.Home);
            Tween(TimeSpan.Zero, RemoveDuration, t =>
                card.Pose(new LocalCardPose(home.X, home.Y + 10 * t, home.Angle + side * 6 * t, 1 - 0.3 * t, start * (1 - t))));
        }
        FinishAfter(RemoveDuration);
    }

    /// <summary>堆叠移除之后补位：每张牌从它原来所在的那一层升到现在这一层，新露出来的从最下面一层淡入。</summary>
    private void RiseStack(ShelfItem?[] shown)
    {
        if (IsClosing || !IsVisible || ViewModel.IsExpanded) return;
        FinishMotion();
        var cards = StackCards;
        var moved = false;
        for (var i = 0; i < cards.Length; i++)
        {
            var card = cards[i];
            if (card.Card.DataContext is not ShelfItem item) continue;
            var then = Array.IndexOf(shown, item);
            if (then == i) continue;
            var from = then > i ? cards[then].Home : cards[^1].Home with { Y = cards[^1].Home.Y + 6, Scale = 0.94, Opacity = 0 };
            Tween(TimeSpan.Zero, CloseUpDuration, t => card.Pose(LocalCardPose.Lerp(from, card.Home, t)));
            moved = true;
        }
        if (!moved) return;
        _motionCleanup = RestCards;
        FinishAfter(CloseUpDuration);
    }

    /// <summary>
    /// 堆叠和列表之间切换：堆叠保持收起时的大小留在左上角，牌飞到列表里自己那一行并缩成行首图标，各行随后从左边滑出；
    /// 收起时各行淡出，图标飞回去叠成一摞。展开时窗口先平滑变大再开始，收起时等这些都结束了窗口再平滑变小。
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
        var listSize = toList ? bodySize : new WpfSize(ItemsList.ActualWidth, ItemsList.ActualHeight);
        var listHeight = listSize.Height;

        UpdateViews();
        StackView.Visibility = Visibility.Visible;
        ItemsList.Visibility = Visibility.Visible;
        // 堆叠和列表都固定为各自显示时的大小，窗口比它们大或小时只是露出或裁掉一部分。
        Pin(StackView, stackSize);
        Pin(ItemsList, listSize);
        StackView.IsHitTestVisible = false;
        Panel.SetZIndex(StackView, 1);
        Body.UpdateLayout();

        var cards = StackCards;
        var thumbs = new List<UIElement>();
        var rows = new List<ListBoxItem>();
        var lead = toList ? ResizeDuration : TimeSpan.Zero;
        var end = TimeSpan.Zero;
        for (var index = 0; index < ViewModel.Items.Count; index++)
            if (ItemsList.ItemContainerGenerator.ContainerFromIndex(index) is ListBoxItem row) rows.Add(row);

        for (var i = 0; i < cards.Length; i++)
        {
            var card = cards[i];
            if (card.Card.DataContext is not ShelfItem item) continue;
            var row = ItemsList.ItemContainerGenerator.ContainerFromItem(item) as ListBoxItem;
            var thumb = row is null ? null : FindNamed(row, "RowThumb");
            LocalCardPose target;
            var landsOnRow = thumb is not null && thumb.TransformToVisual(ItemsList).TransformBounds(new Rect(thumb.RenderSize)) is var bounds &&
                bounds.Top >= 0 && bounds.Bottom <= listHeight;
            if (landsOnRow)
            {
                var box = thumb!.TransformToVisual(StackCanvas).TransformBounds(new Rect(thumb.RenderSize));
                target = new LocalCardPose(box.X + box.Width / 2, box.Y + box.Height / 2, 0, box.Height / Math.Max(card.Card.Width, card.Card.Height));
                thumb.Opacity = 0;
                thumbs.Add(thumb);
            }
            else
            {
                // 这一行在列表可见范围之外：牌朝列表底边飞去并淡出，或从那里淡入。
                var edge = ItemsList.TranslatePoint(new WpfPoint(22, listHeight), StackCanvas);
                target = new LocalCardPose(edge.X, edge.Y, 0, 0.3, 0);
            }

            var delay = lead + FlyStagger * (toList ? i : cards.Length - 1 - i);
            var (from, to) = toList ? (card.Home, target) : (target, card.Home);
            Tween(delay, FlyDuration, t =>
            {
                var pose = LocalCardPose.Lerp(from, to, t);
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
                var delay = lead + RowFadeDelay + FlyStagger * order;
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

        var fadeDelay = toList ? lead : FlyDuration - FadeOutDuration;
        Tween(fadeDelay, toList ? FadeOutDuration : RowFadeDuration, t =>
        {
            StackCaptionPanel.Opacity = StackMark.Opacity = toList ? 1 - t : t;
        });
        end = Max(end, fadeDelay + RowFadeDuration);

        // 展开时上面的动画都推迟到窗口变大之后；收起时牌是从列表里各行的位置飞回来的，等它们都回到堆叠里再把窗口变小。
        TweenWindowSize(toList ? TimeSpan.Zero : end);
        if (!toList) end += ResizeDuration;

        _motionCleanup = () =>
        {
            RestCards();
            foreach (var thumb in thumbs) thumb.ClearValue(OpacityProperty);
            foreach (var row in rows)
            {
                row.ClearValue(OpacityProperty);
                row.ClearValue(RenderTransformProperty);
            }
            StackCaptionPanel.ClearValue(OpacityProperty);
            RestoreStackMark();
            Unpin(StackView);
            Unpin(ItemsList);
            StackView.ClearValue(IsHitTestVisibleProperty);
            StackView.ClearValue(Panel.ZIndexProperty);
            UpdateViews();
            UpdateWindowSize();
        };
        FinishAfter(end);
    }

    /// <summary>逐帧调用 apply，参数是缓动后的进度 0–1；delay 之前停在起点。</summary>
    private void Tween(TimeSpan delay, TimeSpan duration, Action<double> apply, Action? completed = null)
    {
        apply(0);
        // 没有动画在播时时钟从头开始，新动画从它的第一帧起算。
        if (!_motionHooked) _motionClock = new FrameClock();
        _tweens.Add(new MotionTween(_motionClock.Elapsed, delay, duration, apply, completed));
        if (_motionHooked) return;
        _motionHooked = true;
        CompositionTarget.Rendering += OnMotionFrame;
    }

    /// <summary>过了 end，整段动画结束：牌、列表和窗口都回到最终状态。</summary>
    private void FinishAfter(TimeSpan end) => Tween(TimeSpan.Zero, end, _ => { }, FinishMotion);

    /// <summary>所有动画共用一个逐帧回调：同一帧重复触发时跳过，还在等待的动画停在起点不更新；这一帧里各段动画按同一个时刻推进。</summary>
    private void OnMotionFrame(object? sender, EventArgs e)
    {
        if (!_motionClock.Tick(e)) return;
        var now = _motionClock.Elapsed;
        for (var i = 0; i < _tweens.Count; i++)
        {
            var tween = _tweens[i];
            var elapsed = now - tween.Start - tween.Delay;
            if (elapsed < TimeSpan.Zero) continue;
            var progress = tween.Duration <= TimeSpan.Zero ? 1 : Math.Min(elapsed / tween.Duration, 1);
            tween.Apply(MotionEase.Ease(progress));
            if (progress < 1) continue;
            _tweens.RemoveAt(i--);
            // 收尾可能直接结束全部动画并清空列表，循环随之结束。
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

    /// <summary>立即结束进行中的动画：牌、列表和窗口都回到最终状态。</summary>
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
