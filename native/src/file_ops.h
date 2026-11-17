// 复制和移动文件、文件夹，和 C# 版的 FileOperationService 对应。都是同步的，在后台线程上调用。
#pragma once

#include "base.h"
#include "strings.h"

/// 解压和接收附件时给磁盘留下的最少剩余空间。
#define DISK_RESERVE (512ll * 1024 * 1024)

typedef struct FileOpResult
{
    wchar_t *source;
    wchar_t *destination;       // 新位置；还没算出来就失败时是 NULL
    bool succeeded;
    bool source_left;           // 移动时新位置已经是完整的一份，但原来的没能全部删掉（被占用等）；仍算成功
    StringId error;             // 失败时给用户看的原因
} FileOpResult;

typedef struct FileOpResults
{
    FileOpResult *items;
    int count;
} FileOpResults;

/// 逐个复制到 destination 文件夹（不存在时创建），重名时加 (2)、(3)。连同下载来源标记、属性和修改时间一起保留。
FileOpResults file_ops_copy(const StrList *sources, const wchar_t *destination);

/// 逐个移动；不能直接移动（跨磁盘、被占用）时先复制再删除原来的。复制完了原来的却删不干净时，
/// 新位置的那份是完整的，按成功算，source_left 标出原位置还有残留。
FileOpResults file_ops_move(const StrList *sources, const wchar_t *destination);

void file_op_results_free(FileOpResults *results);

/// 路径非空且文件或文件夹存在。
bool path_is_available(const wchar_t *path);

/// 目标文件夹里还没被占用的路径：重名时依次加上 (2)、(3)…；重名项目过多时返回 NULL。调用方负责 free。
wchar_t *path_available(const wchar_t *directory, const wchar_t *base_name, const wchar_t *extension);

/// 和 .NET 的 Path.GetFileNameWithoutExtension / GetExtension 相同的拆法：扩展名从最后一个点开始（含点）。
void path_split_name(const wchar_t *name, wchar_t **stem, wchar_t **extension);

/// 完整路径（展开相对路径和 . ..）。失败时返回副本。调用方负责 free。
wchar_t *path_full(const wchar_t *path);

/// 去掉末尾的分隔符；磁盘根目录（C:\）保留。调用方负责 free。
wchar_t *path_trim_end_separator(const wchar_t *path);

/// 文件夹的名称：最后一段，磁盘根目录时是路径本身。调用方负责 free。
wchar_t *path_folder_name(const wchar_t *folder);

/// path 就是 folder 或在它里面（不分大小写，都先转成完整路径）。
bool path_is_same_or_inside(const wchar_t *path, const wchar_t *folder);

/// path 直接放在 folder 里（上一级就是 folder）。
bool path_is_direct_child(const wchar_t *path, const wchar_t *folder);

/// 目录所在磁盘的剩余空间；查不到（网络位置等）时返回 -1。
long long disk_available(const wchar_t *directory);

/// 下载文件夹（不存在时创建）；临时附件处理后的结果放在这里，置物架关闭后也不会被清理。调用方负责 free。
wchar_t *persistent_output_directory(void);
