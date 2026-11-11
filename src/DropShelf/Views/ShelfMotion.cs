using System.Windows;
using System.Windows.Media.Animation;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.Views;

/// <summary>置物架弹出和关闭的动画参数，集中在这里便于统一调整。</summary>
internal static class ShelfMotion
{
    public static readonly TimeSpan CloseDuration = TimeSpan.FromMilliseconds(170);
    public const double CloseScale = 0.94;

    /// <summary>用户在 Windows 中关闭了动画效果时直接显示和关闭。</summary>
    public static bool Enabled => SystemParameters.ClientAreaAnimation;

    /// <summary>
    /// 关闭了动画效果时让这段动画在第一帧就走到终点（延迟一并取消），照常触发 Completed，收尾的代码不用另写一套；
    /// 打开时原样返回，低性能模式下在高刷新率屏幕上按一半帧率计算，见 LowPerformance.Pace。
    /// </summary>
    public static T Gate<T>(T timeline) where T : Timeline
    {
        if (Enabled) return LowPerformance.Pace(timeline);
        if (timeline.BeginTime is not null) timeline.BeginTime = TimeSpan.Zero;
        timeline.SpeedRatio = 1e6;
        return timeline;
    }

    public sealed record Motion(TimeSpan Duration, TimeSpan FadeDuration, double StartScale, IEasingFunction ScaleEase);

    public static Motion For(ShelfEntrance entrance) => entrance.Kind switch
    {
        // 晃动呼出：从光标处弹出，略带回弹，让人一眼注意到。
        ShelfEntranceKind.Shake => new Motion(TimeSpan.FromMilliseconds(420), TimeSpan.FromMilliseconds(200), 0.82,
            new BackEase { Amplitude = 0.28, EasingMode = EasingMode.EaseOut }),
        ShelfEntranceKind.Edge => new Motion(TimeSpan.FromMilliseconds(380), TimeSpan.FromMilliseconds(220), 0.96,
            new QuinticEase { EasingMode = EasingMode.EaseOut }),
        _ => new Motion(TimeSpan.FromMilliseconds(300), TimeSpan.FromMilliseconds(180), 0.94,
            new QuinticEase { EasingMode = EasingMode.EaseOut })
    };

    /// <summary>窗口滑入前的起始偏移（屏幕像素）：边缘呼出时从屏幕外侧滑入，其余情况从下方升起。</summary>
    public static (int X, int Y) OpenOffset(ShelfEntrance entrance, double scale) => entrance.Kind switch
    {
        ShelfEntranceKind.Edge => Toward(entrance.Edge, 40, scale),
        ShelfEntranceKind.Shake => (0, Pixels(16, scale)),
        _ => (0, Pixels(12, scale))
    };

    /// <summary>关闭时窗口退回的方向：边缘呼出的退回边缘，其余情况轻微下沉。</summary>
    public static (int X, int Y) CloseOffset(ShelfEntrance entrance, double scale) => entrance.Kind == ShelfEntranceKind.Edge
        ? Toward(entrance.Edge, 24, scale)
        : (0, Pixels(10, scale));

    private static (int X, int Y) Toward(ScreenEdge edge, double distance, double scale) => edge switch
    {
        ScreenEdge.Left => (-Pixels(distance, scale), 0),
        ScreenEdge.Right => (Pixels(distance, scale), 0),
        ScreenEdge.Top => (0, -Pixels(distance, scale)),
        _ => (0, Pixels(distance / 2, scale))
    };

    private static int Pixels(double dip, double scale) => (int)Math.Round(dip * scale);
}
