using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media.Animation;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.Views.Controls;

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
        nameof(DisplayText), typeof(string), typeof(HotkeyBox), new PropertyMetadata(string.Empty));

    public static readonly DependencyProperty DisplayTextProperty = DisplayTextKey.DependencyProperty;

    static HotkeyBox()
    {
        DefaultStyleKeyProperty.OverrideMetadata(typeof(HotkeyBox), new FrameworkPropertyMetadata(typeof(HotkeyBox)));
        FocusableProperty.OverrideMetadata(typeof(HotkeyBox), new FrameworkPropertyMetadata(true));
    }

    public HotkeyBox()
    {
        Loaded += (_, _) => LanguageService.LanguageChanged += OnLanguageChanged;
        Unloaded += (_, _) => LanguageService.LanguageChanged -= OnLanguageChanged;
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
        SetValue(DisplayTextKey, Strings.HotkeyPrompt);
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
                SetValue(DisplayTextKey, Strings.HotkeyPrompt);
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
            SetValue(DisplayTextKey, string.IsNullOrEmpty(preview) ? Strings.HotkeyPrompt : preview + "+…");
            return;
        }

        var gesture = new HotkeyGesture(modifiers, key);
        if (!gesture.IsValid)
        {
            SetValue(DisplayTextKey, Strings.HotkeyNeedsModifier);
            return;
        }

        Gesture = gesture;
        StopRecording();
    }

    private static bool IsWindowsKeyDown() => Keyboard.IsKeyDown(Key.LWin) || Keyboard.IsKeyDown(Key.RWin);

    private void OnLanguageChanged(object? sender, EventArgs e) => UpdateDisplay();

    private void StopRecording()
    {
        SetValue(IsRecordingKey, false);
        UpdateDisplay();
    }

    private void UpdateDisplay()
    {
        if (IsRecording) return;
        SetValue(DisplayTextKey, Gesture.IsEmpty ? Strings.HotkeyNone : Gesture.ToString());
    }
}
