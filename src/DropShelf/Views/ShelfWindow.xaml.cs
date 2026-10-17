using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

public interface IShelfHost
{
    AppSettings Settings { get; }
    void OnShelfClosed(ShelfWindow window);
    void ShowSettings();
}

public partial class ShelfWindow : Window
{
    private const double HeaderHeight = 40;
    private const double CaptionHeight = 38;
    private const double ActionBarHeight = 50;
    private const double RowHeight = 46;
    private const int WS_SYSMENU = 0x00080000;
    private const int WS_MINIMIZEBOX = 0x00020000;
    private const int WS_MAXIMIZEBOX = 0x00010000;
    private const int GWL_STYLE = -16;

    private readonly IShelfHost _host;
    private readonly DropTargetHelper _dropHelper = new();
    private readonly DispatcherTimer _toastTimer;
    private readonly DispatcherTimer _leaveCheck;
    private readonly List<(Border Tile, TextBlock Glyph, TextBlock Label, QuickActionInfo Info)> _tiles = [];
    private QuickShelfSize _size;
    private bool _closing;
    private bool _closed;
    private bool _dragInside;
    private bool _dragFromSelf;
    private DropContentKind _dragKind;
    private QuickActionInfo? _hoveredAction;
    private string? _lastDescription;
    private long _lastDragOverTick;
    private WpfPoint _pressPoint;
    private bool _stackPressed;
    private bool _listPressed;
    private bool _deferSelection;
    private ShelfItem? _pressedItem;

    public ShelfWindow(ShelfViewModel viewModel, IShelfHost host)
    {
        InitializeComponent();
        ViewModel = viewModel;
        _host = host;
        DataContext = viewModel;
        viewModel.PropertyChanged += OnViewModelPropertyChanged;
        viewModel.ToastRequested += (_, toast) => ShowToast(toast);

        _toastTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2.4) };
        _toastTimer.Tick += (_, _) => HideToast();
        _leaveCheck = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(220) };
        _leaveCheck.Tick += OnLeaveCheck;

        SourceInitialized += OnSourceInitialized;
        ThemeService.ThemeChanged += OnThemeChanged;
        DragEnter += OnDragEnter;
        DragOver += OnDragOver;
        DragLeave += OnDragLeave;
        Drop += OnDrop;
        PreviewKeyDown += OnPreviewKeyDown;
        Activated += (_, _) => ViewModel.RefreshAvailability();

        Surface.Opacity = 0;
        ApplySettings(host.Settings);
        UpdateViews();
    }

    public ShelfViewModel ViewModel { get; }
    public bool IsClosing => _closing || _closed;
    public bool IsDraggingOut { get; private set; }
    public bool BackdropActive { get; private set; }

    /// <summary>空的、未被使用过的置物架可以直接挪到新的位置复用。</summary>
    public bool IsReusable => !IsClosing && ViewModel.IsEmpty && !ViewModel.IsPinned && !ViewModel.IsBusy && !ViewModel.HasInteracted;

    public void ApplySettings(AppSettings settings)
    {
        _size = settings.QuickShelfSize;
        ViewModel.Actions = settings.QuickActions?.Select(QuickActions.Get).ToArray() ?? [];
        BuildActions();
        UpdateWindowSize();
    }

    /// <summary>以屏幕像素坐标为中心显示置物架。</summary>
    public void ShowAt(POINT center, bool activate)
    {
        ShowActivated = activate;
        WindowPlacement.CenterOn(this, center, Width, Height);
        var wasVisible = IsVisible;
        if (!wasVisible) Show();
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (activate)
        {
            Activate();
            Focus();
        }
        if (!wasVisible) AnimateIn();
    }

    public void CloseAnimated()
    {
        if (_closing || _closed) return;
        _closing = true;
        IsHitTestVisible = false;
        var duration = TimeSpan.FromMilliseconds(120);
        var fade = new DoubleAnimation(0, duration) { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseIn } };
        fade.Completed += (_, _) => Close();
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(0.96, duration));
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(0.96, duration));
        Surface.BeginAnimation(OpacityProperty, fade);
    }

    protected override void OnClosed(EventArgs e)
    {
        _closed = true;
        _toastTimer.Stop();
        _leaveCheck.Stop();
        ThemeService.ThemeChanged -= OnThemeChanged;
        ViewModel.PropertyChanged -= OnViewModelPropertyChanged;
        ViewModel.ReleaseResources();
        base.OnClosed(e);
        _host.OnShelfClosed(this);
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        var handle = WindowEffects.Handle(this);
        WindowEffects.SetToolWindow(this, noActivate: false);
        var style = NativeMethods.GetWindowLong(handle, GWL_STYLE);
        style &= ~(WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        NativeMethods.SetWindowLong(handle, GWL_STYLE, style);
        NativeMethods.SetWindowPos(handle, IntPtr.Zero, 0, 0, 0, 0, NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE |
            NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_FRAMECHANGED);
        WindowEffects.SetRoundedCorners(this);
        BackdropActive = WindowEffects.TryApplyBackdrop(this, SystemBackdrop.Acrylic);
        Root.SetResourceReference(Border.BackgroundProperty, BackdropActive ? "ShelfAcrylicTintBrush" : "ShelfSolidBrush");
        WindowEffects.SetDarkMode(this, ThemeService.IsDark);
    }

    private void OnThemeChanged(object? sender, EventArgs e) => WindowEffects.SetDarkMode(this, ThemeService.IsDark);

    private void AnimateIn()
    {
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Surface.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(160)));
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(0.92, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(0.92, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(ShelfViewModel.Count):
            case nameof(ShelfViewModel.IsExpanded):
                UpdateViews();
                UpdateWindowSize();
                break;
            case nameof(ShelfViewModel.Actions):
                BuildActions();
                UpdateWindowSize();
                break;
        }
    }

    private void UpdateViews()
    {
        StackView.Visibility = ViewModel.HasItems && !ViewModel.IsExpanded ? Visibility.Visible : Visibility.Collapsed;
        ItemsList.Visibility = ViewModel.IsExpanded ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateWindowSize()
    {
        var width = ViewModel.IsExpanded
            ? QuickShelfSizeOptions.ResolveExpandedWidth(_size)
            : QuickShelfSizeOptions.ResolveWidth(_size);
        var body = ViewModel.IsExpanded
            ? Math.Min(Math.Max(ViewModel.Count, 2), 6) * RowHeight + 6
            : QuickShelfSizeOptions.ResolveStackHeight(_size) + CaptionHeight;
        var actions = ViewModel.HasActions ? ActionBarHeight : 10;
        var tileRows = Math.Ceiling(ViewModel.Actions.Count / (ViewModel.Actions.Count == 1 ? 1.0 : 2.0));
        body = Math.Max(body, 66 + tileRows * 50 + 8 - actions);

        ActionBarHost.Visibility = ViewModel.HasActions ? Visibility.Visible : Visibility.Collapsed;
        Body.Margin = new Thickness(10, 2, 10, ViewModel.HasActions ? 0 : 10);
        Width = width;
        Height = HeaderHeight + body + actions;
        if (IsVisible)
            Dispatcher.BeginInvoke(() => WindowPlacement.KeepInWorkArea(this), DispatcherPriority.Background);
    }

    private void BuildActions()
    {
        ActionBar.Children.Clear();
        ActionTiles.Children.Clear();
        _tiles.Clear();
        ActionTiles.Columns = ViewModel.Actions.Count == 1 ? 1 : 2;
        foreach (var info in ViewModel.Actions)
        {
            var button = new System.Windows.Controls.Button
            {
                Style = (Style)FindResource("IconButtonStyle"),
                Width = double.NaN,
                Height = 36,
                Margin = new Thickness(2, 0, 2, 0),
                Content = info.Glyph,
                Tag = info,
                ToolTip = $"{info.Label}：{info.Description}"
            };
            System.Windows.Automation.AutomationProperties.SetName(button, info.Label);
            button.Click += OnActionClick;
            ActionBar.Children.Add(button);

            var glyph = new TextBlock
            {
                Text = info.Glyph,
                FontFamily = (System.Windows.Media.FontFamily)FindResource("IconFont"),
                FontSize = 15,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 1, 7, 0)
            };
            var label = new TextBlock { Text = info.Label, FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
            var tile = new Border
            {
                Height = 44,
                Margin = new Thickness(3),
                CornerRadius = new CornerRadius(8),
                BorderThickness = new Thickness(1),
                Child = new StackPanel
                {
                    Orientation = System.Windows.Controls.Orientation.Horizontal,
                    HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                    Children = { glyph, label }
                }
            };
            _tiles.Add((tile, glyph, label, info));
            ActionTiles.Children.Add(tile);
        }
        SetHoveredAction(null, force: true);
    }

    private void SetHoveredAction(QuickActionInfo? action, bool force = false)
    {
        if (!force && ReferenceEquals(action, _hoveredAction)) return;
        _hoveredAction = action;
        foreach (var (tile, glyph, label, info) in _tiles)
        {
            var active = ReferenceEquals(info, action);
            tile.SetResourceReference(Border.BackgroundProperty, active ? "AccentBrush" : "CardBrush");
            tile.SetResourceReference(Border.BorderBrushProperty, active ? "AccentBrush" : "CardBorderBrush");
            glyph.SetResourceReference(TextBlock.ForegroundProperty, active ? "TextOnAccentBrush" : "TextPrimaryBrush");
            label.SetResourceReference(TextBlock.ForegroundProperty, active ? "TextOnAccentBrush" : "TextPrimaryBrush");
        }
        ShelfDropFrame.Opacity = action is null ? 1 : 0.45;
    }

    // ───────────── 拖入 ─────────────

    private void OnDragEnter(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        _lastDragOverTick = Environment.TickCount64;
        if (!_dragInside)
        {
            _dragInside = true;
            _lastDescription = null;
            _dragFromSelf = DropDataReader.GetSourceShelfId(e.Data) == ViewModel.Id;
            _dragKind = _dragFromSelf ? DropContentKind.None : DropDataReader.Classify(e.Data);
            if (!_dragFromSelf && _dragKind != DropContentKind.None && !ViewModel.IsBusy) ShowDropOverlay();
            _dropHelper.Enter(this, e.Data, e.AllowedEffects & PreferredEffect(e.AllowedEffects));
        }
        UpdateDrag(e);
    }

    private void OnDragOver(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        _lastDragOverTick = Environment.TickCount64;
        UpdateDrag(e);
    }

    private void UpdateDrag(WpfDragEventArgs e)
    {
        if (_dragFromSelf)
        {
            e.Effects = WpfDragDropEffects.None;
            _dropHelper.Over(e.Effects);
            return;
        }

        if (_dragKind == DropContentKind.None || ViewModel.IsBusy)
        {
            e.Effects = WpfDragDropEffects.None;
            SetDescription(e.Data, DropImageType.Warning, ViewModel.IsBusy ? "请稍候" : "不支持此内容");
            _dropHelper.Over(e.Effects);
            return;
        }

        var action = HitTestTiles(e);
        SetHoveredAction(action);
        if (action is not null && !DropDataReader.CanProvideFiles(_dragKind))
        {
            e.Effects = WpfDragDropEffects.None;
            SetDescription(e.Data, DropImageType.Warning, "此操作只支持文件");
        }
        else
        {
            e.Effects = PreferredEffect(e.AllowedEffects);
            SetDescription(e.Data, DropImageType.Label, action?.Label ?? (ViewModel.HasItems ? "添加到置物架" : "放入置物架"));
        }
        _dropHelper.Over(e.Effects);
    }

    private void OnDragLeave(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        if (WindowPlacement.IsCursorInside(this))
        {
            // 在窗口内部的子元素之间移动也会触发离开事件，稍后确认拖动确实结束再收起。
            _leaveCheck.Stop();
            _leaveCheck.Start();
            return;
        }
        EndDrag();
        _dropHelper.Leave();
    }

    private void OnLeaveCheck(object? sender, EventArgs e)
    {
        _leaveCheck.Stop();
        if (!_dragInside || Environment.TickCount64 - _lastDragOverTick < 200) return;
        EndDrag();
        _dropHelper.Leave();
    }

    private async void OnDrop(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        var action = _hoveredAction;
        var fromSelf = _dragFromSelf;
        var kind = _dragKind;
        EndDrag();

        if (fromSelf || kind == DropContentKind.None || ViewModel.IsBusy ||
            (action is not null && !DropDataReader.CanProvideFiles(kind)))
        {
            e.Effects = WpfDragDropEffects.None;
            _dropHelper.Drop(e.Data, e.Effects);
            if (!fromSelf && kind == DropContentKind.None) ViewModel.Notify("来源程序没有提供可读取的内容", false);
            return;
        }

        e.Effects = PreferredEffect(e.AllowedEffects);
        _dropHelper.Drop(e.Data, e.Effects);
        ViewModel.HasInteracted = true;

        try
        {
            if (action is not null)
            {
                var files = await DropDataReader.ReadAsync(e.Data, filesOnly: true);
                if (IsClosing)
                {
                    foreach (var cache in files.CacheDirectories) _ = VirtualFileDropService.DeleteCacheAsync(cache);
                    return;
                }
                var result = await ViewModel.RunActionAsync(action.Type, files, new WindowInteropHelper(this).Handle, PickFolder);
                if (result.Succeeded) await CloseIfStillEmptyAsync();
            }
            else
            {
                var read = await DropDataReader.ReadAsync(e.Data);
                if (IsClosing)
                {
                    foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.DeleteCacheAsync(cache);
                    return;
                }
                var summary = ViewModel.Add(read);
                if (summary.Added == 0) ViewModel.Notify(read.ErrorMessage ?? summary.Message, false);
                else if (summary.Duplicates > 0 || summary.Unavailable > 0 || read.FailedCount > 0)
                    ViewModel.Notify(read.FailedCount > 0 ? $"{summary.Message}，{read.FailedCount} 个附件接收失败" : summary.Message);
                if (ViewModel.IsEmpty) await CloseIfStillEmptyAsync();
            }
        }
        catch (Exception exception)
        {
            LogService.Error("处理拖入内容失败", exception);
            ViewModel.Notify("拖入没有完成，来源内容未受影响", false);
        }
    }

    private void ShowDropOverlay()
    {
        DropZoneText.Text = ViewModel.HasItems ? "松开以添加" : "松开以暂存";
        Body.Visibility = Visibility.Hidden;
        ActionBarHost.Opacity = 0;
        DropOverlay.Visibility = Visibility.Visible;
        WindowEffects.SetBorderColor(this, ThemeService.AccentFill);
    }

    private void EndDrag()
    {
        _leaveCheck.Stop();
        _dragInside = false;
        SetHoveredAction(null);
        DropOverlay.Visibility = Visibility.Collapsed;
        Body.Visibility = Visibility.Visible;
        ActionBarHost.Opacity = 1;
        WindowEffects.SetBorderColor(this, null);
    }

    private QuickActionInfo? HitTestTiles(WpfDragEventArgs e)
    {
        if (DropOverlay.Visibility != Visibility.Visible) return null;
        foreach (var (tile, _, _, info) in _tiles)
        {
            if (tile.ActualWidth <= 0) continue;
            var point = e.GetPosition(tile);
            if (point.X >= -3 && point.Y >= -3 && point.X <= tile.ActualWidth + 3 && point.Y <= tile.ActualHeight + 3) return info;
        }
        return null;
    }

    private void SetDescription(System.Windows.IDataObject data, DropImageType type, string message)
    {
        var key = $"{(int)type}:{message}";
        if (key == _lastDescription) return;
        _lastDescription = key;
        DropTargetHelper.SetDescription(data, type, message);
    }

    private static WpfDragDropEffects PreferredEffect(WpfDragDropEffects allowed)
    {
        if (allowed.HasFlag(WpfDragDropEffects.Copy)) return WpfDragDropEffects.Copy;
        if (allowed.HasFlag(WpfDragDropEffects.Link)) return WpfDragDropEffects.Link;
        if (allowed.HasFlag(WpfDragDropEffects.Move)) return WpfDragDropEffects.Move;
        return WpfDragDropEffects.Copy;
    }

    private async Task CloseIfStillEmptyAsync()
    {
        if (!ViewModel.IsEmpty || ViewModel.IsPinned) return;
        await Task.Delay(1500);
        if (ViewModel.IsEmpty && !ViewModel.IsPinned && !ViewModel.IsBusy && !IsDraggingOut) CloseAnimated();
    }

    // ───────────── 拖出 ─────────────

    private void BeginDragOut(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0 || ViewModel.IsBusy) return;
        IsDraggingOut = true;
        DragOutResult result;
        try
        {
            result = ShelfDragService.DoDragDrop(this, items, ViewModel.Id);
        }
        finally
        {
            IsDraggingOut = false;
        }

        ViewModel.HasInteracted = true;
        if (result == DragOutResult.Cancelled) return;
        ViewModel.ApplyDragOut(items, result, _host.Settings.RemoveAfterDragOut);
        if (ViewModel.IsEmpty && _host.Settings.CloseWhenEmpty && !ViewModel.IsPinned) CloseAnimated();
    }

    private bool PassedDragThreshold(System.Windows.Input.MouseEventArgs e)
    {
        var current = e.GetPosition(this);
        return Math.Abs(current.X - _pressPoint.X) >= SystemParameters.MinimumHorizontalDragDistance ||
               Math.Abs(current.Y - _pressPoint.Y) >= SystemParameters.MinimumVerticalDragDistance;
    }

    private void OnStackMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2 && ViewModel.Count == 1 && ViewModel.Stack0 is { } single)
        {
            _stackPressed = false;
            ViewModel.Open(single);
            e.Handled = true;
            return;
        }
        _stackPressed = true;
        _pressPoint = e.GetPosition(this);
    }

    private void OnStackMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (!_stackPressed || e.LeftButton != MouseButtonState.Pressed || !PassedDragThreshold(e)) return;
        _stackPressed = false;
        BeginDragOut(ViewModel.Items.ToArray());
    }

    private void OnStackMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_stackPressed) return;
        _stackPressed = false;
        ViewModel.HasInteracted = true;
        if (ViewModel.HasMultiple) Expand();
    }

    private void OnStackRightClick(object sender, MouseButtonEventArgs e)
    {
        e.Handled = true;
        OpenMenu(BuildShelfMenu(), StackView);
    }

    private void Expand()
    {
        ViewModel.IsExpanded = true;
        ItemsList.SelectedItems.Clear();
        Dispatcher.BeginInvoke(() => ItemsList.Focus(), DispatcherPriority.Input);
    }

    private void OnCollapseClick(object sender, RoutedEventArgs e) => ViewModel.IsExpanded = false;

    private void OnListMouseDown(object sender, MouseButtonEventArgs e)
    {
        _listPressed = false;
        _deferSelection = false;
        var source = e.OriginalSource as DependencyObject;
        if (FindAncestor<ButtonBase>(source) is not null) return;
        if (FindAncestor<ListBoxItem>(source) is not { DataContext: ShelfItem item } container) return;
        _pressedItem = item;
        _listPressed = true;
        _pressPoint = e.GetPosition(this);
        if (container.IsSelected && Keyboard.Modifiers == ModifierKeys.None && ItemsList.SelectedItems.Count > 1)
        {
            // 保留多选，便于把选中的多个项目一起拖出；未拖动时在松开后再改为单选。
            _deferSelection = true;
            e.Handled = true;
        }
    }

    private void OnListMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (!_listPressed || e.LeftButton != MouseButtonState.Pressed || _pressedItem is null || !PassedDragThreshold(e)) return;
        _listPressed = false;
        _deferSelection = false;
        if (!ItemsList.SelectedItems.Contains(_pressedItem))
        {
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItems.Add(_pressedItem);
        }
        BeginDragOut(GetSelectedItems());
    }

    private void OnListMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (_listPressed && _deferSelection && _pressedItem is not null)
        {
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItems.Add(_pressedItem);
        }
        _listPressed = false;
        _deferSelection = false;
    }

    private void OnListDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (FindAncestor<ButtonBase>(e.OriginalSource as DependencyObject) is not null) return;
        if (FindAncestor<ListBoxItem>(e.OriginalSource as DependencyObject) is { DataContext: ShelfItem item })
            ViewModel.Open(item);
    }

    private void OnListRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (FindAncestor<ListBoxItem>(e.OriginalSource as DependencyObject) is not { DataContext: ShelfItem item } container) return;
        e.Handled = true;
        if (!container.IsSelected)
        {
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItems.Add(item);
        }
        OpenMenu(BuildItemMenu(GetSelectedItems()), ItemsList);
    }

    private void OnRemoveItemClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not ShelfItem item) return;
        RemoveItems([item]);
    }

    private void RemoveItems(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0) return;
        ViewModel.Remove(items);
        ViewModel.HasInteracted = true;
        if (ViewModel.IsEmpty && _host.Settings.CloseWhenEmpty && !ViewModel.IsPinned) CloseAnimated();
    }

    private IReadOnlyList<ShelfItem> GetSelectedItems() =>
        ItemsList.SelectedItems.Cast<ShelfItem>().OrderBy(item => ViewModel.Items.IndexOf(item)).ToArray();

    /// <summary>展开且有选中项时只处理选中项，否则处理整个置物架。</summary>
    private IReadOnlyList<ShelfItem> GetTargets()
    {
        if (ViewModel.IsExpanded && ItemsList.SelectedItems.Count > 0) return GetSelectedItems();
        return ViewModel.Items.ToArray();
    }

    // ───────────── 快速操作与菜单 ─────────────

    private async void OnActionClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not QuickActionInfo info || ViewModel.IsBusy) return;
        await ViewModel.RunActionAsync(info.Type, GetTargets(), new WindowInteropHelper(this).Handle, PickFolder);
    }

    private string? PickFolder()
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = "选择要移动到的文件夹", Multiselect = false };
        return dialog.ShowDialog(this) == true ? dialog.FolderName : null;
    }

    private void OnMoreClick(object sender, RoutedEventArgs e) => OpenMenu(BuildShelfMenu(), MoreButton);

    private void OnCloseClick(object sender, RoutedEventArgs e) => CloseAnimated();

    private void OnHeaderMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ButtonState != MouseButtonState.Pressed) return;
        try
        {
            ViewModel.HasInteracted = true;
            DragMove();
        }
        catch (InvalidOperationException)
        {
        }
    }

    private ContextMenu BuildShelfMenu()
    {
        var targets = GetTargets();
        var hasItems = ViewModel.HasItems;
        var menu = new ContextMenu();
        if (ViewModel.HasMultiple && !ViewModel.IsExpanded)
            menu.Items.Add(CreateMenuItem("展开列表", "", Expand));
        menu.Items.Add(CreateMenuItem("全选", "", SelectAll, "Ctrl+A", ViewModel.HasMultiple));
        menu.Items.Add(CreateMenuItem("复制", "", () => ViewModel.CopyToClipboard(targets), "Ctrl+C", hasItems));
        menu.Items.Add(CreateMenuItem("粘贴", "", Paste, "Ctrl+V"));
        menu.Items.Add(CreateMenuItem("复制路径", "", () => ViewModel.CopyPaths(targets), enabled: targets.Any(item => item.IsFileSystem)));
        menu.Items.Add(CreateMenuItem("在资源管理器中显示", "", () => ViewModel.Reveal(targets), enabled: targets.Any(item => item.IsFileSystem)));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem("清空置物架", "", () => RemoveItems(ViewModel.Items.ToArray()), enabled: hasItems));
        menu.Items.Add(CreateMenuItem("DropShelf 设置", "", _host.ShowSettings));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem("关闭置物架", "", CloseAnimated, "Esc"));
        return menu;
    }

    private ContextMenu BuildItemMenu(IReadOnlyList<ShelfItem> items)
    {
        var menu = new ContextMenu();
        var single = items.Count == 1 ? items[0] : null;
        menu.Items.Add(CreateMenuItem(single?.Kind == ShelfItemKind.Text ? "复制文本" : "打开", "",
            () => { if (single is not null) ViewModel.Open(single); }, "Enter", single is not null));
        menu.Items.Add(CreateMenuItem("在资源管理器中显示", "", () => ViewModel.Reveal(items), enabled: items.Any(item => item.IsFileSystem)));
        menu.Items.Add(CreateMenuItem("复制", "", () => ViewModel.CopyToClipboard(items), "Ctrl+C"));
        menu.Items.Add(CreateMenuItem("复制路径", "", () => ViewModel.CopyPaths(items), enabled: items.Any(item => item.IsFileSystem)));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem("从置物架移除", "", () => RemoveItems(items), "Delete"));
        return menu;
    }

    private static MenuItem CreateMenuItem(string header, string glyph, Action action, string? gesture = null, bool enabled = true)
    {
        var item = new MenuItem
        {
            Header = header,
            Icon = new TextBlock { Text = glyph },
            InputGestureText = gesture ?? string.Empty,
            IsEnabled = enabled
        };
        item.Click += (_, _) => action();
        return item;
    }

    private static void OpenMenu(ContextMenu menu, UIElement target)
    {
        menu.PlacementTarget = target;
        menu.Placement = PlacementMode.MousePoint;
        menu.IsOpen = true;
    }

    private void SelectAll()
    {
        if (!ViewModel.HasMultiple) return;
        if (!ViewModel.IsExpanded) Expand();
        Dispatcher.BeginInvoke(() => ItemsList.SelectAll(), DispatcherPriority.Input);
    }

    private async void Paste()
    {
        var data = ClipboardService.TryGetDataObject();
        if (data is null)
        {
            ViewModel.Notify("剪贴板是空的", false);
            return;
        }
        try
        {
            var read = await DropDataReader.ReadAsync(data);
            if (IsClosing)
            {
                foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.DeleteCacheAsync(cache);
                return;
            }
            var summary = ViewModel.Add(read);
            ViewModel.Notify(summary.Added == 0 ? read.ErrorMessage ?? summary.Message : summary.Message, summary.Added > 0);
        }
        catch (Exception exception)
        {
            LogService.Error("从剪贴板粘贴失败", exception);
            ViewModel.Notify("粘贴没有完成", false);
        }
    }

    private void OnPreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        var control = Keyboard.Modifiers.HasFlag(ModifierKeys.Control);
        switch (e.Key)
        {
            case Key.Escape:
                if (ViewModel.IsExpanded) ViewModel.IsExpanded = false;
                else CloseAnimated();
                e.Handled = true;
                break;
            case Key.Delete when ViewModel.IsExpanded && ItemsList.SelectedItems.Count > 0:
                RemoveItems(GetSelectedItems());
                e.Handled = true;
                break;
            case Key.Enter when ViewModel.IsExpanded && ItemsList.SelectedItems.Count == 1:
                ViewModel.Open(GetSelectedItems()[0]);
                e.Handled = true;
                break;
            case Key.A when control:
                SelectAll();
                e.Handled = true;
                break;
            case Key.C when control:
                ViewModel.CopyToClipboard(GetTargets());
                e.Handled = true;
                break;
            case Key.V when control:
                Paste();
                e.Handled = true;
                break;
        }
    }

    // ───────────── 提示 ─────────────

    private void ShowToast(ShelfToast toast)
    {
        if (IsClosing) return;
        ToastText.Text = toast.Message;
        ToastGlyph.Text = toast.Success ? "" : "";
        ToastGlyph.SetResourceReference(TextBlock.ForegroundProperty, toast.Success ? "SuccessBrush" : "CautionBrush");
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Toast.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(140)));
        ToastOffset.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(8, 0, TimeSpan.FromMilliseconds(200)) { EasingFunction = ease });
        _toastTimer.Stop();
        _toastTimer.Start();
    }

    private void HideToast()
    {
        _toastTimer.Stop();
        Toast.BeginAnimation(OpacityProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(180)));
    }

    private static T? FindAncestor<T>(DependencyObject? source) where T : DependencyObject
    {
        while (source is not null)
        {
            if (source is T match) return match;
            source = source is Visual or System.Windows.Media.Media3D.Visual3D
                ? VisualTreeHelper.GetParent(source)
                : LogicalTreeHelper.GetParent(source);
        }
        return null;
    }
}
