// 读取拖进来（或从剪贴板粘贴）的内容，变成置物架上的项目。和 C# 版的 DropDataReader.ReadAsync 对应。
// 来源程序交出的数据必须在松手的回调里当场取得（之后来源程序可能已经删掉临时文件、不再提供附件流），
// 写盘、复制和统计文件信息放到后台，做完回到界面线程。
#pragma once

#include "drop_data.h"
#include "shelf_item.h"

typedef struct DroppedEntry
{
    ShelfItemKind kind;
    wchar_t *value;
    wchar_t *title;
    bool temporary;
    // 文件和文件夹在后台读好的信息（with_facts 时）。
    bool has_facts;             // false 表示读不到（已删除、没有权限），这一项放不进来
    FileFacts facts;
    wchar_t *type_name;
} DroppedEntry;

/// 读取结果。caches 里的每个缓存文件夹都替接收方持有一次：放进置物架后由置物架接管，不用时要释放。
typedef struct DropReadResult
{
    DroppedEntry *entries;
    int count;
    StrList caches;
    int failed;
    wchar_t *error;             // 什么都没读到时的说明，可以是 NULL
    bool with_facts;
} DropReadResult;

void drop_result_free(DropReadResult *result);               // 不释放缓存
void drop_result_release_caches(DropReadResult *result);
int drop_result_file_count(const DropReadResult *result);
bool drop_result_has_temporary(const DropReadResult *result);

/// 读完后在界面线程调用；由 done 负责 drop_result_free（和释放用不到的缓存）。
typedef void (*DropReadDone)(DropReadResult *result, void *context);

/// 读取 data。必须在松手的回调（或刚取到剪贴板数据时）调用。files_only：只要文件（交给快速操作）。
/// with_facts：顺便在后台读好文件是不是文件夹、有多大、是什么类型。cancelled 变为非零时尽快结束（置物架关闭了）。
/// 返回知道的文件个数（读取较慢时用来显示“正在读取 N 个文件”），不知道时为 0。
int drop_read(IDataObject *data, bool files_only, bool with_facts, volatile LONG *cancelled, DropReadDone done, void *context);

/// 把这些路径当作放进来的文件，在后台读好文件信息后交给 done（命令行、恢复最近关闭的置物架用）。
void drop_read_paths(const StrList *paths, DropReadDone done, void *context);

// ───────────── 聊天软件 ─────────────

/// 文件是否位于来源程序随时可能删除的临时位置（系统临时文件夹、聊天软件的缓存）。DropShelf 自己的缓存不算。
bool chat_is_volatile_path(const wchar_t *path);
