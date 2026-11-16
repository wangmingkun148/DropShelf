// 第一次运行时的引导：四步动画演示，晃动呼出、放进拖出、快速操作。和 C# 版的 OnboardingWindow 对应。
#pragma once

#include "base.h"

/// 打开引导窗口；已经打开时切到前面。关掉后记下已完成第一次运行。
void onboarding_window_show(void);
void onboarding_window_close(void);
bool onboarding_window_is_open(void);
/// 现在在第几步（0 开始，测试用）；没有打开时返回 -1。
int onboarding_window_step(void);
void onboarding_window_go(int step);
/// 现在显示的标题、正文和“下一步”按钮上的文字（测试用）；没有打开时返回 NULL。
const wchar_t *onboarding_window_title_text(void);
const wchar_t *onboarding_window_body_text(void);
const wchar_t *onboarding_window_next_text(void);
