using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
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
    ShelfHistoryService? History { get; }
    void OnShelfClosed(ShelfWindow window, IReadOnlyList<ShelfItem> items);
    void ShowSettings();
    void SetDragOutMode(TransferMode mode);
    void Restore(ShelfHistoryEntry entry, ShelfWindow? target);
    void OpenBeside(ShelfWindow source, IReadOnlyList<string> paths);
}

public enum ShelfEntranceKind
{
    /// <summary>快捷键、托盘菜单等主动打开：轻微浮现。</summary>
    Plain,

    /// <summary>晃动呼出：从光标处弹出。</summary>
    Shake,

    /// <summary>拖到屏幕边缘呼出：从该边缘滑入。</summary>
    Edge
}

public readonly record struct ShelfEntrance(ShelfEntranceKind Kind, ScreenEdge Edge)
{
    public static ShelfEntrance Plain => new(ShelfEntranceKind.Plain, ScreenEdge.None);
    public static ShelfEntrance Shake => new(ShelfEntranceKind.Shake, ScreenEdge.None);
    public static ShelfEntrance FromEdge(ScreenEdge edge) => new(ShelfEntranceKind.Edge, edge);
}

public partial class ShelfWindow : Window
{
    private const double HeaderHeight = 40;
    private const double CaptionHeight = 38;
    private const double ActionBarHeight = 50;
    private const double ActionTileHeight = 94;
    private const double RowHeight = 46;
    private const int WS_SYSMENU = 0x00080000;
    private const int WS_MINIMIZEBOX = 0x00020000;
    private const int WS_MAXIMIZEBOX = 0x00010000;
    private const int GWL_STYLE = -16;
    private static readonly TimeSpan TilesExpandDuration = TimeSpan.FromMilliseconds(180);
    private static readonly TimeSpan TilesCollapseDuration = TimeSpan.FromMilliseconds(240);

    private readonly IShelfHost _host;
    private readonly DropTargetHelper _dropHelper = new();
    private readonly DispatcherTimer _toastTimer;
    private readonly DispatcherTimer _leaveCheck;
    /// <summary>窗口关闭时取消还在接收的附件。</summary>
    private readonly CancellationTokenSource _lifetime = new();
    private readonly List<(Border Tile, TextBlock Glyph, TextBlock Label, QuickActionInfo Info)> _tiles = [];
    private QuickShelfSize _size;
    private TransferMode _dragOutMode;
    private ShelfEntrance _entrance = ShelfEntrance.Plain;
    private EventHandler? _slideFrame;
    private bool _closing;
    private bool _closed;
    private bool _dragInside;
    private bool _dragFromSelf;
    private DropContentKind _dragKind;
    private QuickActionInfo? _hoveredAction;
    private bool _tilesExpanded;
    private bool _layoutPending;
    private bool _countChanged;
    private bool _shownExpanded;
    private (WpfPoint Point, int Added)? _landing;
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
        viewModel.NewItemsProduced += (_, paths) => _host.OpenBeside(this, paths);

        _toastTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2.4) };
        _toastTimer.Tick += (_, _) => HideToast();
        _leaveCheck = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(220) };
        _leaveCheck.Tick += OnLeaveCheck;

        SourceInitialized += OnSourceInitialized;
        ThemeService.ThemeChanged += OnThemeChanged;
        LanguageService.LanguageChanged += OnLanguageChanged;
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
    public bool IsReusable => !IsClosing && ViewModel.IsEmpty && !ViewModel.IsBusy && !ViewModel.HasInteracted;

    public void ApplySettings(AppSettings settings)
    {
        _size = settings.QuickShelfSize;
        _dragOutMode = settings.DragOutMode;
        ViewModel.Actions = QuickActions.Visible(settings);
        UpdateTransferMode();
        UpdateWindowSize();
        UpdateThumbnailSize();
    }

    /// <summary>按堆叠卡片里缩略图的实际像素大小取缩略图：不多占内存，显示时也不用大幅缩小。</summary>
    private void UpdateThumbnailSize()
    {
        // 卡片 84 减去内边距后显示区域为 66；堆叠画布 160×116 随置物架大小缩放，两侧各留 10 的边距。
        var stackScale = Math.Min((QuickShelfSizeOptions.ResolveWidth(_size) - 20) / 160, QuickShelfSizeOptions.ResolveStackHeight(_size) / 116);
        ViewModel.ThumbnailSize = (int)Math.Ceiling(66 * stackScale * DpiScale());
    }

    protected override void OnDpiChanged(System.Windows.DpiScale oldDpi, System.Windows.DpiScale newDpi)
    {
        base.OnDpiChanged(oldDpi, newDpi);
        UpdateThumbnailSize();
    }

    /// <summary>以屏幕像素坐标为中心显示置物架。</summary>
    public void ShowAt(POINT center, bool activate, ShelfEntrance entrance = default)
    {
        ShowActivated = activate;
        var wasVisible = IsVisible;
        // 复用正在滑入的置物架时停止动画，否则下一帧会把窗口拉回原来的位置。
        if (wasVisible) StopSlide();
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (!wasVisible) Show();
        WindowPlacement.CenterOn(this, center, Width, Height);
        if (activate)
        {
            Activate();
            Focus();
        }
        if (!wasVisible)
        {
            _entrance = entrance;
            AnimateIn(center);
        }
    }

    public void CloseAnimated()
    {
        if (_closing || _closed) return;
        _closing = true;
        IsHitTestVisible = false;
        if (!ShelfMotion.Enabled)
        {
            Close();
            return;
        }

        var duration = ShelfMotion.CloseDuration;
        var ease = new CubicEase { EasingMode = EasingMode.EaseIn };
        CacheSurface();
        var fade = new DoubleAnimation(0, duration) { EasingFunction = ease };
        fade.Completed += (_, _) => Close();
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(ShelfMotion.CloseScale, duration) { EasingFunction = ease });
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(ShelfMotion.CloseScale, duration) { EasingFunction = ease });
        Surface.BeginAnimation(OpacityProperty, fade);

        // 窗口本身也朝来时的方向退回一小段，避免内容消失后只剩空的背景。
        if (NativeMethods.GetWindowRect(new WindowInteropHelper(this).Handle, out var rect))
        {
            var (dx, dy) = ShelfMotion.CloseOffset(_entrance, DpiScale());
            SlideWindow(rect.Left, rect.Top, rect.Left + dx, rect.Top + dy, duration, ease);
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        _closed = true;
        _lifetime.Cancel();
        StopSlide();
        StopMotion();
        _toastTimer.Stop();
        _leaveCheck.Stop();
        ThemeService.ThemeChanged -= OnThemeChanged;
        LanguageService.LanguageChanged -= OnLanguageChanged;
        ViewModel.PropertyChanged -= OnViewModelPropertyChanged;
        var items = ViewModel.Items.ToArray();
        ViewModel.ReleaseResources();
        base.OnClosed(e);
        _host.OnShelfClosed(this, items);
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
        BackdropActive = WindowEffects.ApplyTheme(this, SystemBackdrop.Acrylic, ThemeService.IsDark);
        Root.SetResourceReference(Border.BackgroundProperty, BackdropActive ? "ShelfAcrylicTintBrush" : "ShelfSolidBrush");
    }

    private void OnThemeChanged(object? sender, EventArgs e) => WindowEffects.ApplyTheme(this, SystemBackdrop.Acrylic, ThemeService.IsDark);

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        ViewModel.RefreshText();
        UpdateTransferMode();
        UpdateViews();
        // 按钮提示和拖入时的操作名称是代码生成的，需要重新创建；正在拖入时等下一次再更新。
        if (!_dragInside) BuildActions();
    }

    private void UpdateTransferMode()
    {
        var copy = _dragOutMode == TransferMode.Copy;
        TransferModeGlyph.Text = copy ? "\uE8C8" : "\uE7C2";
        TransferModeText.Text = copy ? Strings.TransferCopy : Strings.TransferMove;
        TransferModeButton.ToolTip = Strings.DragOutModeTooltip(_dragOutMode);
        System.Windows.Automation.AutomationProperties.SetName(TransferModeButton, Strings.DragOutModeTooltip(_dragOutMode));
    }

    private void OnTransferModeClick(object sender, RoutedEventArgs e)
    {
        ViewModel.HasInteracted = true;
        var next = _dragOutMode == TransferMode.Move ? TransferMode.Copy : TransferMode.Move;
        _host.SetDragOutMode(next);
        ViewModel.Notify(Strings.DragOutModeChanged(next));
    }

    // ───────────── 动画 ─────────────

    /// <summary>
    /// 弹出动画：窗口从来源方向滑入（亚克力背景随窗口一起移动），内容同时淡入并放大到原尺寸。
    /// 晃动呼出时内容从光标处弹出，带一点回弹；从屏幕边缘呼出时从该边缘滑入。
    /// </summary>
    private void AnimateIn(POINT cursor)
    {
        StopSlide();
        if (!ShelfMotion.Enabled)
        {
            Surface.BeginAnimation(OpacityProperty, null);
            Surface.Opacity = 1;
            return;
        }

        var handle = new WindowInteropHelper(this).Handle;
        var motion = ShelfMotion.For(_entrance);
        if (NativeMethods.GetWindowRect(handle, out var rect))
        {
            // 内容从光标所在的位置展开。
            Surface.RenderTransformOrigin = _entrance.Kind == ShelfEntranceKind.Shake && rect.Width > 0 && rect.Height > 0
                ? new WpfPoint(Math.Clamp((cursor.X - rect.Left) / (double)rect.Width, 0.15, 0.85),
                    Math.Clamp((cursor.Y - rect.Top) / (double)rect.Height, 0.15, 0.85))
                : new WpfPoint(0.5, 0.4);
            var (dx, dy) = ShelfMotion.OpenOffset(_entrance, DpiScale());
            SlideWindow(rect.Left + dx, rect.Top + dy, rect.Left, rect.Top, motion.Duration, new QuinticEase { EasingMode = EasingMode.EaseOut },
                () => WindowPlacement.KeepInWorkArea(this));
        }

        CacheSurface();
        Surface.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, motion.FadeDuration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
        var scale = new DoubleAnimation(motion.StartScale, 1, motion.Duration) { EasingFunction = motion.ScaleEase };
        scale.Completed += (_, _) => Surface.CacheMode = null;
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleXProperty, scale);
        SurfaceScale.BeginAnimation(ScaleTransform.ScaleYProperty, scale);
    }

    /// <summary>
    /// 缩放和淡入淡出期间把内容缓存为位图，每帧只需合成，不必重新绘制虚线边框、阴影和文字。
    /// 按所在显示器的缩放比例缓存，高分屏上也保持清晰，见 CacheScale。
    /// </summary>
    private void CacheSurface() => Surface.CacheMode = new BitmapCache(CacheScale.For(this));

    /// <summary>逐帧移动整个窗口。只改变位置不改变大小，系统材质不需要重新计算，移动很流畅。</summary>
    private void SlideWindow(int fromX, int fromY, int toX, int toY, TimeSpan duration, IEasingFunction ease, Action? completed = null)
    {
        StopSlide();
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero || (fromX == toX && fromY == toY))
        {
            completed?.Invoke();
            return;
        }

        MoveWindow(handle, fromX, fromY);
        var clock = Stopwatch.StartNew();
        var (x, y, frame) = (fromX, fromY, TimeSpan.Zero);
        _slideFrame = (_, e) =>
        {
            // 同一帧可能触发多次；取整后位置没变时也不必移动窗口。
            var time = ((RenderingEventArgs)e).RenderingTime;
            if (time == frame) return;
            frame = time;
            var progress = Math.Min(1, clock.Elapsed.TotalMilliseconds / duration.TotalMilliseconds);
            var eased = ease.Ease(progress);
            var (nextX, nextY) = ((int)Math.Round(fromX + (toX - fromX) * eased), (int)Math.Round(fromY + (toY - fromY) * eased));
            if (nextX != x || nextY != y) MoveWindow(handle, nextX, nextY);
            (x, y) = (nextX, nextY);
            if (progress < 1) return;
            StopSlide();
            completed?.Invoke();
        };
        CompositionTarget.Rendering += _slideFrame;
    }

    private void StopSlide()
    {
        if (_slideFrame is null) return;
        CompositionTarget.Rendering -= _slideFrame;
        _slideFrame = null;
    }

    /// <summary>窗口正在滑入或退出。</summary>
    public bool IsSliding => _slideFrame is not null;

    private static void MoveWindow(IntPtr handle, int x, int y) =>
        NativeMethods.SetWindowPos(handle, IntPtr.Zero, x, y, 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);

    private double DpiScale() => VisualTreeHelper.GetDpi(this).DpiScaleX;

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(ShelfViewModel.Count):
                _countChanged = true;
                goto case nameof(ShelfViewModel.IsExpanded);
            case nameof(ShelfViewModel.IsExpanded):
                // 数量变化时项目集合还在通知列表的途中；此时改窗口大小会立即重新布局，
                // 列表与项目不一致而抛出异常。等通知结束再更新，连续变化只更新一次。
                if (_layoutPending) break;
                _layoutPending = true;
                Dispatcher.BeginInvoke(() =>
                {
                    _layoutPending = false;
                    if (!IsClosing) RefreshViews();
                });
                break;
            case nameof(ShelfViewModel.Actions):
                BuildActions();
                UpdateWindowSize();
                break;
        }
    }

    private void OnRowLoaded(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is ShelfItem item) ViewModel.SetRowVisible(item, true);
    }

    private void OnRowUnloaded(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is ShelfItem item) ViewModel.SetRowVisible(item, false);
    }

    /// <summary>数量或视图变化后更新界面：只是切换了堆叠和列表时播放切换动画，刚拖入文件时播放落下的动画。</summary>
    private void RefreshViews()
    {
        var switched = !_countChanged && _shownExpanded != ViewModel.IsExpanded;
        var landing = _landing;
        _countChanged = false;
        _landing = null;
        _shownExpanded = ViewModel.IsExpanded;
        FinishMotion();
        var animate = ShelfMotion.Enabled && IsVisible && ViewModel.HasItems;
        if (switched && animate)
        {
            AnimateViewSwitch();
            return;
        }
        UpdateViews();
        UpdateWindowSize();
        if (landing is not { } drop || !animate) return;
        if (ViewModel.IsExpanded) PlayListLanding(drop.Point, drop.Added);
        else PlayLanding(drop.Point, drop.Added);
    }

    /// <summary>堆叠上的数量角标；切换视图时和说明文字一起淡出淡入。</summary>
    private UIElement StackMark => CountBadge;

    /// <summary>落下动画开始时角标先显示落下之前的数量。</summary>
    private void BeginLandingMark(int count) => ShowCount(count);

    /// <summary>每落下一张，角标的数字加一并弹一下。</summary>
    private void CardLanded(int count, bool top)
    {
        ShowCount(count);
        if (count < 2) return;
        var pop = new DoubleAnimation(1.28, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = new BackEase { Amplitude = 0.5, EasingMode = EasingMode.EaseOut } };
        CountBadgeScale.BeginAnimation(ScaleTransform.ScaleXProperty, pop);
        CountBadgeScale.BeginAnimation(ScaleTransform.ScaleYProperty, pop);
    }

    /// <summary>只有一项时不显示角标，和绑定的 HasMultiple 一致。</summary>
    private void ShowCount(int count)
    {
        CountText.SetCurrentValue(TextBlock.TextProperty, count.ToString(CultureInfo.InvariantCulture));
        CountBadge.Opacity = count > 1 ? 1 : 0;
    }

    private void RestoreStackMark()
    {
        BindingOperations.GetBindingExpression(CountText, TextBlock.TextProperty)?.UpdateTarget();
        CountBadge.ClearValue(OpacityProperty);
    }

    private void UpdateViews()
    {
        StackView.Visibility = ViewModel.HasItems && !ViewModel.IsExpanded ? Visibility.Visible : Visibility.Collapsed;
        ItemsList.Visibility = ViewModel.IsExpanded ? Visibility.Visible : Visibility.Collapsed;
        ViewToggleButton.Content = ViewModel.IsExpanded ? "\uE81E" : "\uE8FD";
        ViewToggleButton.ToolTip = ViewModel.IsExpanded ? Strings.MenuCollapse : Strings.MenuExpand;
        System.Windows.Automation.AutomationProperties.SetName(ViewToggleButton, (string)ViewToggleButton.ToolTip);
    }

    private void UpdateWindowSize()
    {
        var (width, height) = TargetSize();
        ActionBarHost.Visibility = ViewModel.HasActions ? Visibility.Visible : Visibility.Collapsed;
        Body.Margin = new Thickness(10, 2, 10, ViewModel.HasActions ? 0 : 10);
        Width = width;
        Height = height;
        if (IsVisible)
            Dispatcher.BeginInvoke(() =>
            {
                // 滑入动画结束时会再检查一次，动画进行中不要把窗口拉回原处。
                if (!IsSliding) WindowPlacement.KeepInWorkArea(this);
            }, DispatcherPriority.Background);
    }

    private (double Width, double Height) TargetSize()
    {
        var width = ViewModel.IsExpanded
            ? QuickShelfSizeOptions.ResolveExpandedWidth(_size)
            : QuickShelfSizeOptions.ResolveWidth(_size);
        var body = ViewModel.IsExpanded
            ? Math.Min(Math.Max(ViewModel.Count, 2), 6) * RowHeight + 6
            : QuickShelfSizeOptions.ResolveStackHeight(_size) + CaptionHeight;
        var actions = ViewModel.HasActions ? ActionBarHeight : 10;
        // 拖入时上方的放置区域至少留 66，下方是展开的一行快速操作。
        var tiles = ViewModel.HasActions ? ActionTileHeight + 14 : 8;
        body = Math.Max(body, 66 + tiles - actions);
        return (width, HeaderHeight + body + actions);
    }

    private void BuildActions()
    {
        ActionBar.Children.Clear();
        ActionTiles.Children.Clear();
        _tiles.Clear();
        // 只有两个操作时按钮显示文字。拖放区域的快速操作与按钮一一对齐，从按钮所在位置向上展开。
        var large = ViewModel.Actions.Count <= 2;
        var textFont = (System.Windows.Media.FontFamily)FindResource("TextFont");
        var iconFont = (System.Windows.Media.FontFamily)FindResource("IconFont");
        foreach (var info in ViewModel.Actions)
        {
            var button = new System.Windows.Controls.Button
            {
                Style = (Style)FindResource("IconButtonStyle"),
                Width = double.NaN,
                Height = 36,
                Margin = new Thickness(2, 0, 2, 0),
                Tag = info,
                ToolTip = Strings.QuickActionTooltip(info.Title, info.Description)
            };
            button.Content = large
                ? (object)new DockPanel
                {
                    HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                    Margin = new Thickness(2, 0, 2, 0),
                    Children =
                    {
                        new TextBlock { Text = info.Glyph, FontSize = 15, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 1, 6, 0) },
                        new TextBlock
                        {
                            Text = info.Label, FontFamily = textFont, FontSize = 12, VerticalAlignment = VerticalAlignment.Center,
                            TextTrimming = TextTrimming.CharacterEllipsis
                        }
                    }
                }
                : info.Glyph;
            System.Windows.Automation.AutomationProperties.SetName(button, info.Label);
            button.Click += OnActionClick;
            ActionBar.Children.Add(button);

            var glyph = new TextBlock
            {
                Text = info.Glyph,
                FontFamily = iconFont,
                FontSize = large ? 22 : 20,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                Margin = new Thickness(0, 0, 0, 8)
            };
            var label = new TextBlock
            {
                Text = info.Label,
                FontSize = 12,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis
            };
            var tile = new Border
            {
                Height = ActionTileHeight,
                Margin = new Thickness(2, 3, 2, 3),
                Padding = new Thickness(large ? 6 : 2, 0, large ? 6 : 2, 0),
                CornerRadius = new CornerRadius(10),
                BorderThickness = new Thickness(1),
                Child = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Children = { glyph, label } }
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
            SetDescription(e.Data, DropImageType.Warning, ViewModel.IsBusy ? Strings.PleaseWait : Strings.UnsupportedContent);
            _dropHelper.Over(e.Effects);
            return;
        }

        // 拖到底部按钮区域时展开快速操作；展开后区域变高，移回上方的放置区域再收起。
        if (ViewModel.HasActions) SetTilesExpanded(e.GetPosition(ActionTilesHost).Y >= 0);
        var action = HitTestTiles(e);
        SetHoveredAction(action);
        if (action is not null && !DropDataReader.CanProvideFiles(_dragKind))
        {
            e.Effects = WpfDragDropEffects.None;
            SetDescription(e.Data, DropImageType.Warning, Strings.FilesOnlyAction);
        }
        else
        {
            e.Effects = PreferredEffect(e.AllowedEffects);
            SetDescription(e.Data, DropImageType.Label, action?.Label ?? (ViewModel.HasItems ? Strings.AddToShelf : Strings.PutOnShelf));
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
        var dropPoint = e.GetPosition(this);
        var action = _hoveredAction;
        var fromSelf = _dragFromSelf;
        var kind = _dragKind;
        EndDrag();

        if (fromSelf || kind == DropContentKind.None || ViewModel.IsBusy ||
            (action is not null && !DropDataReader.CanProvideFiles(kind)))
        {
            e.Effects = WpfDragDropEffects.None;
            _dropHelper.Drop(e.Data, e.Effects);
            if (!fromSelf && kind == DropContentKind.None) ViewModel.Notify(Strings.SourceProvidedNothing, false);
            return;
        }

        e.Effects = PreferredEffect(e.AllowedEffects);
        _dropHelper.Drop(e.Data, e.Effects);
        ViewModel.HasInteracted = true;

        try
        {
            if (action is not null)
            {
                var files = await DropDataReader.ReadAsync(e.Data, filesOnly: true, _lifetime.Token);
                if (IsClosing)
                {
                    foreach (var cache in files.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                    return;
                }
                var result = await ViewModel.RunActionAsync(action, files, new WindowInteropHelper(this).Handle, PickFolder);
                if (result.Succeeded) await CloseIfStillEmptyAsync();
            }
            else
            {
                var read = await DropDataReader.ReadAsync(e.Data, cancellationToken: _lifetime.Token);
                if (IsClosing)
                {
                    foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                    return;
                }
                var summary = ViewModel.Add(read);
                if (summary.Added > 0) _landing = (dropPoint, summary.Added);
                if (summary.Added == 0) ViewModel.Notify(read.ErrorMessage ?? summary.Message, false);
                else if (summary.Duplicates > 0 || summary.Unavailable > 0 || read.FailedCount > 0)
                    ViewModel.Notify(read.FailedCount > 0 ? Strings.WithFailedAttachments(summary.Message, read.FailedCount) : summary.Message);
                if (ViewModel.IsEmpty) await CloseIfStillEmptyAsync();
            }
        }
        catch (Exception exception)
        {
            LogService.Error("处理拖入内容失败", exception);
            ViewModel.Notify(Strings.DropFailed, false);
        }
    }

    private void ShowDropOverlay()
    {
        FinishMotion();
        DropZoneText.Text = ViewModel.HasItems ? Strings.ReleaseToAdd : Strings.ReleaseToHold;
        Body.Visibility = Visibility.Hidden;
        ResetTiles();
        DropOverlay.Visibility = Visibility.Visible;
        // 立即完成布局，紧接着判断光标是否在按钮区域时才能拿到正确的位置。
        DropOverlay.UpdateLayout();
        WindowEffects.SetBorderColor(this, ThemeService.AccentFill);
    }

    private void EndDrag()
    {
        _leaveCheck.Stop();
        _dragInside = false;
        SetHoveredAction(null);
        DropOverlay.Visibility = Visibility.Collapsed;
        Body.Visibility = Visibility.Visible;
        ResetTiles();
        WindowEffects.SetBorderColor(this, null);
    }

    /// <summary>快速操作收起为底部按钮栏的高度：按钮栏保持可见，提示可以拖到这里。</summary>
    private void ResetTiles()
    {
        _tilesExpanded = false;
        foreach (var (element, property, value) in TileStates(expanded: false))
        {
            element.BeginAnimation(property, null);
            element.SetValue(property, value);
        }
    }

    private void SetTilesExpanded(bool expanded)
    {
        if (expanded == _tilesExpanded) return;
        _tilesExpanded = expanded;
        var duration = expanded ? TilesExpandDuration : TilesCollapseDuration;
        foreach (var (element, property, value) in TileStates(expanded))
        {
            if (ShelfMotion.Enabled)
                element.BeginAnimation(property, new DoubleAnimation(value, duration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
            else
            {
                element.BeginAnimation(property, null);
                element.SetValue(property, value);
            }
        }
    }

    /// <summary>展开时快速操作从按钮栏向上长高并淡入，按钮栏同时淡出。</summary>
    private IEnumerable<(UIElement Element, DependencyProperty Property, double Value)> TileStates(bool expanded)
    {
        var collapsedHeight = ViewModel.HasActions ? ActionBarHeight : 0;
        if (expanded) ActionTiles.Measure(new System.Windows.Size(ActionTilesHost.ActualWidth, double.PositiveInfinity));
        yield return (ActionTilesHost, HeightProperty, expanded ? ActionTiles.DesiredSize.Height : collapsedHeight);
        yield return (ActionTiles, OpacityProperty, expanded ? 1 : 0);
        yield return (ActionBarHost, OpacityProperty, expanded ? 0 : 1);
    }

    private QuickActionInfo? HitTestTiles(WpfDragEventArgs e)
    {
        if (!_tilesExpanded) return null;
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
        if (!ViewModel.IsEmpty) return;
        await Task.Delay(1500);
        if (ViewModel.IsEmpty && !ViewModel.IsBusy && !IsDraggingOut) CloseAnimated();
    }

    // ───────────── 拖出 ─────────────

    private void BeginDragOut(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0 || ViewModel.IsBusy) return;
        IsDraggingOut = true;
        DragOutResult result;
        IReadOnlyList<ShelfItem> dragged;
        try
        {
            result = ShelfDragService.DoDragDrop(this, items, ViewModel.Id, out dragged, _dragOutMode);
        }
        finally
        {
            IsDraggingOut = false;
        }

        ViewModel.HasInteracted = true;
        if (result == DragOutResult.Cancelled) return;
        // 聊天软件和浏览器常在放下之后才读取文件，临时附件要多保留一会儿。
        if (dragged.Any(item => item.IsTemporaryAttachment)) ViewModel.KeepTemporaryFilesForAWhile();
        var closeWhenEmpty = _host.Settings.CloseWhenEmpty;
        ViewModel.ApplyDragOut(dragged, result, removeCopied: closeWhenEmpty);
        if (ViewModel.IsEmpty && closeWhenEmpty) CloseAnimated();
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
        if (ViewModel.IsEmpty && _host.Settings.CloseWhenEmpty) CloseAnimated();
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
        await ViewModel.RunActionAsync(info, GetTargets(), new WindowInteropHelper(this).Handle, PickFolder);
    }

    private string? PickFolder()
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = Strings.ChooseMoveFolder, Multiselect = false };
        return dialog.ShowDialog(this) == true ? dialog.FolderName : null;
    }

    private void OnMoreClick(object sender, RoutedEventArgs e) => OpenMenu(BuildShelfMenu(), MoreButton);

    private void OnViewToggleClick(object sender, RoutedEventArgs e)
    {
        ViewModel.HasInteracted = true;
        if (ViewModel.IsExpanded) ViewModel.IsExpanded = false;
        else Expand();
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => CloseAnimated();

    private void OnHeaderMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ButtonState != MouseButtonState.Pressed) return;
        try
        {
            StopSlide();
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
        if (ViewModel.IsExpanded)
            menu.Items.Add(CreateMenuItem(Strings.MenuCollapse, "\uE81E", () => ViewModel.IsExpanded = false, "Esc"));
        else if (hasItems)
            menu.Items.Add(CreateMenuItem(Strings.MenuExpand, "\uE8FD", Expand));
        menu.Items.Add(CreateMenuItem(Strings.MenuSelectAll, "", SelectAll, "Ctrl+A", ViewModel.HasMultiple));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopy, "", () => ViewModel.CopyToClipboard(targets), "Ctrl+C", hasItems));
        menu.Items.Add(CreateMenuItem(Strings.MenuPaste, "", Paste, "Ctrl+V"));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopyPath, "", () => ViewModel.CopyPaths(targets), enabled: targets.Any(item => item.IsFileSystem)));
        menu.Items.Add(CreateMenuItem(Strings.MenuReveal, "", () => ViewModel.Reveal(targets), enabled: targets.Any(item => item.IsFileSystem)));
        menu.Items.Add(new Separator());
        if (_host.History is { Latest: { } latest } history)
        {
            menu.Items.Add(CreateMenuItem(Strings.RestoreLastShelf, "", () => Restore(latest)));
            menu.Items.Add(HistoryMenu.Create(history.Entries, Restore, history.Clear));
        }
        menu.Items.Add(CreateMenuItem(Strings.MenuClear, "", () => RemoveItems(ViewModel.Items.ToArray()), enabled: hasItems));
        menu.Items.Add(CreateMenuItem(Strings.MenuSettings, "", _host.ShowSettings));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem(Strings.CloseShelf, "", CloseAnimated, "Esc"));
        return menu;
    }

    /// <summary>恢复的项目放进当前的空置物架；当前置物架已有内容时另开一个。</summary>
    private void Restore(ShelfHistoryEntry entry) => _host.Restore(entry, ViewModel.IsEmpty ? this : null);

    private ContextMenu BuildItemMenu(IReadOnlyList<ShelfItem> items)
    {
        var menu = new ContextMenu();
        var single = items.Count == 1 ? items[0] : null;
        menu.Items.Add(CreateMenuItem(single?.Kind == ShelfItemKind.Text ? Strings.MenuCopyText : Strings.MenuOpen, "",
            () => { if (single is not null) ViewModel.Open(single); }, "Enter", single is not null));
        menu.Items.Add(CreateMenuItem(Strings.MenuReveal, "", () => ViewModel.Reveal(items), enabled: items.Any(item => item.IsFileSystem)));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopy, "", () => ViewModel.CopyToClipboard(items), "Ctrl+C"));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopyPath, "", () => ViewModel.CopyPaths(items), enabled: items.Any(item => item.IsFileSystem)));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem(Strings.MenuRemove, "", () => RemoveItems(items), "Delete"));
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
            ViewModel.Notify(Strings.ClipboardEmpty, false);
            return;
        }
        try
        {
            var read = await DropDataReader.ReadAsync(data, cancellationToken: _lifetime.Token);
            if (IsClosing)
            {
                foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                return;
            }
            var summary = ViewModel.Add(read);
            ViewModel.Notify(summary.Added == 0 ? read.ErrorMessage ?? summary.Message : summary.Message, summary.Added > 0);
        }
        catch (Exception exception)
        {
            LogService.Error("从剪贴板粘贴失败", exception);
            ViewModel.Notify(Strings.PasteFailed, false);
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
