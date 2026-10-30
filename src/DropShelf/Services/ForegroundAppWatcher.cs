using System.IO;
using System.Text;
using DropShelf.Interop;

namespace DropShelf.Services;

/// <summary>
/// 监视前台窗口属于哪个程序。屏蔽列表中的程序在前台时暂停晃动、边缘停留和快捷键，
/// 快捷键注销后按键直接交给该程序。列表为空时不挂钩。
/// </summary>
public sealed class ForegroundAppWatcher : IDisposable
{
    private readonly NativeMethods.WinEventProc _callback;
    private HashSet<string> _blocked = new(StringComparer.OrdinalIgnoreCase);
    private IntPtr _hook;

    // 系统通过这个委托回调，必须一直持有，否则被回收后会崩溃。
    // 通知可能晚到、一次切换也可能连续收到几条，每次都以当前真正的前台窗口为准。
    public ForegroundAppWatcher() => _callback = (_, _, _, _, _, _, _) => Update(NativeMethods.GetForegroundWindow());

    public bool IsBlocked { get; private set; }

    public event EventHandler? BlockedChanged;

    public void SetBlockedApps(IEnumerable<string> names)
    {
        _blocked = new HashSet<string>(names, StringComparer.OrdinalIgnoreCase);
        if (_blocked.Count > 0 && _hook == IntPtr.Zero)
            _hook = NativeMethods.SetWinEventHook(NativeMethods.EVENT_SYSTEM_FOREGROUND, NativeMethods.EVENT_SYSTEM_FOREGROUND,
                IntPtr.Zero, _callback, 0, 0, NativeMethods.WINEVENT_OUTOFCONTEXT);
        else if (_blocked.Count == 0) Unhook();
        Update(NativeMethods.GetForegroundWindow());
    }

    public void Dispose() => Unhook();

    /// <summary>按快捷键时再确认一次前台程序，并校正暂停状态。</summary>
    public bool CheckForeground()
    {
        Update(NativeMethods.GetForegroundWindow());
        return IsBlocked;
    }

    /// <summary>
    /// 识别到手势时再确认一次：前台窗口或光标下的窗口属于屏蔽的程序。
    /// 光标下的也算：从别的程序把文件拖到屏蔽的程序上方时，前台仍是来源程序。
    /// </summary>
    public bool IsBlockedAt(POINT point) => CheckForeground() ||
        _blocked.Count > 0 && IsBlockedWindow(NativeMethods.GetAncestor(NativeMethods.WindowFromPoint(point), NativeMethods.GA_ROOT));

    /// <summary>当前有可见窗口的程序文件名，按名称排序，不含本程序。</summary>
    public static IReadOnlyList<string> GetWindowedApps()
    {
        var processes = new HashSet<uint>();
        NativeMethods.EnumWindows((window, _) =>
        {
            // 与任务栏相同的筛选：可见、没有所有者、有标题，且没有被系统隐藏（挂起的应用商店应用）。
            if (NativeMethods.IsWindowVisible(window) && NativeMethods.GetWindow(window, NativeMethods.GW_OWNER) == IntPtr.Zero &&
                NativeMethods.GetWindowTextLength(window) > 0 &&
                (NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DWMWA_CLOAKED, out var cloaked, sizeof(int)) != 0 || cloaked == 0))
            {
                NativeMethods.GetWindowThreadProcessId(window, out var process);
                processes.Add(process);
            }
            return true;
        }, IntPtr.Zero);
        processes.Remove((uint)Environment.ProcessId);
        return processes.Select(GetProcessFileName).OfType<string>()
            .Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private bool IsBlockedWindow(IntPtr window)
    {
        if (window == IntPtr.Zero) return false;
        NativeMethods.GetWindowThreadProcessId(window, out var process);
        return GetProcessFileName(process) is { } name && _blocked.Contains(name);
    }

    private void Update(IntPtr window)
    {
        var blocked = _blocked.Count > 0 && IsBlockedWindow(window);
        if (blocked == IsBlocked) return;
        IsBlocked = blocked;
        LogService.Trace(blocked ? "屏蔽的应用切到前台，暂停弹出置物架" : "屏蔽的应用离开前台，恢复弹出置物架");
        BlockedChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>进程的程序文件名，如 Photoshop.exe。以管理员身份运行的程序也能读取。</summary>
    private static string? GetProcessFileName(uint processId)
    {
        var process = NativeMethods.OpenProcess(NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION, false, processId);
        if (process == IntPtr.Zero) return GetProtectedProcessFileName(processId);
        try
        {
            var path = new StringBuilder(1024);
            var size = path.Capacity;
            return NativeMethods.QueryFullProcessImageName(process, 0, path, ref size) ? Path.GetFileName(path.ToString()) : null;
        }
        finally
        {
            NativeMethods.CloseHandle(process);
        }
    }

    /// <summary>带反作弊等保护的进程不允许打开，改从系统的进程列表读取名称。</summary>
    private static string? GetProtectedProcessFileName(uint processId)
    {
        try
        {
            using var process = System.Diagnostics.Process.GetProcessById((int)processId);
            return process.ProcessName + ".exe";
        }
        catch (Exception exception) when (exception is ArgumentException or InvalidOperationException)
        {
            // 进程已经退出。
            return null;
        }
    }

    private void Unhook()
    {
        if (_hook == IntPtr.Zero) return;
        NativeMethods.UnhookWinEvent(_hook);
        _hook = IntPtr.Zero;
    }
}
