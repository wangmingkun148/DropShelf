using System.Windows;
using System.Windows.Input;
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

    public void ShowStep(int step)
    {
        _step = Math.Clamp(step, 0, StepCount - 1);
        for (var index = 0; index < _art.Length; index++)
        {
            var active = index == _step;
            _art[index].Visibility = active ? Visibility.Visible : Visibility.Collapsed;
            var story = (Storyboard)Resources[_stories[index]];
            if (active) story.Begin(this, true);
            else story.Stop(this);
        }

        UpdateStepText();
        for (var index = 0; index < Dots.Children.Count; index++)
        {
            var dot = (System.Windows.Shapes.Rectangle)Dots.Children[index];
            dot.Width = index == _step ? 18 : 6;
            dot.SetResourceReference(Shape.FillProperty, index == _step ? "AccentBrush" : "ControlStrongBorderBrush");
            dot.Opacity = index == _step ? 1 : 0.4;
        }

        BackButton.Visibility = _step == 0 ? Visibility.Collapsed : Visibility.Visible;
        SkipButton.Visibility = _step == StepCount - 1 ? Visibility.Collapsed : Visibility.Visible;
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
