// 通知区域（托盘）图标：点击打开设置，右键弹出菜单。
#pragma once

#include "base.h"

/// 托盘菜单里各项要做的事，由程序总控提供。
typedef struct TrayActions
{
    void (*show_window)(void);
    void (*new_shelf)(void);
    void (*quit)(void);
    wchar_t *(*hotkey_text)(void);      // 显示在“新建置物架”右边的快捷键，调用方 free；没有设置时为空字符串
} TrayActions;

/// 在 window（程序的消息窗口）上添加托盘图标。
void tray_create(HWND window, const TrayActions *actions);

/// 处理消息窗口收到的消息；是托盘的消息并且已经处理时返回 true。
bool tray_handle_message(UINT message, WPARAM wparam, LPARAM lparam);

/// 切换界面语言后更新托盘提示文字。
void tray_refresh_text(void);
/// 换风格后换上那种风格的图标。
void tray_refresh_icon(void);

/// 关闭主窗口后提示一次：DropShelf 仍在运行。
void tray_show_still_running_hint(void);

/// 在托盘图标旁显示一条通知。
void tray_show_notice(const wchar_t *title, const wchar_t *body);

void tray_destroy(void);
