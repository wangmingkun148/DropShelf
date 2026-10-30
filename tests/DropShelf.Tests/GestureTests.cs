using System.IO;
using System.IO.Compression;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services.Gestures;
using DropShelf.Views;
using WpfApplication = System.Windows.Application;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static partial class Program
{
    // ───────────── 手势识别 ─────────────

    private static void TestBalancedGestureTriggers()
    {
        var detector = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var samples = new[] { (100, 100, 0L), (150, 101, 60L), (95, 100, 120L), (155, 101, 180L), (90, 100, 240L) };
        Assert(samples.Any(sample => detector.AddSample(sample.Item1, sample.Item2, sample.Item3, false)), "快速左右晃动未被识别");
    }

    private static void TestSensitivityProfilesTrigger()
    {
        Assert(Shake(ShakeSensitivities.Responsive, [100, 134, 100, 134], 70), "灵敏档无法通过短距离两次来回触发");
        Assert(Shake(ShakeSensitivities.Balanced, [100, 143, 100, 143, 100], 70), "均衡档无法通过正常幅度晃动触发");
        Assert(Shake(ShakeSensitivities.Steady, [100, 185, 100, 185, 100, 185, 100, 185], 100), "稳健档无法通过明确晃动触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 165, 100, 165, 100, 165, 100], 70), "最稳健时被 2.5.7 的稳健档晃动触发");

        // 三档的差距：灵敏档轻晃即可，均衡档不行；均衡档的正常晃动在稳健档不够。
        Assert(Shake(ShakeSensitivities.Responsive, [100, 125, 100, 125], 70), "灵敏档无法通过轻晃触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 150, 100, 150, 100, 150, 100], 70), "稳健档被中等幅度晃动触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 165, 100, 165, 100], 70), "稳健档只来回两次就被触发");

        // 拖动时手部的小幅来回不应触发。
        Assert(!Shake(ShakeSensitivities.Responsive, [100, 120, 100, 120], 70), "灵敏档被小幅来回误触发");
        Assert(!Shake(ShakeSensitivities.Balanced, [100, 125, 100, 125, 100], 70), "均衡档被小幅晃动误触发");
        Assert(!Shake(ShakeSensitivities.Steady, [100, 135, 100, 135, 100, 135], 70), "稳健档被小幅晃动误触发");

        // 无级调节：同样的晃动，灵敏度越高只会越容易触发，中途不会突然变难。
        for (var amplitude = 10; amplitude <= 80; amplitude += 2)
        {
            var xs = Enumerable.Range(0, 8).Select(index => index % 2 == 0 ? 100 : 100 + amplitude).ToArray();
            var previous = false;
            for (var level = ShakeSensitivities.Steady; level <= ShakeSensitivities.Responsive; level++)
            {
                var triggered = Shake(level, xs, 70);
                Assert(triggered || !previous, $"灵敏度 {level} 反而比 {level - 1} 更难触发（幅度 {amplitude}）");
                previous = triggered;
            }
        }

        // 设置页的说明：两端和中间与原来的三档一致，拖向灵敏时幅度和次数只减不增，且照说明晃动一定能触发。
        var descriptions = Enumerable.Range(0, 101).Select(MouseGestureDetector.Describe).ToArray();
        Assert(descriptions[0] == (5, 80) && descriptions[50] == (3, 38) && descriptions[100] == (2, 23), "灵敏度两端和中间的说明不正确");
        Assert(descriptions.Zip(descriptions.Skip(1)).All(pair => pair.Second.Distance <= pair.First.Distance && pair.Second.Reversals <= pair.First.Reversals),
            "灵敏度说明没有随滑块单调变化");
        Assert(descriptions.Select((description, level) => (description, level)).All(item =>
                Shake(item.level, Enumerable.Range(0, item.description.Reversals + 2)
                    .Select(index => index % 2 == 0 ? 100 : 101 + item.description.Distance).ToArray(), 70)),
            "按设置页说明的幅度和次数晃动没有触发");

        static bool Shake(int sensitivity, int[] xs, long interval)
        {
            var detector = new MouseGestureDetector(sensitivity);
            return xs.Select((x, index) => (x, index)).Any(sample => detector.AddSample(sample.x, 100, sample.index * interval, false));
        }
    }

    private static void TestGestureDurationBounds()
    {
        var tooFast = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var fastSamples = new[] { (100, 0L), (143, 30L), (100, 60L), (143, 90L), (100, 120L), (143, 150L) };
        Assert(!fastSamples.Any(sample => tooFast.AddSample(sample.Item1, 100, sample.Item2, false)), "瞬间抖动被错误识别为晃动手势");

        var tooSlow = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var slowSamples = new[] { (100, 0L), (143, 300L), (100, 600L), (143, 900L), (100, 1200L), (143, 1500L) };
        Assert(!slowSamples.Any(sample => tooSlow.AddSample(sample.Item1, 100, sample.Item2, false)), "缓慢来回移动被错误识别为晃动手势");
    }

    private static void TestGestureToleratesIrregularSamples()
    {
        var delayed = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var delayedSamples = new[] { (100, 100, 0L), (143, 103, 60L), (100, 99, 380L), (144, 105, 445L), (99, 98, 510L) };
        Assert(delayedSamples.Any(sample => delayed.AddSample(sample.Item1, sample.Item2, sample.Item3, false)),
            "系统短暂忙碌导致采样间隔变长后未能识别手势");

        var compact = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var compactSamples = new[] { 100, 138, 100, 138, 100, 138 };
        Assert(compactSamples.Select((x, index) => (x, index))
                .Any(sample => compact.AddSample(sample.x, 100 + sample.index % 2 * 2, sample.index * 55L, false)),
            "合理幅度和时长的晃动未被识别");
    }

    private static void TestOtherDirectionGesturesTrigger()
    {
        var vertical = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var verticalSamples = new[] { (100, 100), (101, 150), (100, 95), (101, 155), (100, 90) };
        Assert(verticalSamples.Select((point, index) => (point, index))
            .Any(sample => vertical.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 65L, false)), "快速上下晃动未被识别");

        var diagonal = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var diagonalSamples = new[] { (100, 100), (140, 140), (90, 90), (145, 145), (85, 85) };
        Assert(diagonalSamples.Select((point, index) => (point, index))
            .Any(sample => diagonal.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 65L, false)), "快速斜向晃动未被识别");
    }

    private static void TestNormalMovementDoesNotTrigger()
    {
        var straight = new MouseGestureDetector(ShakeSensitivities.Responsive);
        Assert(!Enumerable.Range(0, 12).Any(index => straight.AddSample(100 + index * 12, 100 + index, index * 40L, false)), "普通直线移动被误识别");

        var turning = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var turningSamples = new[] { (100, 100), (140, 100), (140, 140), (100, 140), (100, 100), (140, 100) };
        Assert(!turningSamples.Select((point, index) => (point, index))
            .Any(sample => turning.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 45L, false)), "普通转弯移动被误识别");
    }

    private static void TestScreenEdgeDoesNotTrigger()
    {
        var detector = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var samples = new[] { (100, 0L), (150, 50L), (95, 100L), (155, 150L), (90, 200L) };
        Assert(!samples.Any(sample => detector.AddSample(sample.Item1, 4, sample.Item2, true)), "屏幕边缘移动被误识别");
    }

    private static void TestHighRateShakeTriggers()
    {
        foreach (var sensitivity in new[] { ShakeSensitivities.Steady, 25, ShakeSensitivities.Balanced, 75, ShakeSensitivities.Responsive })
        {
            var detector = new MouseGestureDetector(sensitivity);
            var random = new Random(7);
            var triggered = false;
            // 1000 Hz 回报率，约 4 次/秒、幅度 ±60 像素的左右晃动，叠加 ±1 像素的抖动。
            for (var ms = 0; ms < 1200 && !triggered; ms++)
            {
                var x = 500 + (int)Math.Round(60 * Math.Sin(ms / 1000.0 * 2 * Math.PI * 4)) + random.Next(-1, 2);
                var y = 300 + random.Next(-1, 2);
                triggered = detector.AddRawSample(x, y, ms, false);
            }
            Assert(triggered, $"{sensitivity} 档没有识别高回报率鼠标的晃动");
        }
    }

    private static void TestCoarseTimerShakeTriggers()
    {
        // 复现：系统计时每 15.6 毫秒才跳一次，1000 Hz 鼠标的多次采样时间完全相同。
        foreach (var sensitivity in new[] { ShakeSensitivities.Steady, 25, ShakeSensitivities.Balanced, 75, ShakeSensitivities.Responsive })
        {
            var detector = new MouseGestureDetector(sensitivity);
            var triggered = false;
            for (var ms = 0; ms < 1200 && !triggered; ms++)
            {
                var x = 500 + (int)Math.Round(55 * Math.Sin(ms / 1000.0 * 2 * Math.PI * 4.5));
                var coarse = (long)(Math.Floor(ms / 15.625) * 15.625);
                triggered = detector.AddRawSample(x, 300 + ms % 3, coarse, false);
            }
            Assert(triggered, $"{sensitivity} 档在低精度计时下没有识别晃动");
        }
    }

    private static void TestHighRateNormalMovement()
    {
        var straight = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var straightTriggered = Enumerable.Range(0, 1500).Any(ms => straight.AddRawSample(100 + ms, 300 + ms / 10, ms, false));
        Assert(!straightTriggered, "高回报率的直线拖动被误识别");

        var circle = new MouseGestureDetector(ShakeSensitivities.Responsive);
        var circleTriggered = Enumerable.Range(0, 2000).Any(ms =>
            circle.AddRawSample(500 + (int)(80 * Math.Cos(ms / 300.0)), 400 + (int)(80 * Math.Sin(ms / 300.0)), ms, false));
        Assert(!circleTriggered, "绕圈拖动被误识别");
    }

    private static void TestTurnaroundJitter()
    {
        // 每次掉头时先出现一次几乎垂直的细小位移，再继续反向移动；旧算法会因此清空已记录的往返。
        var samples = new List<(int X, int Y)> { (100, 100) };
        var x = 100;
        for (var cycle = 0; cycle < 5; cycle++)
        {
            var direction = cycle % 2 == 0 ? 1 : -1;
            for (var step = 0; step < 4; step++)
            {
                x += direction * 12;
                samples.Add((x, 100));
            }
            samples.Add((x + direction, 105));
        }
        var detector = new MouseGestureDetector(ShakeSensitivities.Balanced);
        Assert(samples.Select((point, index) => (point, index))
            .Any(sample => detector.AddSample(sample.point.X, sample.point.Y, sample.index * 14L, false)), "掉头时的侧向位移打断了手势");

        var sideways = new MouseGestureDetector(ShakeSensitivities.Balanced);
        var zigzag = new[] { (100, 100), (130, 100), (131, 106), (131, 112), (131, 118), (101, 118), (100, 124), (100, 130) };
        Assert(!zigzag.Select((point, index) => (point, index))
            .Any(sample => sideways.AddSample(sample.point.Item1, sample.point.Item2, sample.index * 40L, false)), "连续侧向移动被误识别");
    }

    private static void TestGestureServiceLifecycle()
    {
        var service = new GestureService(Dispatcher.CurrentDispatcher, ShakeSensitivities.Balanced);
        service.Start();
        Assert(service.IsRunning, "鼠标原始输入没有注册");
        service.SetSensitivity(ShakeSensitivities.Steady);
        service.SetTrigger(AutoOpenTrigger.Edge);
        service.SetEdgeDwell(EdgeDwellTime.Short);
        service.Dispose();
        Thread.Sleep(100);
        Assert(!service.IsRunning, "停止后仍在监听鼠标输入");
    }

    private static void TestEdgeDwellDetector()
    {
        var detector = new EdgeDwellDetector(AutoOpenTriggers.DwellMilliseconds(EdgeDwellTime.Medium));
        Assert(!detector.Update(ScreenEdge.Left, 0, 300, 0) && !detector.Update(ScreenEdge.Left, 0, 310, 300) &&
               detector.Update(ScreenEdge.Left, 0, 320, 600), "在左边缘停留 0.6 秒没有触发");
        Assert(!detector.Update(ScreenEdge.Left, 0, 300, 1000) && !detector.Update(ScreenEdge.None, 40, 300, 1300) &&
               !detector.Update(ScreenEdge.Left, 0, 300, 1400) && !detector.Update(ScreenEdge.Left, 0, 300, 1900), "离开边缘后没有重新计时");
        Assert(!detector.Update(ScreenEdge.Right, 1919, 300, 3000) && !detector.Update(ScreenEdge.Right, 1919, 300 + EdgeDwellDetector.MaximumDrift + 1, 3500) &&
               !detector.Update(ScreenEdge.Right, 1919, 400, 4000), "沿边缘大幅移动后没有重新计时");
        Assert(!detector.Update(ScreenEdge.Top, 100, 0, 5000) && !detector.Update(ScreenEdge.Left, 0, 0, 5400), "换到另一条边缘时没有重新计时");
        detector.DwellMilliseconds = AutoOpenTriggers.DwellMilliseconds(EdgeDwellTime.Short);
        Assert(detector.Update(ScreenEdge.Left, 0, 0, 5700), "停留时间调整后没有生效");
        Assert(AutoOpenTriggers.UsesShake(AutoOpenTrigger.Both) && AutoOpenTriggers.UsesEdge(AutoOpenTrigger.Both) &&
               !AutoOpenTriggers.UsesShake(AutoOpenTrigger.Edge) && !AutoOpenTriggers.UsesEdge(AutoOpenTrigger.Shake), "呼出方式判断不正确");
    }
}
