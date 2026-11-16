// 探针窗口：识别到手势后在光标下短暂放置的几乎透明的窗口。系统拖放会把正在拖动的数据交给它，
// 由此可以准确判断用户是否真的在拖文件、附件或文本，再决定是否弹出置物架。
#pragma once

#include "base.h"

/// 创建（先不显示）。确认正在拖放可接收的内容时调用 detected。需要先 OleInitialize。
bool probe_create(HINSTANCE instance, void (*detected)(void));

/// 以屏幕像素坐标为中心显示，0.9 秒内没有收到拖放就自己隐藏。
void probe_show_at(POINT center);
void probe_hide(void);
void probe_destroy(void);

/// 以 center 为中心放 width × height 的窗口，并保证完整落在 work 里（与 C# 版的 QuickShelfPositioner 相同）。
POINT probe_place(POINT center, RECT work, int width, int height);
