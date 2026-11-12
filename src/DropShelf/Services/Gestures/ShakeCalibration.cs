using DropShelf.Models;

namespace DropShelf.Services.Gestures;

/// <summary>示范时记下的一个鼠标位置（屏幕像素）和时刻（毫秒）。</summary>
public readonly record struct ShakeSample(int X, int Y, long Time);

/// <summary>
/// 按示范的晃动定灵敏度：把每次示范原样交给识别器，看它最稳健能在哪一档触发，
/// 取几次里最吃力的那次再往灵敏方向留一点余量。用的就是平时识别晃动的那套判断，结果和实际使用一致。
/// </summary>
public static class ShakeCalibration
{
    /// <summary>示范几次。</summary>
    public const int Demonstrations = 3;

    /// <summary>建议值比“刚好能触发”再灵敏这么多：平时拖着文件晃，不会每次都和示范时一样用力。</summary>
    public const int Margin = 10;

    /// <summary>这次示范在这个灵敏度下能不能触发。</summary>
    public static bool Triggers(IReadOnlyList<ShakeSample> stroke, int sensitivity)
    {
        var detector = new MouseGestureDetector(sensitivity);
        foreach (var sample in stroke)
            if (detector.AddRawSample(sample.X, sample.Y, sample.Time, nearOuterScreenEdge: false)) return true;
        return false;
    }

    /// <summary>
    /// 这次示范最稳健能在哪个灵敏度触发：从最灵敏往稳健方向找，到第一个触发不了的为止。
    /// 晃得太轻、最灵敏也触发不了时返回 null。
    /// </summary>
    public static int? SteadiestLevel(IReadOnlyList<ShakeSample> stroke)
    {
        int? steadiest = null;
        for (var level = ShakeSensitivities.Responsive; level >= ShakeSensitivities.Steady; level--)
        {
            if (!Triggers(stroke, level)) break;
            steadiest = level;
        }
        return steadiest;
    }

    /// <summary>几次示范都能触发的最稳健的灵敏度，再加上余量。</summary>
    public static int Recommend(IEnumerable<int> steadiestLevels) =>
        ShakeSensitivities.Clamp(steadiestLevels.Max() + Margin);
}
