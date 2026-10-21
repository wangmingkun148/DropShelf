using System.Runtime.InteropServices;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Models;

namespace DropShelf.Services.Gestures;

public sealed class GestureTriggeredEventArgs(int x, int y, ScreenEdge edge = ScreenEdge.None) : EventArgs
{
    /// <summary>触发时的光标位置（屏幕物理像素）。</summary>
    public int X { get; } = x;
    public int Y { get; } = y;

    /// <summary>拖到屏幕边缘停留触发时为对应的边缘，晃动触发时为 None。</summary>
    public ScreenEdge Edge { get; } = edge;
}

/// <summary>
/// 在独立线程上用低级鼠标钩子监听按住鼠标时的晃动，以及拖到屏幕边缘后的停留。钩子回调只做轻量计算，
/// 界面线程繁忙时也不会拖慢系统鼠标；识别到手势后再交给界面线程用探针窗口确认是否真的在拖放。
/// </summary>
public sealed class GestureService : IDisposable
{
    /// <summary>光标停在边缘不动时不会产生鼠标事件，用计时器定期检查。</summary>
    private const uint EdgePollMilliseconds = 40;
    private const int EdgeTolerance = 1;

    private readonly Dispatcher _dispatcher;
    private readonly LowLevelMouseProc _callback;
    private readonly ManualResetEventSlim _started = new();
    private readonly EdgeDwellDetector _edgeDetector;
    private Thread? _thread;
    private uint _threadId;
    private IntPtr _hook;
    private volatile bool _enabled;
    private volatile bool _shakeEnabled = true;
    private volatile bool _edgeEnabled;
    private volatile int _requestedSensitivity;
    private volatile int _requestedDwell;
    private int _appliedSensitivity;
    private MouseGestureDetector _detector;
    private bool _leftDown;
    private bool _rightDown;
    private bool _triggered;
    private long _lastButtonCheck;
    private int _movesThisPress;
    private UIntPtr _edgeTimer;
    private bool _hasMonitor;
    private RECT _monitorBounds;
    private RECT _monitorWork;
    private bool _disposed;

    public event EventHandler<GestureTriggeredEventArgs>? ShakeDetected;

    /// <summary>拖到屏幕边缘并停留了设定的时间。</summary>
    public event EventHandler<GestureTriggeredEventArgs>? EdgeDwellDetected;

    /// <summary>识别到手势后，按住的鼠标键被松开。</summary>
    public event EventHandler? DragReleased;

    public GestureService(Dispatcher dispatcher, GestureSensitivity sensitivity, bool enabled)
    {
        _dispatcher = dispatcher;
        _callback = OnHook;
        _requestedSensitivity = (int)sensitivity;
        _appliedSensitivity = (int)sensitivity;
        _detector = new MouseGestureDetector(sensitivity);
        _requestedDwell = AutoOpenTriggers.DwellMilliseconds(EdgeDwellTime.Medium);
        _edgeDetector = new EdgeDwellDetector(_requestedDwell);
        _enabled = enabled;
    }

    public bool IsRunning => _hook != IntPtr.Zero;

    public bool Enabled
    {
        get => _enabled;
        set => _enabled = value;
    }

    public void SetSensitivity(GestureSensitivity sensitivity) => _requestedSensitivity = (int)sensitivity;

    /// <summary>选择用晃动、屏幕边缘停留或两者来呼出置物架。</summary>
    public void SetTrigger(AutoOpenTrigger trigger)
    {
        _shakeEnabled = AutoOpenTriggers.UsesShake(trigger);
        _edgeEnabled = AutoOpenTriggers.UsesEdge(trigger);
    }

    public void SetEdgeDwell(EdgeDwellTime dwell) => _requestedDwell = AutoOpenTriggers.DwellMilliseconds(dwell);

    public void Start()
    {
        if (_thread is not null || _disposed) return;
        _thread = new Thread(Run) { IsBackground = true, Name = "DropShelf 手势识别" };
        _thread.Start();
        _started.Wait(TimeSpan.FromSeconds(2));
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_threadId != 0) NativeMethods.PostThreadMessage(_threadId, NativeMethods.WM_QUIT, IntPtr.Zero, IntPtr.Zero);
        _thread?.Join(TimeSpan.FromSeconds(1));
        _started.Dispose();
    }

    private void Run()
    {
        _threadId = NativeMethods.GetCurrentThreadId();
        _hook = NativeMethods.SetWindowsHookEx(NativeMethods.WH_MOUSE_LL, _callback, NativeMethods.GetModuleHandle(null), 0);
        if (_hook == IntPtr.Zero)
            LogService.Info($"鼠标钩子安装失败，错误码 {Marshal.GetLastWin32Error()}");
        _started.Set();

        while (NativeMethods.GetMessage(out var message, IntPtr.Zero, 0, 0) > 0)
        {
            if (message.Message == NativeMethods.WM_TIMER && message.Window == IntPtr.Zero && (UIntPtr)message.WParam == _edgeTimer)
            {
                OnEdgeTimer();
                continue;
            }
            NativeMethods.TranslateMessage(ref message);
            NativeMethods.DispatchMessage(ref message);
        }

        StopEdgeTimer();
        if (_hook != IntPtr.Zero) NativeMethods.UnhookWindowsHookEx(_hook);
        _hook = IntPtr.Zero;
    }

    private IntPtr OnHook(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code >= 0)
        {
            try
            {
                Handle((int)wParam, lParam);
            }
            catch
            {
                // 钩子回调中绝不能抛出异常，否则会影响整个系统的鼠标输入。
            }
        }
        return NativeMethods.CallNextHookEx(_hook, code, wParam, lParam);
    }

    private void Handle(int message, IntPtr lParam)
    {
        switch (message)
        {
            case NativeMethods.WM_LBUTTONDOWN:
            case NativeMethods.WM_RBUTTONDOWN:
                if (!_leftDown && !_rightDown) BeginPress();
                if (message == NativeMethods.WM_LBUTTONDOWN) _leftDown = true;
                else _rightDown = true;
                break;

            case NativeMethods.WM_LBUTTONUP:
            case NativeMethods.WM_RBUTTONUP:
                if (message == NativeMethods.WM_LBUTTONUP) _leftDown = false;
                else _rightDown = false;
                if (!_leftDown && !_rightDown) EndPress();
                break;

            case NativeMethods.WM_MOUSEMOVE:
                if (!_leftDown && !_rightDown) return;
                _movesThisPress++;
                var now = NowMilliseconds();
                if (now - _lastButtonCheck > 250)
                {
                    // 遇到安全桌面或钩子被系统跳过时可能漏掉松开事件，定期用实际按键状态校正。
                    _lastButtonCheck = now;
                    if (!NativeMethods.IsButtonPhysicallyDown())
                    {
                        _leftDown = _rightDown = false;
                        EndPress();
                        return;
                    }
                }

                if (!_enabled || _triggered) return;
                var point = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam).Point;
                if (_edgeEnabled && CheckEdge(point, now)) return;
                if (!_shakeEnabled) return;
                ApplySensitivityIfChanged();
                if (!_detector.AddRawSample(point.X, point.Y, now, IsNearOuterScreenEdge(point))) return;

                _triggered = true;
                StopEdgeTimer();
                _dispatcher.BeginInvoke(() => ShakeDetected?.Invoke(this, new GestureTriggeredEventArgs(point.X, point.Y)));
                break;
        }
    }

    /// <summary>检查边缘停留；触发时返回 true。光标到达边缘后启动计时器，静止不动也能按时触发。</summary>
    private bool CheckEdge(POINT point, long now)
    {
        _edgeDetector.DwellMilliseconds = _requestedDwell;
        var edge = HitTestEdge(point);
        if (_edgeDetector.Update(edge, point.X, point.Y, now))
        {
            _triggered = true;
            StopEdgeTimer();
            _dispatcher.BeginInvoke(() => EdgeDwellDetected?.Invoke(this, new GestureTriggeredEventArgs(point.X, point.Y, edge)));
            return true;
        }

        if (edge == ScreenEdge.None) StopEdgeTimer();
        else if (_edgeTimer == UIntPtr.Zero) _edgeTimer = NativeMethods.SetTimer(IntPtr.Zero, UIntPtr.Zero, EdgePollMilliseconds, IntPtr.Zero);
        return false;
    }

    private void OnEdgeTimer()
    {
        if (!_enabled || !_edgeEnabled || _triggered || (!_leftDown && !_rightDown) || !NativeMethods.IsButtonPhysicallyDown())
        {
            StopEdgeTimer();
            return;
        }
        if (NativeMethods.GetCursorPos(out var point)) CheckEdge(point, NowMilliseconds());
    }

    private void StopEdgeTimer()
    {
        if (_edgeTimer == UIntPtr.Zero) return;
        NativeMethods.KillTimer(IntPtr.Zero, _edgeTimer);
        _edgeTimer = UIntPtr.Zero;
    }

    /// <summary>
    /// 光标是否贴在屏幕的外侧边缘：左、右、上三边，且边缘外没有其他显示器。
    /// 任务栏所在位置（工作区以外）和底边不算，避免与拖到任务栏切换窗口冲突。
    /// </summary>
    private ScreenEdge HitTestEdge(POINT point)
    {
        if (!_hasMonitor || !Contains(_monitorBounds, point))
        {
            if (!RefreshMonitor(point)) return ScreenEdge.None;
        }

        var bounds = _monitorBounds;
        var nearLeft = point.X <= bounds.Left + EdgeTolerance;
        var nearRight = point.X >= bounds.Right - 1 - EdgeTolerance;
        var nearTop = point.Y <= bounds.Top + EdgeTolerance;
        if (!nearLeft && !nearRight && !nearTop) return ScreenEdge.None;
        if (!Contains(_monitorWork, point)) return ScreenEdge.None;

        if (nearLeft && IsOutsideAllMonitors(new POINT(bounds.Left - 1, point.Y))) return ScreenEdge.Left;
        if (nearRight && IsOutsideAllMonitors(new POINT(bounds.Right, point.Y))) return ScreenEdge.Right;
        if (nearTop && IsOutsideAllMonitors(new POINT(point.X, bounds.Top - 1))) return ScreenEdge.Top;
        return ScreenEdge.None;
    }

    private bool RefreshMonitor(POINT point)
    {
        var monitor = NativeMethods.MonitorFromPoint(point, NativeMethods.MONITOR_DEFAULTTONULL);
        var info = new MONITORINFO { Size = Marshal.SizeOf<MONITORINFO>() };
        _hasMonitor = monitor != IntPtr.Zero && NativeMethods.GetMonitorInfo(monitor, ref info);
        if (!_hasMonitor) return false;
        _monitorBounds = info.Monitor;
        _monitorWork = info.Work;
        return true;
    }

    private static bool IsOutsideAllMonitors(POINT point) =>
        NativeMethods.MonitorFromPoint(point, NativeMethods.MONITOR_DEFAULTTONULL) == IntPtr.Zero;

    private static bool Contains(RECT rect, POINT point) =>
        point.X >= rect.Left && point.X < rect.Right && point.Y >= rect.Top && point.Y < rect.Bottom;

    /// <summary>
    /// 高精度毫秒时间。Environment.TickCount64 的精度只有约 15.6 毫秒，
    /// 会让高回报率鼠标的多次采样拿到相同时间，导致晃动无法识别。
    /// </summary>
    private static long NowMilliseconds() => System.Diagnostics.Stopwatch.GetTimestamp() * 1000 / System.Diagnostics.Stopwatch.Frequency;

    private void BeginPress()
    {
        _movesThisPress = 0;
        _triggered = false;
        _lastButtonCheck = NowMilliseconds();
        _detector.Reset();
        _edgeDetector.Reset();
        // 每次按下时重新读取显示器信息，显示器布局变化后也能正确判断边缘。
        _hasMonitor = false;
    }

    private void EndPress()
    {
        if (LogService.TraceEnabled && _movesThisPress > 20)
            LogService.Trace($"按住期间收到 {_movesThisPress} 次移动，{(_triggered ? "已识别手势" : "未识别手势")}，启用 {_enabled}");
        var notify = _triggered;
        _triggered = false;
        _detector.Reset();
        _edgeDetector.Reset();
        StopEdgeTimer();
        if (notify) _dispatcher.BeginInvoke(() => DragReleased?.Invoke(this, EventArgs.Empty));
    }

    private void ApplySensitivityIfChanged()
    {
        var requested = _requestedSensitivity;
        if (requested == _appliedSensitivity) return;
        _appliedSensitivity = requested;
        _detector.SetSensitivity((GestureSensitivity)requested);
    }

    private static bool IsNearOuterScreenEdge(POINT point)
    {
        const int margin = 5;
        var left = NativeMethods.GetSystemMetrics(NativeMethods.SM_XVIRTUALSCREEN);
        var top = NativeMethods.GetSystemMetrics(NativeMethods.SM_YVIRTUALSCREEN);
        var width = NativeMethods.GetSystemMetrics(NativeMethods.SM_CXVIRTUALSCREEN);
        var height = NativeMethods.GetSystemMetrics(NativeMethods.SM_CYVIRTUALSCREEN);
        return point.X <= left + margin || point.Y <= top + margin ||
               point.X >= left + width - margin || point.Y >= top + height - margin;
    }
}
