// 自绘的文件属性窗口（Linux 没有系统提供的属性对话框）。
#pragma once

#include "base.h"
#include "quick_action.h"

/// 显示这些项目的属性。已经打开时只带到前面。结果写进 result。
bool properties_show(HWND owner, const StrList *paths, QuickActionResult *result);