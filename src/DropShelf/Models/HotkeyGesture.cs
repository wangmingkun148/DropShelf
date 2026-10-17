using System.Windows.Input;

namespace DropShelf.Models;

/// <summary>全局快捷键，例如 Ctrl+Shift+Space。至少包含 Ctrl、Alt 或 Win 之一（F1–F24 例外）。</summary>
public readonly record struct HotkeyGesture(ModifierKeys Modifiers, Key Key)
{
    public static HotkeyGesture None => default;
    public static HotkeyGesture Default => new(ModifierKeys.Control | ModifierKeys.Shift, Key.Space);

    public bool IsEmpty => Key == Key.None;

    public bool IsValid =>
        !IsEmpty && !IsModifierKey(Key) &&
        ((Modifiers & (ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Windows)) != 0 || Key is >= Key.F1 and <= Key.F24);

    public static bool IsModifierKey(Key key) => key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or
        Key.LeftShift or Key.RightShift or Key.LWin or Key.RWin or Key.System or Key.None;

    public override string ToString()
    {
        var parts = new List<string>(5);
        if (Modifiers.HasFlag(ModifierKeys.Control)) parts.Add("Ctrl");
        if (Modifiers.HasFlag(ModifierKeys.Alt)) parts.Add("Alt");
        if (Modifiers.HasFlag(ModifierKeys.Shift)) parts.Add("Shift");
        if (Modifiers.HasFlag(ModifierKeys.Windows)) parts.Add("Win");
        if (!IsEmpty) parts.Add(KeyName(Key));
        return string.Join("+", parts);
    }

    public static bool TryParse(string? text, out HotkeyGesture gesture)
    {
        gesture = None;
        if (string.IsNullOrWhiteSpace(text)) return true;

        var modifiers = ModifierKeys.None;
        var key = Key.None;
        foreach (var raw in text.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            switch (raw.ToLowerInvariant())
            {
                case "ctrl" or "control":
                    modifiers |= ModifierKeys.Control;
                    break;
                case "alt":
                    modifiers |= ModifierKeys.Alt;
                    break;
                case "shift":
                    modifiers |= ModifierKeys.Shift;
                    break;
                case "win" or "windows":
                    modifiers |= ModifierKeys.Windows;
                    break;
                default:
                    if (key != Key.None || !TryParseKey(raw, out key)) return false;
                    break;
            }
        }

        gesture = new HotkeyGesture(modifiers, key);
        return gesture.IsValid;
    }

    private static string KeyName(Key key) => key switch
    {
        >= Key.D0 and <= Key.D9 => ((int)(key - Key.D0)).ToString(),
        >= Key.NumPad0 and <= Key.NumPad9 => "Num" + (int)(key - Key.NumPad0),
        Key.Space => "Space",
        Key.Return => "Enter",
        Key.Escape => "Esc",
        Key.OemTilde => "`",
        Key.OemMinus => "-",
        Key.OemPlus => "=",
        Key.OemComma => ",",
        Key.OemPeriod => ".",
        Key.OemQuestion => "/",
        Key.OemSemicolon => ";",
        Key.OemQuotes => "'",
        Key.OemOpenBrackets => "[",
        Key.OemCloseBrackets => "]",
        Key.OemPipe => "\\",
        _ => key.ToString()
    };

    private static bool TryParseKey(string text, out Key key)
    {
        key = text switch
        {
            "`" => Key.OemTilde,
            "-" => Key.OemMinus,
            "=" => Key.OemPlus,
            "," => Key.OemComma,
            "." => Key.OemPeriod,
            "/" => Key.OemQuestion,
            ";" => Key.OemSemicolon,
            "'" => Key.OemQuotes,
            "[" => Key.OemOpenBrackets,
            "]" => Key.OemCloseBrackets,
            "\\" => Key.OemPipe,
            "Enter" => Key.Return,
            "Esc" => Key.Escape,
            _ => Key.None
        };
        if (key != Key.None) return true;
        if (text.Length == 1 && char.IsDigit(text[0]))
        {
            key = Key.D0 + (text[0] - '0');
            return true;
        }
        if (text.StartsWith("Num", StringComparison.OrdinalIgnoreCase) && text.Length == 4 && char.IsDigit(text[3]))
        {
            key = Key.NumPad0 + (text[3] - '0');
            return true;
        }
        return Enum.TryParse(text, true, out key) && key != Key.None;
    }
}
