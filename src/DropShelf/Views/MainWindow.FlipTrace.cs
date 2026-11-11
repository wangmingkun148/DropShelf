namespace DropShelf.Views;

public partial class MainWindow
{
    /// <summary>诊断日志打开时（DROPSHELF_TRACE=1），从点击到翻牌结束记下每帧间隔，并标出几个同步步骤各用了多久，见 FrameTrace。</summary>
    private FrameTrace? _trace;

    /// <summary>first 是点击到第一帧的时间，包括点击时同步准备页面；之后是每帧间隔，结束后再记一帧，即去掉缓存后第一次直接绘制。</summary>
    private void StartTrace(string name)
    {
        _trace?.End("replaced");
        _trace = FrameTrace.Start($"Flip {name}");
    }

    /// <summary>在帧间隔里标出一个时刻，以及从 since 到现在同步执行用的时间。</summary>
    private void TraceMark(string label, long since) => _trace?.Mark(label, since);

    private void EndTrace(string ending)
    {
        _trace?.End(ending);
        _trace = null;
    }
}
