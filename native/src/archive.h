// 快速压缩和解压：单个 .zip 或 .7z 解压到同名文件夹，其余打包成 .zip。和 C# 版的 ArchiveService 对应。
// ZIP 用 miniz，7z 交给 Windows 自带的 tar.exe。解压前校验条目路径和大小，不会写到目标文件夹外，也不会把磁盘写满。
// 同步执行，在后台线程上调用。
#pragma once

#include "quick_action.h"

/// output_directory 为 NULL 时放在源文件旁边。
void archive_run(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result);

/// 打包成 .zip。
void archive_compress(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result);

/// 解压后的文件带上压缩包的下载来源标记，和资源管理器一样。
void zone_identifier_propagate(const wchar_t *archive, const wchar_t *directory);
