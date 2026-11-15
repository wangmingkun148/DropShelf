// “关于”窗口：点主窗口右下角的版本号打开。程序和数据的位置、语言包、检查更新、退出与卸载。和 C# 版的 AboutWindow 对应。
#pragma once

#include "base.h"

/// 在 owner 窗口中央打开（模态：关掉之前 owner 不能操作）。
void about_window_show(HWND owner);
void about_window_close(void);
bool about_window_is_open(void);
