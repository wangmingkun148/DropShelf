using System.Windows.Input;
using DropShelf.Interop;
using DropShelf.Models;

namespace DropShelf.Services;

/// <summary>注册系统级快捷键，按下时在光标处打开新的置物架。</summary>
public sealed class HotkeyService : IDisposable
{
    private const int HotkeyId = 0x4453;
    private readonly MessageWindow _window;
    private bool _registered;
    private bool _paused;

    public HotkeyService(MessageWindow window)
    {
        _window = window;
        _window.MessageReceived += OnMessage;
    }

    public event EventHandler? Pressed;

    public HotkeyGesture Current { get; private set; }

    /// <summary>注册新的快捷键；失败时（通常是被其他程序占用）返回 false，之前的快捷键已被注销。</summary>
    public bool Register(HotkeyGesture gesture)
    {
        Unregister();
        Current = gesture;
        if (gesture.IsEmpty || !gesture.IsValid) return gesture.IsEmpty;
        if (_paused) return true;

        uint modifiers = NativeMethods.MOD_NOREPEAT;
        if (gesture.Modifiers.HasFlag(ModifierKeys.Alt)) modifiers |= NativeMethods.MOD_ALT;
        if (gesture.Modifiers.HasFlag(ModifierKeys.Control)) modifiers |= NativeMethods.MOD_CONTROL;
        if (gesture.Modifiers.HasFlag(ModifierKeys.Shift)) modifiers |= NativeMethods.MOD_SHIFT;
        if (gesture.Modifiers.HasFlag(ModifierKeys.Windows)) modifiers |= NativeMethods.MOD_WIN;
        var virtualKey = (uint)KeyInterop.VirtualKeyFromKey(gesture.Key);
        _registered = NativeMethods.RegisterHotKey(_window.Handle, HotkeyId, modifiers, virtualKey);
        if (!_registered) LogService.Info($"快捷键 {gesture} 注册失败，可能已被其他程序占用");
        return _registered;
    }

    /// <summary>暂停时注销快捷键，按键交给前台的程序；恢复时重新注册。</summary>
    public void SetPaused(bool paused)
    {
        if (paused == _paused) return;
        _paused = paused;
        if (paused) Unregister();
        else Register(Current);
    }

    public void Unregister()
    {
        if (!_registered) return;
        NativeMethods.UnregisterHotKey(_window.Handle, HotkeyId);
        _registered = false;
    }

    public void Dispose()
    {
        Unregister();
        _window.MessageReceived -= OnMessage;
    }

    private bool OnMessage(int message, IntPtr wParam, IntPtr lParam)
    {
        if (message != NativeMethods.WM_HOTKEY || wParam.ToInt32() != HotkeyId) return false;
        Pressed?.Invoke(this, EventArgs.Empty);
        return true;
    }
}
