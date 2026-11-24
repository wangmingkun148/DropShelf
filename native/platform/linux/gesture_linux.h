// 手势识别里和显示器有关的纯判断，单独拿出来方便测试。
#pragma once

#include "base.h"
#include "gesture.h"
#include "hyprland_linux.h"

/// 光标贴在哪条屏幕外侧边缘上：左、右、上三边，边缘外没有别的显示器。
/// 面板等保留的区域（工作区以外）不算，避免和拖到面板上切换窗口冲突。
ScreenEdge gesture_edge_at(const HyprMonitor *monitors, int count, int x, int y);

/// 全局左键现在是不是按着：1 按住、0 松开、-1 不知道（没有 evdev 权限或没找到鼠标）。
/// 拖动层壳窗口时用它在自己的事件之外确认松手，GTK 偶尔漏掉 release 时窗口不会一直粘着鼠标。
int gesture_left_button_state(void);