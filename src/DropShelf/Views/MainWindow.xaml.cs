using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
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
            UpdateTableBand();
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
        _flipBorder = new System.Windows.Shapes.Path { Data = _flipEdge, StrokeThickness = SheetBand };
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
        WindowEffects.SetDarkMode(this, ThemeService.IsDark);
        // 在 WindowChrome 之后挂上，先于它处理命中测试。
        System.Windows.Interop.HwndSource.FromHwnd(new System.Windows.Interop.WindowInteropHelper(this).Handle)?.AddHook(OnCaptionMessage);
    }

    // ───────────── 标题栏按钮 ─────────────

    private void OnMinimizeClick(object sender, RoutedEventArgs e) => SystemCommands.MinimizeWindow(this);

    private void OnMaximizeClick(object sender, RoutedEventArgs e) => ToggleMaximize();

    private void OnCaptionCloseClick(object sender, RoutedEventArgs e) => SystemCommands.CloseWindow(this);

    private void ToggleMaximize()
    {
        if (WindowState == WindowState.Maximized) SystemCommands.RestoreWindow(this);
        else SystemCommands.MaximizeWindow(this);
    }

    /// <summary>
    /// 最大化按钮是自己画的，命中测试时报告为系统的最大化按钮，Windows 11 悬停时照常弹出贴靠布局。
    /// 这时鼠标在非客户区，WPF 收不到鼠标事件，悬停、按下的样子和点击都按非客户区消息处理。
    /// </summary>
    private IntPtr OnCaptionMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        var onButton = (int)wParam == NativeMethods.HTMAXBUTTON;
        switch (message)
        {
            case NativeMethods.WM_NCHITTEST when IsOverMaximize(lParam):
                handled = true;
                if (MaximizeButton.Tag is null)
                {
                    MaximizeButton.Tag = "hover";
                    var track = new TRACKMOUSEEVENT
                    {
                        Size = System.Runtime.InteropServices.Marshal.SizeOf<TRACKMOUSEEVENT>(),
                        Flags = NativeMethods.TME_LEAVE | NativeMethods.TME_NONCLIENT, Window = hwnd
                    };
                    NativeMethods.TrackMouseEvent(ref track);
                }
                return NativeMethods.HTMAXBUTTON;
            case NativeMethods.WM_NCLBUTTONDOWN when onButton:
                handled = true;
                MaximizeButton.Tag = "pressed";
                return IntPtr.Zero;
            case NativeMethods.WM_NCLBUTTONUP when onButton:
                handled = true;
                if (Equals(MaximizeButton.Tag, "pressed")) ToggleMaximize();
                MaximizeButton.Tag = "hover";
                return IntPtr.Zero;
            case NativeMethods.WM_NCMOUSEMOVE when !onButton:
            case NativeMethods.WM_NCMOUSELEAVE:
            case NativeMethods.WM_MOUSEMOVE:
                MaximizeButton.Tag = null;
                break;
        }
        return IntPtr.Zero;
    }

    private bool IsOverMaximize(IntPtr lParam)
    {
        if (!MaximizeButton.IsVisible) return false;
        var screen = new System.Windows.Point((short)((long)lParam & 0xFFFF), (short)(((long)lParam >> 16) & 0xFFFF));
        var point = MaximizeButton.PointFromScreen(screen);
        return point.X >= 0 && point.Y >= 0 && point.X < MaximizeButton.ActualWidth && point.Y < MaximizeButton.ActualHeight;
    }

    private void OnThemeChanged(object? sender, EventArgs e)
    {
        CancelFlip();
        WindowEffects.SetDarkMode(this, ThemeService.IsDark);
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

    // ───────────── 卡片翻开成页面 ─────────────

    /// <summary>转到侧面时卡片离屏幕最近，略微变高，背面带一点阴影，看起来像在三维空间里翻转。</summary>
    private const double FlipLift = 1.06;
    private const double FlipShade = 0.22;
    /// <summary>背面就是页面那张牌，圆角和边框与静止时一样，见 PageSheetStyle。</summary>
    private const double SheetRadius = 12;
    /// <summary>设置页四周牌背纹样的宽度，与 PageSheetStyle 的边框一致；翻牌、抽牌途中由背面的轮廓画出这一圈。</summary>
    private const double SheetBand = 10;
    /// <summary>背面上页面缓存的上限；更大时生成缓存本身就会卡顿，背面改为空白。</summary>
    private const double MaxFlipCacheBytes = 16 * 1024 * 1024;
    // 打开：正面翻到侧面，再由背面展开；返回：页面收到侧面，再由正面落位（毫秒）。
    private const double OpenFrontMs = 170, OpenBackMs = 300, BackPageMs = 230, BackFrontMs = 320;
    /// <summary>卡片侧立换面的时刻在整次翻牌中的位置。</summary>
    private const double OpenEdge = OpenFrontMs / (OpenFrontMs + OpenBackMs), BackEdge = BackPageMs / (BackPageMs + BackFrontMs);
    // 打开时概览页在换面后下沉淡出，返回时一开始就从稍低处上移淡入（毫秒）。
    private const double OverviewLeaveMs = 200, OverviewRevealMs = 300;

    // 整次翻牌用一条转动曲线和一条移动曲线从头连到尾，两段动画只是各取一截，侧立换面时速度连续：
    // 不会先停住再突然加速，也不拖很长的尾巴。转过的角度 = 180° × 转动曲线，横向缩放为 |cos 角度|，纵向放大和阴影按 sin 角度变化。
    // 打开：转动从静止起步，侧立前后最快，再慢慢转正；移动比转动晚一点起步，侧立后最快，两者差不多同时停稳。
    private static readonly BezierEase OpenTurn = HalfwayAt(OpenEdge, 0.3);
    private static readonly IEasingFunction OpenTravel = new BezierEase(0.55, 0, 0.25, 1);
    // 返回：页面缩小时逐渐加速，侧立前后最快；卡片落位时略微越过原位（约为全程的 1.8%）再回来，停下时速度为零。
    private static readonly BezierEase BackTurn = HalfwayAt(BackEdge, 0.42);
    private static readonly IEasingFunction BackTravel = new CurveEase(t =>
    {
        var x = -Math.Cos(Math.PI * t / 2);
        return 1 + 1.7 * x * x * x + 0.7 * x * x;
    });
    /// <summary>侧立时已经走过的路程比例；卡片在这里换面。</summary>
    private static readonly double OpenEdgeTravel = OpenTravel.Ease(OpenEdge), BackEdgeTravel = BackTravel.Ease(BackEdge);
    private static readonly IEasingFunction GlideEase = new SineEase { EasingMode = EasingMode.EaseInOut };
    private static readonly IEasingFunction LeaveEase = new CubicEase { EasingMode = EasingMode.EaseIn };

    // 拖动翻牌：拖过与卡片同宽的距离正好翻过半圈，像按着卡片的边把它推过中轴，拖到一半时侧立；距离限制在这个范围内。
    private const double DragTurnMin = 240, DragTurnMax = 480;
    /// <summary>横向移动超过这段距离才开始翻牌；先竖着移动的不算，卡片照常是按钮。</summary>
    private const double DragStart = 8;
    /// <summary>松手时取此前这段时间（毫秒）的平均速度，往前推算 FlingMs：推算的位置过了侧立就翻过去，否则退回。</summary>
    private const double VelocityWindowMs = 80, FlingMs = 150;
    /// <summary>在卡片上只拖动了这么一点（占半圈的比例）就松手，按点击处理。</summary>
    private const double TapTurn = 0.04;

    private string _page = "Overview";
    private bool _morphing;
    /// <summary>每次翻牌的编号；翻牌被打断后，旧动画的回调看到编号变了就不再动页面。</summary>
    private int _flipSession;
    /// <summary>被打断时把页面直接摆到这次翻牌的终点。</summary>
    private Action? _finishFlip;
    private Flip? _flip;
    private bool _flipFrames;
    private string? _prepareCandidate;
    private readonly Dictionary<string, PageView> _pages;

    // 背面的轮廓：页面的裁剪、翻转阴影和概览页上的洞共用同一个圆角矩形和同一个变换，不会彼此错开。
    private readonly MatrixTransform _flipMatrix = new();
    private readonly RectangleGeometry _flipCard = new();
    // 边框线比轮廓向内缩半根线宽，整根线都在裁剪范围内。
    private readonly RectangleGeometry _flipEdge = new();
    private readonly RectangleGeometry _flipBounds = new();
    private readonly GeometryGroup _flipHole;
    private readonly Canvas _flipDecor;
    private readonly System.Windows.Shapes.Path _flipFill, _flipShade, _flipBorder;
    /// <summary>正面转动时卡片本身的变换。</summary>
    private readonly MatrixTransform _tileMatrix = new();

    /// <summary>Sheet 是页面那张牌，内容在牌里的 Scroll 中滚动。</summary>
    private sealed record PageView(Grid Host, Border Sheet, ScrollViewer Scroll, FrameworkElement Content);

    private sealed class CurveEase(Func<double, double> curve) : IEasingFunction
    {
        public double Ease(double t) => curve(t);
    }

    /// <summary>起止都静止、在 edge 处正好过半的曲线：y1 = 0、y2 = 1 时参数 0.5 处 y = 0.5，x = 0.125 + 0.375 × (x1 + x2)。</summary>
    private static BezierEase HalfwayAt(double edge, double x1) => new(x1, 0, (edge - 0.125) / 0.375 - x1, 1);

    /// <summary>
    /// 一次翻牌。打开：卡片转到侧面换成页面，页面再转回正面展开到整页；返回反过来。
    /// 整次翻牌按进度摆放（0 为起点，1 为终点，即平常翻牌的时间比例），转过的角度、位置、大小和概览页的淡入淡出都由进度决定，
    /// 所以能停在任意位置、往回走。点击时进度匀速增加；拖动时跟着鼠标，松手后从当前位置接着走到终点或退回起点。
    /// </summary>
    private sealed class Flip
    {
        public required bool Opening { get; init; }
        public required string Page { get; init; }
        public required PageView View { get; init; }
        public required Controls.PageTile Tile { get; init; }
        /// <summary>其余几张卡片，按牌堆里从近到远排列，以及它们在概览页上的原位。</summary>
        public required Controls.PageTile[] Others { get; init; }
        public required Rect[] OtherHomes { get; init; }
        /// <summary>卡片的原位、页面那张牌的位置和侧立换面时的位置。</summary>
        public required Rect Home { get; init; }
        public required Rect Area { get; init; }
        public required Rect Edge { get; init; }
        public required bool Plain { get; init; }
        /// <summary>拖动开始的翻牌，侧栏标题跟着朝向屏幕的一面切换；点击的翻牌一开始就切到目标页。</summary>
        public required bool Dragged { get; init; }
        public required string Header { get; set; }
        public int Session { get; init; }
        public double Total => Opening ? OpenFrontMs + OpenBackMs : BackPageMs + BackFrontMs;
        public double Progress { get; set; }
        public double? Posed { get; set; }
        /// <summary>当前朝向屏幕的是页面还是卡片。</summary>
        public bool? PageShown { get; set; }
        public bool Dragging { get; set; }
        /// <summary>拖动中转到过的最大比例。</summary>
        public double Peak { get; set; }
        // 不拖动时进度在 RunMs 毫秒内从 RunFrom 走到 RunTo，起止的斜率见 Glide。
        public double RunFrom { get; set; }
        public double RunTo { get; set; }
        public double RunMs { get; set; }
        public double StartSlope { get; set; }
        public double EndSlope { get; set; }
        /// <summary>开始播放的时刻；第一帧按从这里经过的时间摆放，松手后不会先停一帧。</summary>
        public long RunBegun { get; set; }
        public TimeSpan? RunStart { get; set; }
        public TimeSpan Last { get; set; }
    }

    /// <summary>进度为 s 时转过的比例（半圈为 1），随进度单调增加，侧立时正好为 0.5。</summary>
    private static double TurnAt(Flip flip, double s) => (flip.Opening ? OpenTurn : BackTurn).Ease(s);

    private void OpenPage(string page, Controls.PageTile tile)
    {
        if (StartOpen(page, tile, drag: false) is { } flip) Run(flip, 1);
    }

    /// <summary>
    /// 把点开的卡片翻过来：卡片本身转到侧面，换成目标页面作为背面，页面一边转回正面一边从卡片大小展开到整页，落定时就是静止的页面。
    /// 正面一直是卡片本身，转到侧面（宽度为 0）时才换成背面，看不出切换。概览页在这之后下沉淡出，卡片范围内挖空，旧内容不会透过来。
    /// 背面期间页面缓存为可视区大小的位图，每帧只改变它外面的变换和裁剪，不排版也不重画页面。
    /// </summary>
    private Flip? StartOpen(string page, Controls.PageTile tile, bool drag)
    {
        if (_morphing || _sidePanelFade is not null || _page != "Overview" || !_pages.TryGetValue(page, out var view)) return null;
        StartTrace($"{(drag ? "drag " : "")}open {page}");
        var clicked = Stopwatch.GetTimestamp();
        // 页面通常在鼠标移到卡片上时已经排好版；用键盘直接打开时在这里补上。
        PreparePage(view);
        PageStage.UpdateLayout();
        var home = BoundsOf(tile);
        var to = PageArea(view);
        var plain = UsePlainFlip(to);
        var dragged = drag && !plain;
        var others = OthersOf(page);
        if (!dragged) SetPanelHeader(page, animate: true);
        TraceMark(plain ? "prepare plain" : "prepare", clicked);
        // 排版可能触发窗口尺寸变化而打断翻牌，排完再开始计数。
        return BeginSession(new Flip
        {
            Opening = true, Page = page, View = view, Tile = tile, Others = others, OtherHomes = [.. others.Select(BoundsOf)],
            Home = home, Area = to, Edge = Lerp(home, to, OpenEdgeTravel),
            Plain = plain, Dragged = dragged, Header = dragged ? "Overview" : page, Session = ++_flipSession
        });
    }

    private void BackToOverview()
    {
        if (StartBack(drag: false) is { } flip) Run(flip, 1);
    }

    /// <summary>
    /// 返回时反过来：页面带着内容缩小并转到侧面，换成卡片本身转回正面落进原位，落位时略微回弹。
    /// 概览页一开始就在后面上移淡入，卡片范围内挖空。背面空白时先让空白卡片盖住页面，再换回概览页直接播完，不跟手。
    /// </summary>
    private Flip? StartBack(bool drag)
    {
        if (_morphing || _sidePanelFade is not null || _page == "Overview") return null;
        var page = _page;
        if (!_pages.TryGetValue(page, out var view) ||
            PageCards.Children.OfType<Controls.PageTile>().FirstOrDefault(tile => Equals(tile.Tag, page)) is not { } tile)
        {
            SetPanelHeader("Overview", animate: true);
            ShowPage("Overview");
            RevealOverview();
            return null;
        }
        StartTrace($"{(drag ? "drag " : "")}back {page}");
        var from = PageArea(view);
        var plain = UsePlainFlip(from);
        if (!drag || plain) SetPanelHeader("Overview", animate: true);
        if (!plain) return BeginBack(view, tile, from, plain, drag);

        _morphing = true;
        var session = ++_flipSession;
        // 被打断时概览页可能还停在上次淡出后的透明状态，要明确摆回静止状态。
        _finishFlip = () =>
        {
            ClearFlip(view);
            ShowPage("Overview");
            OverviewScroll.ScrollToTop();
            SettleOverview();
            SettleTiles();
        };
        AttachFlip(view, plain);
        PoseBack(from, from, Math.PI);
        var cover = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(120));
        cover.Completed += (_, _) =>
        {
            if (session == _flipSession && BeginBack(view, tile, from, plain, drag: false) is { } flip) Run(flip, 1);
        };
        _flipDecor.BeginAnimation(OpacityProperty, cover);
        return null;
    }

    /// <summary>概览页回到顶部、排好版，按起点摆好再开始；卡片的原位要在概览页静止时量。</summary>
    private Flip? BeginBack(PageView view, Controls.PageTile tile, Rect from, bool plain, bool drag)
    {
        var session = _flipSession;
        var revealed = Stopwatch.GetTimestamp();
        SettleOverview();
        OverviewHost.Visibility = Visibility.Visible;
        OverviewScroll.ScrollToTop();
        PageStage.UpdateLayout();
        TraceMark("overview", revealed);
        if (plain && session != _flipSession) return null;
        var home = BoundsOf(tile);
        var others = OthersOf(_page);
        return BeginSession(new Flip
        {
            Opening = false, Page = _page, View = view, Tile = tile, Others = others, OtherHomes = [.. others.Select(BoundsOf)],
            Home = home, Area = from, Edge = Lerp(from, home, BackEdgeTravel),
            Plain = plain, Dragged = drag, Header = drag ? _page : "Overview", Session = ++_flipSession
        });
    }

    /// <summary>挂上背面，按起点摆好；之后由 Run 播放，或由 DragFlip 跟着鼠标走。</summary>
    private Flip BeginSession(Flip flip)
    {
        _morphing = true;
        _flip = flip;
        flip.Dragging = flip.Dragged;
        _finishFlip = () =>
        {
            // 拖动中被打断时退回起点，其余的直接摆到要去的终点。
            var forward = !flip.Dragging && flip.RunTo >= 1;
            if (flip.Dragging)
            {
                flip.Dragging = false;
                _flipPress = null;
                if (IsMouseCaptured) ReleaseMouseCapture();
            }
            Land(flip, forward, jump: true);
        };
        AttachFlip(flip.View, flip.Plain);
        if (flip.Plain) flip.View.Sheet.Opacity = 0;
        AttachTiles(flip);
        Pose(flip);
        return flip;
    }

    /// <summary>
    /// 从当前进度走到 target（1 为终点，0 为起点）。rate 是起步的速度（每毫秒的进度），默认为匀速播放的速度。
    /// 点击时匀速走完全程。松手时从手上的速度接着走：比匀速快就按平均速度缩短剩下的时间，最快约为平常的三倍；比匀速慢就从这个速度加速。
    /// 到达时回到匀速：翻牌的各条曲线在起点和终点本来就停稳，进度匀速到达时画面也是平稳停下的。
    /// </summary>
    private void Run(Flip flip, double target, double? rate = null)
    {
        flip.Dragging = false;
        var natural = 1 / flip.Total;
        var distance = Math.Abs(target - flip.Progress);
        var speed = Math.Max(0, (rate ?? natural) * Math.Sign(target - flip.Progress));
        var milliseconds = distance / natural;
        if (speed > natural) milliseconds = Math.Max(2 * distance / (speed + natural), milliseconds / 3);
        flip.RunFrom = flip.Progress;
        flip.RunTo = target;
        flip.RunMs = milliseconds;
        flip.StartSlope = distance > 0 ? Math.Min(3, speed * milliseconds / distance) : 1;
        flip.EndSlope = distance > 0 ? natural * milliseconds / distance : 1;
        flip.RunBegun = Stopwatch.GetTimestamp();
        flip.RunStart = null;
        HookFlipFrames();
    }

    /// <summary>从 0 到 1 的三次曲线，起止斜率为 m0、m1（匀速为 1）；斜率都在 0 到 3 之间时单调，不会越过终点。</summary>
    private static double Glide(double u, double m0, double m1) => (((m0 + m1 - 2) * u + 3 - 2 * m0 - m1) * u + m0) * u;

    /// <summary>开始拖动翻牌：tile 不为空时从概览页翻开这张卡片，为空时把当前设置页翻回概览页。背面空白时不跟手，直接播完，返回 false。</summary>
    internal bool BeginFlipDrag(Controls.PageTile? tile)
    {
        var flip = tile is { Tag: string page } ? StartOpen(page, tile, drag: true) : StartBack(drag: true);
        if (flip is null) return false;
        if (flip.Dragging) return true;
        Run(flip, 1);
        return false;
    }

    /// <summary>拖动中摆到转过 turn（半圈为 1）的位置，下一帧画出来。</summary>
    internal void DragFlip(double turn)
    {
        if (_flip is not { Dragging: true } flip) return;
        turn = Math.Clamp(turn, 0, 1);
        flip.Peak = Math.Max(flip.Peak, turn);
        // 由转动曲线反求进度；位置、大小和概览页都按这个进度摆放，和点击时走的是同一条路线。
        flip.Progress = (flip.Opening ? OpenTurn : BackTurn).Inverse(turn);
        HookFlipFrames();
    }

    /// <summary>
    /// 松手：按手上的速度（每毫秒转过的比例）往前推算一小段，过了侧立就翻过去，否则退回；在卡片上几乎没拖动就松手时按点击处理。
    /// 之后从手上的速度接着走，见 Run。cancel 时一律退回。
    /// </summary>
    internal void ReleaseFlip(double velocity, bool cancel = false)
    {
        if (_flip is not { Dragging: true } flip) return;
        var tap = !cancel && flip.Opening && flip.Peak < TapTurn;
        var forward = tap || !cancel && TurnAt(flip, flip.Progress) + velocity * FlingMs >= 0.5;
        // 每毫秒转过的比例换算成每毫秒的进度：除以转动曲线在当前位置的斜率。
        var (low, high) = (Math.Max(0, flip.Progress - 1e-3), Math.Min(1, flip.Progress + 1e-3));
        var slope = (TurnAt(flip, high) - TurnAt(flip, low)) / (high - low);
        Run(flip, forward ? 1 : 0, tap ? null : slope > 0 ? velocity / slope : 0);
    }

    /// <summary>概览页回到顶部、排好版，从稍低处上移淡入。</summary>
    private void RevealOverview()
    {
        OverviewHost.Visibility = Visibility.Visible;
        OverviewScroll.ScrollToTop();
        PageStage.UpdateLayout();
        FadeInPage(OverviewPage, OverviewRevealMs);
    }

    /// <summary>鼠标移到卡片上或卡片获得焦点时，空闲时先把对应页面排好版；快速划过几张卡片时只准备最后一张。</summary>
    private void OnPageTileHover(object sender, RoutedEventArgs e)
    {
        if (sender is not Controls.PageTile { Tag: string page }) return;
        var queued = _prepareCandidate is not null;
        _prepareCandidate = page;
        if (queued) return;
        Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, () =>
        {
            var candidate = _prepareCandidate;
            _prepareCandidate = null;
            if (!_morphing && _page == "Overview" && candidate is not null && _pages.TryGetValue(candidate, out var view)) PreparePage(view);
        });
    }

    /// <summary>页面设为 Hidden：参与排版但不显示，也不挡住概览页。每页滚动区独立，不会改变概览页的滚动范围；一次只准备一页。</summary>
    private void PreparePage(PageView view, bool resetScroll = true)
    {
        foreach (var other in _pages.Values)
            if (other != view && other.Host.Visibility == Visibility.Hidden) other.Host.Visibility = Visibility.Collapsed;
        if (resetScroll) view.Scroll.ScrollToTop();
        if (view.Host.Visibility == Visibility.Collapsed) view.Host.Visibility = Visibility.Hidden;
    }

    /// <summary>软件渲染时缓存和挖空都由处理器完成，可视区很大时生成缓存本身就会卡顿：这两种情况背面保持空白，和以前一样。</summary>
    private bool UsePlainFlip(Rect area) => RenderCapability.Tier >> 16 == 0 || !FitsCache(this, area.Width, PageStage.ActualHeight);

    /// <summary>
    /// 按设备像素估算一张缓存的大小，单张不超过上限。翻牌时最多同时有设置页和概览页两张，合计也就不超过上限的两倍。
    /// 这只是位图本身的大小，WPF 另有内存副本和裁剪用的中间图层，实际占用以实测为准。
    /// </summary>
    private static bool FitsCache(Visual visual, double width, double height)
    {
        var dpi = VisualTreeHelper.GetDpi(visual);
        return width * dpi.DpiScaleX * height * dpi.DpiScaleY * 4 <= MaxFlipCacheBytes;
    }

    /// <summary>
    /// 把页面挂到背面上：页面的滚动区缓存为位图，宿主套上背面的变换和裁剪，边框和阴影盖在页面上面。
    /// 变换和裁剪都在缓存外面，每帧改变它们不会让缓存重建。页面要等换面时才显示，见 ShowSide。
    /// </summary>
    private void AttachFlip(PageView view, bool plain)
    {
        var started = Stopwatch.GetTimestamp();
        if (_flipDecor.Parent is Panel parent) parent.Children.Remove(_flipDecor);
        view.Host.Children.Add(_flipDecor);
        _flipDecor.BeginAnimation(OpacityProperty, null);
        _flipFill.Visibility = plain ? Visibility.Visible : Visibility.Collapsed;
        view.Host.RenderTransform = _flipMatrix;
        view.Host.IsHitTestVisible = false;
        // 滚动条在卡片范围外，翻转时本来就被裁掉；藏起来，缓存也不用包含它。
        if (ScrollBarOf(view.Scroll) is { } bar) bar.Visibility = Visibility.Hidden;
        if (!plain)
        {
            // 牌的边框由背面的轮廓来画：展开途中页面裁得比牌矮，下沿也要有边框。
            view.Sheet.BorderBrush = Brushes.Transparent;
            view.Scroll.CacheMode = new BitmapCache(CacheScale.For(this)) { SnapsToDevicePixels = true };
            view.Host.Clip = _flipCard;
            _flipBounds.Rect = new Rect(-1, -1, OverviewHost.ActualWidth + 2, OverviewHost.ActualHeight + 2);
        }
        TraceMark("attach", started);
    }

    private void HookFlipFrames()
    {
        if (_flipFrames) return;
        _flipFrames = true;
        CompositionTarget.Rendering += OnFlipFrame;
    }

    private void StopFlipFrames()
    {
        _flip = null;
        if (!_flipFrames) return;
        _flipFrames = false;
        CompositionTarget.Rendering -= OnFlipFrame;
    }

    /// <summary>
    /// 播放时按实际经过的时间推进，同一帧重复触发时跳过；拖动时摆到鼠标给出的进度。
    /// 拖动中鼠标不动时不再每帧唤醒界面线程，下次移动时再接上。
    /// </summary>
    private void OnFlipFrame(object? sender, EventArgs e)
    {
        if (_flip is not { } flip)
        {
            StopFlipFrames();
            return;
        }
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (flip.Dragging)
        {
            _flipFrames = false;
            CompositionTarget.Rendering -= OnFlipFrame;
            if (flip.Posed != flip.Progress) Pose(flip);
            return;
        }
        if (flip.RunStart is not null && time == flip.Last) return;
        flip.RunStart ??= time - Stopwatch.GetElapsedTime(flip.RunBegun);
        flip.Last = time;
        var u = flip.RunMs > 0 ? Math.Min(1, (time - flip.RunStart.Value).TotalMilliseconds / flip.RunMs) : 1;
        flip.Progress = flip.RunFrom + (flip.RunTo - flip.RunFrom) * Glide(u, flip.StartSlope, flip.EndSlope);
        Pose(flip);
        if (u >= 1) Land(flip, forward: flip.RunTo >= 1, jump: false);
    }

    /// <summary>按当前进度摆放：侧立前后分别摆卡片本身或背面上的页面，概览页按进度淡入淡出。</summary>
    private void Pose(Flip flip)
    {
        var s = flip.Progress;
        flip.Posed = s;
        var pageSide = flip.Opening ? s >= OpenEdge : s < BackEdge;
        if (flip.PageShown != pageSide) ShowSide(flip, pageSide);
        if (flip.Opening)
        {
            var travel = OpenTravel.Ease(s);
            var angle = Math.PI * OpenTurn.Ease(s);
            if (pageSide) PoseBack(Lerp(flip.Edge, flip.Area, (travel - OpenEdgeTravel) / (1 - OpenEdgeTravel)), flip.Area, angle);
            else PoseTile(flip, travel / OpenEdgeTravel, angle);
            var leave = LeaveEase.Ease(Math.Clamp((s - OpenEdge) * flip.Total / OverviewLeaveMs, 0, 1));
            PoseOverview(1 - leave, 10 * leave, pageSide);
        }
        else
        {
            var travel = BackTravel.Ease(s);
            var angle = Math.PI * (1 - BackTurn.Ease(s));
            if (pageSide) PoseBack(Lerp(flip.Area, flip.Edge, travel / BackEdgeTravel), flip.Area, angle);
            else PoseTile(flip, 1 - (travel - BackEdgeTravel) / (1 - BackEdgeTravel), angle);
            var reveal = MotionEase.Ease(Math.Clamp(s * flip.Total / OverviewRevealMs, 0, 1));
            PoseOverview(reveal, 14 * (1 - reveal), pageSide);
        }
        if (!flip.Plain) PoseEdges(flip);
    }

    /// <summary>
    /// 侧立时换面：朝向屏幕的是页面时，页面显示在背面上，概览页和牌的边都挖出卡片的形状，卡片本身藏起；
    /// 反过来页面藏起，卡片转动时压在其余卡片上面。拖动时侧栏标题跟着切换，往回拖过侧立时再切回来。
    /// </summary>
    private void ShowSide(Flip flip, bool pageSide)
    {
        var swapped = Stopwatch.GetTimestamp();
        flip.PageShown = pageSide;
        flip.View.Host.Visibility = pageSide ? Visibility.Visible : Visibility.Hidden;
        OverviewHost.Clip = StackLayer.Clip = pageSide && !flip.Plain ? _flipHole : null;
        _tileFace.Visibility = pageSide ? Visibility.Hidden : Visibility.Visible;
        var header = pageSide ? flip.Page : "Overview";
        if (flip.Dragged && flip.Header != header)
        {
            flip.Header = header;
            SetPanelHeader(header, animate: true);
        }
        TraceMark(pageSide ? "page" : "tile", swapped);
    }

    /// <summary>卡片本身：从原位走向侧立处的比例为 travel，转过 angle（弧度，0 为正对屏幕，π/2 为侧立）。只改变卡面的变换，不影响排版。</summary>
    private void PoseTile(Flip flip, double travel, double angle)
    {
        var home = flip.Home;
        var card = Lerp(home, flip.Edge, travel);
        var sx = card.Width * Math.Abs(Math.Cos(angle)) / home.Width;
        var sy = card.Height * (1 + (FlipLift - 1) * Math.Sin(angle)) / home.Height;
        _tileMatrix.Matrix = new Matrix(sx, 0, 0, sy, card.X + card.Width / 2 - home.X - sx * home.Width / 2,
            card.Y + card.Height / 2 - home.Y - sy * home.Height / 2);
    }

    /// <summary>
    /// 背面当前的矩形为 card，转过 angle（弧度，侧立为 π/2，背面正对屏幕为 π），绕中心横向缩放 kx、纵向缩放 ky；
    /// 页面按 s = card 宽 / 页面宽 等比缩放后裁成 card 的高度。page 是页面那张牌静止时的位置；展开到 page 且正对屏幕时矩阵正好是单位矩阵。
    /// 圆角和边框跟着页面一起缩放，落定时正好就是那张牌自己的圆角和边框。
    /// </summary>
    private void PoseBack(Rect card, Rect page, double angle)
    {
        var kx = Math.Abs(Math.Cos(angle));
        var ky = 1 + (FlipLift - 1) * Math.Sin(angle);
        var s = card.Width / page.Width;
        var cx = card.X + card.Width / 2;
        var cy = card.Y + card.Height / 2;
        _flipMatrix.Matrix = new Matrix(kx * s, 0, 0, ky * s, cx + kx * (card.X - s * page.X - cx), cy + ky * (card.Y - s * page.Y - cy));
        var height = card.Height / s;
        _flipCard.Rect = new Rect(page.X, page.Y, page.Width, height);
        _flipCard.RadiusX = _flipCard.RadiusY = SheetRadius;
        _flipEdge.Rect = SheetOutline(new Rect(page.X, page.Y, page.Width, height));
        _flipEdge.RadiusX = _flipEdge.RadiusY = SheetRadius - SheetBand / 2;
        _flipShade.Opacity = FlipShade * Math.Sin(angle);
    }

    /// <summary>牌背那一圈的描边路径：描边沿路径两侧各画一半，路径缩进半个宽度，正好画满牌的边缘。</summary>
    private static Rect SheetOutline(Rect card) => new(card.X + SheetBand / 2, card.Y + SheetBand / 2,
        Math.Max(0, card.Width - SheetBand), Math.Max(0, card.Height - SheetBand));

    /// <summary>牌桌铺满可视区的宽度：页面内容有最大宽度且居中，左右各差多少就向外伸出多少，下沿伸到窗口底边。</summary>
    private void UpdateTableBand()
    {
        var width = OverviewScroll.ViewportWidth;
        var content = PageContent.ActualWidth;
        if (width <= 0 || content <= 0) return;
        var inset = PageInset;
        var left = inset.Left + Math.Max(0, (width - inset.Left - inset.Right - content) / 2);
        var margin = new Thickness(-left, 12, -(width - left - content), -inset.Bottom);
        if (TableBand.Margin != margin) TableBand.Margin = margin;
    }

    /// <summary>
    /// 概览页的透明度和下移距离。页面在背面上、概览页半透明时缓存为位图，每帧只叠一张图；
    /// 卡片本身在概览页里转动时不缓存，否则每帧都要重建缓存。
    /// </summary>
    private void PoseOverview(double opacity, double shift, bool pageSide)
    {
        if (OverviewPage.RenderTransform is not TranslateTransform translate) OverviewPage.RenderTransform = translate = new TranslateTransform();
        OverviewPage.Opacity = opacity;
        translate.Y = shift;
        if (pageSide && opacity is > 0 and < 1) CacheWhileFading(OverviewPage);
        else OverviewPage.CacheMode = null;
    }

    private static double Smooth(double x)
    {
        x = Math.Clamp(x, 0, 1);
        return x * x * (3 - 2 * x);
    }

    /// <summary>
    /// 翻牌走到头：页面或概览页回到静止状态。forward 表示走到了终点，否则是退回了起点；
    /// jump 表示被打断，直接摆好，不再播放收尾的动画。
    /// </summary>
    private void Land(Flip flip, bool forward, bool jump)
    {
        var landed = Stopwatch.GetTimestamp();
        StopFlipFrames();
        var view = flip.View;
        var pageShown = flip.Opening == forward;
        var page = pageShown ? flip.Page : "Overview";
        SettleTiles();
        SettleOverview();
        ShowPage(page);
        if (flip.Header != page)
        {
            flip.Header = page;
            SetPanelHeader(page, animate: !jump);
        }
        if (pageShown && flip.Plain && !jump)
        {
            RevealPlainPage(view, flip.Session);
            StackLayer.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180)) { EasingFunction = MotionEase });
        }
        else
        {
            ClearFlip(view);
            if (pageShown && !jump) FadeInScrollBar(view.Scroll);
            EndMorph();
        }
        TraceMark("land", landed);
    }

    /// <summary>页面回到静止状态：去掉变换、裁剪和缓存，拿走边框和阴影，概览页不再挖空。</summary>
    private void ClearFlip(PageView view)
    {
        StopFlipFrames();
        if (_flipDecor.Parent == view.Host) view.Host.Children.Remove(_flipDecor);
        _flipDecor.BeginAnimation(OpacityProperty, null);
        view.Host.RenderTransform = Transform.Identity;
        view.Host.Clip = null;
        view.Host.IsHitTestVisible = true;
        view.Scroll.CacheMode = null;
        view.Scroll.BeginAnimation(OpacityProperty, null);
        view.Scroll.Opacity = 1;
        ScrollBarOf(view.Scroll)?.ClearValue(VisibilityProperty);
        view.Sheet.ClearValue(Border.BorderBrushProperty);
        view.Sheet.Opacity = 1;
        OverviewHost.Clip = StackLayer.Clip = null;
    }

    /// <summary>背面空白时和以前一样：页面落定后淡入，空白卡片同时淡出。</summary>
    private void RevealPlainPage(PageView view, int session)
    {
        view.Sheet.Opacity = 1;
        ScrollBarOf(view.Scroll)?.ClearValue(VisibilityProperty);
        view.Scroll.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180)) { EasingFunction = MotionEase });
        var reveal = new DoubleAnimation(0, TimeSpan.FromMilliseconds(220)) { EasingFunction = GlideEase };
        reveal.Completed += (_, _) =>
        {
            if (session != _flipSession) return;
            ClearFlip(view);
            EndMorph();
        };
        _flipDecor.BeginAnimation(OpacityProperty, reveal);
    }

    private static void FadeInScrollBar(ScrollViewer scroll) =>
        ScrollBarOf(scroll)?.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(150)) { EasingFunction = MotionEase });

    private static ScrollBar? ScrollBarOf(ScrollViewer scroll) => scroll.Template?.FindName("PART_VerticalScrollBar", scroll) as ScrollBar;

    private void EndMorph()
    {
        _finishFlip = null;
        _morphing = false;
        EndTrace("done");
    }

    /// <summary>窗口大小、缩放比例或状态变了，算好的卡片位置已经失效：不再播完，直接摆到这次翻牌的终点。</summary>
    private void CancelFlip()
    {
        if (!_morphing) return;
        EndTrace("cancelled");
        _flipSession++;
        var finish = _finishFlip;
        EndMorph();
        finish?.Invoke();
    }

    // ───────────── 牌堆的边 ─────────────

    // 设置页底下按概览页的顺序露出其余几张牌的边：每条高 EdgeBand，比上面一条两侧各缩进 EdgeInset；
    // 鼠标移上去时这一条再往下露出 EdgeReveal，显示是哪一页，后面的边跟着下移。
    private const double EdgeBand = 6, EdgeInset = 16, EdgeReveal = 20, EdgeRadius = 6;
    /// <summary>概览页卡片的圆角，见 PageTile 的模板。</summary>
    private const double TileRadius = 8;
    /// <summary>收拢时近的边先走，相邻两条错开这么久（毫秒）。</summary>
    private const double EdgeStagger = 30;
    /// <summary>牌面上的字在走完这一段比例前淡出，之后只剩牌的轮廓，拉宽、压扁时不会把字拉变形。</summary>
    private const double EdgeFaceFade = 0.45;

    private sealed record StackEdge(Button Root, Border Frame, Border Face, TextBlock Icon, TextBlock Header,
        FrameworkElement Label, TranslateTransform Shift);

    private readonly StackEdge[] _edges;
    /// <summary>独立绘制模板里的卡面，按钮本身隐藏也不会把卡面一起隐藏。</summary>
    private readonly Border _tileFace = new() { Visibility = Visibility.Hidden, IsHitTestVisible = false };
    /// <summary>上次摆放牌的边时页面那张牌的位置。</summary>
    private Rect _edgeSheet = Rect.Empty;

    private StackEdge CreateEdge(int index)
    {
        var frame = new Border { BorderThickness = new Thickness(1, 0, 1, 1) };
        // 其余几张牌背面朝上，露出的边是牌背的斜格纹，悬停时露出的页名用牌桌上的文字颜色。
        frame.SetResourceReference(Border.BackgroundProperty, "SheetBorderBrush");
        frame.SetResourceReference(Border.BorderBrushProperty, "CardBackLineBrush");
        var face = new Border();
        var icon = new TextBlock { FontSize = 12, Margin = new Thickness(0, 1, 8, 0), VerticalAlignment = VerticalAlignment.Center };
        icon.SetResourceReference(TextBlock.FontFamilyProperty, "IconFont");
        var header = new TextBlock { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
        foreach (var text in new[] { icon, header }) text.SetResourceReference(TextBlock.ForegroundProperty, "TableTextBrush");
        var label = new StackPanel
        {
            Orientation = System.Windows.Controls.Orientation.Horizontal, Height = EdgeReveal, Margin = new Thickness(14, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Bottom, Opacity = 0, Children = { icon, header }
        };
        var shift = new TranslateTransform();
        var root = new Button
        {
            Style = (Style)FindResource("StackEdgeButtonStyle"), ClipToBounds = true, RenderTransform = shift,
            Content = new Grid { Children = { frame, face, label } }
        };
        root.MouseEnter += (_, _) => RevealEdge(index);
        root.MouseLeave += (_, _) => RevealEdge(-1);
        root.GotKeyboardFocus += (_, _) => RevealEdge(index);
        root.LostKeyboardFocus += (_, _) => RevealEdge(-1);
        root.Click += (_, _) => SwitchStackPage(index);
        return new StackEdge(root, frame, face, icon, header, label, shift);
    }

    /// <summary>当前页之外的卡片：按概览页的顺序从当前页往后数，数到头再从第一张接着数，近的是紧挨着的下一页。</summary>
    private Controls.PageTile[] OthersOf(string page)
    {
        var tiles = PageCards.Children.OfType<Controls.PageTile>().ToArray();
        var index = Array.FindIndex(tiles, tile => Equals(tile.Tag, page));
        return [.. Enumerable.Range(1, tiles.Length - 1).Select(offset => tiles[(index + offset) % tiles.Length])];
    }

    private static void AssignEdge(StackEdge edge, Controls.PageTile tile)
    {
        edge.Root.Tag = tile.Tag;
        edge.Root.SetBinding(System.Windows.Automation.AutomationProperties.NameProperty,
            new System.Windows.Data.Binding(nameof(Controls.PageTile.Header)) { Source = tile });
        edge.Icon.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(Controls.PageTile.Icon)) { Source = tile });
        edge.Header.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(Controls.PageTile.Header)) { Source = tile });
    }

    /// <summary>
    /// 模板里的卡面独立画在概览页之外，再隐藏按钮。画刷不包含按钮及其祖先的透明度，
    /// 也不用连续同步生成四张截图；返回时同一张卡面一直画到落位，再交还给按钮。
    /// </summary>
    private void AttachTiles(Flip flip)
    {
        var captured = Stopwatch.GetTimestamp();
        ResetEdgeReveal();
        StackLayer.BeginAnimation(OpacityProperty, null);
        StackLayer.Opacity = 1;
        StackLayer.IsHitTestVisible = false;
        StackLayer.Visibility = Visibility.Visible;
        _tileFace.Background = TileFace(flip.Tile);
        Canvas.SetLeft(_tileFace, flip.Home.X);
        Canvas.SetTop(_tileFace, flip.Home.Y);
        _tileFace.Width = flip.Home.Width;
        _tileFace.Height = flip.Home.Height;
        flip.Tile.Opacity = 0;
        for (var k = 0; k < _edges.Length; k++)
        {
            var edge = _edges[k];
            AssignEdge(edge, flip.Others[k]);
            Panel.SetZIndex(edge.Root, _edges.Length - k);
            // 翻牌途中边的大小每帧都变，页名收起来，不用每帧重新排字。
            edge.Label.Visibility = Visibility.Collapsed;
            edge.Root.Visibility = flip.Plain ? Visibility.Hidden : Visibility.Visible;
            if (flip.Plain) continue;
            edge.Face.Background = TileFace(flip.Others[k]);
            flip.Others[k].Opacity = 0;
        }
        TraceMark("faces", captured);
    }

    private static VisualBrush TileFace(Controls.PageTile tile)
    {
        tile.ApplyTemplate();
        // 画模板的根，而不是马上会被设为透明的 Button。固定 Viewbox，变宽、变矮时整张卡面一起变化。
        var face = (FrameworkElement)VisualTreeHelper.GetChild(tile, 0);
        return new VisualBrush(face)
        {
            Stretch = Stretch.Fill, ViewboxUnits = BrushMappingMode.Absolute,
            Viewbox = new Rect(face.RenderSize), AutoLayoutContent = false
        };
    }

    /// <summary>
    /// 打开时其余卡片在换面后依次离开原位，滑进页面那张牌底下排成牌的边，近的先到，最远的和页面同时落定；
    /// 返回时一开始就从牌底下抽出来，飞回原位，和卡片同时落位，落位时一样略微回弹。被背面挡住的部分由挖空的形状裁掉。
    /// </summary>
    private void PoseEdges(Flip flip)
    {
        var start = flip.Opening ? OpenFrontMs : 0;
        var span = flip.Total - start - EdgeStagger * (_edges.Length - 1);
        for (var k = 0; k < _edges.Length; k++)
        {
            var u = Math.Clamp((flip.Progress * flip.Total - start - EdgeStagger * k) / span, 0, 1);
            PlaceEdge(_edges[k], flip.OtherHomes[k], BandOf(flip.Area, k), flip.Opening ? OpenTravel.Ease(u) : 1 - BackTravel.Ease(u));
        }
    }

    /// <summary>在卡片原位 home 和露出的边 band 之间按 travel 摆放一条边：圆角由卡片的四个角变成只有下面两个，卡面淡出，外框淡入。</summary>
    private static void PlaceEdge(StackEdge edge, Rect home, Rect band, double travel)
    {
        var rect = Lerp(home, band, travel);
        Canvas.SetLeft(edge.Root, rect.X);
        Canvas.SetTop(edge.Root, rect.Y);
        edge.Root.Width = Math.Max(0, rect.Width);
        edge.Root.Height = Math.Max(0, rect.Height);
        var t = Math.Clamp(travel, 0, 1);
        var top = TileRadius * (1 - t);
        var bottom = TileRadius + (EdgeRadius - TileRadius) * t;
        edge.Frame.CornerRadius = new CornerRadius(top, top, bottom, bottom);
        var frame = Smooth(t / EdgeFaceFade);
        edge.Frame.Opacity = frame;
        edge.Face.Opacity = 1 - frame;
    }

    /// <summary>第 k 条边（0 最近）静止时露在页面那张牌 sheet 底下的位置。</summary>
    private static Rect BandOf(Rect sheet, int k)
    {
        var inset = EdgeInset * (k + 1);
        return new Rect(sheet.Left + inset, sheet.Bottom + EdgeBand * k, Math.Max(0, sheet.Width - 2 * inset), EdgeBand);
    }

    /// <summary>翻牌结束：概览页上的卡片恢复显示，解除临时卡面对模板的引用。</summary>
    private void SettleTiles()
    {
        foreach (var tile in PageCards.Children.OfType<UIElement>()) tile.Opacity = 1;
        _tileFace.Background = null;
        _tileFace.Visibility = Visibility.Hidden;
        _tileMatrix.Matrix = Matrix.Identity;
        foreach (var edge in _edges) edge.Face.Background = null;
    }

    /// <summary>设置页静止时露出其余几张牌的边，回到概览页时收起。</summary>
    private void ShowEdges()
    {
        StackLayer.BeginAnimation(OpacityProperty, null);
        StackLayer.Opacity = 1;
        if (_page == "Overview")
        {
            StackLayer.Visibility = Visibility.Collapsed;
            return;
        }
        var others = OthersOf(_page);
        for (var k = 0; k < _edges.Length; k++)
        {
            AssignEdge(_edges[k], others[k]);
            Panel.SetZIndex(_edges[k].Root, _edges.Length - k);
            _edges[k].Label.Visibility = Visibility.Visible;
            _edges[k].Root.Visibility = Visibility.Visible;
        }
        StackLayer.IsHitTestVisible = true;
        StackLayer.Visibility = Visibility.Visible;
        _edgeSheet = Rect.Empty;
        LayoutEdges();
    }

    /// <summary>按页面那张牌现在的位置摆好牌的边，牌没动时什么也不做。</summary>
    private void LayoutEdges()
    {
        if (StackLayer.Visibility != Visibility.Visible || !_pages.TryGetValue(_page, out var view)) return;
        var sheet = PageArea(view);
        if (sheet == _edgeSheet) return;
        _edgeSheet = sheet;
        ResetEdgeReveal();
        for (var k = 0; k < _edges.Length; k++) PlaceEdge(_edges[k], default, BandOf(sheet, k), 1);
    }

    /// <summary>鼠标移到第 index 条边上（-1 为移开）：这一条往下多露出一截，显示是哪一页；后面的边跟着下移，仍然露在它下面。</summary>
    private void RevealEdge(int index)
    {
        if (_morphing || _page == "Overview") return;
        for (var k = 0; k < _edges.Length; k++)
        {
            var edge = _edges[k];
            edge.Root.BeginAnimation(HeightProperty, Reveal(k == index ? EdgeBand + EdgeReveal : EdgeBand));
            edge.Shift.BeginAnimation(TranslateTransform.YProperty, Reveal(index >= 0 && k > index ? EdgeReveal : 0));
            edge.Label.BeginAnimation(OpacityProperty, Reveal(k == index ? 1 : 0));
        }
    }

    private static DoubleAnimation Reveal(double to) => new(to, TimeSpan.FromMilliseconds(180)) { EasingFunction = MotionEase };

    private void ResetEdgeReveal()
    {
        foreach (var edge in _edges)
        {
            edge.Root.BeginAnimation(HeightProperty, null);
            edge.Shift.BeginAnimation(TranslateTransform.YProperty, null);
            edge.Shift.Y = 0;
            edge.Label.BeginAnimation(OpacityProperty, null);
            edge.Label.Opacity = 0;
        }
    }

    // ───────────── 拖动翻牌 ─────────────

    /// <summary>按下鼠标的位置；按在概览页的卡片上时 _pressedTile 是那张卡片，按在设置页的空白处时为空。</summary>
    private System.Windows.Point? _flipPress;
    private Controls.PageTile? _pressedTile;
    private double _dragOrigin, _dragSign, _dragSpan;
    /// <summary>最近各次移动的时刻和转过的比例，松手时用来算速度；最早的一条在 VelocityWindowMs 之前，用来插值出窗口起点的位置。</summary>
    private readonly List<(long Time, double Turn)> _dragSamples = [];

    private void OnFlipPress(object sender, MouseButtonEventArgs e)
    {
        _flipPress = null;
        if (_morphing || sender is Grid host && !IsBlank(e.OriginalSource as DependencyObject, host)) return;
        _pressedTile = sender as Controls.PageTile;
        _flipPress = e.GetPosition(this);
    }

    /// <summary>按在设置页的空白处、文字或设置卡片的底色上，而不是按钮、选项、开关、快捷键框、滚动条或可拖动的操作卡片上。</summary>
    private static bool IsBlank(DependencyObject? source, DependencyObject host)
    {
        for (; source is not null && source != host; source = source is Visual ? VisualTreeHelper.GetParent(source) : LogicalTreeHelper.GetParent(source))
            if (source is Control and not (Controls.SettingsCard or ScrollViewer) ||
                source is FrameworkElement { DataContext: ActionCardViewModel or ActionSlotViewModel }) return false;
        return true;
    }

    protected override void OnPreviewMouseMove(System.Windows.Input.MouseEventArgs e)
    {
        base.OnPreviewMouseMove(e);
        if (_flipPress is not { } press) return;
        if (e.LeftButton != MouseButtonState.Pressed)
        {
            _flipPress = null;
            return;
        }
        var position = e.GetPosition(this);
        if (_flip is not { Dragging: true })
        {
            var delta = position - press;
            if (Math.Abs(delta.X) < DragStart && Math.Abs(delta.Y) < DragStart) return;
            _flipPress = null;
            if (Math.Abs(delta.Y) >= Math.Abs(delta.X)) return;
            // 窗口先拿走鼠标捕获：卡片按钮不再处于按下状态，截下的卡片是平常的样子，松手时也不会再触发点击。
            CaptureMouse();
            var dragging = BeginFlipDrag(_pressedTile);
            if (!dragging || _flip is not { } flip)
            {
                if (IsMouseCaptured) ReleaseMouseCapture();
                return;
            }
            _flipPress = press;
            _dragOrigin = position.X;
            _dragSign = Math.Sign(delta.X);
            _dragSpan = Math.Clamp(flip.Tile.ActualWidth, DragTurnMin, DragTurnMax);
            _dragSamples.Clear();
        }
        var turn = Math.Clamp((position.X - _dragOrigin) * _dragSign / _dragSpan, 0, 1);
        var now = Stopwatch.GetTimestamp();
        _dragSamples.Add((now, turn));
        while (_dragSamples.Count > 2 && Stopwatch.GetElapsedTime(_dragSamples[1].Time, now).TotalMilliseconds > VelocityWindowMs) _dragSamples.RemoveAt(0);
        DragFlip(turn);
    }

    protected override void OnPreviewMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        _flipPress = null;
        if (_flip is { Dragging: true })
        {
            e.Handled = true;
            ReleaseFlip(DragVelocity());
            ReleaseMouseCapture();
        }
        base.OnPreviewMouseLeftButtonUp(e);
    }

    /// <summary>拖动中窗口失去鼠标捕获（如切换了窗口）时退回起点；卡片按钮让出捕获也会冒泡到这里，不算。</summary>
    protected override void OnLostMouseCapture(System.Windows.Input.MouseEventArgs e)
    {
        base.OnLostMouseCapture(e);
        if (!ReferenceEquals(e.OriginalSource, this) || _flip is not { Dragging: true }) return;
        _flipPress = null;
        ReleaseFlip(0, cancel: true);
    }

    /// <summary>
    /// 松手前 VelocityWindowMs 内的平均速度（每毫秒转过的比例）。窗口起点的位置按前后两次移动插值，之后停住的时间也算在内：
    /// 停住超过这段时间再松手时起点和终点都是最后的位置，速度为 0。
    /// </summary>
    private double DragVelocity()
    {
        if (_dragSamples.Count == 0) return 0;
        var now = Stopwatch.GetTimestamp();
        double Age(long time) => Stopwatch.GetElapsedTime(time, now).TotalMilliseconds;
        var span = Math.Min(VelocityWindowMs, Age(_dragSamples[0].Time));
        if (span <= 1) return 0;
        var start = _dragSamples[^1].Turn;
        for (var index = 0; index < _dragSamples.Count; index++)
        {
            var (time, turn) = _dragSamples[index];
            if (Age(time) > span) continue;
            var (before, from) = _dragSamples[Math.Max(0, index - 1)];
            start = Age(before) > Age(time) ? from + (turn - from) * (Age(before) - span) / (Age(before) - Age(time)) : turn;
            break;
        }
        return (_dragSamples[^1].Turn - start) / span;
    }

    /// <summary>诊断日志打开时（DROPSHELF_TRACE=1），从点击到翻牌结束记下每帧间隔，并标出几个同步步骤各用了多久。</summary>
    private sealed class FlipTrace(string name)
    {
        public string Name { get; } = name;
        public long Begun { get; } = Stopwatch.GetTimestamp();
        public StringBuilder Frames { get; } = new();
        public List<double> Intervals { get; } = [];
        public TimeSpan? Last { get; set; }
        public string? Ending { get; set; }
    }

    private FlipTrace? _trace;

    private void StartTrace(string name)
    {
        if (!LogService.TraceEnabled) return;
        if (_trace is null) CompositionTarget.Rendering += OnTraceFrame;
        _trace = new FlipTrace(name);
    }

    /// <summary>first 是点击到第一帧的时间，包括点击时同步准备页面；之后是每帧间隔，结束后再记一帧，即去掉缓存后第一次直接绘制。</summary>
    private void OnTraceFrame(object? sender, EventArgs e)
    {
        if (_trace is not { } trace) return;
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (trace.Last == time) return;
        if (trace.Last is { } last)
        {
            var interval = (time - last).TotalMilliseconds;
            trace.Intervals.Add(interval);
            trace.Frames.Append($" {interval:F0}");
        }
        else trace.Frames.Append($" first={Stopwatch.GetElapsedTime(trace.Begun).TotalMilliseconds:F0}ms:");
        trace.Last = time;
        if (trace.Ending is not { } ending) return;
        _trace = null;
        CompositionTarget.Rendering -= OnTraceFrame;
        var sorted = trace.Intervals.Order().ToList();
        var p95 = sorted.Count > 0 ? sorted[(int)Math.Ceiling(sorted.Count * 0.95) - 1] : 0;
        LogService.Trace($"Flip {trace.Name} {ending} p95={p95:F1}ms max={(sorted.Count > 0 ? sorted[^1] : 0):F1}ms{trace.Frames}");
    }

    /// <summary>在帧间隔里标出一个时刻，以及从 since 到现在同步执行用的时间。</summary>
    private void TraceMark(string label, long since) =>
        _trace?.Frames.Append($" [{label} {Stopwatch.GetElapsedTime(since).TotalMilliseconds:F1}ms]");

    private void EndTrace(string ending)
    {
        if (_trace is { } trace) trace.Ending ??= ending;
    }

    private Rect BoundsOf(FrameworkElement element) => new(element.TranslatePoint(default, PageStage), new Size(element.ActualWidth, element.ActualHeight));

    private static Rect Lerp(Rect from, Rect to, double amount) => new(from.X + (to.X - from.X) * amount, from.Y + (to.Y - from.Y) * amount,
        from.Width + (to.Width - from.Width) * amount, from.Height + (to.Height - from.Height) * amount);

    /// <summary>
    /// 页面那张牌排版后的位置；内容在牌里滚动，牌本身不动。不算变换：翻牌被打断时页面还带着背面的变换，
    /// 状态栏让位时牌和底下的边共用同一个平移，量到的都应是静止时的位置。
    /// </summary>
    private static Rect PageArea(PageView view)
    {
        var offset = VisualTreeHelper.GetOffset(view.Host) + VisualTreeHelper.GetOffset(view.Sheet);
        return new Rect(new System.Windows.Point(offset.X, offset.Y), view.Sheet.RenderSize);
    }

    // ───────────── 状态栏顶部的页面标题 ─────────────

    private static readonly TimeSpan HeaderDuration = TimeSpan.FromMilliseconds(340);
    private static readonly IEasingFunction RollEase = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.35 };
    private static readonly IEasingFunction PopEase = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 };

    /// <summary>
    /// 状态栏显示时，设置页的标题放在状态栏顶部：打开设置页时图标缩小消失，“概览”缩小飞进上方的返回链接，
    /// 页面名称从上往下滚出；返回时页面名称继续向下滚走，“概览”从返回链接放大飞回，图标弹回。
    /// </summary>
    private void SetPanelHeader(string page, bool animate)
    {
        var onOverview = page == "Overview";
        // 从一个设置页抽到另一个设置页：返回链接和图标都不动，只换页名。
        var swap = !onOverview && PanelBackButton.IsHitTestVisible;
        // 绑定到卡片的标题，切换语言后跟着更新。
        if (!onOverview && PageCards.Children.OfType<Controls.PageTile>().FirstOrDefault(tile => Equals(tile.Tag, page)) is { } pageTile)
            PanelPageName.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(Controls.PageTile.Header)) { Source = pageTile });
        PanelBackButton.IsHitTestVisible = !onOverview;
        foreach (var (target, property) in new (Animatable, DependencyProperty)[]
                 {
                     (SidePanelIconScale, ScaleTransform.ScaleXProperty), (SidePanelIconScale, ScaleTransform.ScaleYProperty),
                     (PanelTitleScale, ScaleTransform.ScaleXProperty), (PanelTitleScale, ScaleTransform.ScaleYProperty),
                     (PanelTitleShift, TranslateTransform.XProperty), (PanelTitleShift, TranslateTransform.YProperty),
                     (PanelPageNameShift, TranslateTransform.YProperty)
                 })
            target.BeginAnimation(property, null);
        foreach (var element in new UIElement[] { SidePanelIcon, PanelPageTitle, PanelBackButton, PanelPageName })
            element.BeginAnimation(OpacityProperty, null);

        // 先设好最终状态，动画结束或被清除后停在这里。
        SidePanelIcon.Opacity = onOverview ? 1 : 0;
        PanelBackButton.Opacity = PanelPageName.Opacity = onOverview ? 0 : 1;
        PanelPageTitle.Visibility = _sidePanelShown && onOverview ? Visibility.Visible : Visibility.Collapsed;
        if (!animate || !_sidePanelShown) return;
        if (swap)
        {
            PanelPageNameShift.BeginAnimation(TranslateTransform.YProperty, Header(-40, 0, RollEase, 380));
            PanelPageName.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 200));
            return;
        }

        // “概览”与返回链接里的“概览”之间的位移和缩放，按文字左上角对齐。
        PanelPageTitle.Visibility = Visibility.Visible;
        PanelPageTitle.UpdateLayout();
        var offset = PositionOf(PanelCrumbText) - PositionOf(PanelPageTitle);
        var ratio = PanelCrumbText.FontSize / PanelPageTitle.FontSize;
        var (shiftFrom, shiftTo, scaleFrom, scaleTo) = onOverview ? (offset, default(Vector), ratio, 1.0) : (default(Vector), offset, 1.0, ratio);
        PanelTitleShift.BeginAnimation(TranslateTransform.XProperty, Header(shiftFrom.X, shiftTo.X, MotionEase));
        PanelTitleShift.BeginAnimation(TranslateTransform.YProperty, Header(shiftFrom.Y, shiftTo.Y, MotionEase));
        PanelTitleScale.BeginAnimation(ScaleTransform.ScaleXProperty, Header(scaleFrom, scaleTo, MotionEase));
        PanelTitleScale.BeginAnimation(ScaleTransform.ScaleYProperty, Header(scaleFrom, scaleTo, MotionEase));

        if (onOverview)
        {
            PanelPageNameShift.BeginAnimation(TranslateTransform.YProperty, Header(0, 40, LeaveEase, 200));
            PanelPageName.BeginAnimation(OpacityProperty, Header(1, 0, LeaveEase, 200));
            PanelBackButton.BeginAnimation(OpacityProperty, Header(1, 0, LeaveEase, 120));
            PanelPageTitle.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 140));
            SidePanelIcon.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 200, delay: 120));
            SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleXProperty, Header(0.5, 1, PopEase, 380, delay: 120));
            SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleYProperty, Header(0.5, 1, PopEase, 380, delay: 120));
            return;
        }

        SidePanelIcon.BeginAnimation(OpacityProperty, Header(1, 0, LeaveEase, 180));
        SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleXProperty, Header(1, 0.5, LeaveEase, 180));
        SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleYProperty, Header(1, 0.5, LeaveEase, 180));
        PanelBackButton.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 160, delay: 220));
        PanelPageNameShift.BeginAnimation(TranslateTransform.YProperty, Header(-40, 0, RollEase, 380, delay: 120));
        PanelPageName.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 200, delay: 120));
        // 飞到返回链接处淡出，之后收起并复位，下次返回时从返回链接处飞出。
        var hide = Header(1, 0, MotionEase, 120, delay: 220);
        hide.Completed += (_, _) =>
        {
            if (_page == "Overview") return;
            PanelPageTitle.Visibility = Visibility.Collapsed;
            PanelPageTitle.BeginAnimation(OpacityProperty, null);
            foreach (var property in new[] { TranslateTransform.XProperty, TranslateTransform.YProperty }) PanelTitleShift.BeginAnimation(property, null);
            foreach (var property in new[] { ScaleTransform.ScaleXProperty, ScaleTransform.ScaleYProperty }) PanelTitleScale.BeginAnimation(property, null);
        };
        PanelPageTitle.BeginAnimation(OpacityProperty, hide);
    }

    /// <summary>延迟期间停在起点；用 BeginTime 延迟的动画开始前会显示属性原来的值（即最终状态），出现闪烁。</summary>
    private static AnimationTimeline Header(double from, double to, IEasingFunction ease, double milliseconds = 0, double delay = 0)
    {
        var duration = milliseconds > 0 ? milliseconds : HeaderDuration.TotalMilliseconds;
        return new DoubleAnimationUsingKeyFrames
        {
            KeyFrames =
            {
                new DiscreteDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.Zero)),
                new DiscreteDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(delay))),
                new EasingDoubleKeyFrame(to, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(delay + duration)), ease)
            }
        };
    }

    /// <summary>概览页列出各项设置，点进去显示对应页面，左上角返回。</summary>
    private void ShowPage(string page)
    {
        _page = page;
        OverviewHost.Visibility = page == "Overview" ? Visibility.Visible : Visibility.Collapsed;
        foreach (var (key, view) in _pages) view.Host.Visibility = key == page ? Visibility.Visible : Visibility.Collapsed;
        ShowEdges();
    }

    /// <summary>
    /// 新页面从稍低处上移并淡入；只改变位移和透明度，不影响排版。
    /// 动画期间页面缓存为位图，每帧只叠一张图，不用重画整页内容；结束后去掉缓存。
    /// </summary>
    private static void FadeInPage(FrameworkElement page, double milliseconds)
    {
        if (page.RenderTransform is not TranslateTransform shift) page.RenderTransform = shift = new TranslateTransform();
        CacheWhileFading(page);
        var duration = TimeSpan.FromMilliseconds(milliseconds);
        shift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(14, 0, duration) { EasingFunction = MotionEase });
        var fade = new DoubleAnimation(0, 1, duration) { EasingFunction = MotionEase };
        fade.Completed += (_, _) => page.CacheMode = null;
        page.BeginAnimation(OpacityProperty, fade);
    }

    /// <summary>概览页平时正好撑满可视区；窗口很大、缩放比例很高时缓存超出上限，就直接绘制，不再额外占一大块内存。</summary>
    private static void CacheWhileFading(FrameworkElement page)
    {
        if (page.CacheMode is null && FitsCache(page, page.ActualWidth, page.ActualHeight)) page.CacheMode = new BitmapCache(CacheScale.For(page)) { SnapsToDevicePixels = true };
    }

    /// <summary>概览页停在静止状态：去掉淡入淡出的动画和缓存。</summary>
    private void SettleOverview()
    {
        OverviewPage.BeginAnimation(OpacityProperty, null);
        OverviewPage.Opacity = 1;
        if (OverviewPage.RenderTransform is TranslateTransform shift)
        {
            shift.BeginAnimation(TranslateTransform.YProperty, null);
            shift.Y = 0;
        }
        OverviewPage.CacheMode = null;
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

    /// <summary>页面内容的页边距，所有页面共用；状态栏出现时左边距加上状态栏的宽度。</summary>
    public static readonly DependencyProperty PageInsetProperty = DependencyProperty.Register(nameof(PageInset), typeof(Thickness), typeof(MainWindow),
        new PropertyMetadata(new Thickness(PageMargin, 26, PageMargin, PageMargin)));

    public Thickness PageInset
    {
        get => (Thickness)GetValue(PageInsetProperty);
        set => SetValue(PageInsetProperty, value);
    }

    // 设置页那张牌比页面内容两侧各宽出 SheetPadding（牌里的留白，见 SheetContentStyle），离顶部 SheetTop，底下留 StackRoom 给牌的边和鼠标移上去时露出的部分。
    private const double SheetPadding = 24, SheetTop = 14, StackRoom = 64;

    private static Thickness SheetInsetFor(Thickness page) => new(page.Left - SheetPadding, SheetTop, page.Right - SheetPadding, StackRoom);

    public static readonly DependencyProperty SheetInsetProperty = DependencyProperty.Register(nameof(SheetInset), typeof(Thickness), typeof(MainWindow),
        new PropertyMetadata(SheetInsetFor(new Thickness(PageMargin, 26, PageMargin, PageMargin))));

    public Thickness SheetInset
    {
        get => (Thickness)GetValue(SheetInsetProperty);
        set => SetValue(SheetInsetProperty, value);
    }

    private FrameworkElement CurrentContent => _pages.TryGetValue(_page, out var view) ? view.Content : PageContent;
    private bool _sidePanelShown;
    private bool _sidePanelPending;
    /// <summary>状态栏出现或收起期间页面还在平移：这时不开始翻牌，否则量到的是过渡中的位置，缓存也会每帧重建。</summary>
    private DoubleAnimation? _sidePanelFade;

    private void OnContentLayerSizeChanged(object sender, SizeChangedEventArgs e)
    {
        CancelFlip();
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
        CancelFlip();
        _sidePanelShown = show;

        var onOverview = _page == "Overview";
        var flyingTitle = show ? PanelPageTitle : OverviewTitle;
        var titleShift = show ? PanelTitleShift : OverviewTitleShift;
        var pageBefore = PositionOf(CurrentContent);
        var bodyBefore = PositionOf(OverviewBody);
        var titleBefore = PositionOf(show ? OverviewTitle : PanelPageTitle);
        var buttonSnapshot = animate && show && onOverview ? Snapshot(OverviewOpenButton) : null;
        var panelWasHidden = SidePanel.Visibility != Visibility.Visible;
        var panelOpacity = panelWasHidden ? 0 : SidePanel.Opacity;
        var panelOffset = panelWasHidden ? -24 : SidePanelShift.X;

        PageInset = new Thickness(show ? PageMargin + SidePanelWidth : PageMargin, 26, PageMargin, PageMargin);
        SheetInset = SheetInsetFor(PageInset);
        OverviewHeader.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        // 设置页的标题和返回按钮移到状态栏顶部。
        foreach (var page in new[] { GeneralPage, ShelfPage, ActionsPage, AppearancePage })
            page.Children[0].Visibility = show ? Visibility.Collapsed : Visibility.Visible;
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

        _sidePanelFade = null;
        if (!animate)
        {
            OnSidePanelSettled();
            return;
        }

        ContentLayer.UpdateLayout();
        var pageOffset = pageBefore.X - PositionOf(CurrentContent).X;
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
        SidePanel.CacheMode ??= new BitmapCache(CacheScale.For(SidePanel)) { SnapsToDevicePixels = true };
        var fade = Motion(panelOpacity, show ? 1 : 0);
        fade.Completed += (_, _) =>
        {
            if (ReferenceEquals(_sidePanelFade, fade)) _sidePanelFade = null;
            OnSidePanelSettled();
        };
        _sidePanelFade = fade;
        SidePanel.BeginAnimation(OpacityProperty, fade);
    }

    private System.Windows.Point PositionOf(UIElement element) => element.TranslatePoint(default, ContentLayer);

    private static DoubleAnimation Motion(double from, double to) =>
        new(from, to, MotionDuration) { EasingFunction = MotionEase };

    /// <summary>动画结束后去掉位图缓存；收起的状态栏不再参与排版。</summary>
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
        var image = new System.Windows.Controls.Image { Source = VisualCapture.Of(element), Width = element.ActualWidth, Height = element.ActualHeight, RenderTransform = new TranslateTransform() };
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
        _ghost.Layer = () => !_tornOff || HistoryReturnWeight(CursorPoint()) > 0 ? DragLayer : null;
        _ghost.Begin(image, new Size(card.ActualWidth, card.ActualHeight), start, () => HistoryDragPose(start, container));
    }

    /// <summary>
    /// 拖动分三段：先被牌堆吸住、只跟一小段；拉过一定距离后脱离并略微放大，顿一下跟上光标；
    /// 脱离后其余的牌并拢；拖回牌堆上方时按卡牌位置让出空位，越靠近牌堆，卡牌被空位吸得越紧、越接近原来的大小，提示松手会放到这里。
    /// </summary>
    private CardPose HistoryDragPose(CardPose start, FrameworkElement container)
    {
        // 松手的消息没送到时（如捕获被打断）卡牌会一直粘在光标上：按键实际已经松开就照常放下。
        if (!NativeMethods.IsButtonPhysicallyDown()) Dispatcher.BeginInvoke(() => EndHistoryDrag(cancel: false));
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
            var original = _historyIndex;
            var index = !cancel && _tornOff && panel?.Gap is >= 0 and var gap ? gap : original;
            panel?.SetGap(container, index);
            // 放回的牌立即参与选中，光标下的牌（通常就是它）直接抽出，卡牌落进抽出的位置。
            panel?.UpdateHot();
            ghost.Layer = () => DragLayer;
            ghost.Settle(() => PoseOf(container), () =>
            {
                container.Opacity = 1;
                if (index == original)
                {
                    panel?.SetGap(null, -1);
                    return;
                }
                // 牌堆保持放回后的排法，等卡牌落定、画面静止后再调整列表顺序；卡片沿用原来的，只换位置，记录在后台保存。
                Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, () => _controller.History.Move(entry.Entry, index));
            });
            return;
        }
        ghost.Layer = () => null;
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
