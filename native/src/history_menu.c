#include "history_menu.h"
#include "strings.h"

#include <stdlib.h>

#define GLYPH_HISTORY L"\xE81C"
#define GLYPH_SINGLE L"\xE8A5"
#define GLYPH_MULTIPLE L"\xE8F1"
#define GLYPH_CLEAR L"\xE894"
#define TITLE_MAXIMUM_WIDTH 260.0f

static const wchar_t *keep(HistoryMenu *menu, wchar_t *text)
{
    menu->texts[menu->text_count++] = text;
    return text;
}

void history_menu_free_texts(HistoryMenu *menu)
{
    for (int index = 0; index < menu->text_count; index++) free(menu->texts[index]);
    menu->text_count = 0;
}

void history_menu_release(HistoryMenu *menu)
{
    history_menu_free_texts(menu);
    for (int index = 0; index < menu->count; index++) shelf_history_entry_release(menu->entries[index]);
    menu->count = 0;
    menu->child_count = 0;
}

void history_menu_build(HistoryMenu *menu, int first_id, int clear_id)
{
    history_menu_release(menu);
    menu->first_id = first_id;
    int count;
    ShelfHistoryEntry *const *entries = shelf_history_entries(&count);
    for (int index = 0; index < count && index < SHELF_HISTORY_CAPACITY; index++)
    {
        ShelfHistoryEntry *entry = shelf_history_entry_retain(entries[index]);
        menu->entries[menu->count++] = entry;
        UiMenuItem item = {0};
        item.id = first_id + index;
        item.text = keep(menu, shelf_history_entry_title(entry));
        item.glyph = entry->count == 1 ? GLYPH_SINGLE : GLYPH_MULTIPLE;
        item.gesture = keep(menu, shelf_history_entry_time(entry));
        item.tooltip = keep(menu, shelf_history_entry_preview(entry));
        item.max_width = TITLE_MAXIMUM_WIDTH;
        menu->children[menu->child_count++] = item;
    }
    if (menu->count == 0) return;
    menu->children[menu->child_count++] = (UiMenuItem){ .separator = true };
    menu->children[menu->child_count++] = (UiMenuItem){ .id = clear_id, .text = S(STR_ClearHistory), .glyph = GLYPH_CLEAR };
}

UiMenuItem history_menu_root(const HistoryMenu *menu)
{
    UiMenuItem item = {0};
    item.text = S(STR_RecentShelves);
    item.glyph = GLYPH_HISTORY;
    if (menu->count == 0) item.disabled = true;
    else
    {
        item.children = menu->children;
        item.child_count = menu->child_count;
    }
    return item;
}

ShelfHistoryEntry *history_menu_entry(const HistoryMenu *menu, int id)
{
    int index = id - menu->first_id;
    return index >= 0 && index < menu->count ? menu->entries[index] : NULL;
}
