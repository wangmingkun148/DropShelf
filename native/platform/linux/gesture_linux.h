// 手势识别里和显示器有关的纯判断，单独拿出来方便测试。
#pragma once

#include "base.h"
#include "gesture.h"
#include "hyprland_linux.h"

/// 光标贴在哪条屏幕外侧边缘上：左、右、上三边，边缘外没有别的显示器。
/// 面板等保留的区域（工作区以外）不算，避免和拖到面板上切换窗口冲突。
ScreenEdge gesture_edge_at(const HyprMonitor *monitors, int count, int x, int y);