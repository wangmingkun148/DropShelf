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
        LayoutUpdated += (_, _) => UpdateCardHeight();
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
    }

    private void OnVersionClick(object sender, RoutedEventArgs e) => new AboutWindow(ViewModel) { Owner = this }.ShowDialog();

    // ───────────── 左侧状态栏 ─────────────

    // 内容区宽到放得下状态栏和完整页面时显示；两个阈值之间不切换，拖动窗口边缘时不会反复出现、消失。
    private const double SidePanelShowWidth = 1240;
    private const double SidePanelHideWidth = 1200;
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

    public double SettingsCardHeight
    {
        get => (double)GetValue(SettingsCardHeightProperty);
        set => SetValue(SettingsCardHeightProperty, value);
    }

    /// <summary>
    /// 页面其余内容的高度不变，剩下的空间分给两行卡片；牌堆由布局固定在窗口底边，不受影响。
    /// 每次排版后检查一次，只在目标高度变化时开始平滑过渡。
    /// </summary>
    private void UpdateCardHeight()
    {
        if (OverviewPage.Visibility != Visibility.Visible || PageScroll.ViewportHeight <= 0) return;
        // 概览页被拉伸到可视区域高度，ActualHeight 不能反映内容本身，用测量出的高度。
        var rest = OverviewPage.DesiredSize.Height - 2 * SettingsCardHeight;
        var spare = PageScroll.ViewportHeight - PageContent.Margin.Top - PageContent.Margin.Bottom - rest;
        var headroom = HistorySection.Visibility == Visibility.Visible ? FanHeadroom : 0;
        var cards = Math.Floor(Math.Clamp((spare - headroom) / 2, MinCardHeight, MaxCardHeight));
        if (Math.Abs(cards - _cardHeightTarget) < 1) return;
        _cardHeightTarget = cards;
        BeginAnimation(SettingsCardHeightProperty, IsLoaded ? Motion(SettingsCardHeight, cards) : null);
        if (!IsLoaded) SettingsCardHeight = cards;
    }

    // ───────────── 拖动更换快速操作 ─────────────

    /// <summary>按下鼠标的操作卡片；移动超过拖动距离后才开始拖动。</summary>
    private ActionCardViewModel? _pressedCard;
    private System.Windows.Point _pressPoint;
    private bool _draggingCard;
    private ActionSlotViewModel? _dropTarget;

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

        Canvas.SetLeft(DragGhost, position.X + 12);
        Canvas.SetTop(DragGhost, position.Y + 8);
        SetDropTarget(HitTestSlot(e.GetPosition(ActionSlotsList)), card);
    }

    private void OnActionCardMouseUp(object sender, MouseButtonEventArgs e)
    {
        var card = _pressedCard;
        var target = _dropTarget;
        var dragged = _draggingCard;
        EndCardDrag();
        if (dragged && card is not null && target is not null) ViewModel.AssignSlot(target.Index, card.Action.Id);
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
        DragGhost.Visibility = Visibility.Visible;
        Mouse.Capture(ActionsPage, CaptureMode.SubTree);
    }

    private void EndCardDrag()
    {
        if (_pressedCard is { } card) card.IsDragSource = false;
        SetDropTarget(null, null);
        _pressedCard = null;
        DragGhost.Visibility = Visibility.Collapsed;
        if (!_draggingCard) return;
        _draggingCard = false;
        if (Mouse.Captured == ActionsPage) ActionsPage.ReleaseMouseCapture();
    }

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

    protected override void OnPreviewKeyDown(System.Windows.Input.KeyEventArgs e)
    {
        if (_draggingCard && e.Key == Key.Escape)
        {
            EndCardDrag();
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
