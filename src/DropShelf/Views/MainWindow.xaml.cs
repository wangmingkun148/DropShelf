using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
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
        PageScroll.ScrollToTop();
    }

    private void OnVersionClick(object sender, RoutedEventArgs e) => new AboutWindow(ViewModel) { Owner = this }.ShowDialog();

    // ───────────── 左侧状态栏 ─────────────

    // 内容区宽到放得下状态栏和完整页面时显示；两个阈值之间不切换，拖动窗口边缘时不会反复出现、消失。
    private const double SidePanelShowWidth = 1240;
    private const double SidePanelHideWidth = 1200;
    private const double SidePanelSpace = 292;
    private static readonly Duration SidePanelDuration = TimeSpan.FromMilliseconds(320);
    private static readonly IEasingFunction SidePanelEase = new CubicEase { EasingMode = EasingMode.EaseOut };
    private bool _sidePanelShown;

    private void OnContentLayerSizeChanged(object sender, SizeChangedEventArgs e)
    {
        var width = e.NewSize.Width;
        if (_sidePanelShown ? width < SidePanelHideWidth : width >= SidePanelShowWidth) SetSidePanel(!_sidePanelShown, IsLoaded);
    }

    /// <summary>
    /// 页面只重新排版一次，之后的动画全部是位移和透明度：页面从原位置滑到新位置，状态栏从左侧滑入或滑出。
    /// 动画期间状态栏缓存为位图，每帧只需合成，不必重新绘制。
    /// </summary>
    private void SetSidePanel(bool show, bool animate)
    {
        _sidePanelShown = show;
        var oldLeft = PageContent.Margin.Left;
        var newLeft = show ? PageMargin + SidePanelSpace : PageMargin;
        PageContent.Margin = new Thickness(newLeft, 26, PageMargin, 20);
        var wasCollapsed = SidePanel.Visibility == Visibility.Collapsed;
        SidePanel.Visibility = Visibility.Visible;
        SidePanel.IsHitTestVisible = show;
        OverviewOpenButton.Visibility = Visibility.Visible;
        OverviewOpenButton.IsHitTestVisible = !show;

        if (!animate)
        {
            PageContentShift.BeginAnimation(TranslateTransform.XProperty, null);
            SidePanelShift.BeginAnimation(TranslateTransform.XProperty, null);
            SidePanel.BeginAnimation(OpacityProperty, null);
            OverviewOpenButton.BeginAnimation(OpacityProperty, null);
            OnSidePanelSettled();
            return;
        }

        // 页面在新位置排版后，先反向平移回原来看到的位置，再滑过去。
        var viewport = PageScroll.ViewportWidth > 0 ? PageScroll.ViewportWidth : PageScroll.ActualWidth;
        var moved = PageLeft(viewport, newLeft) - PageLeft(viewport, oldLeft);
        PageContentShift.BeginAnimation(TranslateTransform.XProperty, Animate(PageContentShift.X - moved, 0));
        SidePanelShift.BeginAnimation(TranslateTransform.XProperty, Animate(wasCollapsed ? -24 : null, show ? 0 : -24));
        OverviewOpenButton.BeginAnimation(OpacityProperty, Animate(null, show ? 0 : 1));
        SidePanel.CacheMode ??= new BitmapCache { SnapsToDevicePixels = true };
        var fade = Animate(wasCollapsed ? 0 : null, show ? 1 : 0);
        fade.Completed += (_, _) => OnSidePanelSettled();
        SidePanel.BeginAnimation(OpacityProperty, fade);
    }

    private const double PageMargin = 36;
    private const double PageMaxWidth = 860;

    /// <summary>页面最宽 860，内容区更宽时在左右边距之间居中。</summary>
    private static double PageLeft(double viewport, double left) => left + Math.Max(0, (viewport - left - PageMargin - PageMaxWidth) / 2);

    private static DoubleAnimation Animate(double? from, double to) =>
        new() { From = from, To = to, Duration = SidePanelDuration, EasingFunction = SidePanelEase };

    /// <summary>动画结束后去掉位图缓存，文字恢复清晰的 ClearType；隐藏的元素不再参与排版。</summary>
    private void OnSidePanelSettled()
    {
        if (SidePanel.Opacity != (_sidePanelShown ? 1 : 0) && SidePanel.HasAnimatedProperties) return;
        SidePanel.CacheMode = null;
        if (_sidePanelShown) OverviewOpenButton.Visibility = Visibility.Hidden;
        else SidePanel.Visibility = Visibility.Collapsed;
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
