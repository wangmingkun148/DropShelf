using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

public partial class ShelfWindow
{
    // ───────────── 出场和离场 ─────────────

    /// <summary>以屏幕像素坐标为中心显示置物架。</summary>
    public void ShowAt(POINT center, bool activate, ShelfEntrance entrance = default)
    {
        ShowActivated = activate;
        var wasVisible = IsVisible;
        // 复用正在滑入的置物架时停止动画，否则下一帧会把窗口拉回原来的位置。
        if (wasVisible) StopSlide();
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (!wasVisible) Show();
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (activate)
        {
            Activate();
            Focus();
        }
        if (!wasVisible)
        {
            _entrance = entrance;
            AnimateIn(center);
        }
    }

    public void CloseAnimated()
    {
        if (_closing || _closed) return;
        _closing = true;
        IsHitTestVisible = false;
        if (!ShelfMotion.Enabled)
        {
            Close();
            return;
        }

        var duration = ShelfMotion.CloseDuration;
        var ease = new CubicEase { EasingMode = EasingMode.EaseIn };
        CacheSurface();
        var fade = new DoubleAnimation(0, duration) { EasingFunction = ease };
        fade.Completed += (_, _) => Close();
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(ShelfMotion.CloseScale, duration) { EasingFunction = ease });
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(ShelfMotion.CloseScale, duration) { EasingFunction = ease });
        Surface.BeginAnimation(OpacityProperty, fade);

        // 窗口本身也朝来时的方向退回一小段，避免内容消失后只剩空的背景。
        if (NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect))
        {
            var (dx, dy) = ShelfMotion.CloseOffset(_entrance, DpiScale());
            SlideWindow(rect.Left, rect.Top, rect.Left + dx, rect.Top + dy, duration, ease);
        }
    }

    /// <summary>
    /// 弹出动画：窗口从来源方向滑入（亚克力背景随窗口一起移动），内容同时淡入并放大到原尺寸。
    /// 晃动呼出时内容从光标处弹出，带一点回弹；从屏幕边缘呼出时从该边缘滑入。
    /// </summary>
    private void AnimateIn(POINT cursor)
    {
        StopSlide();
        if (!ShelfMotion.Enabled)
        {
            Surface.BeginAnimation(OpacityProperty, null);
            Surface.Opacity = 1;
            return;
        }

        var handle = new WindowInteropHelper(this).Handle;
        var motion = ShelfMotion.For(_entrance);
        if (NativeMethods.GetWindowRect(handle, out var rect))
        {
            // 内容从光标所在的位置展开。
            Surface.RenderTransformOrigin = _entrance.Kind == ShelfEntranceKind.Shake && rect.Width > 0 && rect.Height > 0
                ? new WpfPoint(Math.Clamp((cursor.X - rect.Left) / (double)rect.Width, 0.15, 0.85),
                    Math.Clamp((cursor.Y - rect.Top) / (double)rect.Height, 0.15, 0.85))
                : new WpfPoint(0.5, 0.4);
            var (dx, dy) = ShelfMotion.OpenOffset(_entrance, DpiScale());
            SlideWindow(rect.Left + dx, rect.Top + dy, rect.Left, rect.Top, motion.Duration, new QuinticEase { EasingMode = EasingMode.EaseOut },
                () => WindowPlacement.KeepInWorkArea(this));
        }

        CacheSurface();
        Surface.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, motion.FadeDuration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
        var scale = new DoubleAnimation(motion.StartScale, 1, motion.Duration) { EasingFunction = motion.ScaleEase };
        scale.Completed += (_, _) => Surface.CacheMode = null;
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, scale);
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, scale);
    }

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

    /// <summary>窗口正在滑入或退出。</summary>
    public bool IsSliding => _slideFrame is not null;

    private static void MoveWindow(IntPtr handle, int x, int y) =>
        NativeMethods.SetWindowPos(handle, IntPtr.Zero, x, y, 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);

    private double DpiScale() => VisualTreeHelper.GetDpi(this).DpiScaleX;
}
