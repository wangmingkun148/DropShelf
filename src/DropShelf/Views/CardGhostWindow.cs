using System.Diagnostics;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Effects;
using DropShelf.Interop;

namespace DropShelf.Views;

/// <summary>卡牌的位置（中心点，屏幕像素）、缩放和角度。</summary>
public readonly record struct CardPose(Point Center, double Scale, double Angle);

/// <summary>
/// 从概览页拖出的卡牌：用一个不接收鼠标的透明小窗口显示卡牌截图，因此可以拖出主窗口。
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
    private readonly ScaleTransform _scale = new();
    private readonly RotateTransform _rotate = new();
    private readonly Stopwatch _clock = new();
    private readonly IntPtr _handle;
    private Func<CardPose?> _target = () => null;
    private Action? _arrived;
    private CardPose _pose;
    private double _sway;
    private double _lastFrame;
    private double _settleStart;
    private bool _fading;
    private bool _running;

    public CardGhostWindow()
    {
        Title = "DropShelf Card";
        WindowStyle = WindowStyle.None;
        AllowsTransparency = true;
        Background = Brushes.Transparent;
        ResizeMode = ResizeMode.NoResize;
        ShowInTaskbar = false;
        ShowActivated = false;
        Topmost = true;
        Focusable = false;
        IsHitTestVisible = false;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = -32000;
        Top = -32000;
        _image.RenderTransformOrigin = new Point(0.5, 0.5);
        _image.RenderTransform = new TransformGroup { Children = { _scale, _rotate } };
        _image.Effect = new DropShadowEffect { BlurRadius = 24, ShadowDepth = 6, Direction = 270, Opacity = 0.25, Color = Colors.Black };
        Content = _image;
        SourceInitialized += (_, _) =>
        {
            WindowEffects.SetToolWindow(this, noActivate: true);
            var handle = WindowEffects.Handle(this);
            var style = NativeMethods.GetWindowLong(handle, NativeMethods.GWL_EXSTYLE);
            NativeMethods.SetWindowLong(handle, NativeMethods.GWL_EXSTYLE, style | NativeMethods.WS_EX_TRANSPARENT);
        };
        // 提前创建窗口句柄，开始拖动时不必再等。
        _handle = WindowEffects.Handle(this);
    }

    /// <summary>正在拖动或播放收尾动画。</summary>
    public bool IsBusy => _running;

    /// <summary>从卡牌当前的样子开始跟随目标。</summary>
    public void Begin(ImageSource image, Size size, CardPose start, Func<CardPose?> target)
    {
        _image.Source = image;
        _image.Width = size.Width;
        _image.Height = size.Height;
        _image.Opacity = 1;
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
        var now = _clock.Elapsed.TotalSeconds;
        // 同一帧可能触发多次；卡顿后也不让卡牌一下子跳过去。
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
            _image.Opacity = Math.Max(0, _image.Opacity - dt / FadeSeconds);
            if (_image.Opacity <= 0) Stop();
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
        var dpi = VisualTreeHelper.GetDpi(this);
        NativeMethods.SetWindowPos(_handle, IntPtr.Zero, (int)Math.Round(_pose.Center.X - Width * dpi.DpiScaleX / 2),
            (int)Math.Round(_pose.Center.Y - Height * dpi.DpiScaleY / 2), 0, 0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);
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
        _arrived = null;
        _clock.Stop();
        Hide();
        _image.Source = null;
    }
}
