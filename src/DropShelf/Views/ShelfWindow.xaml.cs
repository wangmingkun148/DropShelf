using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
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
    private readonly DispatcherTimer _busyTimer;
    private bool _busyShown;
    private long _busyShownAt;
    /// <summary>窗口关闭时取消还在接收的附件。</summary>
    private readonly CancellationTokenSource _lifetime = new();
    private readonly List<ActionTile> _tiles = [];
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
    private (DropImageType Type, string Message)? _lastDescription;
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
        viewModel.RemoveVisibly = RemoveItems;

        _toastTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2.4) };
        _toastTimer.Tick += (_, _) => HideToast();
        _leaveCheck = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(220) };
        _leaveCheck.Tick += OnLeaveCheck;
        _busyTimer = new DispatcherTimer();
        _busyTimer.Tick += (_, _) => UpdateBusyOverlay(waited: true);

        SourceInitialized += OnSourceInitialized;
        ThemeService.ThemeChanged += OnThemeChanged;
        LanguageService.LanguageChanged += OnLanguageChanged;
        DragEnter += OnDragEnter;
        DragOver += OnDragOver;
        DragLeave += OnDragLeave;
        Drop += OnDrop;
        PreviewKeyDown += OnPreviewKeyDown;
        Activated += (_, _) => ViewModel.RefreshAvailabilityInBackground();
        PowerQoS.Track(this);

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

    protected override void OnClosed(EventArgs e)
    {
        _closed = true;
        _lifetime.Cancel();
        StopSlide();
        // 没播完的动画在这里收尾：待移除的项目先移除，不会记进最近关闭。
        FinishMotion();
        ViewModel.RemoveVisibly = null;
        _toastTimer.Stop();
        _leaveCheck.Stop();
        _busyTimer.Stop();
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
            case nameof(ShelfViewModel.IsBusy):
                UpdateBusyOverlay(waited: false);
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
        // 列表里移除之后先补位，窗口等补完再变小，见 CloseUpRows。
        if (!_holdSize) UpdateWindowSize();
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
            // 选中时强调色的底色是盖在卡片上的一层，淡入淡出即可，不用在两种画刷之间过渡；连边框一起盖住。
            var highlight = new Border { Margin = new Thickness(-1), CornerRadius = new CornerRadius(10), Opacity = 0 };
            highlight.SetResourceReference(Border.BackgroundProperty, "AccentBrush");
            var scale = new ScaleTransform();
            var tile = new Border
            {
                Height = ActionTileHeight,
                Margin = new Thickness(2, 3, 2, 3),
                CornerRadius = new CornerRadius(10),
                BorderThickness = new Thickness(1),
                RenderTransformOrigin = new WpfPoint(0.5, 0.5),
                RenderTransform = scale,
                Child = new Grid
                {
                    Children =
                    {
                        highlight,
                        new StackPanel
                        {
                            VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(large ? 6 : 2, 0, large ? 6 : 2, 0),
                            Children = { glyph, label }
                        }
                    }
                }
            };
            tile.SetResourceReference(Border.BackgroundProperty, "CardBrush");
            tile.SetResourceReference(Border.BorderBrushProperty, "CardBorderBrush");
            _tiles.Add(new ActionTile(tile, highlight, scale, glyph, label, info));
            ActionTiles.Children.Add(tile);
        }
        SetHoveredAction(null, force: true);
    }

    /// <summary>拖入时展开的一张快速操作卡片：底色、强调色的一层、放大用的变换和两行文字。</summary>
    private sealed record ActionTile(Border Tile, Border Highlight, ScaleTransform Scale, TextBlock Glyph, TextBlock Label, QuickActionInfo Info);

    private static readonly TimeSpan HoverDuration = TimeSpan.FromMilliseconds(140);
    private const double HoverScale = 1.04;

    /// <summary>
    /// 拖到快速操作卡片上时这张卡片铺上强调色、略微放大，文字颜色跟着过渡；放置区域变淡。
    /// force 时（重新生成卡片）直接到位，不播放动画。
    /// </summary>
    private void SetHoveredAction(QuickActionInfo? action, bool force = false)
    {
        if (!force && ReferenceEquals(action, _hoveredAction)) return;
        _hoveredAction = action;
        var animate = !force && ShelfMotion.Enabled;
        TimeSpan? duration = animate ? HoverDuration : null;
        foreach (var tile in _tiles)
        {
            var active = ReferenceEquals(tile.Info, action);
            Ease(tile.Highlight, OpacityProperty, active ? 1 : 0, duration);
            Ease(tile.Scale, ScaleTransform.ScaleXProperty, active ? HoverScale : 1, duration);
            Ease(tile.Scale, ScaleTransform.ScaleYProperty, active ? HoverScale : 1, duration);
            var text = active ? "TextOnAccentBrush" : "TextPrimaryBrush";
            FadeForeground(tile.Glyph, text, animate);
            FadeForeground(tile.Label, text, animate);
        }
        Ease(ShelfDropFrame, OpacityProperty, action is null ? 1 : 0.45, duration);
    }

    /// <summary>在 duration 内过渡到 to；duration 为 null（不播放动画）时直接设为 to。</summary>
    private static void Ease(DependencyObject target, DependencyProperty property, double to, TimeSpan? duration)
    {
        var animatable = (IAnimatable)target;
        animatable.BeginAnimation(property, duration is { } time ? new DoubleAnimation(to, time) { EasingFunction = MotionEase } : null);
        if (duration is null) target.SetValue(property, to);
    }

    /// <summary>文字颜色换成另一个主题颜色：过渡期间用一支单独的画刷，过渡完交还给主题，切换主题时照常更新。</summary>
    private static void FadeForeground(TextBlock text, string key, bool animate)
    {
        var from = (text.Foreground as SolidColorBrush)?.Color;
        text.SetResourceReference(TextBlock.ForegroundProperty, key);
        if (!animate || from is not { } start || text.Foreground is not SolidColorBrush { Color: var target } || start == target) return;
        var brush = new SolidColorBrush(start);
        text.Foreground = brush;
        var fade = new ColorAnimation(target, HoverDuration);
        fade.Completed += (_, _) =>
        {
            if (ReferenceEquals(text.Foreground, brush)) text.SetResourceReference(TextBlock.ForegroundProperty, key);
        };
        brush.BeginAnimation(SolidColorBrush.ColorProperty, fade);
    }

    // ───────────── 正在处理 ─────────────

    /// <summary>开始处理后过这么久还没完成才显示遮罩；显示后至少停留 BusyMinimum。</summary>
    private static readonly TimeSpan BusyDelay = TimeSpan.FromMilliseconds(150);
    private static readonly TimeSpan BusyMinimum = TimeSpan.FromMilliseconds(300);
    private static readonly TimeSpan BusyFade = TimeSpan.FromMilliseconds(120);

    /// <summary>
    /// 处理中的遮罩：很快就完成的操作不显示，免得一闪而过；显示出来以后至少停留一会儿再淡出。
    /// 遮罩显示之前的这一小段时间里，拖入和快速操作照常因为 IsBusy 而不接收。
    /// </summary>
    private void UpdateBusyOverlay(bool waited)
    {
        _busyTimer.Stop();
        var busy = ViewModel.IsBusy;
        if (busy == _busyShown || IsClosing) return;
        var wait = busy ? BusyDelay : BusyMinimum - Stopwatch.GetElapsedTime(_busyShownAt);
        if (!waited && wait > TimeSpan.Zero)
        {
            _busyTimer.Interval = wait;
            _busyTimer.Start();
            return;
        }
        _busyShown = busy;
        if (busy)
        {
            _busyShownAt = Stopwatch.GetTimestamp();
            BusyOverlay.Visibility = Visibility.Visible;
            BusyOverlay.BeginAnimation(OpacityProperty, ShelfMotion.Enabled ? new DoubleAnimation(0, 1, BusyFade) : null);
            return;
        }
        if (!ShelfMotion.Enabled)
        {
            BusyOverlay.Visibility = Visibility.Collapsed;
            return;
        }
        var fade = new DoubleAnimation(0, BusyFade);
        fade.Completed += (_, _) =>
        {
            if (!_busyShown) BusyOverlay.Visibility = Visibility.Collapsed;
        };
        BusyOverlay.BeginAnimation(OpacityProperty, fade);
    }

    // ───────────── 提示 ─────────────

    private void ShowToast(ShelfToast toast)
    {
        if (IsClosing) return;
        ToastText.Text = toast.Message;
        ToastGlyph.Text = toast.Success ? "" : "";
        ToastGlyph.SetResourceReference(TextBlock.ForegroundProperty, toast.Success ? "SuccessBrush" : "CautionBrush");
        Toast.BeginAnimation(OpacityProperty, ShelfMotion.Gate(new DoubleAnimation(1, TimeSpan.FromMilliseconds(140))));
        ToastOffset.BeginAnimation(TranslateTransform.YProperty, ShelfMotion.Gate(new DoubleAnimation(8, 0, TimeSpan.FromMilliseconds(200)) { EasingFunction = MotionEase }));
        _toastTimer.Stop();
        _toastTimer.Start();
    }

    private void HideToast()
    {
        _toastTimer.Stop();
        Toast.BeginAnimation(OpacityProperty, ShelfMotion.Gate(new DoubleAnimation(0, TimeSpan.FromMilliseconds(180))));
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
