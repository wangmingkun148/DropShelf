// 开机启动：写在当前用户的注册表 Run 项里，登录后在后台运行，不弹出窗口。
#pragma once

#include "base.h"

/// 写进注册表的命令："程序路径" --background。调用方负责 free。
wchar_t *startup_build_command(const wchar_t *executable);

/// 打开或关闭开机启动。写入注册表失败时返回 false，已记入日志。
bool startup_set_enabled(bool enabled);
