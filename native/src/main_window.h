// 主窗口。第 2 步先做一页用界面底座搭出来的设置（语言、开机启动、快捷键、低性能模式、主题），
// 都是真正生效的设置；完整的各页、概览页的卡牌翻页在第 7 步移植。
#pragma once

#include "base.h"

/// 打开主窗口；已经打开时切到前面。
void main_window_show(void);
void main_window_close(void);
bool main_window_is_open(void);

/// 设置变了（可能是别处改的）：让窗口上的控件和说明文字跟上。
void main_window_refresh(void);
