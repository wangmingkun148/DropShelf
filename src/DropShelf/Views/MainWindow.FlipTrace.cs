using System.Diagnostics;
using System.Text;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Services;

namespace DropShelf.Views;

public partial class MainWindow
{
    /// <summary>诊断日志打开时（DROPSHELF_TRACE=1），从点击到翻牌结束记下每帧间隔，并标出几个同步步骤各用了多久。</summary>
    private sealed class FlipTrace(string name)
    {
        public string Name { get; } = name;
        public long Begun { get; } = Stopwatch.GetTimestamp();
        public StringBuilder Frames { get; } = new();
        public List<double> Intervals { get; } = [];
        public TimeSpan? Last { get; set; }
        public string? Ending { get; set; }
    }

    private FlipTrace? _trace;

    private void StartTrace(string name)
    {
        if (!LogService.TraceEnabled) return;
        if (_trace is null) CompositionTarget.Rendering += OnTraceFrame;
        _trace = new FlipTrace(name);
    }

    /// <summary>first 是点击到第一帧的时间，包括点击时同步准备页面；之后是每帧间隔，结束后再记一帧，即去掉缓存后第一次直接绘制。</summary>
    private void OnTraceFrame(object? sender, EventArgs e)
    {
        if (_trace is not { } trace) return;
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (trace.Last == time) return;
        if (trace.Last is { } last)
        {
            var interval = (time - last).TotalMilliseconds;
            trace.Intervals.Add(interval);
            trace.Frames.Append($" {interval:F0}");
        }
        else trace.Frames.Append($" first={Stopwatch.GetElapsedTime(trace.Begun).TotalMilliseconds:F0}ms:");
        trace.Last = time;
        if (trace.Ending is not { } ending) return;
        _trace = null;
        CompositionTarget.Rendering -= OnTraceFrame;
        var sorted = trace.Intervals.Order().ToList();
        var p95 = sorted.Count > 0 ? sorted[(int)Math.Ceiling(sorted.Count * 0.95) - 1] : 0;
        LogService.Trace($"Flip {trace.Name} {ending} p95={p95:F1}ms max={(sorted.Count > 0 ? sorted[^1] : 0):F1}ms{trace.Frames}");
    }

    /// <summary>在帧间隔里标出一个时刻，以及从 since 到现在同步执行用的时间。</summary>
    private void TraceMark(string label, long since) =>
        _trace?.Frames.Append($" [{label} {Stopwatch.GetElapsedTime(since).TotalMilliseconds:F1}ms]");

    private void EndTrace(string ending)
    {
        if (_trace is { } trace) trace.Ending ??= ending;
    }
}
