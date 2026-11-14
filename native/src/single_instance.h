// 单实例：各种名称，以及把命令行转交给已经运行的实例。名称和转交的格式与 C# 版相同。
#pragma once

#include "base.h"

/// 以下名称由调用方 free。开发调试时可通过环境变量 DROPSHELF_INSTANCE 使用独立实例，避免与已安装的版本冲突。
wchar_t *single_instance_mutex_name(void);
wchar_t *single_instance_show_event_name(void);
wchar_t *single_instance_window_title(void);

/// 命令行包含 --shelf 或直接带有文件路径（“发送到”菜单、拖到程序图标上）。arguments 不含程序自己的路径。
bool single_instance_is_shelf_request(const StrList *arguments);

/// 命令行里确实存在的文件和文件夹。
void single_instance_path_arguments(const StrList *arguments, StrList *paths);

/// 已在运行：“发送到”或拖到图标上的文件交给现有实例，否则唤醒它的主窗口。
void single_instance_hand_over(const StrList *arguments);

/// 把命令行发给标题为 title 的消息窗口；找不到窗口或对方没有回应时返回 false。
bool single_instance_send_arguments(const wchar_t *title, const StrList *arguments);

/// 读出 WM_COPYDATA 里转交过来的命令行；不是本程序发来的返回 false。
bool single_instance_read_arguments(const COPYDATASTRUCT *data, StrList *arguments);
