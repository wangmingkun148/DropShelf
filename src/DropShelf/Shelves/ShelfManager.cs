using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views;

namespace DropShelf.Shelves;

/// <summary>管理同时打开的多个置物架，并预先准备一个隐藏窗口，让晃动后能立即弹出。</summary>
public sealed class ShelfManager : IShelfHost
{
    private readonly Func<AppSettings> _settings;
    private readonly Action _showSettings;
    private readonly Dispatcher _dispatcher;
    private readonly List<ShelfWindow> _open = [];
    private ShelfWindow? _spare;
    private bool _shuttingDown;

    public ShelfManager(Func<AppSettings> settings, Action showSettings, Dispatcher dispatcher)
    {
        _settings = settings;
        _showSettings = showSettings;
        _dispatcher = dispatcher;
    }

    public AppSettings Settings => _settings();
    public IReadOnlyList<ShelfWindow> OpenShelves => _open;

    public event EventHandler? ShelvesChanged;

    /// <summary>在空闲时创建一个隐藏的置物架窗口。</summary>
    public void Prewarm()
    {
        if (_shuttingDown || _spare is not null) return;
        _dispatcher.BeginInvoke(() =>
        {
            if (_shuttingDown || _spare is not null) return;
            _spare = CreateWindow();
            WindowEffects.Handle(_spare);
        }, DispatcherPriority.ApplicationIdle);
    }

    /// <summary>在光标位置打开置物架；晃动打开的空置物架会被复用，避免越开越多。</summary>
    public ShelfWindow OpenAt(POINT center, bool openedByGesture, bool activate)
    {
        var shelf = _open.FirstOrDefault(window => window.IsReusable && window.IsVisible);
        if (shelf is null)
        {
            shelf = _spare ?? CreateWindow();
            _spare = null;
            _open.Add(shelf);
            ShelvesChanged?.Invoke(this, EventArgs.Empty);
        }

        shelf.ViewModel.OpenedByGesture = openedByGesture;
        shelf.ShowAt(center, activate);
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

    public void ApplySettings(AppSettings settings)
    {
        foreach (var shelf in _open) shelf.ApplySettings(settings);
        _spare?.ApplySettings(settings);
    }

    public void CloseAll()
    {
        _shuttingDown = true;
        foreach (var shelf in _open.ToArray()) shelf.Close();
        _open.Clear();
        _spare?.Close();
        _spare = null;
    }

    public void OnShelfClosed(ShelfWindow window)
    {
        if (ReferenceEquals(window, _spare)) _spare = null;
        if (_open.Remove(window)) ShelvesChanged?.Invoke(this, EventArgs.Empty);
    }

    public void ShowSettings() => _showSettings();

    private ShelfWindow CreateWindow() => new(new ShelfViewModel(), this);
}
