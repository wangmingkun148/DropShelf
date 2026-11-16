#include "shelf_history.h"
#include "json.h"
#include "log.h"
#include "settings.h"
#include "strings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_ITEMS_PER_ENTRY 500
#define PREVIEW_NAMES 8
#define TICKS_PER_MINUTE (60ull * 10000000ull)

static ShelfHistoryEntry *entries[SHELF_HISTORY_CAPACITY + 1];
static int entry_count;
static wchar_t *history_path;
static bool loaded;             // 读过记录才记录和保存：测试里没有读的时候不会写到真正的数据文件夹
static SaveQueue history_queue = SAVE_QUEUE_INIT(L"保存最近关闭的置物架失败");

typedef struct Listener
{
    void (*function)(void *context);
    void *context;
} Listener;

static Listener listeners[8];
static int listener_count;

// ───────────── 记录 ─────────────

static ShelfHistoryEntry *entry_new(ULONGLONG closed_at, int capacity)
{
    ShelfHistoryEntry *entry = xcalloc(1, sizeof(ShelfHistoryEntry));
    entry->references = 1;
    entry->closed_at = closed_at;
    entry->items = xcalloc((size_t)(capacity ? capacity : 1), sizeof(ShelfHistoryItem));
    return entry;
}

ShelfHistoryEntry *shelf_history_entry_retain(ShelfHistoryEntry *entry)
{
    if (entry) entry->references++;
    return entry;
}

void shelf_history_entry_release(ShelfHistoryEntry *entry)
{
    if (!entry || --entry->references > 0) return;
    for (int index = 0; index < entry->count; index++)
    {
        free(entry->items[index].path);
        free(entry->items[index].name);
    }
    free(entry->items);
    free(entry);
}

static void clear_entries(void)
{
    for (int index = 0; index < entry_count; index++) shelf_history_entry_release(entries[index]);
    entry_count = 0;
}

ShelfHistoryEntry *const *shelf_history_entries(int *count)
{
    *count = entry_count;
    return entries;
}

ShelfHistoryEntry *shelf_history_latest(void)
{
    return entry_count > 0 ? entries[0] : NULL;
}

void shelf_history_add_listener(void (*listener)(void *context), void *context)
{
    if (listener_count < (int)ARRAYSIZE(listeners)) listeners[listener_count++] = (Listener){ listener, context };
}

static int compare_text(const void *a, const void *b)
{
    return wcscmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

/// 内容的标识：全部路径转成大写、排好序再连起来，顺序不同但内容相同的两条算同一条。
static wchar_t *key_of(const ShelfHistoryEntry *entry)
{
    wchar_t **paths = xalloc((size_t)(entry->count ? entry->count : 1) * sizeof(wchar_t *));
    for (int index = 0; index < entry->count; index++)
    {
        paths[index] = wcs_dup(entry->items[index].path);
        _wcsupr_s(paths[index], wcslen(paths[index]) + 1);
    }
    qsort(paths, (size_t)entry->count, sizeof(wchar_t *), compare_text);
    WStr key = {0};
    wstr_append(&key, L"");
    for (int index = 0; index < entry->count; index++)
    {
        if (index > 0) wstr_append_char(&key, L'\n');
        wstr_append(&key, paths[index]);
        free(paths[index]);
    }
    free(paths);
    return wstr_take(&key);
}

// ───────────── 时间 ─────────────

static ULONGLONG now_utc(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

static FILETIME to_filetime(ULONGLONG value)
{
    return (FILETIME){ (DWORD)value, (DWORD)(value >> 32) };
}

static ULONGLONG from_filetime(FILETIME value)
{
    return ((ULONGLONG)value.dwHighDateTime << 32) | value.dwLowDateTime;
}

/// UTC 换成本地时间（按那一刻的时区规则，含夏令时）。
static bool to_local(ULONGLONG utc, SYSTEMTIME *local, ULONGLONG *local_ticks)
{
    FILETIME file = to_filetime(utc);
    SYSTEMTIME universal;
    if (!FileTimeToSystemTime(&file, &universal) || !SystemTimeToTzSpecificLocalTime(NULL, &universal, local)) return false;
    // SYSTEMTIME 只精确到毫秒：用两边的差算出时差，再加回到原来的时刻上，七位小数不丢。
    FILETIME universal_file, local_file;
    if (!SystemTimeToFileTime(&universal, &universal_file) || !SystemTimeToFileTime(local, &local_file)) return false;
    if (local_ticks) *local_ticks = utc + (from_filetime(local_file) - from_filetime(universal_file));
    return true;
}

/// 写成和 C# 的 DateTime.Now 一样的 ISO 8601：2026-11-15T14:05:09.1234567+08:00（小数末尾的 0 去掉）。
static wchar_t *format_closed_at(ULONGLONG utc)
{
    SYSTEMTIME local;
    ULONGLONG local_ticks;
    if (!to_local(utc, &local, &local_ticks)) return wcs_dup(L"0001-01-01T00:00:00");
    long long offset = ((long long)local_ticks - (long long)utc) / (long long)TICKS_PER_MINUTE;
    wchar_t fraction[16] = L"";
    unsigned ticks = (unsigned)(local_ticks % 10000000ull);
    if (ticks > 0)
    {
        swprintf(fraction, ARRAYSIZE(fraction), L".%07u", ticks);
        size_t length = wcslen(fraction);
        while (length > 1 && fraction[length - 1] == L'0') fraction[--length] = 0;
    }
    wchar_t text[64];
    long long magnitude = offset < 0 ? -offset : offset;
    swprintf(text, ARRAYSIZE(text), L"%04u-%02u-%02uT%02u:%02u:%02u%ls%lc%02lld:%02lld", local.wYear, local.wMonth, local.wDay,
        local.wHour, local.wMinute, local.wSecond, fraction, offset < 0 ? L'-' : L'+', magnitude / 60, magnitude % 60);
    return wcs_dup(text);
}

static bool read_digits(const wchar_t **cursor, int count, int *value)
{
    *value = 0;
    for (int index = 0; index < count; index++)
    {
        wchar_t c = (*cursor)[index];
        if (c < L'0' || c > L'9') return false;
        *value = *value * 10 + (c - L'0');
    }
    *cursor += count;
    return true;
}

/// 读 ISO 8601 的时刻：带时差（+08:00、Z）的按时差换算，不带的当作本地时间。读不了时返回 false。
static bool parse_closed_at(const wchar_t *text, ULONGLONG *utc)
{
    if (!text) return false;
    const wchar_t *cursor = text;
    int year, month, day, hour = 0, minute = 0, second = 0;
    if (!read_digits(&cursor, 4, &year) || *cursor++ != L'-' || !read_digits(&cursor, 2, &month) || *cursor++ != L'-' ||
        !read_digits(&cursor, 2, &day))
        return false;
    ULONGLONG ticks = 0;
    if (*cursor == L'T' || *cursor == L' ')
    {
        cursor++;
        if (!read_digits(&cursor, 2, &hour) || *cursor++ != L':' || !read_digits(&cursor, 2, &minute)) return false;
        if (*cursor == L':')
        {
            cursor++;
            if (!read_digits(&cursor, 2, &second)) return false;
            if (*cursor == L'.')
            {
                cursor++;
                ULONGLONG scale = 1000000;
                while (*cursor >= L'0' && *cursor <= L'9')
                {
                    ticks += (ULONGLONG)(*cursor - L'0') * scale;
                    scale /= 10;
                    cursor++;
                }
            }
        }
    }
    SYSTEMTIME time = { (WORD)year, (WORD)month, 0, (WORD)day, (WORD)hour, (WORD)minute, (WORD)second, 0 };
    FILETIME file;
    if (!SystemTimeToFileTime(&time, &file)) return false;
    ULONGLONG value = from_filetime(file) + ticks;
    if (*cursor == L'Z' && cursor[1] == 0)
    {
        *utc = value;
        return true;
    }
    if ((*cursor == L'+' || *cursor == L'-') && cursor[1])
    {
        int sign = *cursor++ == L'-' ? -1 : 1, offset_hours, offset_minutes = 0;
        if (!read_digits(&cursor, 2, &offset_hours)) return false;
        if (*cursor == L':') cursor++;
        if (*cursor && !read_digits(&cursor, 2, &offset_minutes)) return false;
        if (*cursor) return false;
        long long offset = sign * ((long long)offset_hours * 60 + offset_minutes) * (long long)TICKS_PER_MINUTE;
        *utc = (ULONGLONG)((long long)value - offset);
        return true;
    }
    if (*cursor) return false;
    // 没有时差：本地时间。
    SYSTEMTIME universal;
    if (!TzSpecificLocalTimeToSystemTime(NULL, &time, &universal) || !SystemTimeToFileTime(&universal, &file)) return false;
    *utc = from_filetime(file) + ticks;
    return true;
}

// ───────────── 保存和读取 ─────────────

static wchar_t *serialize(void)
{
    JsonWriter writer = {0};
    json_write_begin_array(&writer, NULL);
    for (int index = 0; index < entry_count; index++)
    {
        const ShelfHistoryEntry *entry = entries[index];
        json_write_begin_object(&writer, NULL);
        wchar_t *closed_at = format_closed_at(entry->closed_at);
        json_write_string(&writer, L"ClosedAt", closed_at);
        free(closed_at);
        json_write_begin_array(&writer, L"Items");
        for (int at = 0; at < entry->count; at++)
        {
            json_write_begin_object(&writer, NULL);
            json_write_string(&writer, L"Path", entry->items[at].path);
            json_write_string(&writer, L"Name", entry->items[at].name);
            json_write_end_object(&writer);
        }
        json_write_end_array(&writer);
        json_write_end_object(&writer);
    }
    json_write_end_array(&writer);
    return json_write_finish(&writer);
}

static void ensure_path(void)
{
    if (history_path) return;
    wchar_t *directory = settings_data_directory();
    history_path = path_join(directory, L"history.json");
    free(directory);
}

/// 在后台按顺序保存，界面不用等文件写完；保存的是此刻列表的内容。
static void save_and_notify(void)
{
    ensure_path();
    wchar_t *json = serialize();
    size_t length = 0;
    char *bytes = utf8_from_wide(json, &length);
    free(json);
    save_queue_post(&history_queue, history_path, bytes, length);
    for (int index = 0; index < listener_count; index++) listeners[index].function(listeners[index].context);
}

void shelf_history_flush(void)
{
    save_queue_flush(&history_queue);
}

static bool is_blank_text(const JsonValue *value)
{
    return !value || value->type != JSON_STRING || wcs_is_blank(value->string);
}

void shelf_history_load(const wchar_t *path)
{
    shelf_history_flush();
    clear_entries();
    free(history_path);
    history_path = NULL;
    if (path) history_path = wcs_dup(path);
    ensure_path();
    loaded = true;
    if (!path_exists(history_path)) return;
    wchar_t *text = file_read_text(history_path, 64 * 1024 * 1024);
    JsonValue *root = text ? json_parse(text, 0) : NULL;
    free(text);
    if (!root || root->type != JSON_ARRAY)
    {
        log_error(L"读取最近关闭的置物架失败\r\n%ls", root ? L"文件的格式不正确" : L"无法读取或解析文件");
        json_free(root);
        return;
    }
    for (int index = 0; index < root->count && entry_count < SHELF_HISTORY_CAPACITY; index++)
    {
        const JsonValue *object = root->items[index];
        if (!object || object->type != JSON_OBJECT) continue;
        const JsonValue *items = json_get(object, L"Items");
        const JsonValue *closed = json_get(object, L"ClosedAt");
        ULONGLONG closed_at = 0;
        if (!closed || closed->type != JSON_STRING || !parse_closed_at(closed->string, &closed_at)) closed_at = 0;
        int capacity = items && items->type == JSON_ARRAY ? items->count : 0;
        if (capacity > MAXIMUM_ITEMS_PER_ENTRY) capacity = MAXIMUM_ITEMS_PER_ENTRY;
        ShelfHistoryEntry *entry = entry_new(closed_at, capacity);
        for (int at = 0; items && items->type == JSON_ARRAY && at < items->count && entry->count < MAXIMUM_ITEMS_PER_ENTRY; at++)
        {
            const JsonValue *item = items->items[at];
            if (!item || item->type != JSON_OBJECT) continue;
            const JsonValue *item_path = json_get(item, L"Path");
            if (is_blank_text(item_path)) continue;
            const JsonValue *name = json_get(item, L"Name");
            ShelfHistoryItem *target = &entry->items[entry->count++];
            target->path = wcs_dup(item_path->string);
            target->name = wcs_dup(is_blank_text(name) ? path_file_name(item_path->string) : name->string);
        }
        if (entry->count > 0) entries[entry_count++] = entry;
        else shelf_history_entry_release(entry);
    }
    json_free(root);
}

// ───────────── 改动 ─────────────

bool shelf_history_record_at(ShelfItem *const *items, int count, ULONGLONG closed_at)
{
    if (!loaded) return false;
    ShelfHistoryEntry *entry = entry_new(closed_at, count < MAXIMUM_ITEMS_PER_ENTRY ? count : MAXIMUM_ITEMS_PER_ENTRY);
    for (int index = 0; index < count && entry->count < MAXIMUM_ITEMS_PER_ENTRY; index++)
    {
        ShelfItem *item = items[index];
        if (!shelf_item_is_file_system(item) || item->temporary || !shelf_item_refresh_availability(item)) continue;
        bool duplicate = false;
        for (int at = 0; at < entry->count && !duplicate; at++) duplicate = wcs_equal_ignore_case(entry->items[at].path, item->value);
        if (duplicate) continue;
        entry->items[entry->count].path = wcs_dup(item->value);
        entry->items[entry->count++].name = wcs_dup(shelf_item_display_name(item));
    }
    if (entry->count == 0)
    {
        shelf_history_entry_release(entry);
        return false;
    }
    wchar_t *key = key_of(entry);
    int kept = 0;
    for (int index = 0; index < entry_count; index++)
    {
        wchar_t *other = key_of(entries[index]);
        bool same = wcs_equal(key, other);
        free(other);
        if (same) shelf_history_entry_release(entries[index]);
        else entries[kept++] = entries[index];
    }
    free(key);
    entry_count = kept;
    memmove(&entries[1], &entries[0], (size_t)entry_count * sizeof(entries[0]));
    entries[0] = entry;
    entry_count++;
    while (entry_count > SHELF_HISTORY_CAPACITY) shelf_history_entry_release(entries[--entry_count]);
    save_and_notify();
    return true;
}

bool shelf_history_record(ShelfItem *const *items, int count)
{
    return shelf_history_record_at(items, count, now_utc());
}

static int index_of_entry(const ShelfHistoryEntry *entry)
{
    for (int index = 0; index < entry_count; index++)
        if (entries[index] == entry) return index;
    return -1;
}

void shelf_history_remove(ShelfHistoryEntry *entry)
{
    int index = index_of_entry(entry);
    if (index < 0) return;
    memmove(&entries[index], &entries[index + 1], (size_t)(entry_count - index - 1) * sizeof(entries[0]));
    entry_count--;
    shelf_history_entry_release(entry);
    save_and_notify();
}

void shelf_history_move(ShelfHistoryEntry *entry, int index)
{
    int from = index_of_entry(entry);
    if (from < 0) return;
    if (index < 0) index = 0;
    if (index > entry_count - 1) index = entry_count - 1;
    if (from == index) return;
    memmove(&entries[from], &entries[from + 1], (size_t)(entry_count - from - 1) * sizeof(entries[0]));
    memmove(&entries[index + 1], &entries[index], (size_t)(entry_count - 1 - index) * sizeof(entries[0]));
    entries[index] = entry;
    save_and_notify();
}

void shelf_history_clear(void)
{
    if (entry_count == 0) return;
    clear_entries();
    save_and_notify();
}

// ───────────── 显示 ─────────────

wchar_t *shelf_history_entry_title(const ShelfHistoryEntry *entry)
{
    const wchar_t *first = entry->count > 0 ? entry->items[0].name : S(STR_Shelf);
    return entry->count <= 1 ? wcs_dup(first) : str_history_entry_title(first, entry->count);
}

wchar_t *shelf_history_entry_time(const ShelfHistoryEntry *entry)
{
    SYSTEMTIME closed, today;
    ULONGLONG closed_ticks, today_ticks;
    if (!to_local(entry->closed_at, &closed, &closed_ticks) || !to_local(now_utc(), &today, &today_ticks)) return wcs_dup(L"");
    wchar_t time[16];
    swprintf(time, ARRAYSIZE(time), L"%02u:%02u", closed.wHour, closed.wMinute);
    const ULONGLONG day = 24ull * 60 * TICKS_PER_MINUTE;
    ULONGLONG closed_day = closed_ticks / day, today_day = today_ticks / day;
    if (closed_day == today_day) return wcs_dup(time);
    if (closed_day + 1 == today_day) return str_history_yesterday(time);
    return str_history_date(closed.wMonth, closed.wDay);
}

wchar_t *shelf_history_entry_clock(const ShelfHistoryEntry *entry)
{
    SYSTEMTIME closed;
    ULONGLONG ticks;
    if (!to_local(entry->closed_at, &closed, &ticks)) return wcs_dup(L"");
    wchar_t time[16];
    swprintf(time, ARRAYSIZE(time), L"%02u:%02u", closed.wHour, closed.wMinute);
    return wcs_dup(time);
}

wchar_t *shelf_history_entry_day(const ShelfHistoryEntry *entry)
{
    SYSTEMTIME closed, today;
    ULONGLONG closed_ticks, today_ticks;
    if (!to_local(entry->closed_at, &closed, &closed_ticks) || !to_local(now_utc(), &today, &today_ticks)) return wcs_dup(L"");
    const ULONGLONG day = 24ull * 60 * TICKS_PER_MINUTE;
    ULONGLONG closed_day = closed_ticks / day, today_day = today_ticks / day;
    int days_ago = closed_day == today_day ? 0 : closed_day + 1 == today_day ? 1 : 2;
    return str_history_day(days_ago, closed.wMonth, closed.wDay);
}

wchar_t *shelf_history_entry_preview(const ShelfHistoryEntry *entry)
{
    WStr text = {0};
    wstr_append(&text, L"");
    for (int index = 0; index < entry->count && index < PREVIEW_NAMES; index++)
    {
        if (index > 0) wstr_append(&text, L"\r\n");
        wstr_append(&text, entry->items[index].name);
    }
    if (entry->count > PREVIEW_NAMES) wstr_append(&text, L"\r\n…");
    return wstr_take(&text);
}
