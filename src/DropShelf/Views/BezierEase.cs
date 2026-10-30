using System.Windows.Media.Animation;

namespace DropShelf.Views;

/// <summary>与 CSS 的 cubic-bezier 相同：从 (0,0) 到 (1,1)，两个控制点决定快慢。</summary>
internal sealed class BezierEase(double x1, double y1, double x2, double y2) : IEasingFunction
{
    public double Ease(double t) => Cubic(y1, y2, Solve(x1, x2, t));

    /// <summary>反过来由 y 求 x。</summary>
    public double Inverse(double y) => Cubic(x1, x2, Solve(y1, y2, y));

    /// <summary>两个控制点的这一坐标在 0 到 1 之间时，坐标随参数单调增加：二分求出坐标等于 value 的参数。</summary>
    private static double Solve(double p1, double p2, double value)
    {
        // 两端直接给出，二分只会逼近，动画的起点和终点要正好落在关键帧上。
        if (value <= 0) return 0;
        if (value >= 1) return 1;
        double low = 0, high = 1;
        for (var i = 0; i < 30; i++)
        {
            var middle = (low + high) / 2;
            if (Cubic(p1, p2, middle) < value) low = middle;
            else high = middle;
        }
        return (low + high) / 2;
    }

    private static double Cubic(double p1, double p2, double s) => 3 * p1 * s * (1 - s) * (1 - s) + 3 * p2 * s * s * (1 - s) + s * s * s;
}
