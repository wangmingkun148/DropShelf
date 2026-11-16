#include "shelf_model.h"
#include "attachment_cache.h"
#include "clipboard.h"
#include "log.h"
#include "shelf_drag.h"
#include "strings.h"
#include "task.h"
#include "thumbnail.h"

#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// 交给别的程序的临时附件，关闭置物架后再留这么久（毫秒）。
#define TEMPORARY_FILE_GRACE (10u * 60 * 1000)
/// 拖出后资源管理器可能还在后台移动大文件，过这么久再确认一次。
#define REMOVE_WHEN_GONE_DELAY 2500
#define MAXIMUM_STEM_LENGTH 40
/// 每次项目变化时预先解析最近的这么多个文件（拖出用的缓存能放 256 个）。
#define SHELF_PREFETCH_COUNT 64

// ───────────── 基本 ─────────────

ShelfModel *shelf_model_new(const ShelfModelEvents *events)
{
    ShelfModel *model = xcalloc(1, sizeof(ShelfModel));
    model->references = 1;
    model->id = new_guid_text();
    model->pinned = true;
    model->thumbnail_size = 128;
    model->visible_from = model->visible_to = -1;
    model->busy_text = wcs_dup(L"");
    if (events) model->events = *events;
    return model;
}

ShelfModel *shelf_model_retain(ShelfModel *model)
{
    if (model) model->references++;
    return model;
}

void shelf_model_release(ShelfModel *model)
{
    if (!model || --model->references > 0) return;
    shelf_model_release_resources(model);
    free(model->id);
    free(model->busy_text);
    free(model->items);
    free(model);
}

static void changed(ShelfModel *model, ShelfChange change)
{
    if (!model->released && model->events.changed) model->events.changed(model->events.context, change);
}

static void notify(ShelfModel *model, const wchar_t *message, bool success)
{
    if (!model->released && model->events.notify && message) model->events.notify(model->events.context, message, success);
}

int shelf_model_index_of(const ShelfModel *model, const ShelfItem *item)
{
    for (int index = 0; index < model->count; index++)
        if (model->items[index] == item) return index;
    return -1;
}

bool shelf_model_contains(const ShelfModel *model, const ShelfItem *item)
{
    return shelf_model_index_of(model, item) >= 0;
}

ShelfItem *shelf_model_from_end(const ShelfModel *model, int position)
{
    return model->count >= position ? model->items[model->count - position] : NULL;
}

void shelf_model_set_expanded(ShelfModel *model, bool expanded)
{
    if (model->expanded == expanded) return;
    model->expanded = expanded;
    if (!expanded) model->visible_from = model->visible_to = -1;
    changed(model, SHELF_CHANGED_EXPANDED);
    shelf_model_update_thumbnails(model);
}

void shelf_model_set_busy(ShelfModel *model, const wchar_t *text)
{
    if (text)
    {
        free(model->busy_text);
        model->busy_text = wcs_dup(text);
    }
    bool busy = text != NULL;
    if (busy == model->busy) return;
    model->busy = busy;
    changed(model, SHELF_CHANGED_BUSY);
}

static void insert_item(ShelfModel *model, int index, ShelfItem *item)
{
    if (model->count == model->capacity)
    {
        model->capacity = model->capacity ? model->capacity * 2 : 8;
        model->items = xrealloc(model->items, (size_t)model->capacity * sizeof(ShelfItem *));
    }
    if (index < 0 || index > model->count) index = model->count;
    memmove(&model->items[index + 1], &model->items[index], (size_t)(model->count - index) * sizeof(ShelfItem *));
    model->items[index] = item;
    model->count++;
}

/// 从各个附带的记录里去掉这一项（拖出用的文件、等着移走的）。
static void forget_item(ShelfModel *model, ShelfItem *item)
{
    for (int index = 0; index < model->drag_count; index++)
    {
        if (model->drag_items[index] != item) continue;
        free(model->drag_paths[index]);
        model->drag_items[index] = model->drag_items[--model->drag_count];
        model->drag_paths[index] = model->drag_paths[model->drag_count];
        break;
    }
    for (int index = 0; index < model->leaving_count; index++)
    {
        if (model->leaving[index] != item) continue;
        model->leaving[index] = model->leaving[--model->leaving_count];
        break;
    }
    item->thumbnail_wanted = false;
    item->thumbnail_request++;
    item->selected = false;
}

/// 数量变了：空了就收起列表，再告诉窗口，最后按新的顶部三张和显示的行更新缩略图。
static void items_changed(ShelfModel *model)
{
    if (model->count == 0 && model->expanded)
    {
        model->expanded = false;
        model->visible_from = model->visible_to = -1;
    }
    changed(model, SHELF_CHANGED_ITEMS);
    shelf_model_update_thumbnails(model);
    // 先把文件解析好，拖出时按下就能开始。只管最近放上来的一批，项目很多时不会把缓存挤满后来回重新解析。
    StrList paths = {0};
    for (int index = model->count - 1; index >= 0 && paths.count < SHELF_PREFETCH_COUNT; index--)
        if (shelf_item_is_file_system(model->items[index])) strlist_add(&paths, model->items[index]->value);
    shell_paths_prefetch(&paths);
    strlist_clear(&paths);
}

static void adopt_cache(ShelfModel *model, const wchar_t *directory)
{
    if (wcs_is_blank(directory)) return;
    if (model->released || strlist_index_of(&model->caches, directory, true) >= 0)
    {
        attachment_cache_release(directory);
        return;
    }
    strlist_add(&model->caches, directory);
}

// ───────────── 文件夹统计 ─────────────

typedef struct MeasureJob
{
    ShelfModel *model;
    ShelfItem *item;
    wchar_t *path;
    bool ok;
    int children;
    bool has_size;
    long long size;
    struct MeasureJob *next;
} MeasureJob;

/// 同时统计的文件夹不超过两个：一次拖入很多文件夹时不把磁盘占满。
static MeasureJob *measure_queue_head, *measure_queue_tail;
static int measuring;

static void start_measuring(void);

static void measure_work(void *data)
{
    MeasureJob *job = data;
    job->ok = folder_measure(job->path, &job->children, &job->has_size, &job->size, &job->model->lifetime_over);
}

static void measure_done(void *data)
{
    MeasureJob *job = data;
    measuring--;
    ShelfModel *model = job->model;
    ShelfItem *item = job->item;
    item->measuring = false;
    if (job->ok && !model->released)
    {
        item->has_size = job->has_size;
        item->size_bytes = job->size;
        item->child_count = job->children;
        shelf_item_text_changed(item);
        if (shelf_model_contains(model, item)) changed(model, SHELF_CHANGED_ITEM_TEXT);
    }
    shelf_item_release(item);
    shelf_model_release(model);
    free(job->path);
    free(job);
    start_measuring();
}

static void start_measuring(void)
{
    while (measuring < 2 && measure_queue_head)
    {
        MeasureJob *job = measure_queue_head;
        measure_queue_head = job->next;
        if (!measure_queue_head) measure_queue_tail = NULL;
        if (job->model->released)
        {
            job->item->measuring = false;
            shelf_item_release(job->item);
            shelf_model_release(job->model);
            free(job->path);
            free(job);
            continue;
        }
        measuring++;
        task_run(measure_work, measure_done, job);
    }
}

static void measure_folder(ShelfModel *model, ShelfItem *item)
{
    if (item->measuring) return;
    item->measuring = true;
    MeasureJob *job = xcalloc(1, sizeof(MeasureJob));
    job->model = shelf_model_retain(model);
    job->item = shelf_item_retain(item);
    job->path = wcs_dup(item->value);
    if (measure_queue_tail) measure_queue_tail->next = job;
    else measure_queue_head = job;
    measure_queue_tail = job;
    start_measuring();
}

// ───────────── 放入 ─────────────

static bool has_value(const ShelfModel *model, const ShelfItem *candidate_kind_owner, const wchar_t *value, bool file_system)
{
    (void)candidate_kind_owner;
    for (int index = 0; index < model->count; index++)
    {
        const ShelfItem *item = model->items[index];
        if (shelf_item_is_file_system(item) != file_system) continue;
        if (file_system ? wcs_equal_ignore_case(item->value, value) : wcs_equal(item->value, value)) return true;
    }
    return false;
}

ShelfAddSummary shelf_model_add(ShelfModel *model, DropReadResult *result)
{
    ShelfAddSummary summary = { 0, 0, 0 };
    for (int index = 0; index < result->caches.count; index++) adopt_cache(model, result->caches.items[index]);
    strlist_clear(&result->caches);
    if (model->released) return summary;

    for (int index = 0; index < result->count; index++)
    {
        DroppedEntry *entry = &result->entries[index];
        if (wcs_is_blank(entry->value)) continue;
        if (entry->kind == ITEM_TEXT || entry->kind == ITEM_LINK)
        {
            if (has_value(model, NULL, entry->value, false))
            {
                summary.duplicates++;
                continue;
            }
            insert_item(model, -1, shelf_item_new(entry->kind, entry->value, entry->title, false));
            summary.added++;
            continue;
        }
        if (has_value(model, NULL, entry->value, true))
        {
            summary.duplicates++;
            continue;
        }
        FileFacts facts = {0};
        bool readable = result->with_facts ? entry->has_facts : file_facts_read(entry->value, &facts);
        if (result->with_facts) facts = entry->facts;
        if (!readable)
        {
            summary.unavailable++;
            continue;
        }
        ShelfItem *item = shelf_item_new(facts.is_folder ? ITEM_FOLDER : ITEM_FILE, entry->value, NULL, entry->temporary);
        item->identity = facts.identity;
        if (!facts.is_folder)
        {
            item->has_size = facts.has_size;
            item->size_bytes = facts.size_bytes;
            free(item->type_name);
            item->type_name = entry->type_name ? wcs_dup(entry->type_name) : file_type_name(entry->value);
        }
        insert_item(model, -1, item);
        if (facts.is_folder) measure_folder(model, item);
        summary.added++;
    }
    if (summary.added > 0)
    {
        model->has_interacted = true;
        items_changed(model);
    }
    return summary;
}

wchar_t *shelf_add_summary_message(ShelfAddSummary summary)
{
    if (summary.added == 0 && summary.duplicates > 0 && summary.unavailable == 0) return wcs_dup(S(STR_AlreadyOnShelf));
    if (summary.added == 0) return wcs_dup(S(summary.unavailable > 0 ? STR_ItemsUnavailable : STR_NothingToAdd));
    WStr message = {0};
    wchar_t *added = str_added(summary.added);
    wstr_append(&message, added);
    free(added);
    if (summary.duplicates > 0)
    {
        wchar_t *part = str_skipped_duplicates(summary.duplicates);
        wstr_append(&message, part);
        free(part);
    }
    if (summary.unavailable > 0)
    {
        wchar_t *part = str_unavailable_items(summary.unavailable);
        wstr_append(&message, part);
        free(part);
    }
    return wstr_take(&message);
}

ShelfAddSummary shelf_model_add_files(ShelfModel *model, const StrList *paths)
{
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc((size_t)(paths->count ? paths->count : 1), sizeof(DroppedEntry));
    for (int index = 0; index < paths->count; index++)
    {
        result->entries[result->count].kind = ITEM_FILE;
        result->entries[result->count++].value = wcs_dup(paths->items[index]);
    }
    ShelfAddSummary summary = shelf_model_add(model, result);
    drop_result_free(result);
    return summary;
}

// ───────────── 合并和拆分 ─────────────

int shelf_model_adopt(ShelfModel *model, ShelfItem *const *items, int count, ShelfModel *from)
{
    if (model->released || from == model) return 0;
    // 临时附件所在的缓存这边也持有一次：对方关闭后文件还在。
    StrList caches = {0};
    for (int index = 0; index < count; index++)
    {
        if (!items[index]->temporary) continue;
        wchar_t *directory = attachment_cache_directory_of(items[index]->value);
        if (directory && strlist_index_of(&caches, directory, true) < 0) strlist_add(&caches, directory);
        free(directory);
    }
    for (int index = 0; index < caches.count; index++)
    {
        attachment_cache_retain(caches.items[index]);
        adopt_cache(model, caches.items[index]);
    }
    strlist_clear(&caches);
    if (from && from->delay_cache_cleanup) model->delay_cache_cleanup = true;

    int added = 0;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (has_value(model, NULL, item->value, shelf_item_is_file_system(item))) continue;
        ShelfItem *copy = shelf_item_copy(item);
        insert_item(model, -1, copy);
        // 对方关闭时会取消还没做完的统计，这边接着做。
        if (copy->kind == ITEM_FOLDER && copy->child_count < 0) measure_folder(model, copy);
        added++;
    }
    if (added > 0)
    {
        model->has_interacted = true;
        items_changed(model);
    }
    return added;
}

// ───────────── 移除和撤销 ─────────────

static bool listed(ShelfItem *const *items, int count, const ShelfItem *item)
{
    for (int index = 0; index < count; index++)
        if (items[index] == item) return true;
    return false;
}

ShelfRemoval *shelf_model_remove(ShelfModel *model, ShelfItem *const *items, int count)
{
    ShelfRemoval *removal = NULL;
    int kept = 0;
    for (int index = 0; index < model->count; index++)
    {
        ShelfItem *item = model->items[index];
        if (!listed(items, count, item))
        {
            model->items[kept++] = item;
            continue;
        }
        if (!removal)
        {
            removal = xcalloc(1, sizeof(ShelfRemoval));
            removal->items = xalloc((size_t)count * sizeof(ShelfItem *));
            removal->indices = xalloc((size_t)count * sizeof(int));
        }
        // 记录里拿着项目原来那份引用。
        forget_item(model, item);
        removal->items[removal->count] = item;
        removal->indices[removal->count++] = index;
    }
    if (!removal) return NULL;
    model->count = kept;
    items_changed(model);
    return removal;
}

void shelf_removal_free(ShelfRemoval *removal)
{
    if (!removal) return;
    for (int index = 0; index < removal->count; index++) shelf_item_release(removal->items[index]);
    free(removal->items);
    free(removal->indices);
    free(removal);
}

int shelf_model_restore(ShelfModel *model, const ShelfRemoval *removal, ShelfItem ***restored)
{
    if (restored) *restored = NULL;
    if (model->released || !removal) return 0;
    int count = 0;
    ShelfItem **back = xalloc((size_t)(removal->count ? removal->count : 1) * sizeof(ShelfItem *));
    for (int index = 0; index < removal->count; index++)
    {
        ShelfItem *item = removal->items[index];
        bool file_system = shelf_item_is_file_system(item);
        if (has_value(model, NULL, item->value, file_system) || shelf_model_contains(model, item)) continue;
        int at = removal->indices[index] < model->count ? removal->indices[index] : model->count;
        insert_item(model, at, shelf_item_retain(item));
        back[count++] = item;
    }
    if (count > 0)
    {
        model->has_interacted = true;
        items_changed(model);
    }
    if (restored && count > 0) *restored = back;
    else free(back);
    return count;
}

// ───────────── 文本和链接当文件用 ─────────────

static bool is_shortcut(const ShelfItem *item)
{
    return item->kind == ITEM_LINK && text_is_single_url(item->value);
}

static bool is_invalid_file_name_character(wchar_t c)
{
    return c < 32 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L':' || c == L'*' || c == L'?' || c == L'\\' || c == L'/' ||
           (c >= 0x7F && c <= 0x9F);
}

wchar_t *shelf_item_file_name(const ShelfItem *item)
{
    bool shortcut = is_shortcut(item);
    wchar_t *source;
    if (shortcut) source = !wcs_is_blank(item->title) ? wcs_dup(item->title) : url_host(item->value);
    else
    {
        source = text_summarize(item->value, MAXIMUM_STEM_LENGTH);
        size_t length = wcslen(source);
        while (length > 0 && source[length - 1] == L'…') source[--length] = 0;
    }
    if (!source) source = wcs_dup(L"");
    WStr builder = {0};
    wstr_append(&builder, L"");
    for (const wchar_t *c = source; *c; c++)
    {
        wchar_t next = is_invalid_file_name_character(*c) ? L' ' : *c;
        if (next == L' ' && (builder.length == 0 || builder.data[builder.length - 1] == L' ')) continue;
        wstr_append_char(&builder, next);
        if (builder.length == MAXIMUM_STEM_LENGTH) break;
    }
    free(source);
    // 去掉首尾的空格和点。
    wchar_t *stem = builder.data;
    while (*stem == L' ' || *stem == L'.') stem++;
    size_t length = wcslen(stem);
    while (length > 0 && (stem[length - 1] == L' ' || stem[length - 1] == L'.')) stem[--length] = 0;
    WStr name = {0};
    if (length == 0) wstr_append(&name, S(shortcut ? STR_LinkFileName : STR_TextFileName));
    else
    {
        static const wchar_t *const reserved[] = {
            L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
            L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
        };
        for (size_t index = 0; index < sizeof(reserved) / sizeof(reserved[0]); index++)
            if (wcs_equal_ignore_case(stem, reserved[index])) wstr_append_char(&name, L'_');
        wstr_append(&name, stem);
    }
    wstr_append(&name, shortcut ? L".url" : L".txt");
    wstr_free(&builder);
    return wstr_take(&name);
}

/// 网址里的非 ASCII 字符按 UTF-8 转义，和 Uri.AbsoluteUri 一样能写进 ASCII 的快捷方式文件。
static char *ascii_url(const wchar_t *url)
{
    size_t utf8_length;
    char *utf8 = utf8_from_wide(url, &utf8_length);
    size_t capacity = utf8_length * 3 + 1, length = 0;
    char *result = xalloc(capacity);
    for (size_t index = 0; index < utf8_length; index++)
    {
        unsigned char c = (unsigned char)utf8[index];
        if (c > 0x20 && c < 0x7F) result[length++] = (char)c;
        else length += (size_t)sprintf_s(result + length, capacity - length, "%%%02X", c);
    }
    result[length] = 0;
    free(utf8);
    return result;
}

/// 写到 directory 里，同名时在名称后面加序号，不覆盖已有的文件。失败时返回 NULL。
static wchar_t *write_item_file(const ShelfItem *item, const wchar_t *directory)
{
    wchar_t *name = shelf_item_file_name(item);
    const wchar_t *extension = path_extension(name);
    wchar_t *stem = wcs_dup_n(name, (size_t)(extension - name));
    wchar_t *path = path_join(directory, name);
    for (int number = 2; path_exists(path); number++)
    {
        free(path);
        WStr numbered = {0};
        wstr_appendf(&numbered, L"%ls (%d)%ls", stem, number, extension);
        path = path_join(directory, numbered.data);
        wstr_free(&numbered);
    }
    free(stem);
    free(name);

    char *bytes;
    size_t length;
    if (is_shortcut(item))
    {
        char *url = ascii_url(item->value);
        size_t capacity = strlen(url) + 64;
        bytes = xalloc(capacity);
        length = (size_t)sprintf_s(bytes, capacity, "[InternetShortcut]\r\nURL=%s\r\n", url);
        free(url);
    }
    else
    {
        // 带上字节顺序标记：旧程序在中文系统上默认按本地编码读，没有标记会乱码。
        size_t text_length;
        char *text = utf8_from_wide(item->value, &text_length);
        bytes = xalloc(text_length + 3);
        memcpy(bytes, "\xEF\xBB\xBF", 3);
        memcpy(bytes + 3, text, text_length);
        length = text_length + 3;
        free(text);
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, (DWORD)length, &written, NULL) && written == length;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(bytes);
    if (ok) return path;
    log_win32_error(L"把文本或链接写成文件失败", GetLastError());
    DeleteFileW(path);
    free(path);
    return NULL;
}

/// 置物架自己的临时文件夹，放文本和链接转成的文件；和接收的临时附件一样，关闭后清理。
static const wchar_t *own_cache(ShelfModel *model)
{
    if (model->own_cache && path_is_directory(model->own_cache)) return model->own_cache;
    free(model->own_cache);
    model->own_cache = attachment_cache_create();
    if (model->own_cache) strlist_add(&model->caches, model->own_cache);
    return model->own_cache;
}

int shelf_model_convert_to_files(ShelfModel *model, ShelfItem *const *items, int count)
{
    if (model->released) return 0;
    int converted = 0;
    bool failed = false;
    for (int index = 0; index < count && !failed; index++)
    {
        ShelfItem *item = items[index];
        if (shelf_item_is_file_system(item)) continue;
        int at = shelf_model_index_of(model, item);
        if (at < 0) continue;
        const wchar_t *directory = own_cache(model);
        wchar_t *path = directory ? write_item_file(item, directory) : NULL;
        if (!path)
        {
            failed = true;
            break;
        }
        ShelfItem *file = shelf_item_new(ITEM_FILE, path, NULL, true);
        free(file->id);
        file->id = wcs_dup(item->id);
        file->has_size = file_size_of(path, &file->size_bytes);
        free(file->type_name);
        file->type_name = file_type_name(path);
        file->selected = item->selected;
        forget_item(model, item);
        model->items[at] = file;
        shelf_item_release(item);
        free(path);
        converted++;
    }
    if (failed) notify(model, S(STR_ConvertToFileFailed), false);
    if (converted > 0)
    {
        model->has_interacted = true;
        items_changed(model);
    }
    return converted;
}

void shelf_model_write_for_drag(ShelfModel *model, ShelfItem *const *items, int count, StrList *paths)
{
    *paths = (StrList){0};
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (shelf_item_is_file_system(item) || !shelf_model_contains(model, item)) continue;
        int found = -1;
        for (int at = 0; at < model->drag_count; at++)
            if (model->drag_items[at] == item) found = at;
        if (found >= 0 && path_exists(model->drag_paths[found]))
        {
            strlist_add(paths, model->drag_paths[found]);
            continue;
        }
        const wchar_t *directory = own_cache(model);
        wchar_t *path = directory ? write_item_file(item, directory) : NULL;
        if (!path)
        {
            log_error(L"替文本或链接准备拖出的文件失败");
            strlist_clear(paths);
            return;
        }
        if (found >= 0)
        {
            free(model->drag_paths[found]);
            model->drag_paths[found] = wcs_dup(path);
        }
        else
        {
            model->drag_items = xrealloc(model->drag_items, (size_t)(model->drag_count + 1) * sizeof(ShelfItem *));
            model->drag_paths = xrealloc(model->drag_paths, (size_t)(model->drag_count + 1) * sizeof(wchar_t *));
            model->drag_items[model->drag_count] = item;
            model->drag_paths[model->drag_count++] = wcs_dup(path);
        }
        strlist_add_owned(paths, path);
    }
}

// ───────────── 拖出、打开、复制 ─────────────

static void remove_shown(ShelfModel *model, ShelfItem *const *items, int count)
{
    ShelfItem **removing = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
    int removing_count = 0;
    for (int index = 0; index < count; index++)
        if (shelf_model_contains(model, items[index])) removing[removing_count++] = items[index];
    if (removing_count > 0)
    {
        if (model->events.remove_visibly && !model->released) model->events.remove_visibly(model->events.context, removing, removing_count);
        else shelf_removal_free(shelf_model_remove(model, removing, removing_count));
    }
    free(removing);
}

typedef struct GoneCheck
{
    ShelfModel *model;
    ShelfItem **items;
    int count;
} GoneCheck;

static void remove_when_gone(void *data)
{
    GoneCheck *check = data;
    ShelfModel *model = check->model;
    ShelfItem **gone = xalloc((size_t)check->count * sizeof(ShelfItem *));
    int gone_count = 0;
    for (int index = 0; index < check->count; index++)
    {
        ShelfItem *item = check->items[index];
        for (int at = 0; at < model->leaving_count; at++)
            if (model->leaving[at] == item)
            {
                model->leaving[at] = model->leaving[--model->leaving_count];
                break;
            }
        if (!model->released && shelf_model_contains(model, item) && !shelf_item_refresh_availability(item)) gone[gone_count++] = item;
    }
    if (gone_count > 0) remove_shown(model, gone, gone_count);
    for (int index = 0; index < check->count; index++) shelf_item_release(check->items[index]);
    free(gone);
    free(check->items);
    shelf_model_release(model);
    free(check);
}

void shelf_model_apply_drag_out(ShelfModel *model, ShelfItem *const *dragged, int count, DragOutResult result, bool remove_copied)
{
    if (result == DRAG_OUT_MOVED)
    {
        // 资源管理器可能在后台继续移动大文件：还在的稍后再确认一次。
        ShelfItem **gone = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
        GoneCheck *check = xcalloc(1, sizeof(GoneCheck));
        check->items = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
        int gone_count = 0;
        for (int index = 0; index < count; index++)
        {
            ShelfItem *item = dragged[index];
            if (shelf_item_is_file_system(item) && shelf_item_refresh_availability(item)) check->items[check->count++] = shelf_item_retain(item);
            else gone[gone_count++] = item;
        }
        remove_shown(model, gone, gone_count);
        free(gone);
        if (check->count == 0)
        {
            free(check->items);
            free(check);
            return;
        }
        model->leaving = xrealloc(model->leaving, (size_t)(model->leaving_count + check->count) * sizeof(ShelfItem *));
        for (int index = 0; index < check->count; index++) model->leaving[model->leaving_count++] = check->items[index];
        check->model = shelf_model_retain(model);
        task_delay(REMOVE_WHEN_GONE_DELAY, remove_when_gone, check);
        return;
    }
    if ((result == DRAG_OUT_COPIED || result == DRAG_OUT_LINKED) && remove_copied) remove_shown(model, dragged, count);
}

void shelf_model_keep_temporary_files(ShelfModel *model)
{
    model->delay_cache_cleanup = true;
}

bool shelf_model_copy(ShelfModel *model, ShelfItem *const *items, int count)
{
    if (count == 0) return false;
    // 刚在别处改了名或挪了位置的文件先找回来，和拖出、打开时一样。
    shelf_model_relink(model, items, count);
    StrList files = {0};
    WStr texts = {0};
    bool temporary = false, has_text = false;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (shelf_item_is_file_system(item))
        {
            if (!shelf_item_refresh_availability(item)) continue;
            strlist_add(&files, item->value);
            temporary |= item->temporary;
        }
        else
        {
            if (has_text) wstr_append(&texts, L"\r\n");
            wstr_append(&texts, item->value);
            has_text = true;
        }
    }
    changed(model, SHELF_CHANGED_ITEM_TEXT);
    // 没有文件可复制时只复制文本和链接；已删除或移动的文件不能当成文字把旧路径复制出去。
    if (files.count == 0 && !has_text)
    {
        notify(model, S(count == 1 ? STR_ItemMissing : STR_ItemsMissing), false);
        wstr_free(&texts);
        return false;
    }
    bool copied = files.count > 0 ? clipboard_set_files(&files) : clipboard_set_text(texts.data);
    // 剪贴板里只有临时附件的路径，关闭置物架后可能才去粘贴。
    if (copied && temporary) shelf_model_keep_temporary_files(model);
    if (!copied) notify(model, S(STR_ClipboardBusy), false);
    else if (files.count > 0)
    {
        wchar_t *message = str_copied_files(files.count);
        notify(model, message, true);
        free(message);
    }
    else notify(model, S(STR_CopiedText), true);
    strlist_clear(&files);
    wstr_free(&texts);
    return copied;
}

bool shelf_model_copy_paths(ShelfModel *model, ShelfItem *const *items, int count)
{
    WStr paths = {0};
    int path_count = 0;
    bool temporary = false;
    for (int index = 0; index < count; index++)
    {
        if (!shelf_item_is_file_system(items[index])) continue;
        if (path_count++ > 0) wstr_append(&paths, L"\r\n");
        wstr_append(&paths, items[index]->value);
        temporary |= items[index]->temporary;
    }
    if (path_count == 0)
    {
        notify(model, S(STR_NoPathsToCopy), false);
        return false;
    }
    bool copied = clipboard_set_text(paths.data);
    wstr_free(&paths);
    // 临时附件的路径可能在关闭置物架之后才被使用。
    if (copied && temporary) shelf_model_keep_temporary_files(model);
    if (!copied) notify(model, S(STR_ClipboardBusy), false);
    else if (path_count > 1)
    {
        wchar_t *message = str_copied_paths(path_count);
        notify(model, message, true);
        free(message);
    }
    else notify(model, S(STR_CopiedPath), true);
    return copied;
}

static bool shell_open(const wchar_t *target)
{
    SHELLEXECUTEINFOW info = { .cbSize = sizeof(info) };
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpFile = target;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != 0;
}

void shelf_model_open(ShelfModel *model, ShelfItem *item)
{
    if (item->kind == ITEM_TEXT || (item->kind == ITEM_LINK && !text_is_single_url(item->value)))
    {
        if (clipboard_set_text(item->value)) notify(model, S(STR_CopiedText), true);
        return;
    }
    if (item->kind == ITEM_LINK)
    {
        if (!shell_open(item->value))
        {
            log_win32_error(L"打开项目失败", GetLastError());
            notify(model, S(STR_CannotOpenItem), false);
        }
        return;
    }
    shelf_model_relink(model, &item, 1);
    if (!shelf_item_refresh_availability(item))
    {
        changed(model, SHELF_CHANGED_ITEM_TEXT);
        notify(model, S(STR_ItemMissing), false);
        return;
    }
    // 打开的程序可能在置物架关闭后才读取文件。
    if (item->temporary) shelf_model_keep_temporary_files(model);
    if (!shell_open(item->value))
    {
        log_win32_error(L"打开项目失败", GetLastError());
        notify(model, S(STR_CannotOpenItem), false);
    }
}

void shelf_model_reveal(ShelfModel *model, ShelfItem *const *items, int count)
{
    StrList paths = {0};
    for (int index = 0; index < count; index++)
        if (shelf_item_is_file_system(items[index])) strlist_add(&paths, items[index]->value);
    bool opened = shell_open_folder_and_select(&paths);
    strlist_clear(&paths);
    if (!opened) notify(model, S(STR_CannotOpenFolder), false);
}

// ───────────── 快速操作 ─────────────

static int index_of_path(const ShelfModel *model, const wchar_t *path)
{
    for (int index = 0; index < model->count; index++)
        if (shelf_item_is_file_system(model->items[index]) && wcs_equal_ignore_case(model->items[index]->value, path)) return index;
    return -1;
}

// ───────────── 跟着改名和移动 ─────────────

/// 原来的路径找不到时，按文件标识找它现在的路径（在别处被改名，或在同一个磁盘里被移动）；
/// 没有标识、进了回收站或找不到时返回 NULL。可以在后台线程上调用。
static wchar_t *find_moved_path(ShelfItemKind kind, const wchar_t *path, const FileIdentity *identity)
{
    if ((kind != ITEM_FILE && kind != ITEM_FOLDER) || !file_identity_valid(identity)) return NULL;
    wchar_t *found = file_identity_resolve(identity, path, kind == ITEM_FOLDER);
    if (found && wcs_equal_ignore_case(found, path))
    {
        free(found);
        return NULL;
    }
    return found;
}

static bool is_leaving(const ShelfModel *model, const ShelfItem *item)
{
    for (int index = 0; index < model->leaving_count; index++)
        if (model->leaving[index] == item) return true;
    return false;
}

/// 可以把项目换成新的路径：不在拖出途中（这期间不见了多半是被目标程序移走的）、项目还在置物架上、
/// 不是拖出后正要移走的，新路径也没有别的项目占着。
static bool can_follow(const ShelfModel *model, const ShelfItem *item, const wchar_t *path)
{
    return !model->dragging_out && shelf_model_contains(model, item) && !is_leaving(model, item) && index_of_path(model, path) < 0;
}

void shelf_model_relink(ShelfModel *model, ShelfItem *const *items, int count)
{
    if (model->released || count > SHELF_RELINK_LIMIT) return;
    bool any = false;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (!shelf_item_is_file_system(item) || shelf_item_exists_on_disk(item->kind, item->value)) continue;
        wchar_t *path = find_moved_path(item->kind, item->value, &item->identity);
        if (path && can_follow(model, item, path))
        {
            shelf_item_relocate(item, path);
            any = true;
        }
        free(path);
    }
    if (any) changed(model, SHELF_CHANGED_ITEM_TEXT);
}

/// 移动之后，置物架上的项目跟着指向新位置；缩略图、大小这些不变。
static void apply_moves(ShelfModel *model, const StrList *sources, const StrList *destinations)
{
    bool any = false;
    for (int index = 0; index < sources->count && index < destinations->count; index++)
    {
        int at = index_of_path(model, sources->items[index]);
        if (at < 0) continue;
        ShelfItem *item = model->items[at];
        free(item->value);
        item->value = wcs_dup(destinations->items[index]);
        // 移出了临时文件夹，不再是临时附件。
        item->temporary = false;
        item->available = true;
        file_identity_get(item->value, &item->identity);
        shelf_item_text_changed(item);
        any = true;
    }
    if (any) changed(model, SHELF_CHANGED_ITEM_TEXT);
}

/// 用处理后的文件替换置物架中的原项目；原项目不在置物架上时（直接拖到按钮上）把新文件放进来。
static void apply_replacements(ShelfModel *model, const StrList *sources, const StrList *outputs)
{
    StrList additions = {0};
    bool replaced = false;
    for (int index = 0; index < sources->count && index < outputs->count; index++)
    {
        int at = index_of_path(model, sources->items[index]);
        if (at < 0 || index_of_path(model, outputs->items[index]) >= 0)
        {
            strlist_add(&additions, outputs->items[index]);
            continue;
        }
        ShelfItem *old = model->items[at];
        ShelfItem *item = shelf_item_new(ITEM_FILE, outputs->items[index], NULL, false);
        free(item->id);
        item->id = wcs_dup(old->id);
        item->has_size = file_size_of(item->value, &item->size_bytes);
        free(item->type_name);
        item->type_name = file_type_name(item->value);
        file_identity_get(item->value, &item->identity);
        item->selected = old->selected;
        forget_item(model, old);
        model->items[at] = item;
        shelf_item_release(old);
        replaced = true;
    }
    if (replaced) items_changed(model);
    if (additions.count > 0) shelf_model_add_files(model, &additions);
    strlist_clear(&additions);
}

typedef struct ActionRun
{
    ShelfModel *model;
    StrList held;               // 操作自己再持有一次的缓存
    bool busy;
    ShelfActionFinished finished;
    void *context;
} ActionRun;

static void action_done(QuickActionResult *result, void *context)
{
    ActionRun *run = context;
    ShelfModel *model = run->model;
    if (run->busy) shelf_model_set_busy(model, NULL);
    unsigned delay = result->keeps_temporary_files ? TEMPORARY_FILE_GRACE : 0;
    for (int index = 0; index < run->held.count; index++) attachment_cache_release_later(run->held.items[index], delay);
    // 置物架已在处理途中关闭时不再更新它，生成的文件由窗口放进新的置物架。
    if (!model->released)
    {
        if (result->keeps_temporary_files) model->delay_cache_cleanup = true;
        if (result->move_sources.count > 0) apply_moves(model, &result->move_sources, &result->move_destinations);
        if (result->replaced_sources.count > 0) apply_replacements(model, &result->replaced_sources, &result->replaced_outputs);
        if (result->produced_new_items && result->outputs.count > 0)
        {
            // 空置物架直接放入；已有内容时放进新的置物架，不和原来的项目混在一起。
            if (model->count == 0) shelf_model_add_files(model, &result->outputs);
            else if (model->events.produced) model->events.produced(model->events.context, &result->outputs);
        }
        if (!result->cancelled) notify(model, result->message, result->succeeded);
    }
    if (run->finished) run->finished(model, result, run->context);
    strlist_clear(&run->held);
    shelf_model_release(model);
    free(run);
}

static void run_core(ShelfModel *model, const QuickAction *action, StrList *paths, bool temporary, HWND owner,
    ShelfActionFinished finished, void *context)
{
    model->has_interacted = true;
    ActionRun *run = xcalloc(1, sizeof(ActionRun));
    run->model = shelf_model_retain(model);
    run->finished = finished;
    run->context = context;
    // 操作自己再持有一次输入所在的缓存，处理途中关闭置物架也不会删掉正在使用的文件。
    strlist_clone(&run->held, &model->caches);
    for (int index = 0; index < run->held.count; index++) attachment_cache_retain(run->held.items[index]);

    QuickActionRequest request = {0};
    quick_action_clone(&request.action, action);
    request.paths = *paths;
    *paths = (StrList){0};
    request.has_temporary_files = temporary;
    request.owner = owner;
    // “移动到”先弹出文件夹选择，选好后才显示进度。
    // 选文件夹时界面线程在对话框里，这期间设置可能变了：之后只用自己的副本。
    bool asks = quick_action_asks_for_folder(&request.action);
    if (asks) request.folder = pick_folder(owner, S(STR_ChooseMoveFolder));
    if (quick_action_shows_progress(&request.action) && (!asks || request.folder) && !model->released)
    {
        wchar_t *text = quick_action_busy_text(&request.action);
        shelf_model_set_busy(model, text);
        free(text);
        run->busy = true;
    }
    quick_action_execute(&request, action_done, run);
}

static void finish_without_running(ShelfModel *model, StringId message, ShelfActionFinished finished, void *context)
{
    notify(model, S(message), false);
    QuickActionResult result = {0};
    quick_action_result_fail(&result, message);
    if (finished) finished(model, &result, context);
    quick_action_result_free(&result);
}

void shelf_model_run_action(ShelfModel *model, const QuickAction *action, ShelfItem *const *targets, int count, HWND owner,
    ShelfActionFinished finished, void *context)
{
    shelf_model_relink(model, targets, count);
    // 转成文件时原来的项目会被换掉：先拿住，再按标识找到换上来的文件。
    ShelfItem **items = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
    for (int index = 0; index < count; index++) items[index] = shelf_item_retain(targets[index]);
    bool has_text = false;
    for (int index = 0; index < count; index++) has_text |= !shelf_item_is_file_system(items[index]);
    if (quick_action_takes_text(action) && has_text)
    {
        shelf_model_convert_to_files(model, items, count);
        for (int index = 0; index < count; index++)
        {
            if (shelf_item_is_file_system(items[index]) || shelf_model_contains(model, items[index])) continue;
            for (int at = 0; at < model->count; at++)
            {
                if (!wcs_equal(model->items[at]->id, items[index]->id)) continue;
                shelf_item_release(items[index]);
                items[index] = shelf_item_retain(model->items[at]);
                break;
            }
        }
    }
    StrList paths = {0};
    bool temporary = false;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (!shelf_item_is_file_system(item) || !shelf_item_refresh_availability(item)) continue;
        strlist_add(&paths, item->value);
        temporary |= item->temporary;
    }
    for (int index = 0; index < count; index++) shelf_item_release(items[index]);
    free(items);
    if (paths.count == 0)
    {
        changed(model, SHELF_CHANGED_ITEM_TEXT);
        finish_without_running(model, model->count == 0 ? STR_AddFilesFirst : STR_FilesAndFoldersOnly, finished, context);
        return;
    }
    run_core(model, action, &paths, temporary, owner, finished, context);
}

void shelf_model_run_action_on_dropped(ShelfModel *model, const QuickAction *action, DropReadResult *result, HWND owner,
    ShelfActionFinished finished, void *context)
{
    for (int index = 0; index < result->caches.count; index++) adopt_cache(model, result->caches.items[index]);
    strlist_clear(&result->caches);
    StrList paths = {0};
    bool temporary = false;
    for (int index = 0; index < result->count; index++)
    {
        const DroppedEntry *entry = &result->entries[index];
        if (entry->kind != ITEM_FILE && entry->kind != ITEM_FOLDER) continue;
        if (wcs_is_blank(entry->value)) continue;
        strlist_add(&paths, entry->value);
        temporary |= entry->temporary;
    }
    if (paths.count == 0)
    {
        if (result->error)
        {
            notify(model, result->error, false);
            QuickActionResult failed = {0};
            quick_action_result_set(&failed, false, wcs_dup(result->error));
            if (finished) finished(model, &failed, context);
            quick_action_result_free(&failed);
        }
        else finish_without_running(model, STR_FilesAndFoldersOnly, finished, context);
        return;
    }
    run_core(model, action, &paths, temporary, owner, finished, context);
}

// ───────────── 检查是否还在 ─────────────

typedef struct AvailabilityCheck
{
    ShelfModel *model;
    ShelfItem **items;
    ShelfItemKind *kinds;
    wchar_t **paths;
    FileIdentity *identities;
    bool *present;
    wchar_t **moved;            // 原来的路径找不到时，按标识找到的现在的路径
    int count;
} AvailabilityCheck;

static void availability_work(void *data)
{
    AvailabilityCheck *check = data;
    for (int index = 0; index < check->count; index++)
    {
        check->present[index] = shelf_item_exists_on_disk(check->kinds[index], check->paths[index]);
        // 原来的路径找不到了：看看是不是在别处被改名或移动了。
        if (!check->present[index]) check->moved[index] = find_moved_path(check->kinds[index], check->paths[index], &check->identities[index]);
    }
}

static void availability_done(void *data)
{
    AvailabilityCheck *check = data;
    ShelfModel *model = check->model;
    model->checking_availability = false;
    bool any = false;
    StrList relocated = {0};
    for (int index = 0; index < check->count; index++)
    {
        ShelfItem *item = check->items[index];
        // 检查期间换了路径（转成了文件、被替换）的不按旧路径的结果更新。
        if (!model->released && wcs_equal(item->value, check->paths[index]))
        {
            if (check->moved[index] && can_follow(model, item, check->moved[index]))
            {
                shelf_item_relocate(item, check->moved[index]);
                strlist_add(&relocated, item->value);
                any = true;
            }
            else if (item->available != check->present[index])
            {
                item->available = check->present[index];
                shelf_item_text_changed(item);
                any = true;
            }
        }
        shelf_item_release(item);
        free(check->paths[index]);
        free(check->moved[index]);
    }
    if (any) changed(model, SHELF_CHANGED_ITEM_TEXT);
    shell_paths_prefetch(&relocated);
    strlist_clear(&relocated);
    free(check->items);
    free(check->kinds);
    free(check->paths);
    free(check->identities);
    free(check->moved);
    free(check->present);
    shelf_model_release(model);
    free(check);
}

void shelf_model_refresh_availability_in_background(ShelfModel *model)
{
    if (model->checking_availability || model->released) return;
    int count = 0;
    for (int index = 0; index < model->count; index++)
        if (shelf_item_is_file_system(model->items[index])) count++;
    if (count == 0) return;
    AvailabilityCheck *check = xcalloc(1, sizeof(AvailabilityCheck));
    check->model = shelf_model_retain(model);
    check->items = xalloc((size_t)count * sizeof(ShelfItem *));
    check->kinds = xalloc((size_t)count * sizeof(ShelfItemKind));
    check->paths = xalloc((size_t)count * sizeof(wchar_t *));
    check->present = xcalloc((size_t)count, sizeof(bool));
    check->identities = xalloc((size_t)count * sizeof(FileIdentity));
    check->moved = xcalloc((size_t)count, sizeof(wchar_t *));
    for (int index = 0; index < model->count; index++)
    {
        ShelfItem *item = model->items[index];
        if (!shelf_item_is_file_system(item)) continue;
        check->items[check->count] = shelf_item_retain(item);
        check->identities[check->count] = item->identity;
        check->kinds[check->count] = item->kind;
        check->paths[check->count++] = wcs_dup(item->value);
    }
    model->checking_availability = true;
    task_run(availability_work, availability_done, check);
}

// ───────────── 显示 ─────────────

wchar_t *shelf_model_title(const ShelfModel *model)
{
    return model->count == 0 ? wcs_dup(S(STR_Shelf)) : str_item_count(model->count);
}

wchar_t *shelf_model_stack_caption(ShelfModel *model)
{
    if (model->count == 0) return wcs_dup(L"");
    if (model->count == 1) return wcs_dup(shelf_item_display_name(model->items[0]));
    int files = 0, folders = 0, texts = 0, links = 0;
    for (int index = 0; index < model->count; index++)
    {
        switch (model->items[index]->kind)
        {
            case ITEM_FILE: files++; break;
            case ITEM_FOLDER: folders++; break;
            case ITEM_TEXT: texts++; break;
            case ITEM_LINK: links++; break;
        }
    }
    WStr caption = {0};
    wstr_append(&caption, L"");
    wchar_t *(*const describe[4])(int) = { str_file_count, str_folder_count, str_text_count, str_link_count };
    const int counts[4] = { files, folders, texts, links };
    for (int kind = 0; kind < 4; kind++)
    {
        if (counts[kind] == 0) continue;
        if (caption.length > 0) wstr_append(&caption, L" · ");
        wchar_t *part = describe[kind](counts[kind]);
        wstr_append(&caption, part);
        free(part);
    }
    return wstr_take(&caption);
}

wchar_t *shelf_model_stack_detail(ShelfModel *model)
{
    if (model->count == 1) return wcs_dup(shelf_item_detail(model->items[0]));
    long long total = 0;
    for (int index = 0; index < model->count; index++)
        if (shelf_item_is_file_system(model->items[index]) && model->items[index]->has_size) total += model->items[index]->size_bytes;
    return total > 0 ? format_size(total) : wcs_dup(L"");
}

// ───────────── 缩略图 ─────────────

typedef struct ThumbnailRequest
{
    ShelfModel *model;
    ShelfItem *item;
    unsigned request;
} ThumbnailRequest;

static void thumbnail_ready(GfxImage *image, void *context)
{
    ThumbnailRequest *request = context;
    ShelfItem *item = request->item;
    if (image && !request->model->released && item->thumbnail_wanted && item->thumbnail_request == request->request)
    {
        shelf_item_set_thumbnail(item, image);
        changed(request->model, SHELF_CHANGED_ITEM_TEXT);
    }
    else gfx_image_release(image);
    shelf_item_release(item);
    shelf_model_release(request->model);
    free(request);
}

static bool thumbnail_wanted(const ShelfModel *model, int index)
{
    if (index >= model->count - 3) return true;
    return model->expanded && model->visible_from >= 0 && index >= model->visible_from && index <= model->visible_to;
}

void shelf_model_update_thumbnails(ShelfModel *model)
{
    for (int index = 0; index < model->count; index++)
    {
        ShelfItem *item = model->items[index];
        bool wanted = !model->released && shelf_item_is_file_system(item) && thumbnail_wanted(model, index);
        if (!wanted)
        {
            if (!item->thumbnail_wanted) continue;
            item->thumbnail_wanted = false;
            item->thumbnail_request++;
            shelf_item_set_thumbnail(item, NULL);
            continue;
        }
        if (item->thumbnail_wanted) continue;
        item->thumbnail_wanted = true;
        if (item->thumbnail) continue;
        ThumbnailRequest *request = xcalloc(1, sizeof(ThumbnailRequest));
        request->model = shelf_model_retain(model);
        request->item = shelf_item_retain(item);
        request->request = ++item->thumbnail_request;
        thumbnail_load(item->value, model->thumbnail_size, thumbnail_ready, request);
    }
}

void shelf_model_release_resources(ShelfModel *model)
{
    if (model->released) return;
    model->released = true;
    InterlockedExchange(&model->lifetime_over, 1);
    unsigned delay = model->delay_cache_cleanup ? TEMPORARY_FILE_GRACE : 0;
    for (int index = 0; index < model->caches.count; index++) attachment_cache_release_later(model->caches.items[index], delay);
    strlist_clear(&model->caches);
    free(model->own_cache);
    model->own_cache = NULL;
    for (int index = 0; index < model->count; index++)
    {
        forget_item(model, model->items[index]);
        shelf_item_release(model->items[index]);
    }
    model->count = 0;
    for (int index = 0; index < model->drag_count; index++) free(model->drag_paths[index]);
    free(model->drag_items);
    free(model->drag_paths);
    model->drag_items = NULL;
    model->drag_paths = NULL;
    model->drag_count = 0;
    free(model->leaving);
    model->leaving = NULL;
    model->leaving_count = 0;
}

FileSuit shelf_model_suit(const ShelfModel *model)
{
    if (model->count == 0) return SUIT_OTHER;
    FileSuit *suits = xalloc((size_t)model->count * sizeof(FileSuit));
    for (int index = 0; index < model->count; index++) suits[index] = shelf_item_suit(model->items[index]);
    FileSuit suit = file_suit_dominant(suits, model->count);
    free(suits);
    return suit;
}
