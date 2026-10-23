using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using DropShelf.Models;

namespace DropShelf.Interop;

internal enum SystemBackdrop
{
    None = 1,
    Mica = 2,
    Acrylic = 3,
    MicaAlt = 4
}

internal static class WindowEffects
{
    public static int Build => Environment.OSVersion.Version.Build;
    public static bool IsWindows11 => Build >= 22000;
    public static bool SupportsSystemBackdrop => Build >= 22621;

    public static IntPtr Handle(Window window) => new WindowInteropHelper(window).EnsureHandle();

    /// <summary>
    /// 按主题设置窗口深浅色并启用系统材质，旧系统返回 false 并保持纯色背景。
    /// 材质的深浅在启用时确定，之后只改深浅色不会重绘，所以先设深浅色，主题切换时再调用一次重新启用材质。
    /// </summary>
    public static bool ApplyTheme(Window window, SystemBackdrop backdrop, bool dark)
    {
        SetDarkMode(window, dark);
        return TryApplyBackdrop(window, backdrop);
    }

    private static bool TryApplyBackdrop(Window window, SystemBackdrop backdrop)
    {
        if (!SupportsSystemBackdrop || backdrop == SystemBackdrop.None) return false;
        try
        {
            var handle = Handle(window);
            if (HwndSource.FromHwnd(handle)?.CompositionTarget is not { } target) return false;
            target.BackgroundColor = Colors.Transparent;
            var margins = MARGINS.Sheet;
            NativeMethods.DwmExtendFrameIntoClientArea(handle, ref margins);
            var none = (int)SystemBackdrop.None;
            NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_SYSTEMBACKDROP_TYPE, ref none, sizeof(int));
            var value = (int)backdrop;
            return NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_SYSTEMBACKDROP_TYPE, ref value, sizeof(int)) == 0;
        }
        catch
        {
            return false;
        }
    }

    private static void SetDarkMode(Window window, bool dark)
    {
        try
        {
            var handle = new WindowInteropHelper(window).Handle;
            if (handle == IntPtr.Zero) return;
            var value = dark ? 1 : 0;
            if (NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_USE_IMMERSIVE_DARK_MODE, ref value, sizeof(int)) != 0)
                NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, ref value, sizeof(int));
        }
        catch
        {
            // 旧系统不支持深色标题栏时保持默认外观。
        }
    }

    public static void SetRoundedCorners(Window window, bool small = false)
    {
        if (!IsWindows11) return;
        try
        {
            var value = small ? 3 : 2;
            NativeMethods.DwmSetWindowAttribute(Handle(window), NativeMethods.DWMWA_WINDOW_CORNER_PREFERENCE, ref value, sizeof(int));
        }
        catch
        {
        }
    }

    public static void SetBorderColor(Window window, System.Windows.Media.Color? color)
    {
        if (!IsWindows11) return;
        try
        {
            var handle = new WindowInteropHelper(window).Handle;
            if (handle == IntPtr.Zero) return;
            var value = color is { } c ? (uint)(c.R | (c.G << 8) | (c.B << 16)) : NativeMethods.DWMWA_COLOR_DEFAULT;
            NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_BORDER_COLOR, ref value, sizeof(uint));
        }
        catch
        {
        }
    }

    /// <summary>Windows 11 上不画圆角和边框，透明窗口里只看得到内容。</summary>
    public static void SetFrameless(Window window)
    {
        if (!IsWindows11) return;
        try
        {
            var handle = Handle(window);
            var corner = NativeMethods.DWMWCP_DONOTROUND;
            NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_WINDOW_CORNER_PREFERENCE, ref corner, sizeof(int));
            var border = NativeMethods.DWMWA_COLOR_NONE;
            NativeMethods.DwmSetWindowAttribute(handle, NativeMethods.DWMWA_BORDER_COLOR, ref border, sizeof(uint));
        }
        catch
        {
        }
    }

    /// <summary>不在任务栏和 Alt+Tab 中显示；可选不抢占当前程序的焦点。</summary>
    public static void SetToolWindow(Window window, bool noActivate)
    {
        var handle = Handle(window);
        var style = NativeMethods.GetWindowLong(handle, NativeMethods.GWL_EXSTYLE);
        style |= NativeMethods.WS_EX_TOOLWINDOW;
        style &= ~NativeMethods.WS_EX_APPWINDOW;
        if (noActivate) style |= NativeMethods.WS_EX_NOACTIVATE;
        else style &= ~NativeMethods.WS_EX_NOACTIVATE;
        NativeMethods.SetWindowLong(handle, NativeMethods.GWL_EXSTYLE, style);
    }
}

internal static class CacheScale
{
    /// <summary>
    /// 位图缓存的 RenderAtScale。缓存本身已按系统 DPI（主显示器）生成，只需补上元素所在显示器与它之比：
    /// 两者相同时为 1，再乘一次显示缩放比例在 200% 时会占用四倍内存；所在显示器缩放更高时按比例补足，不会发虚。
    /// </summary>
    public static double For(Visual visual) => VisualTreeHelper.GetDpi(visual).PixelsPerInchX / Math.Max(1, NativeMethods.GetDpiForSystem());
}

internal static class WindowPlacement
{
    public static POINT GetCursor() => NativeMethods.GetCursorPos(out var point) ? point : new POINT(0, 0);

    public static (RECT WorkArea, double Scale) GetMonitor(POINT point)
    {
        var monitor = NativeMethods.MonitorFromPoint(point, NativeMethods.MONITOR_DEFAULTTONEAREST);
        var info = new MONITORINFO { Size = System.Runtime.InteropServices.Marshal.SizeOf<MONITORINFO>() };
        NativeMethods.GetMonitorInfo(monitor, ref info);
        var scale = 1.0;
        try
        {
            if (NativeMethods.GetDpiForMonitor(monitor, NativeMethods.MDT_EFFECTIVE_DPI, out var dpiX, out _) == 0 && dpiX > 0)
                scale = dpiX / 96.0;
        }
        catch
        {
        }
        return (info.Work, scale);
    }

    /// <summary>以屏幕像素坐标为中心放置窗口，并保证完整落在该显示器的工作区内。</summary>
    public static void CenterOn(Window window, POINT center, double widthDip, double heightDip)
    {
        var handle = WindowEffects.Handle(window);
        var (work, scale) = GetMonitor(center);
        var width = (int)Math.Round(widthDip * scale);
        var height = (int)Math.Round(heightDip * scale);
        Move(handle, center, work, width, height);

        // 跨越不同缩放比例的显示器时，系统会按新的缩放重新调整窗口大小，需要用实际尺寸再居中一次。
        if (NativeMethods.GetWindowRect(handle, out var actual) && (actual.Width != width || actual.Height != height))
            Move(handle, center, work, actual.Width, actual.Height);
    }

    /// <summary>窗口尺寸变化后把它推回所在显示器的工作区内。</summary>
    public static void KeepInWorkArea(Window window)
    {
        var handle = new WindowInteropHelper(window).Handle;
        if (handle == IntPtr.Zero || !NativeMethods.GetWindowRect(handle, out var rect)) return;
        var (work, _) = GetMonitor(new POINT(rect.Left + rect.Width / 2, rect.Top + Math.Min(rect.Height / 2, 40)));
        var left = Math.Clamp(rect.Left, work.Left, Math.Max(work.Left, work.Right - rect.Width));
        var top = Math.Clamp(rect.Top, work.Top, Math.Max(work.Top, work.Bottom - rect.Height));
        if (left != rect.Left || top != rect.Top)
            NativeMethods.SetWindowPos(handle, IntPtr.Zero, left, top, 0, 0,
                NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);
    }

    private static void Move(IntPtr handle, POINT center, RECT work, int width, int height)
    {
        var placement = QuickShelfPositioner.Calculate(center.X, center.Y, work.Left, work.Top, work.Right, work.Bottom, width, height);
        NativeMethods.SetWindowPos(handle, IntPtr.Zero, (int)Math.Round(placement.Left), (int)Math.Round(placement.Top), 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);
    }

    public static bool IsCursorInside(Window window)
    {
        var handle = new WindowInteropHelper(window).Handle;
        if (handle == IntPtr.Zero || !NativeMethods.GetWindowRect(handle, out var rect)) return false;
        var cursor = GetCursor();
        return cursor.X >= rect.Left && cursor.X < rect.Right && cursor.Y >= rect.Top && cursor.Y < rect.Bottom;
    }
}
