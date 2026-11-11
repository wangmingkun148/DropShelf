using System.Windows.Media.Animation;
using DropShelf.Interop;

namespace DropShelf.Services;

/// <summary>
/// 低性能模式（常规页的开关），在动画效果上做几处不显眼的让步，减轻显卡和界面线程的负担：
/// 动画和拖动期间的位图缓存降低精度，见 CacheScale；高刷新率屏幕上动画隔一帧更新；
/// 置物架弹出和收起时窗口不动，内容在窗口里移动。
/// </summary>
internal static class LowPerformance
{
    /// <summary>刷新率不低于这个值时动画隔一帧更新，例如 165Hz 降到 82 帧；更低的刷新率照常每帧更新。</summary>
    private const int PacedRefresh = 100;

    private static TimeSpan _frame, _shown, _interval;
    private static bool _skip;

    public static bool Enabled { get; set; }

    /// <summary>
    /// 逐帧回调里调用：这一帧要不要跳过不更新。各窗口的逐帧回调在同一帧拿到同样的时刻，都跳过同一帧，
    /// 不会这边更新单数帧、那边更新双数帧，结果还是每帧都要画。跟着光标的动画不用它，手感不变。
    /// </summary>
    public static bool Skip(TimeSpan time)
    {
        if (!Enabled) return false;
        if (time == _frame) return _skip;
        // 隔了一阵才来新的一帧，是新一段动画开始：重新读一次刷新率（换了显示器也跟着变），这一帧照常更新。
        if (time < _frame || time - _frame > TimeSpan.FromMilliseconds(100))
        {
            var refresh = RefreshRate();
            _interval = refresh >= PacedRefresh ? TimeSpan.FromSeconds(1.5 / refresh) : TimeSpan.Zero;
            _shown = time;
            _skip = false;
        }
        else
        {
            // 距上次更新不到 1.5 帧就跳过，于是正好隔一帧更新；偶尔掉一帧时下一帧马上补上。
            _skip = time - _shown < _interval;
            if (!_skip) _shown = time;
        }
        _frame = time;
        return _skip;
    }

    /// <summary>窗口自带的动画（淡入淡出、缩放、平移）在高刷新率屏幕上按一半帧率计算。</summary>
    public static T Pace<T>(T timeline) where T : Timeline
    {
        if (Enabled && RefreshRate() is var refresh && refresh >= PacedRefresh) Timeline.SetDesiredFrameRate(timeline, refresh / 2);
        return timeline;
    }

    /// <summary>主显示器的刷新率；读不到时为 0。</summary>
    public static int RefreshRate()
    {
        var screen = NativeMethods.GetDC(IntPtr.Zero);
        if (screen == IntPtr.Zero) return 0;
        try
        {
            return NativeMethods.GetDeviceCaps(screen, NativeMethods.VREFRESH);
        }
        finally
        {
            NativeMethods.ReleaseDC(IntPtr.Zero, screen);
        }
    }
}
