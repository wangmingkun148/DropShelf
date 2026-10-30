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
        SetupSensitivityLabels();
        DataContext = new SettingsViewModel(controller);
        ApplySavedBounds(controller.Settings);

        SourceInitialized += OnSourceInitialized;
        StateChanged += (_, _) =>
        {
            if (WindowState == WindowState.Minimized) CancelFlip();
            UpdateMaximizedPadding();
        };
        LayoutUpdated += (_, _) =>
        {
            UpdateCardHeight(OverviewScroll.ViewportHeight);
            if (!_morphing) LayoutEdges();
        };
        ThemeService.ThemeChanged += OnThemeChanged;

        _pages = new()
        {
            ["General"] = new(GeneralHost, GeneralSheet, GeneralScroll, GeneralPage),
            ["Shelf"] = new(ShelfHost, ShelfSheet, ShelfScroll, ShelfPage),
            ["Actions"] = new(ActionsHost, ActionsSheet, ActionsScroll, ActionsPage),
            ["Appearance"] = new(AppearanceHost, AppearanceSheet, AppearanceScroll, AppearancePage)
        };
        // 状态栏出现时所有页面连同底下牌的边一起横向让出位置。
        foreach (var view in _pages.Values) view.Sheet.RenderTransform = PageContentShift;
        StackLayer.RenderTransform = PageContentShift;
        foreach (var tile in PageCards.Children.OfType<Controls.PageTile>())
        {
            tile.MouseEnter += OnPageTileHover;
            tile.GotKeyboardFocus += OnPageTileHover;
            tile.PreviewMouseLeftButtonDown += OnFlipPress;
        }
        // 按住卡片或设置页的空白处横向拖动可以翻牌。
        foreach (var view in _pages.Values) view.Host.PreviewMouseLeftButtonDown += OnFlipPress;

        _flipHole = new GeometryGroup { FillRule = FillRule.EvenOdd, Children = { _flipBounds, new GeometryGroup { Transform = _flipMatrix, Children = { _flipCard } } } };
        _flipFill = new System.Windows.Shapes.Path { Data = _flipCard };
        _flipFill.SetResourceReference(System.Windows.Shapes.Shape.FillProperty, "SheetBrush");
        _flipShade = new System.Windows.Shapes.Path { Data = _flipCard, Fill = System.Windows.Media.Brushes.Black };
        _flipBorder = new System.Windows.Shapes.Path { Data = _flipEdge };
        _flipBorder.SetResourceReference(System.Windows.Shapes.Shape.StrokeProperty, "SheetBorderBrush");
        // 放在画布里：图形每帧变化时只重新测量它们自己，不影响页面排版。
        _flipDecor = new Canvas { IsHitTestVisible = false, Children = { _flipFill, _flipShade, _flipBorder } };

        // 近的边画在上面；转动中的卡片画在所有边上面。
        _edges = [.. Enumerable.Range(0, PageCards.Children.Count - 1).Select(CreateEdge)];
        foreach (var edge in Enumerable.Reverse(_edges)) StackLayer.Children.Add(edge.Root);
        _tileFace.RenderTransform = _tileMatrix;
        Panel.SetZIndex(_tileFace, _edges.Length + 1);
        StackLayer.Children.Add(_tileFace);
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        if (WindowEffects.ApplyTheme(this, SystemBackdrop.Mica, ThemeService.IsDark))
            Background = System.Windows.Media.Brushes.Transparent;
        else
            WindowEffects.ShowOwnCaptionButtons(this, CaptionButtons);
    }

    private void OnMinimizeClick(object sender, RoutedEventArgs e) => SystemCommands.MinimizeWindow(this);

    private void OnMaximizeClick(object sender, RoutedEventArgs e)
    {
        if (WindowState == WindowState.Maximized) SystemCommands.RestoreWindow(this);
        else SystemCommands.MaximizeWindow(this);
    }

    private void OnCaptionCloseClick(object sender, RoutedEventArgs e) => SystemCommands.CloseWindow(this);

    private void OnThemeChanged(object? sender, EventArgs e)
    {
        CancelFlip();
        WindowEffects.ApplyTheme(this, SystemBackdrop.Mica, ThemeService.IsDark);
    }

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

    /// <summary>从有窗口的程序中选择，或浏览选择程序文件。</summary>
    private void OnAddBlockedAppClick(object sender, RoutedEventArgs e)
    {
        var menu = new ContextMenu { PlacementTarget = AddBlockedAppButton, Placement = PlacementMode.Bottom };
        foreach (var app in ForegroundAppWatcher.GetWindowedApps().Except(ViewModel.BlockedApps, StringComparer.OrdinalIgnoreCase))
        {
            // 下划线在菜单里表示快捷键，需要写两个。
            var item = new MenuItem { Header = app.Replace("_", "__") };
            item.Click += (_, _) => ViewModel.AddBlockedApp(app);
            menu.Items.Add(item);
        }
        if (menu.Items.Count > 0) menu.Items.Add(new Separator());
        var browse = new MenuItem { Header = Strings.BrowseApp };
        browse.Click += (_, _) =>
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = Strings.ChooseProgram, Filter = Strings.AppFilter, DereferenceLinks = true };
            if (dialog.ShowDialog(this) == true) ViewModel.AddBlockedApp(Path.GetFileName(dialog.FileName));
        };
        menu.Items.Add(browse);
        menu.IsOpen = true;
    }

    // ───────────── 灵敏度滑块下的三个字 ─────────────

    /// <summary>三个字按这个值放大缩小：拖动时紧跟滑钮，点击轨道或按方向键让滑钮跳过去时用一小段动画跟上。</summary>
    private static readonly DependencyProperty SensitivityLabelValueProperty = DependencyProperty.Register(
        "SensitivityLabelValue", typeof(double), typeof(MainWindow),
        new PropertyMetadata(0.0, (owner, args) => ((MainWindow)owner).PoseSensitivityLabels((double)args.NewValue)));

    private static readonly IEasingFunction LabelEase = new CubicEase { EasingMode = EasingMode.EaseOut };
    private readonly ScaleTransform _steadyScale = new(), _balancedScale = new(), _responsiveScale = new();

    private void SetupSensitivityLabels()
    {
        SteadyLabel.RenderTransform = _steadyScale;
        BalancedLabel.RenderTransform = _balancedScale;
        ResponsiveLabel.RenderTransform = _responsiveScale;
        PoseSensitivityLabels(SensitivitySlider.Value);
        SensitivitySlider.ValueChanged += (_, e) =>
        {
            if (SensitivitySlider.IsMouseCaptureWithin || !SensitivitySlider.IsLoaded)
            {
                BeginAnimation(SensitivityLabelValueProperty, null);
                SetValue(SensitivityLabelValueProperty, e.NewValue);
            }
            else
                BeginAnimation(SensitivityLabelValueProperty, new DoubleAnimation(e.NewValue, TimeSpan.FromMilliseconds(220)) { EasingFunction = LabelEase });
        };
    }

    /// <summary>
    /// 像 macOS 程序坞：离滑钮 50 以内的字按余弦曲线放大、变清楚，相邻两个字此消彼长，合起来正好一份；
    /// 更远的那个保持最小。只改缩放和透明度，不重新排版。
    /// </summary>
    private void PoseSensitivityLabels(double value)
    {
        Pose(SteadyLabel, _steadyScale, value - Models.ShakeSensitivities.Steady);
        Pose(BalancedLabel, _balancedScale, value - Models.ShakeSensitivities.Balanced);
        Pose(ResponsiveLabel, _responsiveScale, value - Models.ShakeSensitivities.Responsive);

        static void Pose(UIElement label, ScaleTransform scale, double distance)
        {
            var weight = Math.Abs(distance) >= 50 ? 0 : 0.5 + 0.5 * Math.Cos(Math.PI * distance / 50);
            scale.ScaleX = scale.ScaleY = 0.85 + 0.35 * weight;
            label.Opacity = 0.5 + 0.5 * weight;
        }
    }

    private void OnPageCardClick(object sender, RoutedEventArgs e)
    {
        if (sender is Controls.PageTile { Tag: string page } tile) OpenPage(page, tile);
    }

    private void OnBackClick(object sender, RoutedEventArgs e) => BackToOverview();

    private void OnVersionClick(object sender, RoutedEventArgs e) => new AboutWindow(ViewModel) { Owner = this }.ShowDialog();

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
        if (OverviewHost.Visibility != Visibility.Visible || OverviewPage.DesiredSize.Height <= 0 || viewport <= 0) return;
        var resized = Math.Abs(viewport - _viewportHeight) >= 1;
        _viewportHeight = viewport;
        // 用测量高度减去卡片区域本身，结果与卡片当前高度无关，动画和像素取整都不会反过来改变目标。
        var rest = OverviewPage.DesiredSize.Height - PageCards.DesiredSize.Height;
        var spare = viewport - PageInset.Top - PageInset.Bottom - rest;
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

    // 翻牌期间页面跟着卡片变形，点击、滚轮和按键都先不处理；Alt+F4 等系统按键照常，拖动翻牌时按 Esc 退回起点。
    protected override void OnPreviewMouseDown(MouseButtonEventArgs e)
    {
        if (_morphing) e.Handled = true;
        base.OnPreviewMouseDown(e);
    }

    protected override void OnPreviewMouseWheel(MouseWheelEventArgs e)
    {
        if (_morphing) e.Handled = true;
        base.OnPreviewMouseWheel(e);
    }

    protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
    {
        CancelFlip();
        base.OnDpiChanged(oldDpi, newDpi);
    }

    protected override void OnPreviewKeyDown(System.Windows.Input.KeyEventArgs e)
    {
        if (_flip is { Dragging: true } && e.Key == Key.Escape)
        {
            _flipPress = null;
            ReleaseFlip(0, cancel: true);
            ReleaseMouseCapture();
        }
        if (_morphing && e.Key != Key.System) e.Handled = true;
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

    // 设置页里 Esc、Alt+← 和鼠标后退键都回到概览；录快捷键、下拉框等先处理过的按键不算。
    protected override void OnKeyDown(System.Windows.Input.KeyEventArgs e)
    {
        base.OnKeyDown(e);
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        var modifiers = Keyboard.Modifiers;
        if (e.Handled || !CanGoBack || !(key == Key.Escape && modifiers == ModifierKeys.None ||
            key == Key.Left && modifiers == ModifierKeys.Alt || key == Key.BrowserBack)) return;
        BackToOverview();
        e.Handled = true;
    }

    protected override void OnMouseDown(MouseButtonEventArgs e)
    {
        base.OnMouseDown(e);
        if (e.Handled || e.ChangedButton != MouseButton.XButton1 || !CanGoBack) return;
        BackToOverview();
        e.Handled = true;
    }

    // 对话框开着时 Esc 留给它的取消按钮。
    private bool CanGoBack => _page != "Overview" && !ViewModel.IsEditorOpen && _pendingDelete is null && !_draggingCard && !_draggingHistory;

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
        CancelFlip();
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
