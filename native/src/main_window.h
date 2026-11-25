// 主窗口：概览页列出各项设置，点开卡片翻成对应的设置页；底部是最近关闭的置物架。和 C# 版的 MainWindow 对应。
#pragma once

#include "base.h"

typedef struct UiWindow UiWindow;

/// 打开主窗口；已经打开时切到前面。
void main_window_show(void);
void main_window_close(void);
bool main_window_is_open(void);

/// 设置变了（可能是别处改的）：让窗口上的控件和说明文字跟上。
void main_window_refresh(void);

/// 换风格：theme_set_look 之前调 prepare（按原来的风格拆掉内容），之后调 apply（按新风格重建，回到原来那一页）。
/// 窗口不关，位置和大小不变。主窗口没打开时什么也不做。
void main_window_prepare_look(void);
void main_window_apply_look(void);

/// 主窗口的界面窗口；没有打开时返回 NULL（测试和引导窗口用）。
UiWindow *main_window_ui(void);
