using System.Diagnostics;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
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
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (activate)
        {
            Activate();
            Focus();
        }
        if (wasVisible) return;
        _entrance = entrance;
        if (dealt) DealIn(center);
        else AnimateIn();
    }

    /// <summary>关闭：一张牌先盖住置物架，置物架隐身后牌再离场，播完才真正关闭窗口。</summary>
    public void CloseAnimated()
    {
        if (_closing || _closed) return;
        _closing = true;
        IsHitTestVisible = false;
        FinishEntrance();
        StopSlide();
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

    /// <summary>主动打开时的弹出动画：窗口从下方升起一点（背景随窗口一起移动），内容同时淡入并放大到原尺寸。</summary>
    private void AnimateIn()
    {
        StopSlide();
        if (!ShelfMotion.Enabled)
        {
            Surface.BeginAnimation(OpacityProperty, null);
            Surface.Opacity = 1;
            return;
        }

        if (NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect))
            SlideWindow(rect.Left, rect.Top + ShelfMotion.RiseOffset(DpiScale()), rect.Left, rect.Top, ShelfMotion.RiseDuration,
                new QuinticEase { EasingMode = EasingMode.EaseOut }, () => WindowPlacement.KeepInWorkArea(this));

        CacheSurface();
        Surface.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, ShelfMotion.RiseFadeDuration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
        var scale = new DoubleAnimation(ShelfMotion.RiseStartScale, 1, ShelfMotion.RiseDuration) { EasingFunction = new QuinticEase { EasingMode = EasingMode.EaseOut } };
        scale.Completed += (_, _) => Surface.CacheMode = null;
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, scale);
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, scale);
    }

    /// <summary>
    /// 晃动或从边缘呼出时的出场：截下置物架作为牌面，置物架隐身期间由牌表演，落定时置物架现身。
    /// 晃动时牌从光标处翻开，光标离置物架中心太远时按靠近中心的位置算，牌不会从老远飞过来。
    /// </summary>
    private void DealIn(POINT cursor)
    {
        Surface.BeginAnimation(OpacityProperty, null);
        Surface.Opacity = 1;
        if (!NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect) || CaptureSurface() is not { } face)
        {
            WindowEffects.SetCloaked(this, false);
            return;
        }

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
        var clock = Stopwatch.StartNew();
        var (x, y, frame) = (fromX, fromY, TimeSpan.Zero);
        _slideFrame = (_, e) =>
        {
            // 同一帧可能触发多次；取整后位置没变时也不必移动窗口。
            var time = ((RenderingEventArgs)e).RenderingTime;
            if (time == frame) return;
            frame = time;
            var progress = Math.Min(1, clock.Elapsed.TotalMilliseconds / duration.TotalMilliseconds);
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
