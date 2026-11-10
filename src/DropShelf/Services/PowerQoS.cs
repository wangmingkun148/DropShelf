using System.Runtime.InteropServices;
using System.Windows;
using DropShelf.Interop;

namespace DropShelf.Services;

/// <summary>
/// 使用电池时，Windows 会把看起来在后台的进程调到能效核、按低频运行。置物架不抢焦点，拖文件时前台一直是别的程序，
/// 常被这样对待，弹出和动画因此卡顿。有窗口显示或按住鼠标（随时可能晃动）时声明需要正常性能；
/// 都没有时交还系统决定，空闲时不多耗电。系统不支持时调用失败，保持原样。
/// </summary>
internal static class PowerQoS
{
    private static readonly object Gate = new();
    private static readonly HashSet<object> Holders = [];
    private static bool _high;

    /// <summary>窗口显示期间保持正常性能，隐藏或关闭后释放。</summary>
    public static void Track(Window window)
    {
        window.IsVisibleChanged += (_, _) => Hold(window, window.IsVisible);
        window.Closed += (_, _) => Hold(window, false);
    }

    /// <summary>可在任意线程调用；任何一方需要时整个进程按正常性能运行。</summary>
    public static void Hold(object holder, bool active)
    {
        lock (Gate)
        {
            if (active ? !Holders.Add(holder) : !Holders.Remove(holder)) return;
            var high = Holders.Count > 0;
            if (high == _high) return;
            _high = high;
            // 明确不节能，也不忽略计时器精度请求；Windows 10 不认识后一项，失败时只设前一项。交还时两项都不再指定。
            var state = new NativeMethods.PowerThrottlingState(high ? NativeMethods.POWER_THROTTLING_EXECUTION_SPEED | NativeMethods.PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0, 0);
            if (SetProcess(state) || !high) return;
            SetProcess(new NativeMethods.PowerThrottlingState(NativeMethods.POWER_THROTTLING_EXECUTION_SPEED, 0));
        }
    }

    /// <summary>当前线程只做不急的后台工作：始终节能运行，把性能核和功耗让给界面。</summary>
    public static void BackgroundThread() => SetThread(new NativeMethods.PowerThrottlingState(NativeMethods.POWER_THROTTLING_EXECUTION_SPEED, NativeMethods.POWER_THROTTLING_EXECUTION_SPEED));

    /// <summary>当前线程不再单独指定，跟随进程。</summary>
    public static void FollowProcess() => SetThread(new NativeMethods.PowerThrottlingState(0, 0));

    private static bool SetProcess(NativeMethods.PowerThrottlingState state) =>
        NativeMethods.SetProcessInformation(NativeMethods.GetCurrentProcess(), NativeMethods.ProcessPowerThrottling, ref state, (uint)Marshal.SizeOf<NativeMethods.PowerThrottlingState>());

    private static void SetThread(NativeMethods.PowerThrottlingState state) =>
        NativeMethods.SetThreadInformation(NativeMethods.GetCurrentThread(), NativeMethods.ThreadPowerThrottling, ref state, (uint)Marshal.SizeOf<NativeMethods.PowerThrottlingState>());
}
