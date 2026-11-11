using System.Diagnostics;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views.Controls;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

public partial class ShelfWindow
{
    // ───────────── 出场和离场 ─────────────

    /// <summary>
    /// 备用窗口在空闲时先到屏幕外显示一次，画出第一帧再隐藏：模板、排版和第一次绘制都在这时做完，
    /// 晃动弹出时显示窗口只剩少量工作。隐藏后才交给 ready；第一帧迟迟画不出来时最多等一秒。
    /// </summary>
    public void Prewarm(Action ready)
    {
        WindowEffects.Handle(this);
        var finished = false;
        void Finish()
        {
            if (finished) return;
            finished = true;
            ContentRendered -= OnRendered;
            if (_closed) return;
            Hide();
            ready();
        }
        void OnRendered(object? sender, EventArgs e) => Dispatcher.BeginInvoke(DispatcherPriority.ContextIdle, new Action(Finish));
        ContentRendered += OnRendered;
        Left = Top = -32000;
        Show();
        _ = Task.Delay(1000).ContinueWith(_ => Dispatcher.BeginInvoke(new Action(Finish)), TaskScheduler.Default);
    }

    /// <summary>
    /// 正在替置物架出场或离场的那张牌。牌和置物架的交接都在这一部分处理，牌有三种结束方式：
    /// 正常播完，出场时置物架现身，离场时关闭窗口；出场途中置物架又被复用或要关闭，见 FinishEntrance，牌立即撤下、置物架现身；
    /// 置物架窗口已经关闭，见 CancelCard，剩下的动画直接取消，不再回调置物架。
    /// </summary>
    private ShelfCardWindow? _card;
    /// <summary>出场时那张牌是上下翻开的，关闭时按同样的方向翻回去。</summary>
    private bool _flippedOnHorizontalAxis;

    /// <summary>以屏幕像素坐标为中心显示置物架。</summary>
    public void ShowAt(POINT center, bool activate, ShelfEntrance entrance = default)
    {
        ShowActivated = activate;
        var wasVisible = IsVisible;
        var trace = wasVisible ? null : FrameTrace.Start($"Shelf open {entrance.Kind}");
        var started = Stopwatch.GetTimestamp();
        // 复用正在出场的置物架时先让它落定，否则下一帧会把窗口拉回原来的位置。
        if (wasVisible)
        {
            StopSlide();
            FinishEntrance();
        }
        // 晃动和边缘呼出由一张牌替置物架出场：置物架先隐身，照常显示和绘制，牌落定时直接现身。
        var dealt = !wasVisible && ShelfMotion.Enabled && entrance.Kind != ShelfEntranceKind.Plain;
        if (dealt) WindowEffects.SetCloaked(this, true);
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (!wasVisible) Show();
        trace?.Mark("show", started);
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (activate)
        {
            Activate();
            Focus();
        }
        if (wasVisible) return;
        _entrance = entrance;
        _openTrace = trace;
        var animating = Stopwatch.GetTimestamp();
        if (dealt) DealIn(center);
        else AnimateIn();
        trace?.Mark("animate", animating);
    }

    /// <summary>诊断日志打开时记下出场动画的帧，见 FrameTrace。</summary>
    private FrameTrace? _openTrace;

    private void EndOpenTrace(string ending)
    {
        _openTrace?.End(ending);
        _openTrace = null;
    }

    /// <summary>关闭：一张牌先盖住置物架，置物架隐身后牌再离场，播完才真正关闭窗口。</summary>
    public void CloseAnimated()
    {
        if (_closing || _closed) return;
        _closing = true;
        IsHitTestVisible = false;
        FinishEntrance();
        StopSlide();
        StopPop();
        // 发牌、移除、补位和窗口大小的动画停在这一帧：截下的牌面和屏幕上一样，置物架隐身后也不再逐帧白做。没做完的收尾（比如待移除的项目）在窗口关闭时补上。
        StopMotion();
        if (!ShelfMotion.Enabled || !IsVisible || !NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect) ||
            CaptureSurface() is not { } face)
        {
            Close();
            return;
        }

        var dpiScale = DpiScale();
        var work = WorkArea(rect);
        var card = _card = ShelfCardWindow.Rent();
        card.Play(rect, work, dpiScale, face, ShelfMotion.Exit(_entrance, _flippedOnHorizontalAxis, rect, work, dpiScale),
            () => WindowEffects.SetCloaked(this, true), () =>
            {
                _card = null;
                Close();
            });
    }

    /// <summary>
    /// 主动打开时的弹出动画：窗口从下方升起一点（背景随窗口一起移动），内容同时淡入并放大到原尺寸。
    /// 低性能模式下窗口直接出现在原处，内容在窗口里升起同样一段：不用逐帧移动窗口。
    /// </summary>
    private void AnimateIn()
    {
        StopSlide();
        StopPop();
        if (!ShelfMotion.Enabled)
        {
            Surface.BeginAnimation(OpacityProperty, null);
            Surface.Opacity = 1;
            EndOpenTrace("instant");
            return;
        }

        var ease = new QuinticEase { EasingMode = EasingMode.EaseOut };
        if (!LowPerformance.Enabled && NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect))
            SlideWindow(rect.Left, rect.Top + ShelfMotion.RiseOffset(DpiScale()), rect.Left, rect.Top, ShelfMotion.RiseDuration, ease,
                () => WindowPlacement.KeepInWorkArea(this));
        // 低性能模式下内容在窗口里升起同样一段（逻辑像素），缓动和升起窗口一样。
        var shift = LowPerformance.Enabled ? ShelfMotion.RiseOffset(1) : 0;

        // 淡入、放大和升起用同一种时钟逐帧推进，第一帧耽搁时一起从头开始，见 FrameClock。
        CacheSurface();
        Surface.BeginAnimation(OpacityProperty, null);
        var clock = new FrameClock();
        void Pose()
        {
            var eased = ease.Ease(Math.Min(1, clock.Elapsed / ShelfMotion.RiseDuration));
            Surface.Opacity = MotionEase.Ease(Math.Min(1, clock.Elapsed / ShelfMotion.RiseFadeDuration));
            SurfaceScale.ScaleX = SurfaceScale.ScaleY = ShelfMotion.RiseStartScale + (1 - ShelfMotion.RiseStartScale) * eased;
            if (shift != 0) SurfaceOffset.Y = shift * (1 - eased);
        }
        Pose();
        _popFrame = (_, e) =>
        {
            // 低性能模式下高刷新率屏幕上隔一帧更新，跳过的帧不推进时钟，见 LowPerformance.Skip。
            if (LowPerformance.Skip(((RenderingEventArgs)e).RenderingTime) || !clock.Tick(e)) return;
            Pose();
            if (clock.Elapsed < ShelfMotion.RiseDuration) return;
            StopPop();
            Surface.CacheMode = null;
            EndOpenTrace("done");
        };
        CompositionTarget.Rendering += _popFrame;
    }

    private EventHandler? _popFrame;

    /// <summary>出场动画还在播。这期间拖入的反馈从简，见 OnDragEnter、OnDragLeave 和 UpdateDrag。</summary>
    private bool IsPopping => _popFrame is not null;

    private void StopPop()
    {
        if (_popFrame is null) return;
        CompositionTarget.Rendering -= _popFrame;
        _popFrame = null;
    }

    /// <summary>
    /// 晃动或从边缘呼出时的出场：截下置物架作为牌面，置物架隐身期间由牌表演，落定时置物架现身。
    /// 晃动时牌从光标处翻开，光标离置物架中心太远时按靠近中心的位置算，牌不会从老远飞过来。
    /// </summary>
    private void DealIn(POINT cursor)
    {
        Surface.BeginAnimation(OpacityProperty, null);
        Surface.Opacity = 1;
        var capturing = Stopwatch.GetTimestamp();
        if (!NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect) || CaptureSurface() is not { } face)
        {
            WindowEffects.SetCloaked(this, false);
            EndOpenTrace("no capture");
            return;
        }
        _openTrace?.Mark("capture", capturing);

        var dpiScale = DpiScale();
        var work = WorkArea(rect);
        var offset = new Vector(
            Math.Clamp(cursor.X - (rect.Left + rect.Width / 2.0), -0.35 * rect.Width, 0.35 * rect.Width) / dpiScale,
            Math.Clamp(cursor.Y - (rect.Top + rect.Height / 2.0), -0.35 * rect.Height, 0.35 * rect.Height) / dpiScale);
        var motion = _entrance.Kind == ShelfEntranceKind.Shake ? ShelfMotion.ShakeIn(offset) : ShelfMotion.EdgeIn(_entrance.Edge, rect, work, dpiScale);
        _flippedOnHorizontalAxis = motion.HorizontalAxis;
        var card = _card = ShelfCardWindow.Rent();
        card.Play(rect, work, dpiScale, face, motion, null, () =>
        {
            _card = null;
            WindowEffects.SetCloaked(this, false);
            EndOpenTrace("done");
        });
    }

    /// <summary>出场的牌还没落定时直接让置物架现身，例如途中又被复用或要关闭。</summary>
    private void FinishEntrance()
    {
        if (_card is not { } card) return;
        _card = null;
        WindowEffects.SetCloaked(this, false);
        card.Cancel();
    }

    /// <summary>窗口关闭时撤下还在播放的牌。</summary>
    private void CancelCard()
    {
        _card?.Cancel();
        _card = null;
    }

    /// <summary>按设备像素截下置物架现在的样子，作为出场和离场那张牌的正面。</summary>
    private BitmapSource? CaptureSurface()
    {
        UpdateLayout();
        return Root.ActualWidth < 1 || Root.ActualHeight < 1 ? null : VisualCapture.Of(Root);
    }

    private static RECT WorkArea(RECT rect) =>
        WindowPlacement.GetMonitor(new POINT(rect.Left + rect.Width / 2, rect.Top + rect.Height / 2)).WorkArea;

    /// <summary>
    /// 缩放和淡入淡出期间把内容缓存为位图，每帧只需合成，不必重新绘制虚线边框、阴影和文字。
    /// 按所在显示器的缩放比例缓存，高分屏上也保持清晰，见 CacheScale。
    /// </summary>
    private void CacheSurface() => Surface.CacheMode = new BitmapCache(CacheScale.For(this));

    /// <summary>逐帧移动整个窗口。只改变位置不改变大小，系统材质不需要重新计算，移动很流畅。</summary>
    private void SlideWindow(int fromX, int fromY, int toX, int toY, TimeSpan duration, IEasingFunction ease, Action? completed = null)
    {
        StopSlide();
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero || (fromX == toX && fromY == toY))
        {
            completed?.Invoke();
            return;
        }

        MoveWindow(handle, fromX, fromY);
        var clock = new FrameClock();
        var (x, y) = (fromX, fromY);
        _slideFrame = (_, e) =>
        {
            // 取整后位置没变时不必移动窗口。
            if (!clock.Tick(e)) return;
            var progress = Math.Min(1, clock.Elapsed / duration);
            var eased = ease.Ease(progress);
            var (nextX, nextY) = ((int)Math.Round(fromX + (toX - fromX) * eased), (int)Math.Round(fromY + (toY - fromY) * eased));
            if (nextX != x || nextY != y) MoveWindow(handle, nextX, nextY);
            (x, y) = (nextX, nextY);
            if (progress < 1) return;
            StopSlide();
            completed?.Invoke();
        };
        CompositionTarget.Rendering += _slideFrame;
    }

    private void StopSlide()
    {
        if (_slideFrame is null) return;
        CompositionTarget.Rendering -= _slideFrame;
        _slideFrame = null;
    }

    /// <summary>窗口正在滑入，或者有牌在替它出场、离场。</summary>
    public bool IsSliding => _slideFrame is not null || _card is not null;

    private static void MoveWindow(IntPtr handle, int x, int y) =>
        NativeMethods.SetWindowPos(handle, IntPtr.Zero, x, y, 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);

    private double DpiScale() => VisualTreeHelper.GetDpi(this).DpiScaleX;
}
