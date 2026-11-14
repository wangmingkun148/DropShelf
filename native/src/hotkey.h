// 全局快捷键：写法（如 Ctrl+Shift+Space）的解析与显示，以及向系统注册。
#pragma once

#include "base.h"

/// 修饰键用 RegisterHotKey 的 MOD_ALT、MOD_CONTROL、MOD_SHIFT、MOD_WIN；key 是虚拟键码，0 表示没有设置。
typedef struct HotkeyGesture
{
    unsigned modifiers;
    unsigned key;
} HotkeyGesture;

#define HOTKEY_NONE ((HotkeyGesture){ 0, 0 })
#define HOTKEY_DEFAULT ((HotkeyGesture){ MOD_CONTROL | MOD_SHIFT, VK_SPACE })

bool hotkey_is_empty(HotkeyGesture gesture);

/// 至少包含 Ctrl、Alt 或 Win 之一（F1–F24 例外），而且主键不能是修饰键本身。
bool hotkey_is_valid(HotkeyGesture gesture);
bool hotkey_is_modifier_key(unsigned key);

/// 写成 Ctrl+Shift+Space 这样的文字，没有设置时是空字符串。调用方负责 free。
wchar_t *hotkey_to_text(HotkeyGesture gesture);

/// 空白的文字表示关闭快捷键，算解析成功；写法不对或组合无效时返回 false。
bool hotkey_parse(const wchar_t *text, HotkeyGesture *gesture);

// ───────────── 注册 ─────────────

/// 在 window 上注册快捷键，按下时调用 pressed。
void hotkey_service_start(HWND window, void (*pressed)(void));

/// 注册新的快捷键；失败时（通常是被其他程序占用）返回 false，之前的快捷键已被注销。
bool hotkey_service_register(HotkeyGesture gesture);

/// 暂停时注销快捷键，按键交给前台的程序；恢复时重新注册。
void hotkey_service_set_paused(bool paused);

/// 处理窗口消息；是这个快捷键的 WM_HOTKEY 时返回 true。
bool hotkey_service_handle_message(UINT message, WPARAM wparam);
void hotkey_service_stop(void);
