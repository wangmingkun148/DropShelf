using System.Diagnostics;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Effects;
using DropShelf.Interop;

namespace DropShelf.Views;

/// <summary>卡牌的位置（中心点，屏幕像素）、缩放和角度。</summary>
public readonly record struct CardPose(Point Center, double Scale, double Angle);

/// <summary>
/// 从概览页拖出的卡牌：用一个不接收鼠标的透明小窗口显示卡牌截图，因此可以拖出主窗口。
/// 透明靠 DWM 把玻璃区扩展到整个窗口，不用分层窗口：分层窗口每帧要把画面从显卡读回内存再提交，拖动时会掉帧。
/// 在牌堆附近时卡牌改画在主窗口里，和牌堆的动画同一帧画出，也不必每帧移动另一个窗口。
/// 位置、大小和角度每帧按指数衰减向目标靠拢，跟手但略有滞后；横向移动越快卡牌越向移动方向倾斜，停下后慢慢回正，带出阻尼感。
/// 只在拖动和收尾动画期间挂接每帧回调，平时不占用资源。
/// </summary>
public sealed class CardGhostWindow : Window
{
    private const double FollowRate = 60;
    private const double TurnRate = 9;
    private const double ScaleRate = 14;
    private const double SwayPerSpeed = 0.007;
    private const double MaxSway = 8;
    private const double FadeSeconds = 0.18;
    private const double SettleTimeout = 0.8;

    private readonly System.Windows.Controls.Image _image = new() { Stretch = Stretch.Fill };
    private readonly System.Windows.Controls.Grid _card = new() { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    private readonly System.Windows.Controls.Grid _root = new();
    private readonly ScaleTransform _scale = new();
    private readonly RotateTransform _rotate = new();
    private readonly TranslateTransform _offset = new();
    private readonly Stopwatch _clock = new();
    private readonly IntPtr _handle;
    private Func<CardPose?> _target = () => null;
    private Action? _arrived;
    private CardPose _pose;
    private double _sway;
    private double _lastFrame;
    private TimeSpan _lastRendering;
    private double _settleStart;
    private bool _fading;
    private bool _running;

    public CardGhostWindow()
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
        _card.RenderTransformOrigin = new Point(0.5, 0.5);
        _card.RenderTransform = new TransformGroup { Children = { _scale, _rotate, _offset } };
        _image.Effect = new DropShadowEffect { BlurRadius = 24, ShadowDepth = 6, Direction = 270, Opacity = 0.25, Color = Colors.Black };
        _card.Children.Add(_image);
        _root.Children.Add(_card);
        Content = _root;
        SourceInitialized += (_, _) =>
        {
            WindowEffects.SetToolWindow(this, noActivate: true);
            WindowEffects.SetFrameless(this);
            // 收尾动画期间点到卡牌上的鼠标交给下面的窗口。
            HwndSource.FromHwnd(WindowEffects.Handle(this))?.AddHook((IntPtr _, int message, IntPtr _, IntPtr _, ref bool handled) =>
            {
                if (message != NativeMethods.WM_NCHITTEST) return IntPtr.Zero;
                handled = true;
                return new IntPtr(NativeMethods.HTTRANSPARENT);
            });
        };
        // 提前创建窗口句柄，开始拖动时不必再等。
        _handle = WindowEffects.Handle(this);
    }

    /// <summary>正在拖动或播放收尾动画。</summary>
    public bool IsBusy => _running;

    /// <summary>卡牌要画在主窗口的这一层时返回它（超出窗口底边的部分随牌堆一起被裁掉），null 表示画在独立的窗口里。</summary>
    public Func<System.Windows.Controls.Panel?> Layer { get; set; } = () => null;

    /// <summary>从卡牌当前的样子开始跟随目标。</summary>
    public void Begin(ImageSource image, Size size, CardPose start, Func<CardPose?> target)
    {
        _image.Source = image;
        _image.Width = size.Width;
        _image.Height = size.Height;
        _card.Opacity = 1;
        // 卡牌连同阴影只在开始时画一次并缓存为位图，之后每帧只移动、转动这张图，不再重新计算阴影模糊。
        _card.CacheMode = new BitmapCache(CacheScale.For(this));
        // 留出旋转、放大和阴影的空间。
        Width = Height = Math.Ceiling(Math.Sqrt(size.Width * size.Width + size.Height * size.Height) * 1.2 + 48);
        _pose = start;
        _sway = 0;
        _target = target;
        _arrived = null;
        _fading = false;
        Apply();
        if (!IsVisible) Show();
        Apply();
        if (_running) return;
        _running = true;
        _clock.Restart();
        _lastFrame = 0;
        CompositionTarget.Rendering += OnFrame;
    }

    /// <summary>飞向新的目标，到达后调用 arrived 并隐藏；目标不存在时淡出。</summary>
    public void Settle(Func<CardPose?> target, Action arrived)
    {
        _target = target;
        _arrived = arrived;
        _settleStart = _clock.Elapsed.TotalSeconds;
    }

    /// <summary>缩小并淡出，同时移向指定位置（新置物架的中心）。</summary>
    public void Vanish(Point center)
    {
        _target = () => new CardPose(center, 0.7, 0);
        _fading = true;
    }

    private void OnFrame(object? sender, EventArgs e)
    {
        // 同一帧可能触发多次，其间时钟仍在走，按这一帧的绘制时间去重。
        var rendering = ((RenderingEventArgs)e).RenderingTime;
        if (rendering == _lastRendering) return;
        _lastRendering = rendering;
        var now = _clock.Elapsed.TotalSeconds;
        // 卡顿后也不让卡牌一下子跳过去。
        var dt = Math.Min(now - _lastFrame, 0.05);
        if (dt <= 0) return;
        _lastFrame = now;

        // 要放回的卡牌已不在（如列表刷新）：照常收尾，然后原地淡出。
        if (_target() is not { } goal)
        {
            _arrived?.Invoke();
            _arrived = null;
            _fading = true;
            goal = _pose;
        }

        var previousX = _pose.Center.X;
        var follow = 1 - Math.Exp(-dt * FollowRate);
        var center = _pose.Center + (goal.Center - _pose.Center) * follow;
        var speed = (center.X - previousX) / dt / VisualTreeHelper.GetDpi(this).DpiScaleX;
        _sway += (Math.Clamp(speed * SwayPerSpeed, -MaxSway, MaxSway) - _sway) * (1 - Math.Exp(-dt * TurnRate));
        var angle = _pose.Angle + (goal.Angle - _pose.Angle) * (1 - Math.Exp(-dt * TurnRate));
        var scale = _pose.Scale + (goal.Scale - _pose.Scale) * (1 - Math.Exp(-dt * ScaleRate));
        _pose = new CardPose(center, scale, angle);
        Apply();

        if (_fading)
        {
            _card.Opacity = Math.Max(0, _card.Opacity - dt / FadeSeconds);
            if (_card.Opacity <= 0) Stop();
            return;
        }

        if (_arrived is null) return;
        var arrived = (goal.Center - center).Length < 1 && Math.Abs(goal.Scale - scale) < 0.01 && Math.Abs(goal.Angle - angle - _sway) < 0.5;
        if (!arrived && now - _settleStart < SettleTimeout) return;
        _arrived();
        Stop();
    }

    private void Apply()
    {
        _scale.ScaleX = _scale.ScaleY = _pose.Scale;
        _rotate.Angle = _pose.Angle + _sway;
        var layer = Layer();
        MoveTo(layer ?? _root);
        if (layer is not null)
        {
            // 用平移摆放而不改 Canvas.Left/Top，每帧只需重新合成，不会让主窗口重新排版。
            var point = layer.PointFromScreen(_pose.Center);
            _offset.X = point.X - _image.Width / 2;
            _offset.Y = point.Y - _image.Height / 2;
            return;
        }
        _offset.X = _offset.Y = 0;
        var dpi = VisualTreeHelper.GetDpi(this);
        NativeMethods.SetWindowPos(_handle, IntPtr.Zero, (int)Math.Round(_pose.Center.X - Width * dpi.DpiScaleX / 2),
            (int)Math.Round(_pose.Center.Y - Height * dpi.DpiScaleY / 2), 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);
    }

    /// <summary>把卡牌挪到要画它的地方；挪进主窗口后独立窗口留空，不再移动。</summary>
    private void MoveTo(System.Windows.Controls.Panel host)
    {
        if (ReferenceEquals(_card.Parent, host)) return;
        (_card.Parent as System.Windows.Controls.Panel)?.Children.Remove(_card);
        host.Children.Add(_card);
    }

    protected override void OnClosed(EventArgs e)
    {
        CompositionTarget.Rendering -= OnFrame;
        base.OnClosed(e);
    }

    private void Stop()
    {
        CompositionTarget.Rendering -= OnFrame;
        _running = false;
        // 不再引用旧卡片的位置回调。
        _target = () => null;
        _arrived = null;
        _clock.Stop();
        Hide();
        MoveTo(_root);
        _image.Source = null;
        _card.CacheMode = null;
    }
}
