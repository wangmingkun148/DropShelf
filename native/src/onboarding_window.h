// 第一次运行时的引导：五步动画演示，晃动呼出、放进拖出、快速操作，最后问要不要观察文件夹。和 C# 版的 OnboardingWindow 对应。
#pragma once

#include "base.h"
#include "ui.h"

/// 打开引导窗口；已经打开时切到前面。关掉后记下已完成第一次运行，也算问过要不要观察文件夹。
void onboarding_window_show(void);
/// 只有问观察文件夹的那一步（升级来的老用户没在引导里见过）。
void onboarding_window_show_watch(void);
bool onboarding_window_is_watch_only(void);
void onboarding_window_close(void);
bool onboarding_window_is_open(void);
/// 现在在第几步（0 开始，测试用）；没有打开时返回 -1。
int onboarding_window_step(void);
void onboarding_window_go(int step);
/// 现在显示的标题、正文、“下一步”和“添加文件夹”按钮上的文字（测试用）；没有打开或按钮没显示时返回 NULL。
const wchar_t *onboarding_window_title_text(void);
const wchar_t *onboarding_window_body_text(void);
const wchar_t *onboarding_window_next_text(void);
const wchar_t *onboarding_window_add_folder_text(void);

/// 换风格后换上那种风格的系统材质和图标（演示画面按新风格重画）。
void onboarding_window_apply_look(void);

/// 欢迎引导的窗口（换风格时一起翻牌）；没打开时是 NULL。
UiWindow *onboarding_window_ui(void);

/// 升级来的老用户：只问用哪种风格（引导里选风格的那一步），关掉后不再问。
void onboarding_window_show_look(void);
bool onboarding_window_is_look_only(void);
