using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Services;

namespace DropShelf.Views;

public partial class OnboardingWindow : Window
{
    private const int StepCount = 4;
    private readonly FrameworkElement[] _art;
    private readonly string[] _stories = ["Step1Story", "Step2Story", "Step3Story", "Step4Story"];
    private int _step;

    public OnboardingWindow()
    {
        InitializeComponent();
        PowerQoS.Track(this);
        _art = [Step1Art, Step2Art, Step3Art, Step4Art];
        for (var index = 0; index < StepCount; index++)
            Dots.Children.Add(new System.Windows.Shapes.Rectangle { Width = 6, Height = 6, RadiusX = 3, RadiusY = 3, Margin = new Thickness(0, 0, 8, 0) });

        SourceInitialized += (_, _) => WindowEffects.SetDarkMode(this, ThemeService.IsDark);
        ThemeService.ThemeChanged += OnThemeChanged;
        LanguageService.LanguageChanged += OnLanguageChanged;
        PreviewKeyDown += OnPreviewKeyDown;
        Loaded += (_, _) => ShowStep(0);
    }

    public int CurrentStep => _step;

    /// <summary>切换步骤时演示画面交叉淡入淡出、说明文字从前进的方向滑入、指示点平滑变宽的时间。</summary>
    private static readonly TimeSpan StepFade = TimeSpan.FromMilliseconds(200);
    private static readonly IEasingFunction StepEase = new CubicEase { EasingMode = EasingMode.EaseOut };

    /// <summary>关闭了动画效果时各步演示停在这一刻：画面里该有的都在（1 静止、2 置物架已弹出、3 文件在架上、4 拖到快速操作上）。</summary>
    private static readonly TimeSpan[] StillFrames =
        [TimeSpan.Zero, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(1.3), TimeSpan.FromSeconds(1.1)];

    private bool _stepShown;

    /// <summary>
    /// 换到第 step 步：旧的演示淡出、新的淡入，说明文字从前进的方向滑入，指示点变宽变色。只改透明度、平移和几个小圆点的宽度，很轻。
    /// 第一次显示和关闭了动画效果时直接到位；关闭了动画效果时演示也不循环播放，停在能看懂的一帧。
    /// </summary>
    public void ShowStep(int step)
    {
        var previous = _step;
        _step = Math.Clamp(step, 0, StepCount - 1);
        var animate = _stepShown && previous != _step && ShelfMotion.Enabled;
        _stepShown = true;
        for (var index = 0; index < _art.Length; index++)
        {
            var art = _art[index];
            var story = (Storyboard)Resources[_stories[index]];
            if (index == _step)
            {
                // 刚换上来的从透明淡入；正在淡出又被切回来的从当前的样子接着淡入。
                if (animate && art.Visibility != Visibility.Visible)
                {
                    art.BeginAnimation(OpacityProperty, null);
                    art.Opacity = 0;
                }
                art.Visibility = Visibility.Visible;
                story.Begin(this, true);
                if (!ShelfMotion.Enabled)
                {
                    story.Pause(this);
                    story.SeekAlignedToLastTick(this, StillFrames[index], TimeSeekOrigin.BeginTime);
                }
                FadeTo(art, 1, animate);
            }
            else if (index == previous && animate && art.Visibility == Visibility.Visible)
            {
                var stepAtFade = index;
                FadeTo(art, 0, true, () =>
                {
                    if (_step == stepAtFade) return;
                    art.Visibility = Visibility.Collapsed;
                    story.Stop(this);
                });
            }
            else
            {
                art.BeginAnimation(OpacityProperty, null);
                art.Visibility = Visibility.Collapsed;
                story.Stop(this);
            }
        }

        UpdateStepText();
        if (animate)
        {
            var from = 14 * Math.Sign(_step - previous);
            StepText.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, StepFade) { EasingFunction = StepEase });
            StepTextShift.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(from, 0, StepFade) { EasingFunction = StepEase });
        }
        for (var index = 0; index < Dots.Children.Count; index++)
        {
            var dot = (System.Windows.Shapes.Rectangle)Dots.Children[index];
            var active = index == _step;
            dot.SetResourceReference(Shape.FillProperty, active ? "AccentBrush" : "ControlStrongBorderBrush");
            AnimateTo(dot, WidthProperty, active ? 18 : 6, animate);
            AnimateTo(dot, OpacityProperty, active ? 1 : 0.4, animate);
        }

        BackButton.Visibility = _step == 0 ? Visibility.Collapsed : Visibility.Visible;
        SkipButton.Visibility = _step == StepCount - 1 ? Visibility.Collapsed : Visibility.Visible;
    }

    /// <summary>透明度过渡到 to，结束时调用 done；不播动画时直接到位。</summary>
    private static void FadeTo(UIElement element, double to, bool animate, Action? done = null)
    {
        if (!animate)
        {
            element.BeginAnimation(OpacityProperty, null);
            element.Opacity = to;
            done?.Invoke();
            return;
        }
        var fade = new DoubleAnimation(to, StepFade) { EasingFunction = StepEase };
        if (done is not null) fade.Completed += (_, _) => done();
        element.BeginAnimation(OpacityProperty, fade);
    }

    /// <summary>属性过渡到 to（从当前显示的值起），不播动画时直接到位。终点先写进属性，动画只在播放期间盖在上面。</summary>
    private static void AnimateTo(FrameworkElement element, DependencyProperty property, double to, bool animate)
    {
        var from = (double)element.GetValue(property);
        element.BeginAnimation(property, null);
        element.SetValue(property, to);
        if (!animate || from == to) return;
        element.BeginAnimation(property, new DoubleAnimation(from, to, StepFade) { EasingFunction = StepEase, FillBehavior = FillBehavior.Stop });
    }

    private void UpdateStepText()
    {
        var (title, body) = GetStepText(_step);
        StepTitleText.Text = title;
        StepBodyText.Text = body;
        NextButton.Content = _step == StepCount - 1 ? Strings.GetStarted : Strings.Next;
    }

    private static (string Title, string Body) GetStepText(int step) => step switch
    {
        0 => (Strings.OnboardingWelcomeTitle, Strings.OnboardingWelcomeBody),
        1 => (Strings.OnboardingShakeTitle, Strings.OnboardingShakeBody),
        2 => (Strings.OnboardingStackTitle, Strings.OnboardingStackBody),
        _ => (Strings.OnboardingActionsTitle, BuildLastStepText())
    };

    private static string BuildLastStepText()
    {
        var hotkey = AppController.Current?.Settings.HotkeyGesture ?? Models.HotkeyGesture.Default;
        return Strings.OnboardingActionsBody(hotkey.IsEmpty ? null : hotkey.ToString());
    }

    private void OnNextClick(object sender, RoutedEventArgs e)
    {
        if (_step >= StepCount - 1) Close();
        else ShowStep(_step + 1);
    }

    private void OnBackClick(object sender, RoutedEventArgs e) => ShowStep(_step - 1);

    private void OnSkipClick(object sender, RoutedEventArgs e) => Close();

    private void OnPreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Right:
                if (_step < StepCount - 1) ShowStep(_step + 1);
                e.Handled = true;
                break;
            case Key.Left:
                if (_step > 0) ShowStep(_step - 1);
                e.Handled = true;
                break;
            case Key.Escape:
                Close();
                e.Handled = true;
                break;
        }
    }

    private void OnCaptionCloseClick(object sender, RoutedEventArgs e) => SystemCommands.CloseWindow(this);

    private void OnThemeChanged(object? sender, EventArgs e) => WindowEffects.SetDarkMode(this, ThemeService.IsDark);

    private void OnLanguageChanged(object? sender, EventArgs e)
    {
        if (IsLoaded) UpdateStepText();
    }

    protected override void OnClosed(EventArgs e)
    {
        foreach (var key in _stories) ((Storyboard)Resources[key]).Stop(this);
        ThemeService.ThemeChanged -= OnThemeChanged;
        LanguageService.LanguageChanged -= OnLanguageChanged;
        base.OnClosed(e);
    }
}
