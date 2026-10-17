using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Services;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;

namespace DropShelf.Views;

/// <summary>
/// 晃动后在光标下短暂放置的几乎透明的窗口。系统拖放会把正在拖动的数据交给它，
/// 由此可以准确判断用户是否真的在拖文件、附件或文本，再决定是否弹出置物架。
/// </summary>
public sealed class ProbeWindow : Window
{
    private readonly DispatcherTimer _timeout;

    public ProbeWindow()
    {
        Title = "DropShelf Probe";
        WindowStyle = WindowStyle.None;
        AllowsTransparency = true;
        ResizeMode = ResizeMode.NoResize;
        ShowInTaskbar = false;
        ShowActivated = false;
        Topmost = true;
        Focusable = false;
        AllowDrop = true;
        Width = 320;
        Height = 320;
        // 透明度为 1/255：肉眼不可见，但仍能接收拖放（完全透明的像素会被系统穿透）。
        Background = new SolidColorBrush(System.Windows.Media.Color.FromArgb(1, 128, 128, 128));
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = -32000;
        Top = -32000;

        _timeout = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(900) };
        _timeout.Tick += (_, _) =>
        {
            LogService.Trace("探针窗口超时：没有收到拖放");
            HideProbe();
        };
        SourceInitialized += (_, _) => WindowEffects.SetToolWindow(this, noActivate: true);
        DragEnter += OnDragEnter;
        DragOver += OnDragOver;
    }

    /// <summary>确认正在拖放可接收的内容。</summary>
    public event EventHandler<System.Windows.IDataObject>? DragDetected;

    public void ShowAt(POINT center)
    {
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (!IsVisible) Show();
        WindowPlacement.CenterOn(this, center, Width, Height);
        _timeout.Stop();
        _timeout.Start();
    }

    public void HideProbe()
    {
        _timeout.Stop();
        if (IsVisible) Hide();
    }

    private void OnDragEnter(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        var kind = DropDataReader.Classify(e.Data);
        var fromShelf = DropDataReader.GetSourceShelfId(e.Data) is not null;
        if (LogService.TraceEnabled)
            LogService.Trace($"探针收到拖放：{kind}，来自置物架 {fromShelf}，格式 {string.Join(", ", e.Data.GetFormats(false).Take(20))}");
        e.Effects = kind == DropContentKind.None || fromShelf ? WpfDragDropEffects.None : WpfDragDropEffects.Copy;
        var data = e.Data;
        // 不在系统拖放回调中直接切换窗口，交给下一轮消息处理。
        Dispatcher.BeginInvoke(() =>
        {
            HideProbe();
            if (kind != DropContentKind.None && !fromShelf) DragDetected?.Invoke(this, data);
        });
    }

    private static void OnDragOver(object sender, WpfDragEventArgs e)
    {
        e.Effects = WpfDragDropEffects.None;
        e.Handled = true;
    }
}
