#include "hotkey.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

typedef struct KeyName
{
    const wchar_t *name;
    unsigned key;
} KeyName;

/// 设置文件里写的符号键。
static const KeyName symbol_keys[] = {
    { L"`", VK_OEM_3 }, { L"-", VK_OEM_MINUS }, { L"=", VK_OEM_PLUS }, { L",", VK_OEM_COMMA }, { L".", VK_OEM_PERIOD },
    { L"/", VK_OEM_2 }, { L";", VK_OEM_1 }, { L"'", VK_OEM_7 }, { L"[", VK_OEM_4 }, { L"]", VK_OEM_6 }, { L"\\", VK_OEM_5 },
    { L"Enter", VK_RETURN }, { L"Esc", VK_ESCAPE }, { L"Space", VK_SPACE },
};

/// 其余按键的名称，与 C# 版用的 WPF 按键名称相同，旧设置文件里写的名称都能认出来。
/// 同一个键有几个名称时，显示用排在前面的那个。字母、数字、小键盘数字和 F1–F24 另外处理。
static const KeyName named_keys[] = {
    { L"Cancel", VK_CANCEL }, { L"Back", VK_BACK }, { L"Tab", VK_TAB }, { L"Clear", VK_CLEAR },
    { L"Return", VK_RETURN }, { L"Enter", VK_RETURN }, { L"Pause", VK_PAUSE },
    { L"Capital", VK_CAPITAL }, { L"CapsLock", VK_CAPITAL }, { L"Escape", VK_ESCAPE }, { L"Space", VK_SPACE },
    { L"PageUp", VK_PRIOR }, { L"Prior", VK_PRIOR }, { L"Next", VK_NEXT }, { L"PageDown", VK_NEXT },
    { L"End", VK_END }, { L"Home", VK_HOME }, { L"Left", VK_LEFT }, { L"Up", VK_UP }, { L"Right", VK_RIGHT }, { L"Down", VK_DOWN },
    { L"Select", VK_SELECT }, { L"Print", VK_PRINT }, { L"Execute", VK_EXECUTE },
    { L"Snapshot", VK_SNAPSHOT }, { L"PrintScreen", VK_SNAPSHOT }, { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE }, { L"Help", VK_HELP },
    { L"LWin", VK_LWIN }, { L"RWin", VK_RWIN }, { L"Apps", VK_APPS }, { L"Sleep", VK_SLEEP },
    { L"Multiply", VK_MULTIPLY }, { L"Add", VK_ADD }, { L"Separator", VK_SEPARATOR }, { L"Subtract", VK_SUBTRACT },
    { L"Decimal", VK_DECIMAL }, { L"Divide", VK_DIVIDE }, { L"NumLock", VK_NUMLOCK }, { L"Scroll", VK_SCROLL },
    { L"LeftShift", VK_LSHIFT }, { L"RightShift", VK_RSHIFT }, { L"LeftCtrl", VK_LCONTROL }, { L"RightCtrl", VK_RCONTROL },
    { L"LeftAlt", VK_LMENU }, { L"RightAlt", VK_RMENU }, { L"System", VK_MENU },
    { L"BrowserBack", VK_BROWSER_BACK }, { L"BrowserForward", VK_BROWSER_FORWARD }, { L"BrowserRefresh", VK_BROWSER_REFRESH },
    { L"BrowserStop", VK_BROWSER_STOP }, { L"BrowserSearch", VK_BROWSER_SEARCH }, { L"BrowserFavorites", VK_BROWSER_FAVORITES },
    { L"BrowserHome", VK_BROWSER_HOME }, { L"VolumeMute", VK_VOLUME_MUTE }, { L"VolumeDown", VK_VOLUME_DOWN }, { L"VolumeUp", VK_VOLUME_UP },
    { L"MediaNextTrack", VK_MEDIA_NEXT_TRACK }, { L"MediaPreviousTrack", VK_MEDIA_PREV_TRACK }, { L"MediaStop", VK_MEDIA_STOP },
    { L"MediaPlayPause", VK_MEDIA_PLAY_PAUSE }, { L"LaunchMail", VK_LAUNCH_MAIL }, { L"SelectMedia", VK_LAUNCH_MEDIA_SELECT },
    { L"LaunchApplication1", VK_LAUNCH_APP1 }, { L"LaunchApplication2", VK_LAUNCH_APP2 },
    { L"Oem1", VK_OEM_1 }, { L"OemSemicolon", VK_OEM_1 }, { L"OemPlus", VK_OEM_PLUS }, { L"OemComma", VK_OEM_COMMA },
    { L"OemMinus", VK_OEM_MINUS }, { L"OemPeriod", VK_OEM_PERIOD }, { L"OemQuestion", VK_OEM_2 }, { L"Oem2", VK_OEM_2 },
    { L"Oem3", VK_OEM_3 }, { L"OemTilde", VK_OEM_3 }, { L"AbntC1", 0xC1 }, { L"AbntC2", 0xC2 },
    { L"OemOpenBrackets", VK_OEM_4 }, { L"Oem4", VK_OEM_4 }, { L"Oem5", VK_OEM_5 }, { L"OemPipe", VK_OEM_5 },
    { L"Oem6", VK_OEM_6 }, { L"OemCloseBrackets", VK_OEM_6 }, { L"OemQuotes", VK_OEM_7 }, { L"Oem7", VK_OEM_7 },
    { L"Oem8", VK_OEM_8 }, { L"OemBackslash", VK_OEM_102 }, { L"Oem102", VK_OEM_102 },
};

#define COUNT_OF(array) ((int)(sizeof(array) / sizeof((array)[0])))

bool hotkey_is_empty(HotkeyGesture gesture)
{
    return gesture.key == 0;
}

bool hotkey_is_modifier_key(unsigned key)
{
    switch (key)
    {
        case 0:
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

bool hotkey_is_valid(HotkeyGesture gesture)
{
    if (hotkey_is_empty(gesture) || hotkey_is_modifier_key(gesture.key)) return false;
    return (gesture.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0 || (gesture.key >= VK_F1 && gesture.key <= VK_F24);
}

static void append_key_name(WStr *text, unsigned key)
{
    if (key >= L'0' && key <= L'9' || key >= L'A' && key <= L'Z')
    {
        wstr_append_char(text, (wchar_t)key);
        return;
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
    {
        wstr_appendf(text, L"Num%u", key - VK_NUMPAD0);
        return;
    }
    if (key >= VK_F1 && key <= VK_F24)
    {
        wstr_appendf(text, L"F%u", key - VK_F1 + 1);
        return;
    }
    for (int index = 0; index < COUNT_OF(symbol_keys); index++)
    {
        if (symbol_keys[index].key != key) continue;
        wstr_append(text, symbol_keys[index].name);
        return;
    }
    for (int index = 0; index < COUNT_OF(named_keys); index++)
    {
        if (named_keys[index].key != key) continue;
        wstr_append(text, named_keys[index].name);
        return;
    }
    wstr_appendf(text, L"%u", key);
}

wchar_t *hotkey_to_text(HotkeyGesture gesture)
{
    WStr text = {0};
    if (gesture.modifiers & MOD_CONTROL) wstr_append(&text, L"Ctrl+");
    if (gesture.modifiers & MOD_ALT) wstr_append(&text, L"Alt+");
    if (gesture.modifiers & MOD_SHIFT) wstr_append(&text, L"Shift+");
    if (gesture.modifiers & MOD_WIN) wstr_append(&text, L"Win+");
    if (!hotkey_is_empty(gesture)) append_key_name(&text, gesture.key);
    else if (text.length > 0) text.data[--text.length] = 0;
    return wstr_take(&text);
}

static bool parse_key(const wchar_t *text, unsigned *key)
{
    size_t length = wcslen(text);
    // 符号和 Enter、Esc 的写法区分大小写，和 C# 版一样。
    for (int index = 0; index < COUNT_OF(symbol_keys) - 1; index++)
    {
        if (wcscmp(symbol_keys[index].name, text) != 0) continue;
        *key = symbol_keys[index].key;
        return true;
    }
    if (length == 1 && text[0] >= L'0' && text[0] <= L'9')
    {
        *key = (unsigned)text[0];
        return true;
    }
    if (length == 4 && _wcsnicmp(text, L"Num", 3) == 0 && text[3] >= L'0' && text[3] <= L'9')
    {
        *key = VK_NUMPAD0 + (unsigned)(text[3] - L'0');
        return true;
    }
    if (length == 1 && (text[0] >= L'A' && text[0] <= L'Z' || text[0] >= L'a' && text[0] <= L'z'))
    {
        *key = (unsigned)towupper(text[0]);
        return true;
    }
    if (length == 2 && (text[0] == L'D' || text[0] == L'd') && text[1] >= L'0' && text[1] <= L'9')
    {
        *key = (unsigned)text[1];
        return true;
    }
    if ((length == 2 || length == 3) && (text[0] == L'F' || text[0] == L'f'))
    {
        wchar_t *end;
        long number = wcstol(text + 1, &end, 10);
        if (*end == 0 && text[1] != L'0' && text[1] != L'+' && text[1] != L'-' && number >= 1 && number <= 24)
        {
            *key = VK_F1 + (unsigned)number - 1;
            return true;
        }
    }
    if (length == 7 && _wcsnicmp(text, L"NumPad", 6) == 0 && text[6] >= L'0' && text[6] <= L'9')
    {
        *key = VK_NUMPAD0 + (unsigned)(text[6] - L'0');
        return true;
    }
    for (int index = 0; index < COUNT_OF(named_keys); index++)
    {
        if (_wcsicmp(named_keys[index].name, text) != 0) continue;
        *key = named_keys[index].key;
        return true;
    }
    return false;
}

bool hotkey_parse(const wchar_t *text, HotkeyGesture *gesture)
{
    *gesture = HOTKEY_NONE;
    if (wcs_is_blank(text)) return true;

    unsigned modifiers = 0;
    unsigned key = 0;
    const wchar_t *cursor = text;
    while (*cursor)
    {
        const wchar_t *end = wcschr(cursor, L'+');
        size_t length = end ? (size_t)(end - cursor) : wcslen(cursor);
        wchar_t *raw = wcs_dup_n(cursor, length);
        wchar_t *part = wcs_trim_dup(raw);
        free(raw);
        cursor += length + (end ? 1 : 0);

        bool ok = true;
        if (*part == 0) { /* 空的一段直接跳过 */ }
        else if (_wcsicmp(part, L"ctrl") == 0 || _wcsicmp(part, L"control") == 0) modifiers |= MOD_CONTROL;
        else if (_wcsicmp(part, L"alt") == 0) modifiers |= MOD_ALT;
        else if (_wcsicmp(part, L"shift") == 0) modifiers |= MOD_SHIFT;
        else if (_wcsicmp(part, L"win") == 0 || _wcsicmp(part, L"windows") == 0) modifiers |= MOD_WIN;
        else ok = key == 0 && parse_key(part, &key);
        free(part);
        if (!ok) return false;
    }

    HotkeyGesture parsed = { modifiers, key };
    if (!hotkey_is_valid(parsed)) return false;
    *gesture = parsed;
    return true;
}

// ───────────── 注册 ─────────────

#define HOTKEY_ID 0x4453

static HWND service_window;
static void (*service_pressed)(void);
static HotkeyGesture service_current;
static bool service_registered;
static bool service_paused;

static void service_unregister(void)
{
    if (!service_registered) return;
    UnregisterHotKey(service_window, HOTKEY_ID);
    service_registered = false;
}

void hotkey_service_start(HWND window, void (*pressed)(void))
{
    service_window = window;
    service_pressed = pressed;
}

bool hotkey_service_register(HotkeyGesture gesture)
{
    service_unregister();
    service_current = gesture;
    if (hotkey_is_empty(gesture) || !hotkey_is_valid(gesture)) return hotkey_is_empty(gesture);
    if (service_paused) return true;

    service_registered = RegisterHotKey(service_window, HOTKEY_ID, gesture.modifiers | MOD_NOREPEAT, gesture.key) != 0;
    if (!service_registered)
    {
        wchar_t *text = hotkey_to_text(gesture);
        log_info(L"快捷键 %ls 注册失败，可能已被其他程序占用", text);
        free(text);
    }
    return service_registered;
}

void hotkey_service_set_paused(bool paused)
{
    if (paused == service_paused) return;
    service_paused = paused;
    if (paused) service_unregister();
    else hotkey_service_register(service_current);
}

bool hotkey_service_handle_message(UINT message, WPARAM wparam)
{
    if (message != WM_HOTKEY || wparam != HOTKEY_ID) return false;
    if (service_pressed) service_pressed();
    return true;
}

void hotkey_service_stop(void)
{
    service_unregister();
    service_window = NULL;
    service_pressed = NULL;
}
