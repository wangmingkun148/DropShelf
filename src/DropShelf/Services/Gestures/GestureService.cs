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
/// 在独立线程上用原始输入（Raw Input）监听按住鼠标时的晃动，以及拖到屏幕边缘后的停留。
/// 原始输入由系统异步投递，不像低级鼠标钩子那样每次移动都要等本程序处理完才放行，
/// 因此本程序繁忙、被系统降频或回收内存时都不会拖慢鼠标。
/// 识别到手势后再交给界面线程用探针窗口确认是否真的在拖放。
/// </summary>
public sealed class GestureService : IDisposable
{
    /// <summary>光标停着不动时没有鼠标输入，按住期间用计时器定期检查是否停在边缘。</summary>
    private const uint EdgePollMilliseconds = 40;
    private const int EdgeTolerance = 1;

    private static readonly uint RawInputSize = (uint)Marshal.SizeOf<RAWINPUTMOUSE>();
    private static readonly uint RawHeaderSize = (uint)Marshal.SizeOf<RAWINPUTHEADER>();

    private readonly Dispatcher _dispatcher;
    private readonly ManualResetEventSlim _started = new();
    private readonly EdgeDwellDetector _edgeDetector;
    private Thread? _thread;
    private uint _threadId;
    private IntPtr _window;
    private volatile bool _listening;
    private volatile bool _shakeEnabled = true;
    private volatile bool _edgeEnabled;
    private volatile bool _paused;
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

    public GestureService(Dispatcher dispatcher, int sensitivity)
    {
        _dispatcher = dispatcher;
        _requestedSensitivity = sensitivity;
        _appliedSensitivity = sensitivity;
        _detector = new MouseGestureDetector(sensitivity);
        _requestedDwell = AutoOpenTriggers.DwellMilliseconds(EdgeDwellTime.Medium);
        _edgeDetector = new EdgeDwellDetector(_requestedDwell);
    }

    public bool IsRunning => _listening;

    public void SetSensitivity(int sensitivity) => _requestedSensitivity = sensitivity;

    /// <summary>选择用晃动、屏幕边缘停留或两者来呼出置物架。</summary>
    public void SetTrigger(AutoOpenTrigger trigger)
    {
        _shakeEnabled = AutoOpenTriggers.UsesShake(trigger);
        _edgeEnabled = AutoOpenTriggers.UsesEdge(trigger);
    }

    public void SetEdgeDwell(EdgeDwellTime dwell) => _requestedDwell = AutoOpenTriggers.DwellMilliseconds(dwell);

    /// <summary>屏蔽的应用在前台时暂停识别晃动和边缘停留。</summary>
    public void SetPaused(bool paused) => _paused = paused;

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
        _listening = StartListening();
        _started.Set();

        while (NativeMethods.GetMessage(out var message, IntPtr.Zero, 0, 0) > 0)
        {
            if (message.Message == NativeMethods.WM_TIMER && message.Window == IntPtr.Zero && (UIntPtr)message.WParam == _edgeTimer)
            {
                OnEdgeTimer();
                continue;
            }
            // 原始输入处理完仍要交给默认窗口过程，系统才会释放这条输入。
            if (message.Message == NativeMethods.WM_INPUT) OnInput(message.LParam);
            NativeMethods.TranslateMessage(ref message);
            NativeMethods.DispatchMessage(ref message);
        }

        StopEdgeTimer();
        StopListening();
    }

    /// <summary>创建只接收消息的隐藏窗口，并登记为程序在后台时也接收鼠标原始输入。</summary>
    private bool StartListening()
    {
        _window = NativeMethods.CreateWindowEx(0, "STATIC", "DropShelf Input", 0, 0, 0, 0, 0,
            NativeMethods.HWND_MESSAGE, IntPtr.Zero, NativeMethods.GetModuleHandle(null), IntPtr.Zero);
        if (_window == IntPtr.Zero)
        {
            LogService.Info($"鼠标输入窗口创建失败，错误码 {Marshal.GetLastWin32Error()}");
            return false;
        }
        if (RegisterMouse(NativeMethods.RIDEV_INPUTSINK, _window)) return true;

        LogService.Info($"鼠标原始输入注册失败，错误码 {Marshal.GetLastWin32Error()}");
        NativeMethods.DestroyWindow(_window);
        _window = IntPtr.Zero;
        return false;
    }

    private void StopListening()
    {
        if (_listening) RegisterMouse(NativeMethods.RIDEV_REMOVE, IntPtr.Zero);
        _listening = false;
        if (_window != IntPtr.Zero) NativeMethods.DestroyWindow(_window);
        _window = IntPtr.Zero;
    }

    private static bool RegisterMouse(uint flags, IntPtr target) => NativeMethods.RegisterRawInputDevices(
        [new RAWINPUTDEVICE
        {
            UsagePage = NativeMethods.HID_USAGE_PAGE_GENERIC,
            Usage = NativeMethods.HID_USAGE_GENERIC_MOUSE,
            Flags = flags,
            Target = target
        }], 1, (uint)Marshal.SizeOf<RAWINPUTDEVICE>());

    /// <summary>处理一条鼠标原始输入。原始输入只有相对位移，光标位置另行读取。</summary>
    private void OnInput(IntPtr rawInput)
    {
        try
        {
            var size = RawInputSize;
            if (NativeMethods.GetRawInputData(rawInput, NativeMethods.RID_INPUT, out var input, ref size, RawHeaderSize) == uint.MaxValue ||
                input.Header.Type != NativeMethods.RIM_TYPEMOUSE) return;

            // 同一条输入可能同时包含按下、移动和松开，按发生顺序处理。
            var buttons = input.Mouse.ButtonFlags;
            if ((buttons & NativeMethods.RI_MOUSE_LEFT_BUTTON_DOWN) != 0) Press(left: true);
            if ((buttons & NativeMethods.RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) Press(left: false);
            if (input.Mouse.LastX != 0 || input.Mouse.LastY != 0 || (input.Mouse.Flags & NativeMethods.MOUSE_MOVE_ABSOLUTE) != 0)
                OnMove();
            if ((buttons & NativeMethods.RI_MOUSE_LEFT_BUTTON_UP) != 0) Release(left: true);
            if ((buttons & NativeMethods.RI_MOUSE_RIGHT_BUTTON_UP) != 0) Release(left: false);
        }
        catch
        {
            // 单条输入处理失败不影响之后的识别。
        }
    }

    private void Press(bool left)
    {
        if (!_leftDown && !_rightDown) BeginPress();
        if (left) _leftDown = true;
        else _rightDown = true;
    }

    private void Release(bool left)
    {
        // 计时器发现按键已松开时会提前结束本次按住，之后到达的松开输入直接忽略。
        if (!_leftDown && !_rightDown) return;
        if (left) _leftDown = false;
        else _rightDown = false;
        if (!_leftDown && !_rightDown) EndPress();
    }

    private void OnMove()
    {
        if (!_leftDown && !_rightDown) return;
        _movesThisPress++;
        var now = NowMilliseconds();
        if (now - _lastButtonCheck > 250)
        {
            // 遇到安全桌面等情况可能收不到松开的输入，定期用实际按键状态校正。
            _lastButtonCheck = now;
            if (!NativeMethods.IsButtonPhysicallyDown())
            {
                _leftDown = _rightDown = false;
                EndPress();
                return;
            }
        }

        if (_triggered || _paused || !NativeMethods.GetCursorPos(out var point)) return;
        if (_edgeEnabled && CheckEdge(point, now)) return;
        if (!_shakeEnabled) return;
        ApplySensitivityIfChanged();
        if (!_detector.AddRawSample(point.X, point.Y, now, IsNearOuterScreenEdge(point))) return;

        _triggered = true;
        StopEdgeTimer();
        _dispatcher.BeginInvoke(() => ShakeDetected?.Invoke(this, new GestureTriggeredEventArgs(point.X, point.Y)));
    }

    /// <summary>检查边缘停留；触发时返回 true。</summary>
    private bool CheckEdge(POINT point, long now)
    {
        _edgeDetector.DwellMilliseconds = _requestedDwell;
        var edge = HitTestEdge(point);
        if (!_edgeDetector.Update(edge, point.X, point.Y, now)) return false;

        _triggered = true;
        StopEdgeTimer();
        _dispatcher.BeginInvoke(() => EdgeDwellDetected?.Invoke(this, new GestureTriggeredEventArgs(point.X, point.Y, edge)));
        return true;
    }

    private void OnEdgeTimer()
    {
        if (!_edgeEnabled || _triggered || _paused || (!_leftDown && !_rightDown))
        {
            StopEdgeTimer();
            return;
        }
        if (!NativeMethods.IsButtonPhysicallyDown())
        {
            _leftDown = _rightDown = false;
            EndPress();
            return;
        }
        if (NativeMethods.GetCursorPos(out var point)) CheckEdge(point, NowMilliseconds());
    }

    private void StartEdgeTimer()
    {
        if (_edgeTimer == UIntPtr.Zero) _edgeTimer = NativeMethods.SetTimer(IntPtr.Zero, UIntPtr.Zero, EdgePollMilliseconds, IntPtr.Zero);
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
        if (_edgeEnabled && !_paused) StartEdgeTimer();
    }

    private void EndPress()
    {
        if (LogService.TraceEnabled && _movesThisPress > 20)
            LogService.Trace($"按住期间收到 {_movesThisPress} 次移动，{(_triggered ? "已识别手势" : "未识别手势")}");
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
        _detector.SetSensitivity(requested);
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
