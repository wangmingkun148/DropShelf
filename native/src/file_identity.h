// 文件在磁盘卷上的标识（卷序列号加文件编号），和 C# 版的 FileIdentity 对应。
// 改名、在同一个磁盘里移动都不会变，所以原来的路径找不到时可以按它找到文件现在在哪。
// 只在 NTFS 上使用：别的文件系统的编号不稳定。取标识和按标识查找都只打开文件的属性，不读内容，也不妨碍别的程序使用文件。
#pragma once

#include "base.h"

typedef struct FileIdentity
{
    DWORD volume_serial;
    ULONGLONG file_index;       // 0 表示没有标识
} FileIdentity;

static inline bool file_identity_valid(const FileIdentity *identity) { return identity->file_index != 0; }

/// 取文件或文件夹的标识；网络路径、非 NTFS 磁盘或打不开时返回 false（identity 清零）。可以在后台线程上调用。
bool file_identity_get(const wchar_t *path, FileIdentity *identity);

/// 按标识找文件现在的路径。original_path 是它原来的路径，用来确定在哪个磁盘上找；找不到、已经进了回收站，
/// 或者找到的不是同一类（文件变成了文件夹）时返回 NULL。可以在后台线程上调用。调用方负责 free。
wchar_t *file_identity_resolve(const FileIdentity *identity, const wchar_t *original_path, bool is_folder);
