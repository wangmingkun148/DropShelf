// 运行日志：%LOCALAPPDATA%\DropShelf\Logs\dropshelf-yyyy-MM-dd.log，格式与 C# 版相同。
#pragma once

#include "base.h"

/// 日志所在的文件夹；环境变量 DROPSHELF_LOG_DIRECTORY 可以改到别处。调用方负责 free。
wchar_t *log_directory(void);

void log_info(const wchar_t *format, ...);

/// 设置环境变量 DROPSHELF_TRACE=1 时记录手势和拖放的诊断信息。
bool log_trace_enabled(void);
void log_trace(const wchar_t *format, ...);

void log_error(const wchar_t *format, ...);

/// 记录一条错误，后面跟上 Windows 错误码和它的说明。
void log_win32_error(const wchar_t *message, DWORD code);

/// 删除 14 天前的日志，避免长期运行后占用空间。
void log_cleanup_old(void);
