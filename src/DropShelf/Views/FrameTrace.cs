using System.Diagnostics;
using System.Text;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Services;

namespace DropShelf.Views;

/// <summary>
/// 诊断日志打开时（DROPSHELF_TRACE=1）记下一段动画的每帧间隔和两帧之间界面线程忙了多久，记作“间隔/忙”（毫秒）。
/// 间隔长而界面线程不忙，是显卡或系统合成跟不上；两者一起变长，是界面线程算不过来。
/// 每条记录附上电源、节能状态、渲染层级和刷新率，方便对比插电和用电池时的差别。
/// </summary>
internal sealed class FrameTrace
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    private readonly string _name;
    private readonly long _begun = Stopwatch.GetTimestamp();
    private readonly Dispatcher _dispatcher = Dispatcher.CurrentDispatcher;
    private readonly StringBuilder _frames = new();
    private readonly List<double> _intervals = [];
    private readonly List<double> _busy = [];
    private TimeSpan? _last;
    private string? _ending;
    private long _busyTicks;
    private long _busyAtFrame;
    private long _operationStart;
    private int _depth;

    private FrameTrace(string name)
    {
        _name = name;
        _dispatcher.Hooks.OperationStarted += OnOperationStarted;
        _dispatcher.Hooks.OperationCompleted += OnOperationFinished;
        _dispatcher.Hooks.OperationAborted += OnOperationFinished;
        CompositionTarget.Rendering += OnFrame;
    }

    /// <summary>诊断日志没打开时返回 null，调用方用 ?. 调用，不增加任何开销。</summary>
    public static FrameTrace? Start(string name) => LogService.TraceEnabled ? new FrameTrace(name) : null;

    /// <summary>在帧间隔里标出一个时刻，以及从 since 到现在同步执行用的时间。</summary>
    public void Mark(string label, long since) =>
        _frames.Append($" [{label} {Stopwatch.GetElapsedTime(since).TotalMilliseconds:F1}ms]");

    /// <summary>再记一帧后写入日志：结束后的第一帧通常是去掉缓存后直接绘制，也值得一看。</summary>
    public void End(string ending) => _ending ??= ending;

    /// <summary>
    /// 系统拖放的回调（拖入、悬停、离开）由系统直接调进界面线程，不经过调度器，上面的钩子看不到：
    /// 晃动呼出时它们正好压在出场动画上，不算进去就会显得界面线程不忙。在回调末尾调用，把从 since 起用的时间也算作忙。
    /// </summary>
    public void Outside(long since)
    {
        if (_depth == 0) _busyTicks += Stopwatch.GetTimestamp() - since;
    }

    private void OnOperationStarted(object? sender, DispatcherHookEventArgs e)
    {
        if (_depth++ == 0) _operationStart = Stopwatch.GetTimestamp();
    }

    private void OnOperationFinished(object? sender, DispatcherHookEventArgs e)
    {
        if (_depth == 0) return;
        if (--_depth == 0) _busyTicks += Stopwatch.GetTimestamp() - _operationStart;
    }

    private void OnFrame(object? sender, EventArgs e)
    {
        var time = ((RenderingEventArgs)e).RenderingTime;
        // 低性能模式下动画隔一帧更新，只记更新了的帧，间隔反映实际的节奏。
        if (_last == time || LowPerformance.Skip(time)) return;
        var busy = Stopwatch.GetElapsedTime(0, _busyTicks - _busyAtFrame).TotalMilliseconds;
        _busyAtFrame = _busyTicks;
        if (_last is { } last)
        {
            var interval = (time - last).TotalMilliseconds;
            _intervals.Add(interval);
            _busy.Add(busy);
            _frames.Append($" {interval:F0}/{busy:F0}");
        }
        else _frames.Append($" first={Stopwatch.GetElapsedTime(_begun).TotalMilliseconds:F0}ms:");
        _last = time;
        if (_ending is null && Stopwatch.GetElapsedTime(_begun) > Timeout) _ending = "timeout";
        if (_ending is not { } ending) return;

        CompositionTarget.Rendering -= OnFrame;
        _dispatcher.Hooks.OperationStarted -= OnOperationStarted;
        _dispatcher.Hooks.OperationCompleted -= OnOperationFinished;
        _dispatcher.Hooks.OperationAborted -= OnOperationFinished;
        LogService.Trace($"{_name} {ending} {Percentiles(_intervals, "间隔")} {Percentiles(_busy, "忙")} {Conditions()}{_frames}");
    }

    private static string Percentiles(List<double> values, string label)
    {
        if (values.Count == 0) return $"{label} -";
        var sorted = values.Order().ToList();
        return $"{label} p50={sorted[sorted.Count / 2]:F1} p95={sorted[(int)Math.Ceiling(sorted.Count * 0.95) - 1]:F1} max={sorted[^1]:F1}ms";
    }

    /// <summary>电源（交流/电池、节电模式）、本程序是否声明了正常性能、渲染层级（2 是显卡完整加速，0 是软件绘制）和屏幕刷新率。</summary>
    private static string Conditions()
    {
        var power = NativeMethods.GetSystemPowerStatus(out var status)
            ? $"{(status.ACLineStatus == 1 ? "交流" : status.ACLineStatus == 0 ? "电池" : "未知")}{(status.SystemStatusFlag == 1 ? "+节电" : "")}"
            : "未知";
        return $"[电源 {power} 性能 {(PowerQoS.IsHigh ? "正常" : "系统决定")} 渲染 {RenderCapability.Tier >> 16} {LowPerformance.RefreshRate()}Hz{(LowPerformance.Enabled ? " 低性能" : "")}]";
    }
}
