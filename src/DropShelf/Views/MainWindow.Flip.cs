using System.Diagnostics;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow
{
    // ───────────── 卡片翻开成页面 ─────────────

    /// <summary>转到侧面时卡片离屏幕最近，略微变高，背面带一点阴影，看起来像在三维空间里翻转。</summary>
    private const double FlipLift = 1.06;
    private const double FlipShade = 0.22;
    /// <summary>背面就是页面那张牌，圆角和边框与静止时一样，见 PageSheetStyle。</summary>
    private const double SheetRadius = 12;
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
        _flipEdge.Rect = new Rect(page.X + 0.5, page.Y + 0.5, Math.Max(0, page.Width - 1), Math.Max(0, height - 1));
        _flipEdge.RadiusX = _flipEdge.RadiusY = SheetRadius - 0.5;
        _flipShade.Opacity = FlipShade * Math.Sin(angle);
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
}
