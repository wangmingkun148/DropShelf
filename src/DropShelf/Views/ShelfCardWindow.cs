using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using DropShelf.Interop;

namespace DropShelf.Views;

/// <summary>
/// 牌在某一时刻的姿态：中心相对落点的偏移 X、Y（DIP），缩放，平面内的转角（度），绕翻转轴转过的角度 Turn（0 正面朝外，180 背面朝外）和透明度。
/// Ease 是从这一帧到下一帧的缓动，不填为匀速。
/// </summary>
internal readonly record struct CardKey(double Ms, double X = 0, double Y = 0, double Scale = 1, double Angle = 0, double Turn = 0,
    double Opacity = 1, IEasingFunction? Ease = null);

/// <summary>一段牌的动画：按时间排好的关键帧。HorizontalAxis 为真时绕横轴上下翻，否则绕竖轴左右翻；开头 FadeInMs 毫秒内淡入。</summary>
internal sealed record CardMotion(IReadOnlyList<CardKey> Keys, bool HorizontalAxis = false, double FadeInMs = 0)
{
    public double Duration => Keys[^1].Ms;

    /// <summary>ms 时刻的姿态：在前后两帧之间按前一帧的缓动插值。</summary>
    public CardKey At(double ms)
    {
        for (var i = 1; i < Keys.Count; i++)
        {
            var (from, to) = (Keys[i - 1], Keys[i]);
            if (ms >= to.Ms) continue;
            var t = Math.Clamp((ms - from.Ms) / Math.Max(1, to.Ms - from.Ms), 0, 1);
            t = from.Ease?.Ease(t) ?? t;
            return FadeIn(new CardKey(ms, Lerp(from.X, to.X, t), Lerp(from.Y, to.Y, t), Lerp(from.Scale, to.Scale, t),
                Lerp(from.Angle, to.Angle, t), Lerp(from.Turn, to.Turn, t), Lerp(from.Opacity, to.Opacity, t)), ms);
        }
        return FadeIn(Keys[^1], ms);
    }

    private CardKey FadeIn(CardKey pose, double ms) =>
        ms < FadeInMs ? pose with { Opacity = pose.Opacity * Math.Max(0, ms) / FadeInMs } : pose;

    private static double Lerp(double from, double to, double t) => from + (to - from) * t;
}

/// <summary>
/// 置物架出场和离场时替它表演的那张牌。透明、置顶、不抢焦点的窗口里画一张牌：正面是置物架的截图，背面是牌背，按关键帧移动、转动和翻面。
/// 透明和拖动中的卡牌一样靠 DWM 把玻璃区扩展到整个窗口，不用分层窗口。窗口只盖住牌经过的范围并限制在工作区内，
/// 牌从屏幕外进来时，伸出工作区的部分自然被窗口裁掉。窗口不接收拖放：动画期间在牌上松手什么也不会发生，不会落进下面的程序。
/// 正反两面连同阴影各缓存一次位图，每帧只改变换、明暗和显示哪一面。翻面不做真正的透视：沿翻转方向按余弦压扁，另一方向略微放大，和设置页的翻牌一样。
/// 播完隐藏后放回池里，下次直接复用，不必再创建窗口。
/// </summary>
internal sealed class ShelfCardWindow : Window
{
    private const double Lift = 1.06;
    private const double MaxShade = 0.3;
    private const double ShadowRoom = 44;
    private static readonly List<ShelfCardWindow> Idle = [];

    private readonly IntPtr _handle;
    private readonly Grid _card = new();
    private readonly MatrixTransform _transform = new();
    private readonly Grid _front = new();
    private readonly System.Windows.Controls.Image _face = new() { Stretch = Stretch.Fill };
    private readonly Border _faceEdge = new() { BorderThickness = new Thickness(1) };
    private readonly Grid _back = new();
    private readonly Border _backFace = new() { BorderThickness = new Thickness(1.2) };
    private readonly Border _backLine = new()
    {
        Margin = new Thickness(9), BorderThickness = new Thickness(1), BorderBrush = new SolidColorBrush(Color.FromArgb(0x99, 0xFB, 0xFA, 0xF6))
    };
    private readonly Border _shade = new() { Background = Brushes.Black };
    private readonly Stopwatch _clock = new();
    private CardMotion _motion = new([new CardKey(0)]);
    private RECT _target;
    private RECT _bounds;
    private Action? _shown;
    private Action? _finished;
    private Stage _stage;
    private int _frames;
    private TimeSpan _lastFrame;

    private enum Stage
    {
        Idle,
        /// <summary>刚显示，等第一帧画到屏幕上。</summary>
        Showing,
        Playing,
        /// <summary>播完后再停留两帧，盖住置物架现身前的那一帧。</summary>
        Lingering
    }

    private ShelfCardWindow()
    {
        Title = "DropShelf Card";
        WindowStyle = WindowStyle.None;
        Background = Brushes.Transparent;
        System.Windows.Shell.WindowChrome.SetWindowChrome(this, new System.Windows.Shell.WindowChrome
        {
            CaptionHeight = 0, GlassFrameThickness = new Thickness(-1), ResizeBorderThickness = default, CornerRadius = default,
            UseAeroCaptionButtons = false
        });
        ResizeMode = ResizeMode.NoResize;
        ShowInTaskbar = false;
        ShowActivated = false;
        Topmost = true;
        Focusable = false;
        IsHitTestVisible = false;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = -32000;
        Top = -32000;

        _faceEdge.SetResourceReference(Border.BorderBrushProperty, "ShelfBorderBrush");
        _backFace.SetResourceReference(Border.BackgroundProperty, "SheetBorderBrush");
        _backFace.SetResourceReference(Border.BorderBrushProperty, "CardBackLineBrush");
        _backFace.Child = _backLine;
        // 阴影加在里层、外层缓存为位图：阴影只模糊一次，之后每帧只变换这张图。
        var shadow = new DropShadowEffect { BlurRadius = 30, ShadowDepth = 8, Direction = 270, Opacity = 0.3, Color = Colors.Black };
        shadow.Freeze();
        _front.Children.Add(new Grid { Effect = shadow, Children = { _face, _faceEdge } });
        _backFace.Effect = shadow;
        _back.Children.Add(_backFace);
        _card.Children.Add(_front);
        _card.Children.Add(_back);
        _card.Children.Add(_shade);
        _card.RenderTransform = _transform;
        Content = new Canvas { Children = { _card } };
        SourceInitialized += (_, _) =>
        {
            WindowEffects.SetToolWindow(this, noActivate: true);
            WindowEffects.SetFrameless(this);
        };
        // 提前创建窗口句柄，呼出置物架时不必再等。
        _handle = WindowEffects.Handle(this);
    }

    /// <summary>取一个空闲的窗口，没有就新建。</summary>
    public static ShelfCardWindow Rent()
    {
        if (Idle.Count == 0) return new ShelfCardWindow();
        var window = Idle[^1];
        Idle.RemoveAt(Idle.Count - 1);
        return window;
    }

    /// <summary>空闲时先备好一个窗口。</summary>
    public static void Prewarm()
    {
        if (Idle.Count == 0) Idle.Add(new ShelfCardWindow());
    }

    /// <summary>
    /// 播放一段动画。target 是牌落定（关键帧各项为零）时在屏幕上的位置，area 是窗口能占用的范围，都是屏幕像素；scale 是那里的缩放比例。
    /// 窗口的第一帧画到屏幕上之后调用 shown，然后才开始计时；播完调用 finished，之后还停留两帧才隐藏。
    /// </summary>
    public void Play(RECT target, RECT area, double scale, ImageSource face, CardMotion motion, Action? shown, Action? finished)
    {
        _motion = motion;
        _target = target;
        _shown = shown;
        _finished = finished;
        var size = new Size(target.Width / scale, target.Height / scale);
        _card.Width = size.Width;
        _card.Height = size.Height;
        // Windows 11 上置物架窗口是圆角的，牌也画成同样的圆角，现身时轮廓不变。
        var radius = WindowEffects.IsWindows11 ? 8 : 0;
        _face.Source = face;
        _face.Clip = new RectangleGeometry(new Rect(size), radius, radius);
        _faceEdge.CornerRadius = _backFace.CornerRadius = _shade.CornerRadius = new CornerRadius(radius);
        _backLine.CornerRadius = new CornerRadius(Math.Max(2, radius - 4));
        _bounds = Bounds(target, area, scale, motion);
        Place();
        if (!IsVisible) Show();
        // 第一次显示时窗口会回到构造时设定的位置，跨越缩放不同的显示器时大小也可能被系统改掉，再放一次。
        Place();
        var cache = CacheScale.For(this);
        _front.CacheMode = new BitmapCache(cache);
        _back.CacheMode = new BitmapCache(cache);
        Apply(motion.At(0));
        _stage = Stage.Showing;
        _frames = 0;
        CompositionTarget.Rendering -= OnFrame;
        CompositionTarget.Rendering += OnFrame;
    }

    /// <summary>立即停下并隐藏，不再调用 shown 和 finished。</summary>
    public void Cancel()
    {
        _shown = null;
        _finished = null;
        if (_stage != Stage.Idle) Release();
    }

    private void OnFrame(object? sender, EventArgs e)
    {
        // 同一帧可能触发多次。
        var time = ((RenderingEventArgs)e).RenderingTime;
        if (time == _lastFrame) return;
        _lastFrame = time;
        switch (_stage)
        {
            case Stage.Showing:
                if (++_frames < 2) return;
                _stage = Stage.Playing;
                _clock.Restart();
                Take(ref _shown)?.Invoke();
                break;
            case Stage.Playing:
                var ms = _clock.Elapsed.TotalMilliseconds;
                Apply(_motion.At(ms));
                if (ms < _motion.Duration) return;
                _stage = Stage.Lingering;
                _frames = 0;
                Take(ref _finished)?.Invoke();
                break;
            case Stage.Lingering:
                if (++_frames >= 2) Release();
                break;
        }
    }

    private static Action? Take(ref Action? action)
    {
        var taken = action;
        action = null;
        return taken;
    }

    /// <summary>按姿态摆放：先绕中心压扁或放大、再转动，最后移到落点加偏移处；翻过侧面后换成另一面。</summary>
    private void Apply(CardKey pose)
    {
        var dpi = VisualTreeHelper.GetDpi(this);
        var x = (_target.Left - _bounds.Left + _target.Width / 2.0) / dpi.DpiScaleX + pose.X;
        var y = (_target.Top - _bounds.Top + _target.Height / 2.0) / dpi.DpiScaleY + pose.Y;
        var turn = pose.Turn * Math.PI / 180;
        var squash = Math.Max(Math.Abs(Math.Cos(turn)), 0.01) * pose.Scale;
        var lift = (1 + (Lift - 1) * Math.Abs(Math.Sin(turn))) * pose.Scale;
        var matrix = Matrix.Identity;
        matrix.Translate(-_card.Width / 2, -_card.Height / 2);
        if (_motion.HorizontalAxis) matrix.Scale(lift, squash);
        else matrix.Scale(squash, lift);
        matrix.Rotate(pose.Angle);
        matrix.Translate(x, y);
        _transform.Matrix = matrix;
        var front = Math.Cos(turn) >= 0;
        _front.Visibility = front ? Visibility.Visible : Visibility.Hidden;
        _back.Visibility = front ? Visibility.Hidden : Visibility.Visible;
        _shade.Opacity = MaxShade * Math.Abs(Math.Sin(turn));
        _card.Opacity = pose.Opacity;
    }

    /// <summary>窗口的范围：盖住每一帧牌的外接矩形（按转角和翻面时最大的放大量估算），再留出阴影，限制在 area 内。</summary>
    private static RECT Bounds(RECT target, RECT area, double scale, CardMotion motion)
    {
        var (centerX, centerY) = (target.Left + target.Width / 2.0, target.Top + target.Height / 2.0);
        var (halfWidth, halfHeight) = (target.Width / 2.0, target.Height / 2.0);
        double left = target.Left, top = target.Top, right = target.Right, bottom = target.Bottom;
        foreach (var key in motion.Keys)
        {
            var angle = Math.Abs(key.Angle) * Math.PI / 180;
            var grow = key.Scale * Lift;
            var extentX = (Math.Cos(angle) * halfWidth + Math.Sin(angle) * halfHeight) * grow;
            var extentY = (Math.Sin(angle) * halfWidth + Math.Cos(angle) * halfHeight) * grow;
            var (x, y) = (centerX + key.X * scale, centerY + key.Y * scale);
            left = Math.Min(left, x - extentX);
            right = Math.Max(right, x + extentX);
            top = Math.Min(top, y - extentY);
            bottom = Math.Max(bottom, y + extentY);
        }
        var room = ShadowRoom * scale;
        var bounds = new RECT
        {
            Left = (int)Math.Max(area.Left, Math.Floor(left - room)),
            Top = (int)Math.Max(area.Top, Math.Floor(top - room)),
            Right = (int)Math.Min(area.Right, Math.Ceiling(right + room)),
            Bottom = (int)Math.Min(area.Bottom, Math.Ceiling(bottom + room))
        };
        if (bounds.Width <= 0 || bounds.Height <= 0) bounds = target;
        return bounds;
    }

    private void Place() => NativeMethods.SetWindowPos(_handle, IntPtr.Zero, _bounds.Left, _bounds.Top, _bounds.Width, _bounds.Height,
        NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);

    private void Release()
    {
        CompositionTarget.Rendering -= OnFrame;
        _clock.Stop();
        _stage = Stage.Idle;
        Hide();
        // 不再引用置物架的截图，缓存的位图也一起释放。
        _face.Source = null;
        _front.CacheMode = null;
        _back.CacheMode = null;
        Idle.Add(this);
    }

    protected override void OnClosed(EventArgs e)
    {
        CompositionTarget.Rendering -= OnFrame;
        Idle.Remove(this);
        base.OnClosed(e);
    }
}
