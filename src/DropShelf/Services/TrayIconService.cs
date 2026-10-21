using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Interop;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;

namespace DropShelf.Services;

/// <summary>隐藏的顶层消息窗口：接收托盘图标回调、全局快捷键和资源管理器重启通知。</summary>
public sealed class MessageWindow : IDisposable
{
    private const int CopyDataMarker = 0x44534831;
    private const char ArgumentSeparator = '\n';
    private readonly HwndSource _source;

    public MessageWindow(string title)
    {
        var parameters = new HwndSourceParameters(title)
        {
            Width = 0,
            Height = 0,
            WindowStyle = 0,
            ExtendedWindowStyle = NativeMethods.WS_EX_TOOLWINDOW
        };
        _source = new HwndSource(parameters);
        _source.AddHook(OnMessage);
    }

    public IntPtr Handle => _source.Handle;

    public event Func<int, IntPtr, IntPtr, bool>? MessageReceived;

    /// <summary>另一个 DropShelf 进程转交过来的命令行参数（例如“发送到”菜单中的文件）。</summary>
    public event Action<string[]>? ArgumentsReceived;

    /// <summary>把参数交给已经运行的实例；找不到或对方没有响应时返回 false。</summary>
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

    private IntPtr OnMessage(IntPtr window, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (message == NativeMethods.WM_COPYDATA)
        {
            var data = Marshal.PtrToStructure<COPYDATASTRUCT>(lParam);
            if (data.Data.ToInt64() == CopyDataMarker && data.Pointer != IntPtr.Zero && data.Size > 0)
            {
                var text = Marshal.PtrToStringUni(data.Pointer, data.Size / 2).TrimEnd(char.MinValue);
                var arguments = text.Split(ArgumentSeparator, StringSplitOptions.RemoveEmptyEntries);
                Dispatcher.CurrentDispatcher.BeginInvoke(() => ArgumentsReceived?.Invoke(arguments));
                handled = true;
                return new IntPtr(1);
            }
        }
        if (MessageReceived?.Invoke(message, wParam, lParam) == true) handled = true;
        return IntPtr.Zero;
    }

    public void Dispose() => _source.Dispose();
}

public sealed record TrayMenuState(string Hotkey, IReadOnlyList<ShelfHistoryEntry> History);

public sealed class TrayIconService : IDisposable
{
    private const uint IconId = 1;
    private const int CallbackMessage = NativeMethods.WM_APP + 1;
    private readonly MessageWindow _window;
    private readonly uint _taskbarCreatedMessage;
    private readonly Func<TrayMenuState> _getState;
    private readonly Action _showWindow;
    private readonly Action _newShelf;
    private readonly Action<ShelfHistoryEntry> _restore;
    private readonly Action _clearHistory;
    private readonly Action _exit;
    private IntPtr _icon;
    private bool _added;
    private bool _disposed;
    private ContextMenu? _menu;

    public TrayIconService(MessageWindow window, Func<TrayMenuState> getState, Action showWindow, Action newShelf,
        Action<ShelfHistoryEntry> restore, Action clearHistory, Action exit)
    {
        _window = window;
        _getState = getState;
        _showWindow = showWindow;
        _newShelf = newShelf;
        _restore = restore;
        _clearHistory = clearHistory;
        _exit = exit;
        _taskbarCreatedMessage = NativeMethods.RegisterWindowMessage("TaskbarCreated");
        _icon = LoadTrayIcon();
        _window.MessageReceived += OnMessage;
        AddIcon();
    }

    public void ShowStillRunningHint()
    {
        if (_disposed || !_added) return;
        var data = CreateData(NativeMethods.NIF_INFO);
        data.InfoTitle = Strings.StillRunningTitle;
        data.Info = Strings.StillRunningBody;
        data.InfoFlags = NativeMethods.NIIF_INFO | NativeMethods.NIIF_RESPECT_QUIET_TIME;
        NativeMethods.Shell_NotifyIcon(NativeMethods.NIM_MODIFY, ref data);
    }

    /// <summary>切换界面语言后更新托盘提示文字。</summary>
    public void RefreshText()
    {
        if (_disposed || !_added) return;
        var data = CreateData(NativeMethods.NIF_TIP | NativeMethods.NIF_SHOWTIP);
        NativeMethods.Shell_NotifyIcon(NativeMethods.NIM_MODIFY, ref data);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _window.MessageReceived -= OnMessage;
        if (_added)
        {
            var data = CreateData(0);
            NativeMethods.Shell_NotifyIcon(NativeMethods.NIM_DELETE, ref data);
            _added = false;
        }
        if (_icon != IntPtr.Zero) NativeMethods.DestroyIcon(_icon);
        _icon = IntPtr.Zero;
    }

    private void AddIcon()
    {
        var data = CreateData(NativeMethods.NIF_MESSAGE | NativeMethods.NIF_ICON | NativeMethods.NIF_TIP | NativeMethods.NIF_SHOWTIP);
        _added = NativeMethods.Shell_NotifyIcon(NativeMethods.NIM_ADD, ref data);
        if (!_added) return;
        data.TimeoutOrVersion = NativeMethods.NOTIFYICON_VERSION_4;
        NativeMethods.Shell_NotifyIcon(NativeMethods.NIM_SETVERSION, ref data);
    }

    private NOTIFYICONDATAW CreateData(uint flags) => new()
    {
        Size = Marshal.SizeOf<NOTIFYICONDATAW>(),
        Window = _window.Handle,
        Id = IconId,
        Flags = flags,
        CallbackMessage = CallbackMessage,
        Icon = _icon,
        Tip = Strings.TrayTip,
        Info = string.Empty,
        InfoTitle = string.Empty
    };

    private bool OnMessage(int message, IntPtr wParam, IntPtr lParam)
    {
        if (message == (int)_taskbarCreatedMessage && _taskbarCreatedMessage != 0)
        {
            // 资源管理器重启后需要重新添加托盘图标。
            _added = false;
            AddIcon();
            return false;
        }

        if (message != CallbackMessage) return false;
        switch ((int)(lParam.ToInt64() & 0xFFFF))
        {
            case NativeMethods.NIN_SELECT:
            case NativeMethods.NIN_KEYSELECT:
            case NativeMethods.WM_LBUTTONUP:
                _showWindow();
                return true;
            case NativeMethods.WM_CONTEXTMENU:
            case NativeMethods.WM_RBUTTONUP:
                ShowMenu();
                return true;
        }
        return false;
    }

    private void ShowMenu()
    {
        if (_menu is { IsOpen: true }) return;
        var state = _getState();
        _menu = new ContextMenu { Placement = PlacementMode.MousePoint };
        _menu.Items.Add(CreateItem(Strings.NewShelf, "", _newShelf, state.Hotkey));
        if (state.History.Count > 0)
        {
            _menu.Items.Add(Views.HistoryMenu.Create(state.History, _restore, _clearHistory));
        }
        else
        {
            var empty = CreateItem(Strings.RecentShelves, "\uE81C", () => { });
            empty.IsEnabled = false;
            _menu.Items.Add(empty);
        }
        _menu.Items.Add(new Separator());
        _menu.Items.Add(CreateItem(Strings.OpenSettings, "", _showWindow));
        _menu.Items.Add(CreateItem(Strings.QuitApp, "", _exit));
        _menu.IsOpen = true;

        // 先把菜单窗口设为前台，点击其他位置时菜单才会正常关闭。
        if (PresentationSource.FromVisual(_menu) is HwndSource source)
            NativeMethods.SetForegroundWindow(source.Handle);
    }

    private static MenuItem CreateItem(string header, string glyph, Action action, string? gesture = null)
    {
        var item = new MenuItem
        {
            Header = header,
            Icon = new TextBlock { Text = glyph },
            InputGestureText = gesture ?? string.Empty
        };
        item.Click += (_, _) => action();
        return item;
    }

    private static IntPtr LoadTrayIcon()
    {
        try
        {
            var resource = System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/DropShelf;component/Assets/DropShelf.ico"));
            if (resource is null) return IntPtr.Zero;
            using var stream = resource.Stream;
            using var memory = new MemoryStream();
            stream.CopyTo(memory);
            var size = NativeMethods.GetSystemMetrics(NativeMethods.SM_CXSMICON);
            return IconLoader.CreateFromIco(memory.ToArray(), size <= 0 ? 16 : size);
        }
        catch (Exception exception)
        {
            LogService.Error("加载托盘图标失败", exception);
            return IntPtr.Zero;
        }
    }
}

internal static class IconLoader
{
    /// <summary>从 .ico 文件内容中选择最接近目标尺寸的图像并创建图标句柄。</summary>
    public static IntPtr CreateFromIco(byte[] ico, int desiredSize)
    {
        if (ico.Length < 6) return IntPtr.Zero;
        var count = BitConverter.ToUInt16(ico, 4);
        var bestIndex = -1;
        var bestScore = int.MaxValue;
        for (var index = 0; index < count; index++)
        {
            var entry = 6 + index * 16;
            if (entry + 16 > ico.Length) break;
            var width = ico[entry] == 0 ? 256 : ico[entry];
            var score = width >= desiredSize ? width - desiredSize : (desiredSize - width) * 4;
            if (score < bestScore)
            {
                bestScore = score;
                bestIndex = index;
            }
        }
        if (bestIndex < 0) return IntPtr.Zero;

        var position = 6 + bestIndex * 16;
        var length = (int)BitConverter.ToUInt32(ico, position + 8);
        var offset = (int)BitConverter.ToUInt32(ico, position + 12);
        if (offset < 0 || length <= 0 || offset + length > ico.Length) return IntPtr.Zero;
        var image = new byte[length];
        Buffer.BlockCopy(ico, offset, image, 0, length);
        return NativeMethods.CreateIconFromResourceEx(image, (uint)length, true, 0x00030000, desiredSize, desiredSize, 0);
    }
}
