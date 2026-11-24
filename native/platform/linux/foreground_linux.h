// 前台程序的识别（Linux 上就是 Hyprland 的窗口 class / app_id）。解析函数是纯的，方便测试。
#pragma once

#include "base.h"

/// 从 hyprctl -j activewindow 的回复里取应用标识；没有活动的窗口时返回 NULL。
wchar_t *foreground_active_class(const wchar_t *json);

/// 从 hyprctl -j activewindow 的回复里看前台是不是全屏（游戏、演示、视频）。
bool foreground_active_fullscreen(const wchar_t *json);

/// 现在前台有没有全屏的窗口。全屏时晃动和边缘停留要暂停。
bool foreground_is_fullscreen(void);

/// 从 hyprctl -j clients 的回复里取有窗口的程序，去重、排序（不含本程序）。
void foreground_clients_apps(const wchar_t *json, StrList *names);

/// 光标下（可能叠着好几个）窗口的应用标识。
void foreground_clients_at(const wchar_t *json, int x, int y, StrList *classes);