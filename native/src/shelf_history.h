// 最近关闭的置物架，误关或退出后可以恢复，和 C# 版的 ShelfHistoryService 对应。
// 只记录文件和文件夹的路径与名称，临时附件、文本和链接不会记录；最多保留 10 个。
// 保存在设置文件旁边的 history.json，格式与 C# 版相同。只在界面线程上使用，保存在后台按顺序进行。
#pragma once

#include "shelf_item.h"

#define SHELF_HISTORY_CAPACITY 10

typedef struct ShelfHistoryItem
{
    wchar_t *path;
    wchar_t *name;
} ShelfHistoryItem;

/// 一条记录创建后不再修改；菜单打开期间拿着引用，记录被移除了也还能读。
typedef struct ShelfHistoryEntry
{
    LONG references;
    ULONGLONG closed_at;        // 关闭的时刻（UTC 的 FILETIME）
    ShelfHistoryItem *items;
    int count;
} ShelfHistoryEntry;

ShelfHistoryEntry *shelf_history_entry_retain(ShelfHistoryEntry *entry);
void shelf_history_entry_release(ShelfHistoryEntry *entry);

/// 读取记录；path 为 NULL 时用设置文件旁边的 history.json。之前读过的先丢掉（测试用）。
void shelf_history_load(const wchar_t *path);

/// 现在的记录，最近关闭的在前。数组属于历史，下一次改动后失效。
ShelfHistoryEntry *const *shelf_history_entries(int *count);
ShelfHistoryEntry *shelf_history_latest(void);

/// 记录一个刚关闭的置物架；没有可记录的文件时返回 false。内容相同的旧记录会被新记录取代。
bool shelf_history_record(ShelfItem *const *items, int count);
/// 同上，指定关闭的时刻（UTC 的 FILETIME，测试用）。
bool shelf_history_record_at(ShelfItem *const *items, int count, ULONGLONG closed_at);

void shelf_history_remove(ShelfHistoryEntry *entry);
/// 把记录移到列表中的指定位置；记录已被移除时不做任何事。
void shelf_history_move(ShelfHistoryEntry *entry, int index);
void shelf_history_clear(void);

/// 等后台的保存写完；退出前调用，最后一次改动不会丢。
void shelf_history_flush(void);

/// 记录有变化时通知（托盘菜单、主窗口的列表）。
void shelf_history_add_listener(void (*listener)(void *context), void *context);

// ───────────── 显示 ─────────────

/// 菜单里的标题：一项时是它的名称，多项时是“报告.pdf 等 3 项”。调用方负责 free。
wchar_t *shelf_history_entry_title(const ShelfHistoryEntry *entry);
/// 关闭的时间：今天的写“14:05”，昨天的写“昨天 14:05”，更早的写日期。调用方负责 free。
wchar_t *shelf_history_entry_time(const ShelfHistoryEntry *entry);

/// 历史牌（纸牌）右上角：关闭的时刻（HH:mm），下面一行是今天、昨天或日期。
wchar_t *shelf_history_entry_clock(const ShelfHistoryEntry *entry);
wchar_t *shelf_history_entry_day(const ShelfHistoryEntry *entry);
/// 提示里列出的名称：最多 8 个，每行一个，更多时最后一行是“…”。调用方负责 free。
wchar_t *shelf_history_entry_preview(const ShelfHistoryEntry *entry);
