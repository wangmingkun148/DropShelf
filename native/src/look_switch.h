// 换风格（简洁、纸牌）：颜色、主窗口、开着的置物架和其他窗口一起换。有动画效果时主窗口和置物架像一串多米诺一样依次翻成新风格。
#pragma once

#include "theme.h"

/// 设置里的风格变了之后调用（app_update_settings 里）。和现在的一样时什么也不做。
void look_switch_to(Look look);

/// 正在翻牌换风格（测试用：等翻完再检查）。
bool look_switch_busy(void);
