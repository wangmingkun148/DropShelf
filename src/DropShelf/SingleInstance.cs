using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using DropShelf.Interop;
using DropShelf.Services;

namespace DropShelf;

/// <summary>
/// 程序入口。先用系统调用判断是不是已经在运行：“发送到”菜单、拖到程序图标上或重复启动时打开的第二个进程，
/// 把文件转交给已经运行的实例（或唤醒它的主窗口）后直接退出，不必先加载 WPF、解析界面样式。
/// </summary>
internal static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        using var mutex = new Mutex(true, SingleInstance.MutexName, out var owns);
        if (!owns)
        {
            SingleInstance.HandOver(args);
            return;
        }
        try
        {
            RunApp();
        }
        finally
        {
            mutex.ReleaseMutex();
        }
    }

    /// <summary>单独一个方法：转交给已有实例时 Main 不会因为编译到这里而加载 WPF。</summary>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void RunApp()
    {
        var app = new App();
        app.InitializeComponent();
        app.Run();
    }
}

/// <summary>单实例的名称，以及把命令行转交给已经运行的实例。这里只用到系统调用。</summary>
internal static class SingleInstance
{
    private const int CopyDataMarker = 0x44534831;
    private const char ArgumentSeparator = '\n';

    /// <summary>开发调试时可通过环境变量使用独立实例，避免与已安装的版本冲突。</summary>
    public static string InstanceSuffix =>
        Environment.GetEnvironmentVariable("DROPSHELF_INSTANCE") is { Length: > 0 } suffix ? "." + suffix : string.Empty;

    public static string MutexName => @"Local\DropShelf.SingleInstance.v1" + InstanceSuffix;
    public static string ShowWindowEventName => @"Local\DropShelf.ShowMainWindow.v1" + InstanceSuffix;
    public static string MessageWindowTitle => "DropShelfMessageWindow" + InstanceSuffix;

    /// <summary>命令行包含 --shelf 或直接带有文件路径（“发送到”菜单、拖到程序图标上）。</summary>
    public static bool IsShelfRequest(IReadOnlyList<string> arguments) =>
        arguments.Any(argument => string.Equals(argument, "--shelf", StringComparison.OrdinalIgnoreCase)) ||
        GetPathArguments(arguments).Count > 0;

    public static IReadOnlyList<string> GetPathArguments(IReadOnlyList<string> arguments) => arguments
        .Where(argument => !argument.StartsWith("--", StringComparison.Ordinal))
        .Where(argument => System.IO.File.Exists(argument) || System.IO.Directory.Exists(argument))
        .ToArray();

    /// <summary>
    /// 已在运行：“发送到”或拖到图标上的文件交给现有实例，否则唤醒主窗口。
    /// 先允许现有实例切到前台，否则它的窗口可能打开在其他窗口后面，看起来像没有反应。
    /// </summary>
    public static void HandOver(IReadOnlyList<string> arguments)
    {
        NativeMethods.AllowSetForegroundWindow(-1);
        var forwarded = IsShelfRequest(arguments) && SendArguments(MessageWindowTitle, arguments);
        if (!forwarded) SignalExistingInstance();
    }

    /// <summary>把命令行发给标题为 title 的消息窗口；找不到窗口或对方没有回应时返回 false。</summary>
    public static bool SendArguments(string title, IReadOnlyList<string> arguments)
    {
        var window = NativeMethods.FindWindow(null, title);
        if (window == IntPtr.Zero) return false;
        var text = string.Join(ArgumentSeparator, arguments);
        var pointer = Marshal.StringToHGlobalUni(text);
        try
        {
            var data = new COPYDATASTRUCT { Data = new IntPtr(CopyDataMarker), Size = (text.Length + 1) * 2, Pointer = pointer };
            return NativeMethods.SendMessageTimeout(window, NativeMethods.WM_COPYDATA, IntPtr.Zero, ref data, 0x0002, 3000, out _) != IntPtr.Zero;
        }
        finally
        {
            Marshal.FreeHGlobal(pointer);
        }
    }

    /// <summary>收到的转交内容；不是本程序发来的返回 null。</summary>
    public static string[]? ReadArguments(COPYDATASTRUCT data)
    {
        if (data.Data.ToInt64() != CopyDataMarker || data.Pointer == IntPtr.Zero || data.Size <= 0) return null;
        return Marshal.PtrToStringUni(data.Pointer, data.Size / 2).TrimEnd('\0').Split(ArgumentSeparator, StringSplitOptions.RemoveEmptyEntries);
    }

    private static void SignalExistingInstance()
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            try
            {
                using var showEvent = EventWaitHandle.OpenExisting(ShowWindowEventName);
                showEvent.Set();
                return;
            }
            catch (WaitHandleCannotBeOpenedException)
            {
                Thread.Sleep(80);
            }
            catch (Exception exception)
            {
                LogService.Error("无法唤醒已经运行的 DropShelf", exception);
                return;
            }
        }
    }
}
