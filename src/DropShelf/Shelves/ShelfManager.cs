using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views;

namespace DropShelf.Shelves;

/// <summary>管理同时打开的多个置物架，并预先准备一个隐藏窗口，让晃动后能立即弹出。</summary>
public sealed class ShelfManager : IShelfHost
{
    private readonly Func<AppSettings> _settings;
    private readonly Action _showSettings;
    private readonly Action<Action<AppSettings>> _updateSettings;
    private readonly Dispatcher _dispatcher;
    private readonly List<ShelfWindow> _open = [];
    private ShelfWindow? _spare;
    private DispatcherTimer? _prewarmTimer;
    private bool _shuttingDown;

    public ShelfManager(Func<AppSettings> settings, Action showSettings, Action<Action<AppSettings>> updateSettings,
        ShelfHistoryService? history, Dispatcher dispatcher)
    {
        _settings = settings;
        _showSettings = showSettings;
        _updateSettings = updateSettings;
        History = history;
        _dispatcher = dispatcher;
    }

    public AppSettings Settings => _settings();
    public ShelfHistoryService? History { get; }
    public IReadOnlyList<ShelfWindow> OpenShelves => _open;

    /// <summary>
    /// 准备一个隐藏的置物架窗口。创建窗口会占用界面线程几十毫秒，若在弹出动画或拖动途中进行会造成卡顿，
    /// 所以等鼠标松开、动画结束、置物架不再自动收起后再创建。
    /// </summary>
    public void Prewarm()
    {
        if (_shuttingDown || _spare is not null) return;
        if (_prewarmTimer is null)
        {
            _prewarmTimer = new DispatcherTimer(DispatcherPriority.ApplicationIdle, _dispatcher) { Interval = TimeSpan.FromMilliseconds(600) };
            _prewarmTimer.Tick += OnPrewarmTick;
        }
        _prewarmTimer.Stop();
        _prewarmTimer.Start();
    }

    private void OnPrewarmTick(object? sender, EventArgs e)
    {
        if (_shuttingDown || _spare is not null)
        {
            _prewarmTimer!.Stop();
            return;
        }
        // 仍在拖动、有置物架正在播放动画，或晃动呼出的空置物架松手后马上要收起时，稍后再试。
        if (NativeMethods.IsButtonPhysicallyDown() ||
            _open.Any(shelf => shelf.IsSliding || shelf.ViewModel.OpenedByGesture && shelf.IsReusable)) return;
        _prewarmTimer!.Stop();
        _spare = CreateWindow();
        WindowEffects.Handle(_spare);
    }

    /// <summary>
    /// 在光标位置打开置物架；晃动打开的空置物架会被复用，避免越开越多。
    /// 从屏幕边缘呼出时传入对应的边缘，置物架会从那一侧滑入。
    /// </summary>
    public ShelfWindow OpenAt(POINT center, bool openedByGesture, bool activate, ScreenEdge edge = ScreenEdge.None)
    {
        var shelf = _open.FirstOrDefault(window => window.IsReusable && window.IsVisible);
        if (shelf is null)
        {
            shelf = _spare ?? CreateWindow();
            _spare = null;
            _open.Add(shelf);
        }

        shelf.ViewModel.OpenedByGesture = openedByGesture;
        shelf.ShowAt(center, activate, openedByGesture ? edge == ScreenEdge.None ? ShelfEntrance.Shake : ShelfEntrance.FromEdge(edge) : ShelfEntrance.Plain);
        Prewarm();
        return shelf;
    }

    public ShelfWindow OpenAtCursor(bool activate) => OpenAt(WindowPlacement.GetCursor(), openedByGesture: false, activate);

    /// <summary>晃动后松开鼠标：没有收到任何内容的置物架自动关闭。</summary>
    public void OnDragReleased()
    {
        _dispatcher.BeginInvoke(async () =>
        {
            // 放下操作的数据交接可能晚于松开鼠标到达，稍等片刻再判断。
            await Task.Delay(350);
            foreach (var shelf in _open.ToArray())
            {
                if (shelf.ViewModel.OpenedByGesture && shelf.IsReusable && !shelf.IsDraggingOut)
                    shelf.CloseAnimated();
            }
        });
    }

    /// <summary>重新打开最近关闭的置物架：放进指定的置物架，或在光标处打开一个。已删除或移动的文件会跳过。</summary>
    public void Restore(ShelfHistoryEntry entry, ShelfWindow? target = null)
    {
        var shelf = target is { IsClosing: false } ? target : OpenAtCursor(activate: true);
        var summary = shelf.ViewModel.AddFiles(entry.Items.Select(item => item.Path));
        History?.Remove(entry);
        shelf.ViewModel.HasInteracted = true;
        shelf.ViewModel.Notify(summary.Added > 0 ? Strings.Restored(summary.Added, summary.Unavailable) : Strings.RestoreUnavailable,
            summary.Added > 0);
    }

    /// <summary>在窗口旁边打开置物架：优先放在右侧，放不下时放在左侧，两侧都放不下时贴着窗口右边缘。</summary>
    public ShelfWindow OpenNextTo(System.Windows.Window anchor, bool activate)
    {
        var handle = new System.Windows.Interop.WindowInteropHelper(anchor).Handle;
        if (!NativeMethods.GetWindowRect(handle, out var rect)) return OpenAtCursor(activate);
        var (work, scale) = WindowPlacement.GetMonitor(new POINT(rect.Left + rect.Width / 2, rect.Top + rect.Height / 2));
        var width = (int)Math.Round(QuickShelfSizeOptions.ResolveWidth(Settings.QuickShelfSize) * scale);
        var gap = (int)Math.Round(12 * scale);
        var x = rect.Right + gap + width <= work.Right ? rect.Right + gap + width / 2
            : rect.Left - gap - width >= work.Left ? rect.Left - gap - width / 2
            : rect.Right - gap - width / 2;
        return OpenAt(new POINT(x, rect.Top + rect.Height / 2), openedByGesture: false, activate);
    }

    /// <summary>操作生成的新文件放进旁边的新置物架。</summary>
    public void OpenBeside(ShelfWindow source, IReadOnlyList<string> paths) =>
        OpenNextTo(source, activate: false).ViewModel.AddFiles(paths);

    public void SetDragOutMode(TransferMode mode) => _updateSettings(settings => settings.DragOutMode = mode);

    public void ApplySettings(AppSettings settings)
    {
        foreach (var shelf in _open) shelf.ApplySettings(settings);
        _spare?.ApplySettings(settings);
    }

    public void CloseAll()
    {
        _shuttingDown = true;
        _prewarmTimer?.Stop();
        foreach (var shelf in _open.ToArray()) shelf.Close();
        _open.Clear();
        _spare?.Close();
        _spare = null;
    }

    public void OnShelfClosed(ShelfWindow window, IReadOnlyList<ShelfItem> items)
    {
        // 关闭时仍有文件的置物架记入最近关闭，误关或退出后可以恢复。
        if (items.Count > 0) History?.Record(items);
        if (ReferenceEquals(window, _spare)) _spare = null;
        _open.Remove(window);
        // 拖入的数据、缩略图和动画位图大多占用原生内存，托管堆很小时很少触发回收；全部关闭后空闲时回收一次。
        if (_open.Count == 0 && !_shuttingDown) _dispatcher.BeginInvoke(() => GC.Collect(), DispatcherPriority.ApplicationIdle);
    }

    public void ShowSettings() => _showSettings();

    private ShelfWindow CreateWindow() => new(new ShelfViewModel(), this);
}
