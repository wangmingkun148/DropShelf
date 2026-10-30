using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
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
using DropShelf.Views.Controls;
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
    private static readonly TimeSpan RankRollDuration = TimeSpan.FromMilliseconds(220);
    private static readonly TimeSpan DealDuration = TimeSpan.FromMilliseconds(180);
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
    private int _shownCount;
    private string? _lastDescription;
    private long _lastDragOverTick;
    private WpfPoint _pressPoint;
    private bool _stackPressed;
    /// <summary>刚由单击牌堆摊开，紧接着的双击第二下不算数。</summary>
    private bool _expandedByClick;
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
        _shownCount = viewModel.Count;
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
        CancelCard();
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
        WindowEffects.SetDarkMode(this, ThemeService.IsDark);
    }

    private void OnThemeChanged(object? sender, EventArgs e) => WindowEffects.SetDarkMode(this, ThemeService.IsDark);

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
                if (ViewModel.Count > _shownCount && _shownCount > 0) RollRank();
                _shownCount = ViewModel.Count;
                _countChanged = true;
                goto case nameof(ShelfViewModel.IsExpanded);
            case nameof(ShelfViewModel.IsExpanded):
                // 数量变化时项目集合还在通知列表控件的途中；此时改窗口大小会立即重新布局，
                // 平铺与项目不一致而抛出异常。等通知结束再更新，连续变化只更新一次。
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

    /// <summary>新牌落进这一摞：角标的数字从下面滚上来。</summary>
    private void RollRank()
    {
        if (!ShelfMotion.Enabled || !IsVisible) return;
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        StackRankShift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(8, 0, RankRollDuration) { EasingFunction = ease });
        StackRank.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, RankRollDuration) { EasingFunction = ease });
    }

    /// <summary>数量或视图变化后更新界面：只是切换了堆叠和平铺时播放切换动画，刚拖入文件时播放落下的动画。</summary>
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
        if (ViewModel.IsExpanded) PlayGridLanding(drop.Point, drop.Added);
        else PlayLanding(drop.Point, drop.Added);
    }

    /// <summary>最上面一张牌的角标；切换视图时和说明文字一起淡出淡入。</summary>
    private UIElement StackMark => StackRankCorner;

    /// <summary>落下动画期间最上面那张还没落下，先藏起角标。</summary>
    private void BeginLandingMark(int count) => StackRankCorner.Opacity = 0;

    /// <summary>最上面那张落下后角标出现，数字从下面滚上来。</summary>
    private void CardLanded(int count, bool top)
    {
        if (!top) return;
        StackRankCorner.Opacity = 1;
        RollRank();
    }

    private void RestoreStackMark() => StackRankCorner.ClearValue(OpacityProperty);

    private void OnRowLoaded(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is ShelfItem item) ViewModel.SetRowVisible(item, true);
    }

    private void OnRowUnloaded(object sender, RoutedEventArgs e)
    {
        if (((FrameworkElement)sender).DataContext is ShelfItem item) ViewModel.SetRowVisible(item, false);
    }

    private void UpdateViews()
    {
        StackView.Visibility = ViewModel.HasItems && !ViewModel.IsExpanded ? Visibility.Visible : Visibility.Collapsed;
        ItemsList.Visibility = ViewModel.IsExpanded ? Visibility.Visible : Visibility.Collapsed;
        ViewToggleButton.Content = ViewModel.IsExpanded ? "\uE81E" : "\uF0E2";
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
        // 平铺时窗口按一行的牌数定宽：内容区左右各留 10，列表默认模板四周还有 1，少算这 2 就会少放一列。
        var width = ViewModel.IsExpanded
            ? CardGridPanel.GridColumns(_size) * CardGridPanel.SlotWidth + 22
            : QuickShelfSizeOptions.ResolveWidth(_size);
        // 平铺最多显示两行，再多就滚动。
        var body = ViewModel.IsExpanded
            ? CardGridPanel.HeightFor(Math.Clamp(CardGridPanel.RowsFor(ViewModel.Count, width - 22), 1, 2))
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
                Style = (Style)FindResource("ShelfActionButtonStyle"),
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
                CornerRadius = new CornerRadius(6),
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
            tile.SetResourceReference(Border.BorderBrushProperty, active ? "AccentBrush" : "ShelfUnderCardBorderBrush");
            glyph.SetResourceReference(TextBlock.ForegroundProperty, active ? "TextOnAccentBrush" : "TextPrimaryBrush");
            label.SetResourceReference(TextBlock.ForegroundProperty, active ? "TextOnAccentBrush" : "TextPrimaryBrush");
        }
        ShelfDropFrame.Opacity = action is null ? 1 : 0.45;
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
