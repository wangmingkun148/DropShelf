using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow : Window
{
    private readonly AppController _controller;

    public MainWindow(AppController controller)
    {
        InitializeComponent();
        _controller = controller;
        DataContext = new SettingsViewModel(controller);
        ApplySavedBounds(controller.Settings);

        SourceInitialized += OnSourceInitialized;
        StateChanged += (_, _) => UpdateMaximizedPadding();
        LayoutUpdated += (_, _) => UpdateCardHeight(PageScroll.ViewportHeight);
        ThemeService.ThemeChanged += OnThemeChanged;
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        if (WindowEffects.ApplyTheme(this, SystemBackdrop.Mica, ThemeService.IsDark))
            Background = System.Windows.Media.Brushes.Transparent;
    }

    private void OnThemeChanged(object? sender, EventArgs e) => WindowEffects.ApplyTheme(this, SystemBackdrop.Mica, ThemeService.IsDark);

    private SettingsViewModel ViewModel => (SettingsViewModel)DataContext;

    private void OnBrowseFolderClick(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Editor is not { } editor) return;
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = Strings.ChooseTargetFolder, Multiselect = false };
        if (Directory.Exists(editor.Folder)) dialog.InitialDirectory = editor.Folder;
        if (dialog.ShowDialog(this) == true) editor.Folder = dialog.FolderName;
    }

    private void OnBrowseProgramClick(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Editor is not { } editor) return;
        var dialog = new Microsoft.Win32.OpenFileDialog { Title = Strings.ChooseProgram, Filter = Strings.ProgramFilter, DereferenceLinks = true };
        if (File.Exists(editor.ProgramPath)) dialog.InitialDirectory = Path.GetDirectoryName(editor.ProgramPath);
        if (dialog.ShowDialog(this) == true) editor.ProgramPath = dialog.FileName;
    }

    private void OnPageCardClick(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string page }) ShowPage(page);
    }

    private void OnBackClick(object sender, RoutedEventArgs e) => ShowPage("Overview");

    /// <summary>概览页列出各项设置，点进去显示对应页面，左上角返回。</summary>
    private void ShowPage(string page)
    {
        OverviewPage.Visibility = page == "Overview" ? Visibility.Visible : Visibility.Collapsed;
        GeneralPage.Visibility = page == "General" ? Visibility.Visible : Visibility.Collapsed;
        ShelfPage.Visibility = page == "Shelf" ? Visibility.Visible : Visibility.Collapsed;
        ActionsPage.Visibility = page == "Actions" ? Visibility.Visible : Visibility.Collapsed;
        AppearancePage.Visibility = page == "Appearance" ? Visibility.Visible : Visibility.Collapsed;
        PanelPageTitle.Visibility = _sidePanelShown && page == "Overview" ? Visibility.Visible : Visibility.Collapsed;
        PageScroll.ScrollToTop();
        FadeInPage(page switch
        {
            "General" => GeneralPage,
            "Shelf" => ShelfPage,
            "Actions" => ActionsPage,
            "Appearance" => AppearancePage,
            _ => OverviewPage
        });
    }

    /// <summary>
    /// 新页面从稍低处上移并淡入；只改变位移和透明度，不影响排版。
    /// 动画期间页面缓存为位图，每帧只叠一张图，不用重画整页内容；结束后去掉缓存，文字恢复清晰的 ClearType。
    /// </summary>
    private static void FadeInPage(FrameworkElement page)
    {
        if (page.RenderTransform is not TranslateTransform shift) page.RenderTransform = shift = new TranslateTransform();
        page.CacheMode ??= new BitmapCache { SnapsToDevicePixels = true };
        var duration = TimeSpan.FromMilliseconds(300);
        shift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(14, 0, duration) { EasingFunction = MotionEase });
        var fade = new DoubleAnimation(0, 1, duration) { EasingFunction = MotionEase };
        fade.Completed += (_, _) => page.CacheMode = null;
        page.BeginAnimation(OpacityProperty, fade);
    }

    private void OnVersionClick(object sender, RoutedEventArgs e) => new AboutWindow(ViewModel) { Owner = this }.ShowDialog();

    // ───────────── 左侧状态栏 ─────────────

    // 内容区宽到放得下状态栏和完整页面时显示；两个阈值之间不切换，拖动窗口边缘时不会反复出现、消失。
    private const double SidePanelShowWidth = 1260;
    private const double SidePanelHideWidth = 1160;
    private const double SidePanelWidth = 300;
    private const double PageMargin = 36;
    private static readonly Duration MotionDuration = TimeSpan.FromMilliseconds(340);
    private static readonly IEasingFunction MotionEase = new CubicEase { EasingMode = EasingMode.EaseOut };
    private bool _sidePanelWanted;
    private bool _sidePanelShown;
    private bool _sidePanelPending;

    private void OnContentLayerSizeChanged(object sender, SizeChangedEventArgs e)
    {
        var width = e.NewSize.Width;
        if (_sidePanelWanted ? width >= SidePanelHideWidth : width < SidePanelShowWidth) return;
        _sidePanelWanted = !_sidePanelWanted;
        if (!IsLoaded)
        {
            SetSidePanel(animate: false);
            return;
        }
        if (_sidePanelPending) return;
        // 排版过程中不能再排版：等这一帧排完再切换，切换后测量各元素的新位置。
        _sidePanelPending = true;
        Dispatcher.BeginInvoke(() =>
        {
            _sidePanelPending = false;
            SetSidePanel(animate: true);
        });
    }

    /// <summary>
    /// 先记下各元素当前看到的位置，再一次性改成新布局并排版，然后让它们从原位置滑到新位置：
    /// 页面横向让出状态栏，概览标题飞到状态栏图标右侧（或飞回），下面的内容补上标题让出的空间。
    /// 动画只改变位移和透明度，每帧不需要重新排版；状态栏动画期间缓存为位图。
    /// </summary>
    private void SetSidePanel(bool animate)
    {
        var show = _sidePanelWanted;
        if (show == _sidePanelShown) return;
        _sidePanelShown = show;

        var onOverview = OverviewPage.Visibility == Visibility.Visible;
        var flyingTitle = show ? PanelPageTitle : OverviewTitle;
        var titleShift = show ? PanelTitleShift : OverviewTitleShift;
        var pageBefore = PositionOf(PageContent);
        var bodyBefore = PositionOf(OverviewBody);
        var titleBefore = PositionOf(show ? OverviewTitle : PanelPageTitle);
        var buttonSnapshot = animate && show && onOverview ? Snapshot(OverviewOpenButton) : null;
        var panelWasHidden = SidePanel.Visibility != Visibility.Visible;
        var panelOpacity = panelWasHidden ? 0 : SidePanel.Opacity;
        var panelOffset = panelWasHidden ? -24 : SidePanelShift.X;

        PageContent.Margin = new Thickness(show ? PageMargin + SidePanelWidth : PageMargin, 26, PageMargin, PageMargin);
        OverviewHeader.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        PanelPageTitle.Visibility = show && onOverview ? Visibility.Visible : Visibility.Collapsed;
        SidePanel.Visibility = Visibility.Visible;
        SidePanel.IsHitTestVisible = show;
        foreach (var (target, property) in new (Animatable, DependencyProperty)[]
                 {
                     (PageContentShift, TranslateTransform.XProperty), (OverviewBodyShift, TranslateTransform.YProperty),
                     (OverviewTitleShift, TranslateTransform.XProperty), (OverviewTitleShift, TranslateTransform.YProperty),
                     (PanelTitleShift, TranslateTransform.XProperty), (PanelTitleShift, TranslateTransform.YProperty),
                     (SidePanelShift, TranslateTransform.XProperty)
                 })
        {
            target.BeginAnimation(property, null);
            target.SetValue(property, 0.0);
        }
        SidePanel.BeginAnimation(OpacityProperty, null);
        OverviewOpenButton.BeginAnimation(OpacityProperty, null);

        if (!animate)
        {
            OnSidePanelSettled();
            return;
        }

        ContentLayer.UpdateLayout();
        var pageOffset = pageBefore.X - PositionOf(PageContent).X;
        PageContentShift.BeginAnimation(TranslateTransform.XProperty, Motion(pageOffset, 0));
        if (onOverview)
        {
            OverviewBodyShift.BeginAnimation(TranslateTransform.YProperty, Motion(bodyBefore.Y - PositionOf(OverviewBody).Y, 0));
            // 页面内的标题还会随页面一起平移，要扣除这部分。
            var titleAfter = PositionOf(flyingTitle);
            titleShift.BeginAnimation(TranslateTransform.XProperty, Motion(titleBefore.X - titleAfter.X - (show ? 0 : pageOffset), 0));
            titleShift.BeginAnimation(TranslateTransform.YProperty, Motion(titleBefore.Y - titleAfter.Y, 0));
            if (!show) OverviewOpenButton.BeginAnimation(OpacityProperty, Motion(0, 1));
        }
        if (buttonSnapshot is { } snapshot) FadeOutSnapshot(snapshot, -pageOffset);

        SidePanelShift.BeginAnimation(TranslateTransform.XProperty, Motion(panelOffset, show ? 0 : -24));
        SidePanel.CacheMode ??= new BitmapCache { SnapsToDevicePixels = true };
        var fade = Motion(panelOpacity, show ? 1 : 0);
        fade.Completed += (_, _) => OnSidePanelSettled();
        SidePanel.BeginAnimation(OpacityProperty, fade);
    }

    private System.Windows.Point PositionOf(UIElement element) => element.TranslatePoint(default, ContentLayer);

    private static DoubleAnimation Motion(double from, double to) =>
        new(from, to, MotionDuration) { EasingFunction = MotionEase };

    /// <summary>动画结束后去掉位图缓存，文字恢复清晰的 ClearType；收起的状态栏不再参与排版。</summary>
    private void OnSidePanelSettled()
    {
        if (SidePanel.HasAnimatedProperties && SidePanel.Opacity != (_sidePanelShown ? 1 : 0)) return;
        SidePanel.CacheMode = null;
        if (!_sidePanelShown) SidePanel.Visibility = Visibility.Collapsed;
    }

    /// <summary>概览页右上角的按钮随标题行一起收起；先截一张图留在原处，跟着页面平移并淡出。</summary>
    private (System.Windows.Controls.Image Image, System.Windows.Point Position)? Snapshot(FrameworkElement element)
    {
        if (!element.IsVisible || element.ActualWidth < 1) return null;
        var dpi = VisualTreeHelper.GetDpi(element);
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(element.ActualWidth * dpi.DpiScaleX), (int)Math.Ceiling(element.ActualHeight * dpi.DpiScaleY),
            dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
            context.DrawRectangle(new VisualBrush(element), null, new Rect(0, 0, element.ActualWidth, element.ActualHeight));
        bitmap.Render(visual);
        bitmap.Freeze();
        var image = new System.Windows.Controls.Image { Source = bitmap, Width = element.ActualWidth, Height = element.ActualHeight, RenderTransform = new TranslateTransform() };
        return (image, element.TranslatePoint(default, DragLayer));
    }

    private void FadeOutSnapshot((System.Windows.Controls.Image Image, System.Windows.Point Position) snapshot, double distance)
    {
        var (image, position) = snapshot;
        Canvas.SetLeft(image, position.X);
        Canvas.SetTop(image, position.Y);
        DragLayer.Children.Add(image);
        image.RenderTransform.BeginAnimation(TranslateTransform.XProperty, Motion(0, distance));
        var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(180));
        fade.Completed += (_, _) => DragLayer.Children.Remove(image);
        image.BeginAnimation(OpacityProperty, fade);
    }

    // ───────────── 设置卡片高度 ─────────────

    /// <summary>概览页四张设置卡片的高度：窗口较高时拉长卡片填满页面，避免纵向留白。</summary>
    public static readonly DependencyProperty SettingsCardHeightProperty =
        DependencyProperty.Register(nameof(SettingsCardHeight), typeof(double), typeof(MainWindow), new PropertyMetadata(MinCardHeight));

    private const double MinCardHeight = 70;
    private const double MaxCardHeight = 230;
    // 牌抽出后会超出牌堆标题约这么高；窗口够高时在卡片和牌堆之间留出这段空间，不够高时才允许牌盖住卡片。
    private const double FanHeadroom = 72;
    private double _cardHeightTarget = MinCardHeight;
    private double _viewportHeight;
    private double _measuredHeight;

    public double SettingsCardHeight
    {
        get => (double)GetValue(SettingsCardHeightProperty);
        set => SetValue(SettingsCardHeightProperty, value);
    }

    /// <summary>
    /// 页面其余内容的高度不变，剩下的空间分给两行卡片；牌堆由布局固定在窗口底边，不受影响。
    /// 每次排版后检查一次：拖动窗口大小时直接跟随，只有页面内容变化（如收起入门提示）时才平滑过渡。
    /// </summary>
    private void UpdateCardHeight(double viewport)
    {
        // 概览页刚从隐藏切回来、还没测量时 DesiredSize 是 0，等排版后再算。
        if (OverviewPage.Visibility != Visibility.Visible || OverviewPage.DesiredSize.Height <= 0 || viewport <= 0) return;
        var resized = Math.Abs(viewport - _viewportHeight) >= 1;
        _viewportHeight = viewport;
        // 用测量高度减去卡片区域本身，结果与卡片当前高度无关，动画和像素取整都不会反过来改变目标。
        var rest = OverviewPage.DesiredSize.Height - PageCards.DesiredSize.Height;
        var spare = viewport - PageContent.Margin.Top - PageContent.Margin.Bottom - rest;
        var headroom = HistorySection.Visibility == Visibility.Visible ? FanHeadroom : 0;
        var gap = ((FrameworkElement)PageCards.Children[0]).Margin.Bottom;
        var cards = Math.Floor(Math.Clamp((spare - headroom) / 2 - gap, MinCardHeight, MaxCardHeight));
        if (Math.Abs(cards - _cardHeightTarget) < 1) return;
        _cardHeightTarget = cards;
        // 动画落后于窗口边缘时页面会时高时矮、滚动条反复出现，看起来整页都在抖。
        if (IsLoaded && !resized)
        {
            BeginAnimation(SettingsCardHeightProperty, Motion(SettingsCardHeight, cards));
            return;
        }
        BeginAnimation(SettingsCardHeightProperty, null);
        SettingsCardHeight = cards;
    }

    /// <summary>
    /// 窗口高度变化时，在排版之前就按新的可视高度算好卡片高度。否则会先按旧高度排一遍：
    /// 窗口变矮时内容一度超出窗口、出现滚动条，排完才缩回去，拖动窗口边缘时看起来在抖。
    /// </summary>
    protected override Size MeasureOverride(Size availableSize)
    {
        if (_viewportHeight > 0 && _measuredHeight > 0 && availableSize.Height != _measuredHeight && !double.IsInfinity(availableSize.Height))
            UpdateCardHeight(_viewportHeight + availableSize.Height - _measuredHeight);
        _measuredHeight = availableSize.Height;
        return base.MeasureOverride(availableSize);
    }

    // ───────────── 拖动更换快速操作 ─────────────

    /// <summary>按下鼠标的操作卡片；移动超过拖动距离后才开始拖动。</summary>
    private ActionCardViewModel? _pressedCard;
    private System.Windows.Point _pressPoint;
    private bool _draggingCard;
    private ActionSlotViewModel? _dropTarget;
    private bool _overDeleteZone;

    private void OnActionCardMouseDown(object sender, MouseButtonEventArgs e)
    {
        var source = e.OriginalSource as DependencyObject;
        if (FindAncestor<ButtonBase>(source) is not null) return;
        _pressedCard = FindAncestor<FrameworkElement>(source, element => element.DataContext is ActionCardViewModel)?.DataContext as ActionCardViewModel;
        _pressPoint = e.GetPosition(DragLayer);
    }

    private void OnActionCardMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (_pressedCard is not { } card || e.LeftButton != MouseButtonState.Pressed) return;
        var position = e.GetPosition(DragLayer);
        if (!_draggingCard)
        {
            if (Math.Abs(position.X - _pressPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
                Math.Abs(position.Y - _pressPoint.Y) < SystemParameters.MinimumVerticalDragDistance) return;
            BeginCardDrag(card);
        }

        // 内置操作拖进底部区域时像碰到弹性边界：越往下越拖不动，最多陷进去一小段。
        var zoneTop = DragLayer.ActualHeight - DeleteZone.ActualHeight;
        var y = card.IsCustom || position.Y <= zoneTop
            ? position.Y
            : zoneTop + LockedSink * (1 - Math.Exp(-(position.Y - zoneTop) / LockedSink));
        Canvas.SetLeft(DragGhost, position.X + 12);
        Canvas.SetTop(DragGhost, y + 8);
        SetOverDeleteZone(position.Y >= zoneTop);
        SetDropTarget(_overDeleteZone ? null : HitTestSlot(e.GetPosition(ActionSlotsList)), card);
    }

    private void OnActionCardMouseUp(object sender, MouseButtonEventArgs e)
    {
        var card = _pressedCard;
        var target = _dropTarget;
        var dragged = _draggingCard;
        var overZone = _overDeleteZone;
        EndCardDrag();
        if (!dragged || card is null) return;
        if (overZone && card.IsCustom) ConfirmDeleteCustomAction(card);
        else if (overZone) BounceBackGhost();
        else if (target is not null) ViewModel.AssignSlot(target.Index, card.Action.Id);
    }

    /// <summary>内置操作在底部松手：跟随的卡片弹回区域上方并淡出。</summary>
    private void BounceBackGhost()
    {
        DragGhost.Visibility = Visibility.Visible;
        var top = DragLayer.ActualHeight - DeleteZone.ActualHeight - DragGhost.ActualHeight;
        DragGhost.BeginAnimation(Canvas.TopProperty, new DoubleAnimation(Math.Min(Canvas.GetTop(DragGhost), top), TimeSpan.FromMilliseconds(320))
        {
            EasingFunction = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 }
        });
        var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(180)) { BeginTime = TimeSpan.FromMilliseconds(160) };
        fade.Completed += (_, _) =>
        {
            if (_draggingCard) return;
            DragGhost.Visibility = Visibility.Collapsed;
            ClearGhostAnimations();
        };
        DragGhost.BeginAnimation(OpacityProperty, fade);
    }

    private void ClearGhostAnimations()
    {
        DragGhost.BeginAnimation(Canvas.TopProperty, null);
        DragGhost.BeginAnimation(OpacityProperty, null);
    }

    /// <summary>要删除的自定义操作，确认对话框打开期间有值。</summary>
    private ActionCardViewModel? _pendingDelete;

    /// <summary>与编辑对话框相同样式的确认框：遮罩淡入，对话框从略小处放大到原尺寸。</summary>
    private void ConfirmDeleteCustomAction(ActionCardViewModel card)
    {
        _pendingDelete = card;
        ConfirmMessage.Text = Strings.ConfirmDeleteAction(card.Title);
        ConfirmOverlay.Visibility = Visibility.Visible;
        ConfirmOverlay.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(150)));
        ConfirmDialogScale.BeginAnimation(ScaleTransform.ScaleXProperty, Motion(0.96, 1));
        ConfirmDialogScale.BeginAnimation(ScaleTransform.ScaleYProperty, Motion(0.96, 1));
        ConfirmCancelButton.Focus();
    }

    private void OnConfirmDeleteClick(object sender, RoutedEventArgs e)
    {
        if (_pendingDelete?.Action.Custom is { } custom) ViewModel.DeleteCustomAction(custom);
        CloseConfirm();
    }

    private void OnConfirmCancelClick(object sender, RoutedEventArgs e) => CloseConfirm();

    private void CloseConfirm()
    {
        if (_pendingDelete is null) return;
        _pendingDelete = null;
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(120));
        fade.Completed += (_, _) =>
        {
            if (_pendingDelete is null) ConfirmOverlay.Visibility = Visibility.Collapsed;
        };
        ConfirmOverlay.BeginAnimation(OpacityProperty, fade);
    }

    /// <summary>子元素失去捕获也会冒泡到这里，只处理拖动时页面本身失去捕获（如切换窗口）。</summary>
    private void OnActionCardLostCapture(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (ReferenceEquals(e.OriginalSource, ActionsPage)) EndCardDrag();
    }

    private void BeginCardDrag(ActionCardViewModel card)
    {
        _draggingCard = true;
        card.IsDragSource = true;
        DragGhostGlyph.Text = card.Glyph;
        DragGhostText.Text = card.Title;
        ClearGhostAnimations();
        DragGhost.Visibility = Visibility.Visible;
        var zoneBrush = (System.Windows.Media.Brush)FindResource(card.IsCustom ? "DeleteBrush" : "LockedBrush");
        DeleteGlow.Fill = DeleteHint.Background = zoneBrush;
        DeleteHintGlyph.Text = (string)FindResource(card.IsCustom ? "Glyph.Delete" : "Glyph.Lock");
        DeleteHintText.Text = card.IsCustom ? Strings.ReleaseToDelete : Strings.BuiltInCannotDelete;
        Fade(DeleteZone, 1);
        Mouse.Capture(ActionsPage, CaptureMode.SubTree);
    }

    private void EndCardDrag()
    {
        if (_pressedCard is { } card) card.IsDragSource = false;
        SetDropTarget(null, null);
        SetOverDeleteZone(false);
        Fade(DeleteZone, 0);
        _pressedCard = null;
        DragGhost.Visibility = Visibility.Collapsed;
        if (!_draggingCard) return;
        _draggingCard = false;
        if (Mouse.Captured == ActionsPage) ActionsPage.ReleaseMouseCapture();
    }

    /// <summary>内置操作拖进底部区域时最多陷进去的深度。</summary>
    private const double LockedSink = 28;

    /// <summary>拖进底部区域时光效变亮，并提示松开删除或不能删除。</summary>
    private void SetOverDeleteZone(bool value)
    {
        if (value == _overDeleteZone) return;
        _overDeleteZone = value;
        Fade(DeleteGlow, value ? 0.8 : 0.35);
        Fade(DeleteHint, value ? 1 : 0);
    }

    private static void Fade(UIElement element, double to) =>
        element.BeginAnimation(OpacityProperty, new DoubleAnimation(to, TimeSpan.FromMilliseconds(150)));

    /// <summary>悬停的位置显示“交换”（从另一个位置拖来）或“替换”（从更多操作拖来）。</summary>
    private void SetDropTarget(ActionSlotViewModel? target, ActionCardViewModel? card)
    {
        if (ReferenceEquals(target, card)) target = null;
        if (ReferenceEquals(target, _dropTarget)) return;
        if (_dropTarget is not null) _dropTarget.DropHint = null;
        _dropTarget = target;
        if (target is not null) target.DropHint = card is ActionSlotViewModel ? Strings.DropHintSwap : Strings.DropHintReplace;
    }

    private ActionSlotViewModel? HitTestSlot(System.Windows.Point point) =>
        ActionSlotsList.IsVisible && ActionSlotsList.InputHitTest(point) is DependencyObject hit
            ? FindAncestor<FrameworkElement>(hit, element => element.DataContext is ActionSlotViewModel)?.DataContext as ActionSlotViewModel
            : null;

    // ───────────── 拖出最近关闭的置物架 ─────────────

    /// <summary>卡牌拖到牌堆上方这么高的范围内松手算放回；越靠近牌堆，空位对卡牌的吸引越强。</summary>
    private const double HistoryReturnReach = 60;
    private const double DetachedScale = 1.06;

    /// <summary>拖动不到这个距离时卡牌被牌堆吸住，只跟随一小部分；超过后脱离。</summary>
    private const double TearDistance = 48;
    private const double HoldFollow = 0.3;

    /// <summary>拖回牌堆上时卡牌最多被空位拉过去这一比例。</summary>
    private const double ReturnPull = 0.25;

    private HistoryEntryViewModel? _pressedEntry;
    private FrameworkElement? _draggedContainer;
    private Controls.FanPanel? _historyPanel;
    private int _historyIndex;
    private System.Windows.Point _historyPressPoint;
    private Vector _grabOffset;
    private System.Windows.Point _dragStartCursor;
    private bool _tornOff;
    private bool _draggingHistory;
    private CardGhostWindow? _ghost;

    private void OnHistoryMouseDown(object sender, MouseButtonEventArgs e)
    {
        _pressedEntry = null;
        var source = e.OriginalSource as DependencyObject;
        if (_ghost?.IsBusy == true ||
            FindAncestor<FrameworkElement>(source, element => element.DataContext is HistoryEntryViewModel) is not { DataContext: HistoryEntryViewModel entry } ||
            FindAncestor<ButtonBase>(source)?.Command != entry.RestoreCommand) return;
        _pressedEntry = entry;
        _historyPressPoint = e.GetPosition(this);
        _ghost ??= new CardGhostWindow();
    }

    private void OnHistoryMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (_draggingHistory || _pressedEntry is not { } entry || e.LeftButton != MouseButtonState.Pressed) return;
        var position = e.GetPosition(this);
        if (Math.Abs(position.X - _historyPressPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(position.Y - _historyPressPoint.Y) < SystemParameters.MinimumVerticalDragDistance) return;
        BeginHistoryDrag(entry);
    }

    private void OnHistoryMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_draggingHistory)
        {
            _pressedEntry = null;
            return;
        }
        e.Handled = true;
        EndHistoryDrag(cancel: false);
    }

    /// <summary>子元素失去捕获也会冒泡到这里，只处理拖动时列表本身失去捕获（如切换窗口）。</summary>
    private void OnHistoryLostCapture(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (ReferenceEquals(e.OriginalSource, HistoryList)) EndHistoryDrag(cancel: true);
    }

    /// <summary>
    /// 卡牌从当前的位置、大小和角度脱离牌堆，换成跟随光标的截图窗口，原位留出空位。
    /// 截图前先收起选中状态（强调色边框、“点击恢复”提示），拖着的是一张干净的卡牌。
    /// </summary>
    private void BeginHistoryDrag(HistoryEntryViewModel entry)
    {
        _pressedEntry = null;
        if (_ghost is null || HistoryList.ItemContainerGenerator.ContainerFromItem(entry) is not ContentPresenter container ||
            VisualTreeHelper.GetChildrenCount(container) == 0 || VisualTreeHelper.GetChild(container, 0) is not FrameworkElement card ||
            PoseOf(container) is not { } start || !Mouse.Capture(HistoryList, CaptureMode.Element)) return;

        _draggingHistory = true;
        _pressedEntry = entry;
        _draggedContainer = container;
        _dragStartCursor = CursorPoint();
        _grabOffset = _dragStartCursor - start.Center;
        _tornOff = false;
        _historyPanel = VisualTreeHelper.GetParent(container) as Controls.FanPanel;
        _historyIndex = HistoryList.ItemContainerGenerator.IndexFromContainer(container);
        _historyPanel?.ClearHot();
        (container.ContentTemplate?.FindName("RestoreHint", container) as UIElement)?.BeginAnimation(OpacityProperty, null);
        card.UpdateLayout();
        if (Snapshot(card)?.Image.Source is not { } image)
        {
            EndHistoryDrag(cancel: true);
            return;
        }
        container.Opacity = 0;
        _historyPanel?.SetGap(container, _historyIndex);
        _ghost.ClipBottom = () => !_tornOff || HistoryReturnWeight(CursorPoint()) > 0 ? WindowBottom() : null;
        _ghost.Begin(image, new Size(card.ActualWidth, card.ActualHeight), start, () => HistoryDragPose(start, container));
    }

    /// <summary>
    /// 拖动分三段：先被牌堆吸住、只跟一小段；拉过一定距离后脱离并略微放大，顿一下跟上光标；
    /// 脱离后其余的牌并拢；拖回牌堆上方时按卡牌位置让出空位，越靠近牌堆，卡牌被空位吸得越紧、越接近原来的大小，提示松手会放到这里。
    /// </summary>
    private CardPose HistoryDragPose(CardPose start, FrameworkElement container)
    {
        var cursor = CursorPoint();
        var pulled = cursor - _dragStartCursor;
        if (!_tornOff && pulled.Length / VisualTreeHelper.GetDpi(this).DpiScaleX < TearDistance)
            return start with { Center = start.Center + pulled * HoldFollow };
        _tornOff = true;
        var free = cursor - _grabOffset;
        var weight = HistoryReturnWeight(cursor);
        _historyPanel?.SetGap(container, weight > 0 ? _historyPanel.SlotAt(_historyPanel.PointFromScreen(free).X) : -1);
        if (weight <= 0 || PoseOf(container) is not { } slot) return new CardPose(free, DetachedScale, 0);
        var pull = ReturnPull * weight;
        return new CardPose(free + (slot.Center - free) * pull, DetachedScale + (slot.Scale - DetachedScale) * weight, slot.Angle * pull);
    }

    /// <summary>在牌堆上松手时卡牌飞进让出的空位并移到这个位置，取消时飞回原位；其他位置松手时在光标处打开置物架，卡牌缩小淡出。</summary>
    private void EndHistoryDrag(bool cancel)
    {
        if (!_draggingHistory) return;
        _draggingHistory = false;
        var entry = _pressedEntry!;
        var container = _draggedContainer!;
        var panel = _historyPanel;
        _pressedEntry = null;
        _draggedContainer = null;
        _historyPanel = null;
        if (Mouse.Captured == HistoryList) HistoryList.ReleaseMouseCapture();
        if (_ghost is not { IsBusy: true } ghost)
        {
            container.Opacity = 1;
            panel?.SetGap(null, -1);
            return;
        }

        var cursor = CursorPoint();
        if (cancel || !_tornOff || HistoryReturnWeight(cursor) > 0)
        {
            var index = !cancel && _tornOff && panel?.Gap is >= 0 and var gap ? gap : _historyIndex;
            panel?.SetGap(container, index);
            ghost.ClipBottom = WindowBottom;
            ghost.Settle(() => PoseOf(container), () =>
            {
                container.Opacity = 1;
                panel?.SetGap(null, -1);
                // 卡牌落定、画面静止后再重排列表，重新生成卡片的停顿不会出现在动画中。
                Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, () => _controller.History.Move(entry.Entry, index));
            });
            return;
        }
        ghost.ClipBottom = () => null;
        ghost.Vanish(cursor);
        _controller.RestoreAt(entry.Entry, new POINT((int)cursor.X, (int)cursor.Y));
    }

    /// <summary>光标离牌堆多近：0 表示不在放回范围内，从范围上沿到牌堆上沿平滑增加到 1。</summary>
    private double HistoryReturnWeight(System.Windows.Point screen)
    {
        var window = PointFromScreen(screen);
        var list = HistoryList.PointFromScreen(screen);
        if (window.X < 0 || window.Y < 0 || window.X > ActualWidth || window.Y > ActualHeight ||
            list.X < 0 || list.X > HistoryList.ActualWidth || list.Y < -HistoryReturnReach || list.Y > HistoryList.ActualHeight) return 0;
        var t = Math.Clamp((list.Y + HistoryReturnReach) / HistoryReturnReach, 0.01, 1);
        return t * t * (3 - 2 * t);
    }

    /// <summary>窗口内容底边的屏幕坐标，牌堆的下半截在这里被裁掉。</summary>
    private double? WindowBottom() =>
        PresentationSource.FromVisual(RootGrid) is null ? null : RootGrid.PointToScreen(new System.Windows.Point(0, RootGrid.ActualHeight)).Y;

    /// <summary>元素在屏幕上的中心、缩放和角度，包含牌堆给它的变换和动画。</summary>
    private static CardPose? PoseOf(FrameworkElement element)
    {
        if (!element.IsVisible || element.ActualWidth < 1 || PresentationSource.FromVisual(element) is null) return null;
        var origin = element.PointToScreen(default);
        var edge = element.PointToScreen(new System.Windows.Point(element.ActualWidth, 0)) - origin;
        var center = element.PointToScreen(new System.Windows.Point(element.ActualWidth / 2, element.ActualHeight / 2));
        return new CardPose(center, edge.Length / (element.ActualWidth * VisualTreeHelper.GetDpi(element).DpiScaleX),
            Math.Atan2(edge.Y, edge.X) * 180 / Math.PI);
    }

    private static System.Windows.Point CursorPoint()
    {
        var cursor = WindowPlacement.GetCursor();
        return new System.Windows.Point(cursor.X, cursor.Y);
    }

    protected override void OnPreviewKeyDown(System.Windows.Input.KeyEventArgs e)
    {
        if (_draggingCard && e.Key == Key.Escape)
        {
            EndCardDrag();
            e.Handled = true;
        }
        if (_pendingDelete is not null && e.Key == Key.Escape)
        {
            CloseConfirm();
            e.Handled = true;
        }
        if (_draggingHistory && e.Key == Key.Escape)
        {
            EndHistoryDrag(cancel: true);
            e.Handled = true;
        }
        base.OnPreviewKeyDown(e);
    }

    private static T? FindAncestor<T>(DependencyObject? source, Func<T, bool>? match = null) where T : DependencyObject
    {
        while (source is not null)
        {
            if (source is T candidate && (match is null || match(candidate))) return candidate;
            source = source is Visual ? VisualTreeHelper.GetParent(source) : LogicalTreeHelper.GetParent(source);
        }
        return null;
    }

    /// <summary>自定义标题栏的窗口最大化时会超出屏幕边缘，需要留出边框宽度。</summary>
    private void UpdateMaximizedPadding()
    {
        var border = SystemParameters.WindowResizeBorderThickness;
        RootGrid.Margin = WindowState == WindowState.Maximized
            ? new Thickness(border.Left + 4, border.Top + 4, border.Right + 4, border.Bottom + 4)
            : new Thickness(0);
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        SaveBounds();
        base.OnClosing(e);
    }

    /// <summary>主窗口关闭后释放，下次打开时重新创建；程序留在通知区域，手势识别继续运行。</summary>
    protected override void OnClosed(EventArgs e)
    {
        ThemeService.ThemeChanged -= OnThemeChanged;
        ViewModel.Detach();
        _ghost?.Close();
        base.OnClosed(e);
    }

    private void ApplySavedBounds(AppSettings settings)
    {
        Width = settings.WindowWidth;
        Height = settings.WindowHeight;
        if (!IsVisiblePosition(settings.WindowLeft, settings.WindowTop, Width)) return;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = settings.WindowLeft;
        Top = settings.WindowTop;
    }

    private void SaveBounds()
    {
        if (WindowState != WindowState.Normal || !IsLoaded) return;
        _controller.SaveWindowBounds(new Rect(Left, Top, ActualWidth, ActualHeight));
    }

    private static bool IsVisiblePosition(double left, double top, double width) =>
        double.IsFinite(left) && double.IsFinite(top) &&
        left + width >= SystemParameters.VirtualScreenLeft + 120 &&
        top >= SystemParameters.VirtualScreenTop - 10 &&
        left <= SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth - 120 &&
        top <= SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight - 80;
}
