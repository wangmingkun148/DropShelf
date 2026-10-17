using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Models;

namespace DropShelf.Views.Controls;

/// <summary>Windows 11 设置页风格的一行设置：图标、标题、说明，右侧放置控件。</summary>
public class SettingsCard : ContentControl
{
    public static readonly DependencyProperty HeaderProperty =
        DependencyProperty.Register(nameof(Header), typeof(string), typeof(SettingsCard), new PropertyMetadata(string.Empty));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(nameof(Description), typeof(string), typeof(SettingsCard), new PropertyMetadata(null));

    public static readonly DependencyProperty IconProperty =
        DependencyProperty.Register(nameof(Icon), typeof(string), typeof(SettingsCard), new PropertyMetadata(null));

    static SettingsCard()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(SettingsCard), new FrameworkPropertyMetadata(typeof(SettingsCard)));
    }

    public string Header
    {
        get => (string)GetValue(HeaderProperty);
        set => SetValue(HeaderProperty, value);
    }

    public string? Description
    {
        get => (string?)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public string? Icon
    {
        get => (string?)GetValue(IconProperty);
        set => SetValue(IconProperty, value);
    }
}

/// <summary>不确定进度的环形指示器。</summary>
public class ProgressRing : Control
{
    private RotateTransform? _rotation;

    static ProgressRing()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(ProgressRing), new FrameworkPropertyMetadata(typeof(ProgressRing)));
    }

    public ProgressRing()
    {
        IsVisibleChanged += (_, _) => UpdateAnimation();
    }

    public override void OnApplyTemplate()
    {
        base.OnApplyTemplate();
        _rotation = GetTemplateChild("PART_Rotation") as RotateTransform;
        UpdateAnimation();
    }

    private void UpdateAnimation()
    {
        if (_rotation is null) return;
        if (IsVisible)
            _rotation.BeginAnimation(RotateTransform.AngleProperty,
                new DoubleAnimation(0, 360, TimeSpan.FromSeconds(0.9)) { RepeatBehavior = RepeatBehavior.Forever });
        else
            _rotation.BeginAnimation(RotateTransform.AngleProperty, null);
    }
}

/// <summary>点击后录制一组全局快捷键；Esc 取消，Backspace 清除。</summary>
public class HotkeyBox : Control
{
    public static readonly DependencyProperty GestureProperty = DependencyProperty.Register(
        nameof(Gesture), typeof(HotkeyGesture), typeof(HotkeyBox),
        new FrameworkPropertyMetadata(HotkeyGesture.None, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault,
            (d, _) => ((HotkeyBox)d).UpdateDisplay()));

    private static readonly DependencyPropertyKey IsRecordingKey = DependencyProperty.RegisterReadOnly(
        nameof(IsRecording), typeof(bool), typeof(HotkeyBox), new PropertyMetadata(false));

    public static readonly DependencyProperty IsRecordingProperty = IsRecordingKey.DependencyProperty;

    private static readonly DependencyPropertyKey DisplayTextKey = DependencyProperty.RegisterReadOnly(
        nameof(DisplayText), typeof(string), typeof(HotkeyBox), new PropertyMetadata("未设置"));

    public static readonly DependencyProperty DisplayTextProperty = DisplayTextKey.DependencyProperty;

    static HotkeyBox()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(HotkeyBox), new FrameworkPropertyMetadata(typeof(HotkeyBox)));
        FocusableProperty.OverrideMetadata(typeof(HotkeyBox), new FrameworkPropertyMetadata(true));
    }

    public HotkeyGesture Gesture
    {
        get => (HotkeyGesture)GetValue(GestureProperty);
        set => SetValue(GestureProperty, value);
    }

    public bool IsRecording => (bool)GetValue(IsRecordingProperty);
    public string DisplayText => (string)GetValue(DisplayTextProperty);

    public override void OnApplyTemplate()
    {
        base.OnApplyTemplate();
        if (GetTemplateChild("PART_Clear") is Button clear)
            clear.Click += (_, _) =>
            {
                Gesture = HotkeyGesture.None;
                StopRecording();
            };
        UpdateDisplay();
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        base.OnMouseLeftButtonDown(e);
        Focus();
        SetValue(IsRecordingKey, true);
        SetValue(DisplayTextKey, "请按下快捷键…");
        e.Handled = true;
    }

    protected override void OnLostKeyboardFocus(KeyboardFocusChangedEventArgs e)
    {
        base.OnLostKeyboardFocus(e);
        StopRecording();
    }

    protected override void OnPreviewKeyDown(System.Windows.Input.KeyEventArgs e)
    {
        base.OnPreviewKeyDown(e);
        if (!IsRecording)
        {
            if (e.Key is Key.Enter or Key.Space)
            {
                SetValue(IsRecordingKey, true);
                SetValue(DisplayTextKey, "请按下快捷键…");
                e.Handled = true;
            }
            return;
        }

        e.Handled = true;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        var modifiers = Keyboard.Modifiers;
        if (IsWindowsKeyDown()) modifiers |= ModifierKeys.Windows;

        if (key == Key.Escape && modifiers == ModifierKeys.None)
        {
            StopRecording();
            return;
        }

        if (key is Key.Back or Key.Delete && modifiers == ModifierKeys.None)
        {
            Gesture = HotkeyGesture.None;
            StopRecording();
            return;
        }

        if (HotkeyGesture.IsModifierKey(key))
        {
            var preview = new HotkeyGesture(modifiers, Key.None).ToString();
            SetValue(DisplayTextKey, string.IsNullOrEmpty(preview) ? "请按下快捷键…" : preview + "+…");
            return;
        }

        var gesture = new HotkeyGesture(modifiers, key);
        if (!gesture.IsValid)
        {
            SetValue(DisplayTextKey, "需要搭配 Ctrl、Alt 或 Win");
            return;
        }

        Gesture = gesture;
        StopRecording();
    }

    private static bool IsWindowsKeyDown() => Keyboard.IsKeyDown(Key.LWin) || Keyboard.IsKeyDown(Key.RWin);

    private void StopRecording()
    {
        SetValue(IsRecordingKey, false);
        UpdateDisplay();
    }

    private void UpdateDisplay()
    {
        if (IsRecording) return;
        SetValue(DisplayTextKey, Gesture.IsEmpty ? "未设置" : Gesture.ToString());
    }
}
