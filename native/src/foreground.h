// 监视前台窗口属于哪个程序。屏蔽列表中的程序在前台时暂停晃动、边缘停留和快捷键，
// 快捷键注销后按键直接交给该程序。列表为空时不挂钩。只在界面线程使用。
#pragma once

#include "base.h"

/// 屏蔽状态变化时调用。
void foreground_watcher_set_listener(void (*changed)(bool blocked));

/// 换一份屏蔽列表（程序文件名，如 Photoshop.exe，不分大小写）并立即按当前前台窗口判断一次。
void foreground_watcher_set_blocked_apps(const StrList *names);
bool foreground_watcher_is_blocked(void);

/// 按快捷键时再确认一次前台程序，并校正暂停状态。
bool foreground_watcher_check(void);

/// 识别到手势时再确认一次：前台窗口或光标下的窗口属于屏蔽的程序。
/// 光标下的也算：从别的程序把文件拖到屏蔽的程序上方时，前台仍是来源程序。
bool foreground_watcher_is_blocked_at(POINT point);
void foreground_watcher_stop(void);

/// 前台窗口是不是全屏的程序（游戏、全屏视频、演示）。识别到手势和按快捷键时现查，不跟踪变化：
/// 同一个窗口进出全屏（按 F11、双击视频）时没有通知，暂停状态一旦记下就恢复不了。
bool foreground_is_fullscreen(void);

/// 当前有可见窗口的程序文件名，按名称排序，不含本程序。
void foreground_windowed_apps(StrList *names);

/// 进程的程序文件名，如 Photoshop.exe。以管理员身份运行的程序也能读取；进程已经退出时返回 NULL。
wchar_t *process_file_name(DWORD process_id);
