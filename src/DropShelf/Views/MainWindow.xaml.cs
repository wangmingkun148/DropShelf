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
        DataContext = new SettingsViewModel(controller);
        ApplySavedBounds(controller.Settings);

        SourceInitialized += OnSourceInitialized;
        StateChanged += (_, _) =>
        {
            if (WindowState == WindowState.Minimized) CancelFlip();
            UpdateMaximizedPadding();
        };
        LayoutUpdated += (_, _) => UpdateCardHeight(OverviewScroll.ViewportHeight);
        ThemeService.ThemeChanged += OnThemeChanged;

        _pages = new()
        {
            ["General"] = new(GeneralHost, GeneralScroll, GeneralPage),
            ["Shelf"] = new(ShelfHost, ShelfScroll, ShelfPage),
            ["Actions"] = new(ActionsHost, ActionsScroll, ActionsPage),
            ["Appearance"] = new(AppearanceHost, AppearanceScroll, AppearancePage)
        };
        // 状态栏出现时所有页面一起横向让出位置。
        foreach (var view in _pages.Values) view.Content.RenderTransform = PageContentShift;
        foreach (var tile in PageCards.Children.OfType<Controls.PageTile>())
        {
            tile.MouseEnter += OnPageTileHover;
            tile.GotKeyboardFocus += OnPageTileHover;
        }

        _flipHole = new GeometryGroup { FillRule = FillRule.EvenOdd, Children = { _flipBounds, new GeometryGroup { Transform = _flipMatrix, Children = { _flipCard } } } };
        _flipFill = new System.Windows.Shapes.Path { Data = _flipCard };
        _flipFill.SetResourceReference(System.Windows.Shapes.Shape.FillProperty, "CardBrush");
        _flipShade = new System.Windows.Shapes.Path { Data = _flipCard, Fill = System.Windows.Media.Brushes.Black };
        _flipBorder = new System.Windows.Shapes.Path { Data = _flipEdge };
        _flipBorder.SetResourceReference(System.Windows.Shapes.Shape.StrokeProperty, "CardBorderBrush");
        // 放在画布里：图形每帧变化时只重新测量它们自己，不影响页面排版。
        _flipDecor = new Canvas { IsHitTestVisible = false, Children = { _flipFill, _flipShade, _flipBorder } };
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        if (WindowEffects.ApplyTheme(this, SystemBackdrop.Mica, ThemeService.IsDark))
            Background = System.Windows.Media.Brushes.Transparent;
    }

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

    private void OnPageCardClick(object sender, RoutedEventArgs e)
    {
        if (sender is Controls.PageTile { Tag: string page } tile) OpenPage(page, tile);
    }

    private void OnBackClick(object sender, RoutedEventArgs e) => BackToOverview();

    // ───────────── 卡片翻开成页面 ─────────────

    /// <summary>转到侧面时卡片离屏幕最近，略微变高，背面带一点阴影，看起来像在三维空间里翻转。</summary>
    private const double FlipLift = 1.06;
    private const double FlipShade = 0.22;
    private const double CardRadius = 8;
    /// <summary>背面的边框和圆角在落定前这段时间（毫秒）内收掉，落定时背面已经和静止的页面一样。</summary>
    private const double OutlineFade = 70;
    /// <summary>背面上页面缓存的上限；更大时生成缓存本身就会卡顿，背面改为空白。</summary>
    private const double MaxFlipCacheBytes = 16 * 1024 * 1024;
    // 打开：正面翻到侧面，再由背面展开；返回：页面收到侧面，再由正面落位（毫秒）。
    private const double OpenFrontMs = 170, OpenBackMs = 300, BackPageMs = 230, BackFrontMs = 320;
    /// <summary>卡片侧立换面的时刻在整次翻牌中的位置。</summary>
    private const double OpenEdge = OpenFrontMs / (OpenFrontMs + OpenBackMs), BackEdge = BackPageMs / (BackPageMs + BackFrontMs);

    // 整次翻牌用一条转动曲线和一条移动曲线从头连到尾，两段动画只是各取一截，侧立换面时速度连续：
    // 不会先停住再突然加速，也不拖很长的尾巴。转过的角度 = 180° × 转动曲线，横向缩放为 |cos 角度|，纵向放大和阴影按 sin 角度变化。
    // 打开：转动从静止起步，侧立前后最快，再慢慢转正；移动比转动晚一点起步，侧立后最快，两者差不多同时停稳。
    private static readonly IEasingFunction OpenTurn = HalfwayAt(OpenEdge, 0.3);
    private static readonly IEasingFunction OpenTravel = new BezierEase(0.55, 0, 0.25, 1);
    // 返回：页面缩小时逐渐加速，侧立前后最快；卡片落位时略微越过原位（约为全程的 1.8%）再回来，停下时速度为零。
    private static readonly IEasingFunction BackTurn = HalfwayAt(BackEdge, 0.42);
    private static readonly IEasingFunction BackTravel = new CurveEase(t =>
    {
        var x = -Math.Cos(Math.PI * t / 2);
        return 1 + 1.7 * x * x * x + 0.7 * x * x;
    });
    /// <summary>侧立时已经走过的路程比例；卡片在这里换面。</summary>
    private static readonly double OpenEdgeTravel = OpenTravel.Ease(OpenEdge), BackEdgeTravel = BackTravel.Ease(BackEdge);
    private static readonly IEasingFunction GlideEase = new SineEase { EasingMode = EasingMode.EaseInOut };
    private static readonly IEasingFunction LeaveEase = new CubicEase { EasingMode = EasingMode.EaseIn };
    private string _page = "Overview";
    private bool _morphing;
    /// <summary>每次翻牌的编号；翻牌被打断后，旧动画的回调看到编号变了就不再动页面。</summary>
    private int _flipSession;
    /// <summary>被打断时把页面直接摆到这次翻牌的终点。</summary>
    private Action? _finishFlip;
    private PageFlip? _flip;
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

    private sealed record PageView(Grid Host, ScrollViewer Scroll, FrameworkElement Content);

    /// <summary>与 CSS 的 cubic-bezier 相同：从 (0,0) 到 (1,1)，两个控制点决定快慢。</summary>
    private sealed class BezierEase(double x1, double y1, double x2, double y2) : IEasingFunction
    {
        public double Ease(double t)
        {
            // 控制点的 x 在 0 到 1 之间时 x 随参数单调增加：二分求出 x(s) = t 的参数，再取 y(s)。
            double low = 0, high = 1;
            for (var i = 0; i < 30; i++)
            {
                var middle = (low + high) / 2;
                if (Cubic(x1, x2, middle) < t) low = middle;
                else high = middle;
            }
            return Cubic(y1, y2, (low + high) / 2);
        }

        private static double Cubic(double p1, double p2, double s) => 3 * p1 * s * (1 - s) * (1 - s) + 3 * p2 * s * s * (1 - s) + s * s * s;
    }

    private sealed class CurveEase(Func<double, double> curve) : IEasingFunction
    {
        public double Ease(double t) => curve(t);
    }

    /// <summary>起止都静止、在 edge 处正好过半的曲线：y1 = 0、y2 = 1 时参数 0.5 处 y = 0.5，x = 0.125 + 0.375 × (x1 + x2)。</summary>
    private static BezierEase HalfwayAt(double edge, double x1) => new(x1, 0, (edge - 0.125) / 0.375 - x1, 1);

    /// <summary>后半段翻牌：背面从 From 变到 To；Page 是页面静止时的可见区域，背面上的页面按它等比缩放。</summary>
    private sealed class PageFlip
    {
        public required PageView View { get; init; }
        public Rect From { get; init; }
        public Rect To { get; init; }
        public Rect Page { get; init; }
        public bool Closing { get; init; }
        public bool Plain { get; init; }
        public double Milliseconds { get; init; }
        /// <summary>按时间比例给出从 From 走向 To 的比例，和背面转过的角度（弧度，侧立为 π/2，背面正对屏幕为 π）。</summary>
        public required Func<double, double> Travel { get; init; }
        public required Func<double, double> Angle { get; init; }
        public required Action Done { get; init; }
        public TimeSpan? Start { get; set; }
        public TimeSpan Last { get; set; }
    }

    /// <summary>
    /// 把点开的卡片翻过来：卡片本身转到侧面，换成目标页面作为背面，页面一边转回正面一边从卡片大小展开到整页，落定时就是静止的页面。
    /// 正面一直是卡片本身，转到侧面（宽度为 0）时才换成背面，看不出切换。概览页在这之后下沉淡出，卡片范围内挖空，旧内容不会透过来。
    /// 背面期间页面缓存为可视区大小的位图，每帧只改变它外面的变换和裁剪，不排版也不重画页面。
    /// </summary>
    private void OpenPage(string page, Controls.PageTile tile)
    {
        if (_morphing || _sidePanelFade is not null || _page != "Overview" || !_pages.TryGetValue(page, out var view)) return;
        StartTrace($"open {page}");
        var clicked = Stopwatch.GetTimestamp();
        // 页面通常在鼠标移到卡片上时已经排好版；用键盘直接打开时在这里补上。
        PreparePage(view);
        PageStage.UpdateLayout();
        var home = BoundsOf(tile);
        var to = PageArea(view);
        var edge = Lerp(home, to, OpenEdgeTravel);
        var plain = UsePlainFlip(to);
        SetPanelHeader(page, animate: true);
        TraceMark(plain ? "prepare plain" : "prepare", clicked);
        // 排版可能触发窗口尺寸变化而打断翻牌，排完再开始计数。
        _morphing = true;
        var session = ++_flipSession;
        _finishFlip = () =>
        {
            ClearFlip(view);
            ResetTile(tile);
            tile.Opacity = 1;
            ShowPage(page);
            SettleOverview();
        };
        FlipTile(tile, home, edge, OpenFrontMs, u => OpenTravel.Ease(OpenEdge * u) / OpenEdgeTravel, u => Math.PI * OpenTurn.Ease(OpenEdge * u), () =>
        {
            if (session != _flipSession) return;
            tile.Opacity = 0;
            ResetTile(tile);
            FadeOutPage(OverviewPage, () =>
            {
                if (session != _flipSession) return;
                OverviewHost.Visibility = Visibility.Collapsed;
                OverviewHost.Clip = null;
            });
            BeginFlip(new PageFlip
            {
                View = view, From = edge, To = to, Page = to, Plain = plain, Milliseconds = OpenBackMs,
                Travel = t => (OpenTravel.Ease(OpenEdge + (1 - OpenEdge) * t) - OpenEdgeTravel) / (1 - OpenEdgeTravel),
                Angle = t => Math.PI * OpenTurn.Ease(OpenEdge + (1 - OpenEdge) * t),
                Done = () =>
                {
                    var landed = Stopwatch.GetTimestamp();
                    tile.Opacity = 1;
                    ShowPage(page);
                    if (plain) RevealPlainPage(view, session);
                    else
                    {
                        ClearFlip(view);
                        FadeInScrollBar(view.Scroll);
                        EndMorph();
                    }
                    TraceMark("land", landed);
                }
            });
        });
    }

    /// <summary>
    /// 返回时反过来：页面带着内容缩小并转到侧面，换成卡片本身转回正面落进原位，落位时略微回弹。
    /// 概览页一开始就在后面上移淡入，卡片范围内挖空。
    /// </summary>
    private void BackToOverview()
    {
        if (_morphing || _sidePanelFade is not null || _page == "Overview") return;
        SetPanelHeader("Overview", animate: true);
        var page = _page;
        if (!_pages.TryGetValue(page, out var view) ||
            PageCards.Children.OfType<Controls.PageTile>().FirstOrDefault(tile => Equals(tile.Tag, page)) is not { } tile)
        {
            ShowPage("Overview");
            RevealOverview();
            return;
        }
        StartTrace($"back {page}");
        _morphing = true;
        var session = ++_flipSession;
        var from = PageArea(view);
        var plain = UsePlainFlip(from);
        // 被打断时概览页可能还停在上次淡出后的透明状态，要明确摆回静止状态。
        _finishFlip = () =>
        {
            ClearFlip(view);
            ShowPage("Overview");
            OverviewScroll.ScrollToTop();
            SettleOverview();
            ResetTile(tile);
            tile.Opacity = 1;
        };

        void Flip()
        {
            var revealed = Stopwatch.GetTimestamp();
            RevealOverview();
            TraceMark("overview", revealed);
            if (session != _flipSession) return;
            // 概览页正从稍低处上移，扣掉这段位移才是卡片最终的位置。
            var home = BoundsOf(tile);
            home.Y -= (OverviewPage.RenderTransform as TranslateTransform)?.Y ?? 0;
            var edge = Lerp(from, home, BackEdgeTravel);
            tile.Opacity = 0;
            BeginFlip(new PageFlip
            {
                View = view, From = from, To = edge, Page = from, Closing = true, Plain = plain, Milliseconds = BackPageMs,
                Travel = t => BackTravel.Ease(BackEdge * t) / BackEdgeTravel,
                Angle = t => Math.PI * (1 - BackTurn.Ease(BackEdge * t)),
                Done = () =>
                {
                    var swapped = Stopwatch.GetTimestamp();
                    ClearFlip(view);
                    ShowPage("Overview");
                    _finishFlip = () =>
                    {
                        ResetTile(tile);
                        SettleOverview();
                    };
                    // 先摆到侧面再显示，不会先在原位闪一下。
                    double Time(double u) => BackEdge + (1 - BackEdge) * u;
                    FlipTile(tile, home, edge, BackFrontMs, u => 1 - (BackTravel.Ease(Time(u)) - BackEdgeTravel) / (1 - BackEdgeTravel),
                        u => Math.PI * (1 - BackTurn.Ease(Time(u))), () =>
                    {
                        if (session != _flipSession) return;
                        ResetTile(tile);
                        EndMorph();
                    });
                    tile.Opacity = 1;
                    TraceMark("edge", swapped);
                }
            });
        }

        if (!plain)
        {
            Flip();
            return;
        }
        // 背面空白时和以前一样：先让空白卡片盖住页面，再换回概览页开始翻转。
        AttachFlip(new PageFlip { View = view, From = from, To = from, Page = from, Closing = true, Plain = true, Milliseconds = BackPageMs, Travel = _ => 0, Angle = _ => Math.PI, Done = () => { } });
        var cover = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(120));
        cover.Completed += (_, _) =>
        {
            if (session == _flipSession) Flip();
        };
        _flipDecor.BeginAnimation(OpacityProperty, cover);
    }

    /// <summary>概览页回到顶部、排好版，从稍低处上移淡入。</summary>
    private void RevealOverview()
    {
        OverviewHost.Visibility = Visibility.Visible;
        OverviewScroll.ScrollToTop();
        PageStage.UpdateLayout();
        FadeInPage(OverviewPage, 300);
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
    private void PreparePage(PageView view)
    {
        foreach (var other in _pages.Values)
            if (other != view && other.Host.Visibility == Visibility.Hidden) other.Host.Visibility = Visibility.Collapsed;
        view.Scroll.ScrollToTop();
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
    /// 把页面挂到背面上：页面的滚动区缓存为位图，宿主套上背面的变换和裁剪，边框和阴影盖在页面上面，概览页挖出卡片的形状。
    /// 变换和裁剪都在缓存外面，每帧改变它们不会让缓存重建。
    /// </summary>
    private void AttachFlip(PageFlip flip)
    {
        var started = Stopwatch.GetTimestamp();
        var view = flip.View;
        if (_flipDecor.Parent is Panel parent) parent.Children.Remove(_flipDecor);
        view.Host.Children.Add(_flipDecor);
        _flipDecor.BeginAnimation(OpacityProperty, null);
        _flipFill.Visibility = flip.Plain ? Visibility.Visible : Visibility.Collapsed;
        view.Host.RenderTransform = _flipMatrix;
        view.Host.IsHitTestVisible = false;
        // 滚动条在卡片范围外，翻转时本来就被裁掉；藏起来，缓存也不用包含它。
        if (ScrollBarOf(view.Scroll) is { } bar) bar.Visibility = Visibility.Hidden;
        if (!flip.Plain)
        {
            // 缓存本身已按显示缩放比例生成，RenderAtScale 保持 1；再乘一次缩放比例，200% 时会占用四倍内存。
            view.Scroll.CacheMode = new BitmapCache { SnapsToDevicePixels = true };
            view.Host.Clip = _flipCard;
            _flipBounds.Rect = new Rect(-1, -1, OverviewHost.ActualWidth + 2, OverviewHost.ActualHeight + 2);
            OverviewHost.Clip = _flipHole;
        }
        view.Host.Visibility = Visibility.Visible;
        PoseFlip(flip, 0);
        TraceMark("attach", started);
    }

    private void BeginFlip(PageFlip flip)
    {
        AttachFlip(flip);
        if (flip.Plain) flip.View.Scroll.Opacity = 0;
        _flip = flip;
        CompositionTarget.Rendering += OnFlipFrame;
    }

    /// <summary>按实际经过的时间推进，同一帧重复触发时跳过；背面、页面、边框、阴影和概览页上的洞在同一次回调里更新。</summary>
    private void OnFlipFrame(object? sender, EventArgs e)
    {
        if (_flip is not { } flip) return;
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (flip.Start is not null && time == flip.Last) return;
        flip.Start ??= time;
        flip.Last = time;
        var elapsed = (time - flip.Start.Value).TotalMilliseconds;
        PoseFlip(flip, elapsed);
        if (elapsed < flip.Milliseconds) return;
        StopFlipFrames();
        flip.Done();
    }

    /// <summary>
    /// 背面当前的矩形 R 由 From 走向 To，绕中心横向缩放 kx、纵向缩放 ky；页面按 s = R 宽 / 页面宽 等比缩放后裁成 R 的高度。
    /// 打开时 kx 从 0 到 1，返回时从 1 到 0。打开的终点 s = kx = ky = 1，矩阵正好是单位矩阵。
    /// </summary>
    private void PoseFlip(PageFlip flip, double elapsed)
    {
        var t = Math.Clamp(elapsed / flip.Milliseconds, 0, 1);
        var angle = flip.Angle(t);
        var kx = Math.Abs(Math.Cos(angle));
        var ky = 1 + (FlipLift - 1) * Math.Sin(angle);
        var shade = FlipShade * Math.Sin(angle);
        var outline = flip.Plain ? 1 : Smooth(flip.Closing ? elapsed / OutlineFade : (flip.Milliseconds - elapsed) / OutlineFade);
        var card = Lerp(flip.From, flip.To, flip.Travel(t));
        var page = flip.Page;
        var s = card.Width / page.Width;
        var cx = card.X + card.Width / 2;
        var cy = card.Y + card.Height / 2;
        _flipMatrix.Matrix = new Matrix(kx * s, 0, 0, ky * s, cx + kx * (card.X - s * page.X - cx), cy + ky * (card.Y - s * page.Y - cy));
        var height = card.Height / s;
        var radius = CardRadius * outline / s;
        var line = 1 / s;
        _flipCard.Rect = new Rect(page.X, page.Y, page.Width, height);
        _flipCard.RadiusX = _flipCard.RadiusY = radius;
        _flipEdge.Rect = new Rect(page.X + line / 2, page.Y + line / 2, Math.Max(0, page.Width - line), Math.Max(0, height - line));
        _flipEdge.RadiusX = _flipEdge.RadiusY = Math.Max(0, radius - line / 2);
        _flipBorder.StrokeThickness = line;
        _flipBorder.Opacity = outline;
        _flipShade.Opacity = shade;
    }

    private static double Smooth(double x)
    {
        x = Math.Clamp(x, 0, 1);
        return x * x * (3 - 2 * x);
    }

    private void StopFlipFrames()
    {
        if (_flip is not { } flip) return;
        _flip = null;
        CompositionTarget.Rendering -= OnFlipFrame;
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
        OverviewHost.Clip = null;
    }

    /// <summary>背面空白时和以前一样：页面落定后淡入，空白卡片同时淡出。</summary>
    private void RevealPlainPage(PageView view, int session)
    {
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

    /// <summary>页面静止时可见的区域：页面内容的宽度，从内容顶部（滚动后为可视区域顶部）一直到可视区域底边，页面内容本来就显示到那里。</summary>
    private Rect PageArea(PageView view)
    {
        var content = BoundsOf(view.Content);
        var top = Math.Max(content.Top, 0);
        return new Rect(content.Left, top, content.Width, Math.Max(1, PageStage.ActualHeight - top));
    }

    /// <summary>
    /// 卡片本身转四分之一圈：travel 给出从原位 home 走向 away 的比例，angle 给出转过的角度（弧度，0 为正对屏幕，π/2 为侧立）。
    /// 每个属性的缓动直接给出它的值，和背面的动画取自同一组曲线。只改变卡片的变换，不影响排版；转动期间压在相邻的卡片上面。
    /// </summary>
    private static void FlipTile(FrameworkElement tile, Rect home, Rect away, double milliseconds, Func<double, double> travel, Func<double, double> angle, Action done)
    {
        var size = new ScaleTransform();
        var flip = new ScaleTransform();
        var move = new TranslateTransform();
        tile.RenderTransformOrigin = new System.Windows.Point(0.5, 0.5);
        tile.RenderTransform = new TransformGroup { Children = { size, flip, move } };
        Panel.SetZIndex(tile, 1);
        var path = new CurveEase(travel);
        Tween(size, ScaleTransform.ScaleXProperty, 1, away.Width / home.Width, milliseconds, path);
        Tween(size, ScaleTransform.ScaleYProperty, 1, away.Height / home.Height, milliseconds, path);
        Tween(move, TranslateTransform.XProperty, 0, away.X + away.Width / 2 - home.X - home.Width / 2, milliseconds, path);
        Tween(move, TranslateTransform.YProperty, 0, away.Y + away.Height / 2 - home.Y - home.Height / 2, milliseconds, path);
        Tween(flip, ScaleTransform.ScaleYProperty, 1, FlipLift, milliseconds, new CurveEase(u => Math.Sin(angle(u))));
        Tween(flip, ScaleTransform.ScaleXProperty, 1, 0, milliseconds, new CurveEase(u => 1 - Math.Cos(angle(u))), done);
    }

    private static void ResetTile(FrameworkElement tile)
    {
        tile.RenderTransform = Transform.Identity;
        Panel.SetZIndex(tile, 0);
    }

    /// <summary>done 在动画结束时调用；Completed 必须在 BeginAnimation 之前挂上，之后挂上的不会触发。</summary>
    private static void Tween(IAnimatable target, DependencyProperty property, double from, double to, double milliseconds, IEasingFunction ease, Action? done = null)
    {
        var animation = new DoubleAnimation(from, to, TimeSpan.FromMilliseconds(milliseconds)) { EasingFunction = ease };
        if (done is not null) animation.Completed += (_, _) => done();
        target.BeginAnimation(property, animation);
    }

    /// <summary>页面略微下沉并淡出，动画期间缓存为位图，淡出后去掉缓存。</summary>
    private static void FadeOutPage(FrameworkElement page, Action done)
    {
        if (page.RenderTransform is not TranslateTransform shift) page.RenderTransform = shift = new TranslateTransform();
        CacheWhileFading(page);
        var duration = TimeSpan.FromMilliseconds(200);
        shift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, 10, duration) { EasingFunction = LeaveEase });
        var fade = new DoubleAnimation(1, 0, duration) { EasingFunction = LeaveEase };
        fade.Completed += (_, _) =>
        {
            page.CacheMode = null;
            done();
        };
        page.BeginAnimation(OpacityProperty, fade);
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
        if (page.CacheMode is null && FitsCache(page, page.ActualWidth, page.ActualHeight)) page.CacheMode = new BitmapCache { SnapsToDevicePixels = true };
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
        SidePanel.CacheMode ??= new BitmapCache { SnapsToDevicePixels = true };
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
        var dpi = VisualTreeHelper.GetDpi(element);
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(element.ActualWidth * dpi.DpiScaleX), (int)Math.Ceiling(element.ActualHeight * dpi.DpiScaleY),
            dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            // 只截元素自身的范围：被裁掉的溢出部分不能算进画面，否则截图会被压扁。
            var bounds = new Rect(0, 0, element.ActualWidth, element.ActualHeight);
            context.DrawRectangle(new VisualBrush(element) { Viewbox = bounds, ViewboxUnits = BrushMappingMode.Absolute }, null, bounds);
        }
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
            var original = _historyIndex;
            var index = !cancel && _tornOff && panel?.Gap is >= 0 and var gap ? gap : original;
            panel?.SetGap(container, index);
            // 放回的牌立即参与选中，光标下的牌（通常就是它）直接抽出，卡牌落进抽出的位置。
            panel?.UpdateHot();
            ghost.ClipBottom = WindowBottom;
            ghost.Settle(() => PoseOf(container), () =>
            {
                container.Opacity = 1;
                if (index == original)
                {
                    panel?.SetGap(null, -1);
                    return;
                }
                // 牌堆保持放回后的排法，等卡牌落定、画面静止后再重排列表，重新生成卡片的停顿不会出现在动画中。
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

    // 翻牌期间页面跟着卡片变形，点击、滚轮和按键都先不处理；Alt+F4 等系统按键照常。
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
