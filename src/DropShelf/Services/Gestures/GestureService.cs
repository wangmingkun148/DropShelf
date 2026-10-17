using System.Runtime.InteropServices;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Models;

namespace DropShelf.Services.Gestures;

public sealed class GestureTriggeredEventArgs(int x, int y) : EventArgs
{
    /// <summary>触发时的光标位置（屏幕物理像素）。</summary>
    public int X { get; } = x;
    public int Y { get; } = y;
}

/// <summary>
/// 在独立线程上用低级鼠标钩子监听按住鼠标时的晃动。钩子回调只做轻量计算，
/// 界面线程繁忙时也不会拖慢系统鼠标；识别到晃动后再交给界面线程用探针窗口确认是否真的在拖放。
/// </summary>
public sealed class GestureService : IDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly LowLevelMouseProc _callback;
    private readonly ManualResetEventSlim _started = new();
    private Thread? _thread;
    private uint _threadId;
    private IntPtr _hook;
    private volatile bool _enabled;
    private volatile int _requestedSensitivity;
    private int _appliedSensitivity;
    private MouseGestureDetector _detector;
    private bool _leftDown;
    private bool _rightDown;
    private bool _triggered;
    private long _lastButtonCheck;
    private int _movesThisPress;
    private bool _disposed;

    public event EventHandler<GestureTriggeredEventArgs>? ShakeDetected;

    /// <summary>识别到晃动后，按住的鼠标键被松开。</summary>
    public event EventHandler? DragReleased;

    public GestureService(Dispatcher dispatcher, GestureSensitivity sensitivity, bool enabled)
    {
        _dispatcher = dispatcher;
        _callback = OnHook;
        _requestedSensitivity = (int)sensitivity;
        _appliedSensitivity = (int)sensitivity;
        _detector = new MouseGestureDetector(sensitivity);
        _enabled = enabled;
    }

    public bool IsRunning => _hook != IntPtr.Zero;

    public bool Enabled
    {
        get => _enabled;
        set => _enabled = value;
    }

    public void SetSensitivity(GestureSensitivity sensitivity) => _requestedSensitivity = (int)sensitivity;

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
            NativeMethods.TranslateMessage(ref message);
            NativeMethods.DispatchMessage(ref message);
        }

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
                ApplySensitivityIfChanged();
                var data = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
                if (!_detector.AddRawSample(data.Point.X, data.Point.Y, now, IsNearOuterScreenEdge(data.Point))) return;

                _triggered = true;
                var point = data.Point;
                _dispatcher.BeginInvoke(() => ShakeDetected?.Invoke(this, new GestureTriggeredEventArgs(point.X, point.Y)));
                break;
        }
    }

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
    }

    private void EndPress()
    {
        if (LogService.TraceEnabled && _movesThisPress > 20)
            LogService.Trace($"按住期间收到 {_movesThisPress} 次移动，{(_triggered ? "已识别晃动" : "未识别晃动")}，启用 {_enabled}");
        var notify = _triggered;
        _triggered = false;
        _detector.Reset();
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
