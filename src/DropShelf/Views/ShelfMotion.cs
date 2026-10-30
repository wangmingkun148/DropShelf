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
    /// 晃动呼出：牌背从光标处冒出来，一边放大、一边飞向落点、一边翻面，最后略微超出原大再收回，全程约 350 毫秒。cursor 是光标相对落点中心的位置（DIP）。
    /// </summary>
    public static CardMotion ShakeIn(Vector cursor)
    {
        var (x, y) = (cursor.X * 0.9, cursor.Y * 0.9);
        return Pick(4, ref _lastShake) switch
        {
            0 => FlipSideways(x, y),
            1 => FlipUpward(x, y),
            2 => SpinOpen(x, y),
            _ => DropOpen(x, y)
        };
    }

    /// <summary>绕竖轴左右翻开。x、y 是起手时牌中心相对落点的位置（DIP）。</summary>
    private static CardMotion FlipSideways(double x, double y) => new([
        new(0, X: x, Y: y, Scale: 0.5, Angle: -8, Turn: 180, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
        new(135, X: x * 0.5, Y: y * 0.5, Scale: 0.74, Angle: -5, Turn: 128),
        new(230, X: x * 0.1, Y: y * 0.1, Scale: 0.97, Angle: -1, Turn: 30, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
        new(300, Scale: 1.025, Angle: 0.4, Turn: -4, Ease: EaseInOut),
        new(355)], FadeInMs: 40);

    /// <summary>绕横轴从下往上翻起，落定前略微抬高。</summary>
    private static CardMotion FlipUpward(double x, double y) => new([
        new(0, X: x, Y: y, Scale: 0.5, Angle: 5, Turn: 180, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
        new(130, X: x * 0.55, Y: y * 0.55 - 6, Scale: 0.76, Angle: 3, Turn: 125),
        new(225, X: x * 0.12, Y: y * 0.12 - 2, Scale: 0.98, Angle: 0.5, Turn: 28, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
        new(290, Y: -2, Scale: 1.02, Turn: -5, Ease: EaseInOut),
        new(345)], HorizontalAxis: true, FadeInMs: 40);

    /// <summary>转着翻开：起手倾角大，边翻边转正。</summary>
    private static CardMotion SpinOpen(double x, double y) => new([
        new(0, X: x, Y: y, Scale: 0.45, Angle: -24, Turn: 180, Ease: new BezierEase(0.2, 0.65, 0.35, 1)),
        new(145, X: x * 0.45, Y: y * 0.45, Scale: 0.78, Angle: -10, Turn: 120),
        new(240, X: x * 0.08, Y: y * 0.08, Scale: 0.99, Angle: 2, Turn: 24, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
        new(305, Scale: 1.02, Angle: -0.8, Turn: -3, Ease: EaseInOut),
        new(360)], FadeInMs: 40);

    /// <summary>从光标上方落下来，划一道弧翻开，倾角朝另一边。</summary>
    private static CardMotion DropOpen(double x, double y) => new([
        new(0, X: x, Y: y - 36, Scale: 0.5, Angle: 9, Turn: 180, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
        new(135, X: x * 0.5, Y: y * 0.5 - 22, Scale: 0.75, Angle: 5, Turn: 126),
        new(230, X: x * 0.1, Y: y * 0.1 - 5, Scale: 0.97, Angle: 1, Turn: 28, Ease: new BezierEase(0.3, 0.7, 0.4, 1)),
        new(300, Y: 1.5, Scale: 1.02, Angle: -0.4, Turn: -4, Ease: EaseInOut),
        new(355)], FadeInMs: 40);

    /// <summary>
    /// 从边缘呼出：牌带着倾角从屏幕外滑进来，边走边回正，冲过落点一点再停稳。target 是落定时的位置，work 是工作区，都是屏幕像素；
    /// dpiScale 是那里的显示缩放比例。左右两边的倾角互为镜像，顶边的倾角减半。
    /// </summary>
    public static CardMotion EdgeIn(ScreenEdge edge, RECT target, RECT work, double dpiScale)
    {
        Func<double, Step[]> steps = Pick(4, ref _lastEdge) switch { 0 => Fling, 1 => SpinIn, 2 => SlideStop, _ => TossDown };
        var tilt = Tilt(edge);
        // 起手那一帧的转角和缩放决定牌要退到多远才完全在工作区外，它们和距离无关，先按距离为零取出来。
        var start = steps(0)[0];
        var d = Outside(edge, target, work, dpiScale, start.Angle * Math.Abs(tilt), start.Scale);
        return new CardMotion(steps(d).Select(step => Key(edge, tilt, step)).ToArray());
    }

    /// <summary>甩出：一路回正，冲过落点几像素，反向偏一点再停稳。d 是起手时牌中心离落点有多远（DIP）。</summary>
    private static Step[] Fling(double d) => [
        new(0, Out: d, Across: 14, Angle: 16, Ease: new BezierEase(0.2, 0.5, 0.45, 1)),
        new(112, Out: d * 0.44, Across: 6, Angle: 8, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
        new(252, Out: -9, Across: -1, Scale: 1.02, Angle: -2, Ease: EaseInOut),
        new(378)];

    /// <summary>旋入：转得更多，从略偏上的地方划进来。</summary>
    private static Step[] SpinIn(double d) => [
        new(0, Out: d, Across: -40, Angle: 28, Ease: new BezierEase(0.2, 0.55, 0.4, 1)),
        new(130, Out: d * 0.4, Across: -14, Angle: 12, Ease: new BezierEase(0.25, 0.6, 0.4, 1)),
        new(260, Out: -6, Across: 2, Scale: 1.015, Angle: -3, Ease: EaseInOut),
        new(330, Out: 2, Angle: 1, Ease: EaseInOut),
        new(400)];

    /// <summary>滑停：几乎不转，沿直线滑进来，像被桌面摩擦着来回晃两下停住。</summary>
    private static Step[] SlideStop(double d) => [
        new(0, Out: d, Angle: 6, Ease: new BezierEase(0.15, 0.55, 0.3, 1)),
        new(200, Out: -12, Angle: -2.5, Ease: EaseInOut),
        new(290, Out: 4, Angle: 1.2, Ease: EaseInOut),
        new(360, Out: -1.5, Angle: -0.4, Ease: EaseInOut),
        new(420)];

    /// <summary>抛起落下：飞行中略微放大像离开了桌面，落下时轻轻一压。</summary>
    private static Step[] TossDown(double d) => [
        new(0, Out: d, Across: 20, Scale: 1.04, Angle: 18, Ease: new BezierEase(0.2, 0.5, 0.45, 1)),
        new(140, Out: d * 0.38, Across: 8, Scale: 1.06, Angle: 7, Ease: new BezierEase(0.3, 0.6, 0.4, 1)),
        new(270, Out: -5, Across: -1, Scale: 0.985, Angle: -1.5, Ease: EaseInOut),
        new(380)];

    /// <summary>
    /// 离场开头置物架还垫在牌底下，带着系统画的窗口阴影。牌先不画自己的阴影，等置物架隐身后才在这段时间内淡入，两层阴影不会叠在一起闪一下。
    /// </summary>
    private const double ExitShadowInMs = 80;

    /// <summary>关闭：从边缘呼出的先往里收一下再甩回那条边；其余的翻回背面、缩小淡出，翻的方向和出场时一致。</summary>
    public static CardMotion Exit(ShelfEntrance entrance, bool horizontalAxis, RECT target, RECT work, double dpiScale)
    {
        if (entrance.Kind != ShelfEntranceKind.Edge) return FlipAway(horizontalAxis);
        var edge = entrance.Edge;
        var tilt = Tilt(edge);
        // 最后一帧的牌连同阴影要完全落到工作区外。
        var end = FlingBack(0)[^1];
        var d = Outside(edge, target, work, dpiScale, end.Angle * Math.Abs(tilt), end.Scale);
        return new CardMotion(FlingBack(d).Select(step => Key(edge, tilt, step)).ToArray(), ShadowInMs: ExitShadowInMs);
    }

    /// <summary>甩回：先往里收一下，再甩回那条边。d 是最后牌中心离原位有多远（DIP）。</summary>
    private static Step[] FlingBack(double d) => [
        new(0, Ease: new BezierEase(0.3, 0, 0.5, 1)),
        new(70, Out: -5, Scale: 1.01, Angle: -1, Ease: new BezierEase(0.55, 0, 0.85, 0.35)),
        new(270, Out: d, Across: 12, Angle: 16)];

    /// <summary>翻回背面，缩小淡出。</summary>
    private static CardMotion FlipAway(bool horizontalAxis) => new([
        new(0, Ease: new BezierEase(0.45, 0, 0.75, 0.45)),
        new(140, Y: 4, Scale: 0.93, Angle: 1.5, Turn: 90, Ease: new BezierEase(0.25, 0.5, 0.5, 1)),
        new(280, Y: 16, Scale: 0.72, Angle: 4, Turn: 180, Opacity: 0)], horizontalAxis, ShadowInMs: ExitShadowInMs);

    /// <summary>窗口滑入前的起始偏移（屏幕像素）：从下方升起。</summary>
    public static int RiseOffset(double dpiScale) => (int)Math.Round(RiseDistance * dpiScale);

    /// <summary>边缘方向上的一帧：Out 是沿离开那条边的方向走了多远，Across 是沿着这条边偏了多少（DIP）。</summary>
    private readonly record struct Step(double Ms, double Out = 0, double Across = 0, double Scale = 1, double Angle = 0, IEasingFunction? Ease = null);

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
    private static double Outside(ScreenEdge edge, RECT target, RECT work, double dpiScale, double angle, double grow)
    {
        var radians = angle * Math.PI / 180;
        var (halfWidth, halfHeight) = (target.Width / 2.0 / dpiScale, target.Height / 2.0 / dpiScale);
        var (centerX, centerY) = (target.Left + target.Width / 2.0, target.Top + target.Height / 2.0);
        var across = Math.Cos(radians) * halfWidth + Math.Sin(radians) * halfHeight;
        var down = Math.Sin(radians) * halfWidth + Math.Cos(radians) * halfHeight;
        var (gap, extent) = edge switch
        {
            ScreenEdge.Left => ((centerX - work.Left) / dpiScale, across),
            ScreenEdge.Top => ((centerY - work.Top) / dpiScale, down),
            _ => ((work.Right - centerX) / dpiScale, across)
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
