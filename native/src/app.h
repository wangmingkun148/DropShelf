// 程序总控：持有设置并协调手势、置物架、托盘、快捷键和各个窗口。
#pragma once

#include "base.h"
#include "settings.h"

/// 读取设置，创建消息窗口、托盘图标，注册快捷键并开始识别手势。start_in_background 为 false 时随后打开主窗口。
bool app_start(HINSTANCE instance, bool start_in_background);

/// 处理消息直到退出，返回退出码。
int app_run(void);

/// 命令行（或另一个进程转交过来的命令行）：带文件时打开置物架并放入这些文件，否则打开主窗口。
void app_handle_arguments(const StrList *arguments);

const AppSettings *app_settings(void);
bool app_is_hotkey_registered(void);
bool app_is_gesture_running(void);

/// 修改设置并立即生效、自动保存。change 在设置的副本上修改。
void app_update_settings(void (*change)(AppSettings *settings, void *context), void *context);

/// 改所有置物架拖出时移动还是复制（TransferMode）。
void app_update_drag_out_mode(int mode);

/// 观察文件夹：加入（已经有了不重复加）、移除。
void app_watch_folder(const wchar_t *folder);
void app_unwatch_folder(const wchar_t *folder);

/// 选一个文件夹加入观察（对话框从下载文件夹开始）。取消时返回 false。
bool app_choose_watch_folder(HWND owner);

/// 已经问过要不要观察文件夹（引导里点了“添加文件夹”或“不用了”，或者关掉了引导）。
void app_mark_folder_watch_asked(void);

/// 修改开机启动；写入注册表失败时保持原值并返回 false。
bool app_set_start_with_windows(bool enabled);

void app_show_main_window(void);

/// 主窗口关掉了：第一次关掉时提示程序仍在通知区域运行。
void app_main_window_closed(void);
void app_new_shelf(void);
void app_exit(void);

/// 保存设置并释放托盘图标、快捷键等；退出和注销、关机时调用，可以重复调用。
void app_prepare_for_shutdown(void);
