using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using DropShelf.Interop;
using DropShelf.Services;

namespace DropShelf.Views;

public partial class OnboardingWindow : Window
{
    private static readonly (string Title, string Body)[] Steps =
    [
        ("欢迎使用 DropShelf", "一个随叫随到的临时置物架：把文件先放进来，换个窗口、换个文件夹，再从这里拖出去。"),
        ("拖住文件，晃一晃", "在资源管理器、桌面、浏览器或聊天软件里拖住文件，快速左右晃动鼠标，置物架就会出现在光标处。"),
        ("放进来，再拖出去", "松开即可暂存。拖动缩略图堆叠可一次带走全部文件；点开堆叠后也能多选，只拖走需要的部分。"),
        ("快速操作与快捷键", "把文件拖到底部按钮上，可以直接压缩、复制路径、移动或查看属性。按 Ctrl+Shift+Space 也能随时打开空置物架。")
    ];

    private readonly FrameworkElement[] _art;
    private readonly string[] _stories = ["Step1Story", "Step2Story", "Step3Story", "Step4Story"];
    private int _step;

    public OnboardingWindow()
    {
        InitializeComponent();
        _art = [Step1Art, Step2Art, Step3Art, Step4Art];
        for (var index = 0; index < Steps.Length; index++)
            Dots.Children.Add(new System.Windows.Shapes.Rectangle { Width = 6, Height = 6, RadiusX = 3, RadiusY = 3, Margin = new Thickness(0, 0, 8, 0) });

        SourceInitialized += (_, _) =>
        {
            if (WindowEffects.TryApplyBackdrop(this, SystemBackdrop.Mica))
                Background = System.Windows.Media.Brushes.Transparent;
            WindowEffects.SetDarkMode(this, ThemeService.IsDark);
        };
        ThemeService.ThemeChanged += OnThemeChanged;
        PreviewKeyDown += OnPreviewKeyDown;
        Loaded += (_, _) => ShowStep(0);
    }

    public int CurrentStep => _step;

    public void ShowStep(int step)
    {
        _step = Math.Clamp(step, 0, Steps.Length - 1);
        for (var index = 0; index < _art.Length; index++)
        {
            var active = index == _step;
            _art[index].Visibility = active ? Visibility.Visible : Visibility.Collapsed;
            var story = (Storyboard)Resources[_stories[index]];
            if (active) story.Begin(this, true);
            else story.Stop(this);
        }

        StepTitleText.Text = Steps[_step].Title;
        StepBodyText.Text = _step == Steps.Length - 1 ? BuildLastStepText() : Steps[_step].Body;
        for (var index = 0; index < Dots.Children.Count; index++)
        {
            var dot = (System.Windows.Shapes.Rectangle)Dots.Children[index];
            dot.Width = index == _step ? 18 : 6;
            dot.SetResourceReference(Shape.FillProperty, index == _step ? "AccentBrush" : "ControlStrongBorderBrush");
            dot.Opacity = index == _step ? 1 : 0.4;
        }

        BackButton.Visibility = _step == 0 ? Visibility.Collapsed : Visibility.Visible;
        SkipButton.Visibility = _step == Steps.Length - 1 ? Visibility.Collapsed : Visibility.Visible;
        NextButton.Content = _step == Steps.Length - 1 ? "开始使用" : "下一步";
    }

    private static string BuildLastStepText()
    {
        var hotkey = AppController.Current?.Settings.HotkeyGesture ?? Models.HotkeyGesture.Default;
        return hotkey.IsEmpty
            ? "把文件拖到底部按钮上，可以直接压缩、复制路径、移动或查看属性。也可以在设置中添加全局快捷键，随时打开空置物架。"
            : $"把文件拖到底部按钮上，可以直接压缩、复制路径、移动或查看属性。按 {hotkey} 也能随时打开空置物架。";
    }

    private void OnNextClick(object sender, RoutedEventArgs e)
    {
        if (_step >= Steps.Length - 1) Close();
        else ShowStep(_step + 1);
    }

    private void OnBackClick(object sender, RoutedEventArgs e) => ShowStep(_step - 1);

    private void OnSkipClick(object sender, RoutedEventArgs e) => Close();

    private void OnPreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Right:
                if (_step < Steps.Length - 1) ShowStep(_step + 1);
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

    protected override void OnClosed(EventArgs e)
    {
        foreach (var key in _stories) ((Storyboard)Resources[key]).Stop(this);
        ThemeService.ThemeChanged -= OnThemeChanged;
        base.OnClosed(e);
    }
}
