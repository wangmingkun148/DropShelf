using System.Windows;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Models;

namespace DropShelf.Views;

/// <summary>
/// 置物架出场和离场的动画参数，集中在这里便于统一调整。
/// 晃动呼出时一张牌背从光标处翻过来；从边缘呼出时一张牌从那条边被甩上桌面。两种各有几个大体相似、细节不同的版本，
/// 每次随机挑一个且不和上一次相同，看多了也不腻。关闭时，边缘呼出的甩回那条边，其余的翻回背面收走。
/// </summary>
internal static class ShelfMotion
{
    /// <summary>用户在 Windows 中关闭了动画效果时直接显示和关闭。</summary>
    public static bool Enabled => SystemParameters.ClientAreaAnimation;

    // 快捷键、托盘菜单等主动打开：窗口从下方升起一点，内容淡入并放大到原尺寸。
    public static readonly TimeSpan RiseDuration = TimeSpan.FromMilliseconds(300);
    public static readonly TimeSpan RiseFadeDuration = TimeSpan.FromMilliseconds(180);
    public const double RiseStartScale = 0.94;
    public const double RiseDistance = 12;

    private static readonly IEasingFunction EaseInOut = new BezierEase(0.42, 0, 0.58, 1);
    private static int _lastShake = -1;
    private static int _lastEdge = -1;

    /// <summary>
    /// 晃动呼出：牌背从光标处冒出来，一边放大、一边飞向落点、一边翻面，最后略微超出原大再收回。cursor 是光标相对落点中心的位置（DIP）。
    /// </summary>
    public static CardMotion ShakeIn(Vector cursor)
    {
        var (x, y) = (cursor.X * 0.9, cursor.Y * 0.9);
        return Pick(4, ref _lastShake) switch
        {
            // 绕竖轴左右翻开。
            0 => new CardMotion([
                new(0, x, y, 0.5, -8, 180, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
                new(170, x * 0.5, y * 0.5, 0.74, -5, 128),
                new(290, x * 0.1, y * 0.1, 0.97, -1, 30, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
                new(374, 0, 0, 1.025, 0.4, -4, Ease: EaseInOut),
                new(442)], FadeInMs: 50),
            // 绕横轴从下往上翻起，落定前略微抬高。
            1 => new CardMotion([
                new(0, x, y, 0.5, 5, 180, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
                new(160, x * 0.55, y * 0.55 - 6, 0.76, 3, 125),
                new(280, x * 0.12, y * 0.12 - 2, 0.98, 0.5, 28, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
                new(360, 0, -2, 1.02, 0, -5, Ease: EaseInOut),
                new(430)], HorizontalAxis: true, FadeInMs: 50),
            // 转着翻开：起手倾角大，边翻边转正。
            2 => new CardMotion([
                new(0, x, y, 0.45, -24, 180, Ease: new BezierEase(0.2, 0.65, 0.35, 1)),
                new(180, x * 0.45, y * 0.45, 0.78, -10, 120),
                new(300, x * 0.08, y * 0.08, 0.99, 2, 24, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
                new(380, 0, 0, 1.02, -0.8, -3, Ease: EaseInOut),
                new(450)], FadeInMs: 50),
            // 从光标上方落下来，划一道弧翻开，倾角朝另一边。
            _ => new CardMotion([
                new(0, x, y - 36, 0.5, 9, 180, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
                new(170, x * 0.5, y * 0.5 - 22, 0.75, 5, 126),
                new(290, x * 0.1, y * 0.1 - 5, 0.97, 1, 28, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
                new(374, 0, 1.5, 1.02, -0.4, -4, Ease: EaseInOut),
                new(442)], FadeInMs: 50)
        };
    }

    /// <summary>
    /// 从边缘呼出：牌带着倾角从屏幕外滑进来，边走边回正，冲过落点一点再停稳。target 是落定时的位置，work 是工作区，都是屏幕像素。
    /// 左右两边的倾角互为镜像，顶边的倾角减半。
    /// </summary>
    public static CardMotion EdgeIn(ScreenEdge edge, RECT target, RECT work, double scale)
    {
        var variant = Pick(4, ref _lastEdge);
        var tilt = Tilt(edge);
        var (startAngle, startScale) = variant switch { 0 => (16.0, 1.0), 1 => (28.0, 1.0), 2 => (6.0, 1.0), _ => (18.0, 1.04) };
        var d = Outside(edge, target, work, scale, startAngle * Math.Abs(tilt), startScale);
        Step[] steps = variant switch
        {
            // 甩出：一路回正，冲过落点几像素，反向偏一点再停稳。
            0 => [
                new(0, d, 14, 1, 16, new BezierEase(0.2, 0.5, 0.45, 1)),
                new(112, d * 0.44, 6, 1, 8, new BezierEase(0.25, 0.6, 0.4, 1)),
                new(252, -9, -1, 1.02, -2, EaseInOut),
                new(378, 0, 0, 1, 0)],
            // 旋入：转得更多，从略偏上的地方划进来。
            1 => [
                new(0, d, -40, 1, 28, new BezierEase(0.2, 0.55, 0.4, 1)),
                new(130, d * 0.4, -14, 1, 12, new BezierEase(0.25, 0.6, 0.4, 1)),
                new(260, -6, 2, 1.015, -3, EaseInOut),
                new(330, 2, 0, 1, 1, EaseInOut),
                new(400, 0, 0, 1, 0)],
            // 滑停：几乎不转，沿直线滑进来，像被桌面摩擦着来回晃两下停住。
            2 => [
                new(0, d, 0, 1, 6, new BezierEase(0.15, 0.55, 0.3, 1)),
                new(200, -12, 0, 1, -2.5, EaseInOut),
                new(290, 4, 0, 1, 1.2, EaseInOut),
                new(360, -1.5, 0, 1, -0.4, EaseInOut),
                new(420, 0, 0, 1, 0)],
            // 抛起落下：飞行中略微放大像离开了桌面，落下时轻轻一压。
            _ => [
                new(0, d, 20, 1.04, 18, new BezierEase(0.2, 0.5, 0.45, 1)),
                new(140, d * 0.38, 8, 1.06, 7, new BezierEase(0.3, 0.6, 0.4, 1)),
                new(270, -5, -1, 0.985, -1.5, EaseInOut),
                new(380, 0, 0, 1, 0)]
        };
        return new CardMotion(steps.Select(step => Key(edge, tilt, step)).ToArray());
    }

    /// <summary>关闭：从边缘呼出的先往里收一下再甩回那条边；其余的翻回背面、缩小淡出，翻的方向和出场时一致。</summary>
    public static CardMotion Exit(ShelfEntrance entrance, bool horizontalAxis, RECT target, RECT work, double scale)
    {
        if (entrance.Kind == ShelfEntranceKind.Edge)
        {
            var edge = entrance.Edge;
            var tilt = Tilt(edge);
            var d = Outside(edge, target, work, scale, 16 * Math.Abs(tilt), 1);
            return new CardMotion([
                Key(edge, tilt, new(0, 0, 0, 1, 0, new BezierEase(0.3, 0, 0.5, 1))),
                Key(edge, tilt, new(70, -5, 0, 1.01, -1, new BezierEase(0.55, 0, 0.85, 0.35))),
                Key(edge, tilt, new(270, d, 12, 1, 16))]);
        }
        return new CardMotion([
            new(0, Ease: new BezierEase(0.45, 0, 0.75, 0.45)),
            new(140, 0, 4, 0.93, 1.5, 90, Ease: new BezierEase(0.25, 0.5, 0.5, 1)),
            new(280, 0, 16, 0.72, 4, 180, 0)], horizontalAxis);
    }

    /// <summary>窗口滑入前的起始偏移（屏幕像素）：从下方升起。</summary>
    public static int RiseOffset(double scale) => (int)Math.Round(RiseDistance * scale);

    /// <summary>边缘方向上的一帧：Out 是沿离开那条边的方向走了多远，Across 是沿着这条边偏了多少（DIP）。</summary>
    private readonly record struct Step(double Ms, double Out, double Across, double Scale, double Angle, IEasingFunction? Ease = null);

    private static CardKey Key(ScreenEdge edge, double tilt, Step step)
    {
        var (outX, outY) = edge switch { ScreenEdge.Left => (-1, 0), ScreenEdge.Top => (0, -1), _ => (1, 0) };
        // 左右两边沿竖直方向偏，顶边沿水平方向偏。
        var (acrossX, acrossY) = (Math.Abs(outY), Math.Abs(outX));
        return new CardKey(step.Ms, outX * step.Out + acrossX * step.Across, outY * step.Out + acrossY * step.Across, step.Scale,
            step.Angle * tilt, Ease: step.Ease);
    }

    private static double Tilt(ScreenEdge edge) => edge switch { ScreenEdge.Left => -1, ScreenEdge.Top => 0.5, _ => 1 };

    /// <summary>牌中心要沿离开那条边的方向走多远（DIP），转过 angle、放大 grow 倍的牌连同阴影才完全落到工作区外。</summary>
    private static double Outside(ScreenEdge edge, RECT target, RECT work, double scale, double angle, double grow)
    {
        var radians = angle * Math.PI / 180;
        var (halfWidth, halfHeight) = (target.Width / 2.0 / scale, target.Height / 2.0 / scale);
        var (centerX, centerY) = (target.Left + target.Width / 2.0, target.Top + target.Height / 2.0);
        var across = Math.Cos(radians) * halfWidth + Math.Sin(radians) * halfHeight;
        var down = Math.Sin(radians) * halfWidth + Math.Cos(radians) * halfHeight;
        var (gap, extent) = edge switch
        {
            ScreenEdge.Left => ((centerX - work.Left) / scale, across),
            ScreenEdge.Top => ((centerY - work.Top) / scale, down),
            _ => ((work.Right - centerX) / scale, across)
        };
        return gap + extent * grow + 24;
    }

    /// <summary>从 count 个版本里随机挑一个，不和上一次相同。</summary>
    private static int Pick(int count, ref int last)
    {
        var next = last < 0 ? Random.Shared.Next(count) : Random.Shared.Next(count - 1);
        if (last >= 0 && next >= last) next++;
        last = next;
        return next;
    }
}
