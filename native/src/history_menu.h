// “最近关闭的置物架”子菜单，托盘菜单和置物架的“更多”菜单共用，和 C# 版的 HistoryMenu 对应。
#pragma once

#include "shelf_history.h"
#include "ui.h"

/// 菜单打开期间拿着各条记录的引用：选中时记录已被别处移除也还能恢复。
typedef struct HistoryMenu
{
    ShelfHistoryEntry *entries[SHELF_HISTORY_CAPACITY];
    int count;
    int first_id;
    UiMenuItem children[SHELF_HISTORY_CAPACITY + 2];
    int child_count;
    wchar_t *texts[SHELF_HISTORY_CAPACITY * 3];
    int text_count;
} HistoryMenu;

/// 按现在的记录生成子菜单：第 i 条的编号是 first_id + i，“清除记录”是 clear_id。之前拿着的记录先放掉。
void history_menu_build(HistoryMenu *menu, int first_id, int clear_id);

/// 子菜单的标题项“最近关闭的置物架”；没有记录时是灰的、没有子菜单。
UiMenuItem history_menu_root(const HistoryMenu *menu);

/// 菜单已经显示（ui_menu_show 复制了文字）：放掉生成的文字，记录还拿着。
void history_menu_free_texts(HistoryMenu *menu);

/// 选中的编号对应哪条记录；不是记录时返回 NULL。
ShelfHistoryEntry *history_menu_entry(const HistoryMenu *menu, int id);

void history_menu_release(HistoryMenu *menu);
