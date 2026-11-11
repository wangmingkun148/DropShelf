using System.Windows.Media;

namespace DropShelf.Views;

/// <summary>
/// 逐帧动画用的时钟：从第一帧画出来开始计时，一帧最多前进 <see cref="MaxStep"/>。
/// 按真实时间推进时，某一帧耽搁了（刚显示窗口、处理器被压到低频），下一帧画面就直接跳过一截；
/// 这样改为动画略微拖长、轨迹保持连续。帧率不低于每秒 30 帧时和真实时间一致。
/// 整段动画最多因此拖长 <see cref="MaxLag"/>：偶尔的一两帧耽搁能接住，一直跑不满帧时不会越拖越长变成慢动作，
/// 用完之后按真实时间推进，帧数少一些，动画照常按时结束。
/// </summary>
internal sealed class FrameClock
{
    /// <summary>60Hz 下两帧的时间。</summary>
    public static readonly TimeSpan MaxStep = TimeSpan.FromMilliseconds(34);

    /// <summary>一段动画累计最多比真实时间慢这么多。</summary>
    public static readonly TimeSpan MaxLag = TimeSpan.FromMilliseconds(100);

    private TimeSpan? _last;
    private TimeSpan _lag;

    public TimeSpan Elapsed { get; private set; }

    /// <summary>在 CompositionTarget.Rendering 里调用；同一帧重复触发时返回 false。</summary>
    public bool Tick(EventArgs e)
    {
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (time == _last) return false;
        if (_last is { } last) Elapsed += Step(time - last, ref _lag);
        _last = time;
        return true;
    }

    /// <summary>过了 passed 的一帧动画前进多少；lag 是这段动画已经拖长的时间，随之累加。</summary>
    public static TimeSpan Step(TimeSpan passed, ref TimeSpan lag)
    {
        if (passed <= MaxStep) return passed;
        var held = passed - MaxStep;
        if (held > MaxLag - lag) held = MaxLag - lag;
        lag += held;
        return passed - held;
    }
}
