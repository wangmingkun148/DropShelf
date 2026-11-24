using DropShelf.Models;

namespace DropShelf.Services.Gestures;

/// <summary>
/// 判断拖动时光标是否在屏幕边缘停留了足够长的时间。光标沿边缘移动太远说明还在找位置，重新计时；
/// 离开边缘立即取消。是否位于边缘由调用方判断，这里只负责计时，便于测试。
/// </summary>
public sealed class EdgeDwellDetector
{
    /// <summary>沿边缘移动超过这个距离（屏幕像素）就重新计时。</summary>
    public const int MaximumDrift = 80;

    private ScreenEdge _edge;
    private long _since;
    private int _anchor;

    public EdgeDwellDetector(int dwellMilliseconds) => DwellMilliseconds = dwellMilliseconds;

    public int DwellMilliseconds { get; set; }

    /// <summary>报告光标的最新位置；在同一边缘停留满设定时间时返回 true（之后需要重新进入边缘才会再次触发）。</summary>
    public bool Update(ScreenEdge edge, int x, int y, long timestampMilliseconds)
    {
        if (edge == ScreenEdge.None)
        {
            Reset();
            return false;
        }

        var along = edge == ScreenEdge.Top ? x : y;
        if (edge != _edge || Math.Abs(along - _anchor) > MaximumDrift)
        {
            _edge = edge;
            _since = timestampMilliseconds;
            _anchor = along;
            return false;
        }

        if (timestampMilliseconds - _since < DwellMilliseconds) return false;
        Reset();
        return true;
    }

    public void Reset()
    {
        _edge = ScreenEdge.None;
        _since = 0;
        _anchor = 0;
    }
}
