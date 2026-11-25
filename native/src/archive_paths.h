// 压缩和解压共用的路径整理：Windows（archive.c）和 Linux（archive_linux.c）都用这一份。
#pragma once

#include "base.h"

/// 存在的路径，转成完整路径，去掉重复。Windows 的文件名不分大小写，Linux 分（photo.JPG 和 photo.jpg 是两个文件）。
StrList archive_sources(const StrList *paths);

/// 输出放在哪里：指定了就用指定的；否则文件放在它旁边，文件夹放在它的上一级（磁盘根目录不行）。
wchar_t *archive_output_directory(const wchar_t *source, const wchar_t *requested);

/// 压缩包和解压文件夹的名字：文件去掉扩展名，文件夹用它的名称。
wchar_t *archive_source_name(const wchar_t *source);

/// 压缩包里还没用过的条目名：重名时加 (2)、(3)，分隔符换成 /。
/// 不分大小写地比较，解到 Windows 上也不会互相覆盖。
wchar_t *archive_entry_name(const wchar_t *requested, StrList *used);
