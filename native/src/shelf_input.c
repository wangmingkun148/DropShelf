// 置物架的鼠标、键盘、右键菜单、拖入拖出、提示和处理中的遮罩，和 C# 版的
// ShelfWindow.DropIn.cs、ShelfWindow.DragOut.cs、ShelfWindow.Menu.cs 以及 ShelfWindow.xaml.cs 里的提示部分对应。
#include "look_variant.h"
#include "shelf_window_internal.h"
#include "attachment_cache.h"
#ifdef _WIN32
#include "clipboard.h"
#else
#include "ui_linux.h"
#include <gdk/gdk.h>
#endif
#include "folder_watch.h"
#include "history_menu.h"
#include "log.h"
#include "task.h"
#include "theme.h"

#include <math.h>
#if defined(_WIN32) && defined(LOOK_VARIANT_CARDS)
#include <shellapi.h>
#endif
#include <stdlib.h>
#include <string.h>

#define TOAST_DURATION 2400
#define ACTION_TOAST_DURATION 5000      // 带按钮的提示多停留一会儿，来得及看清和点到
#define BUSY_DELAY 150                  // 开始处理后过这么久还没完成才显示遮罩
#define BUSY_MINIMUM 300                // 显示后至少停留这么久
#define BUSY_FADE 0.120

enum
{
    MENU_EXPAND = 1,
    MENU_COLLAPSE,
    MENU_SELECT_ALL,
    MENU_COPY,
    MENU_PASTE,
    MENU_COPY_PATH,
    MENU_REVEAL,
    MENU_CONVERT,
    MENU_CLEAR,
    MENU_SETTINGS,
    MENU_CLOSE,
    MENU_OPEN,
    MENU_REMOVE,
    MENU_RESTORE_LAST,
    MENU_RECENT,
    MENU_CLEAR_HISTORY,
    MENU_MOVE_TO_NEW_SHELF,
    MENU_HISTORY_FIRST = 100
};

/// “最近关闭的置物架”子菜单打开期间拿着的记录。
static HistoryMenu history_menu;

bool shelf_close_when_empty(void)
{
    return app_settings()->close_when_empty;
}

double shelf_seconds_since(LARGE_INTEGER start)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

void shelf_fade(Anim *anim, double to, double duration)
{
    if (anim->value == to && !anim->running) return;
    anim_start(anim, to, duration, EASE_OF(EASE_CUBIC_OUT));
}

void shelf_request_frame(ShelfWindow *shelf)
{
    if (shelf->ui) ui_window_request_frame(shelf->ui);
}

// ───────────── 选中的项目 ─────────────

/// 选中的项目，按在置物架里的顺序。调用方 free 数组。
ShelfItem **shelf_selected_items(ShelfWindow *shelf, int *count)
{
    ShelfModel *model = shelf->model;
    ShelfItem **items = xalloc((size_t)(model->count ? model->count : 1) * sizeof(ShelfItem *));
    *count = 0;
    for (int index = 0; index < model->count; index++)
        if (model->items[index]->selected) items[(*count)++] = model->items[index];
    return items;
}

/// 展开且有选中项时只处理选中项，否则处理整个置物架。
static ShelfItem **target_items(ShelfWindow *shelf, int *count)
{
    ShelfItem **items = shelf->model->expanded ? shelf_selected_items(shelf, count) : NULL;
    if (items && *count > 0) return items;
    free(items);
    *count = shelf->model->count;
    items = xalloc((size_t)(*count ? *count : 1) * sizeof(ShelfItem *));
    memcpy(items, shelf->model->items, (size_t)*count * sizeof(ShelfItem *));
    return items;
}

void shelf_clear_selection(ShelfWindow *shelf)
{
    for (int index = 0; index < shelf->model->count; index++) shelf->model->items[index]->selected = false;
}

static void expand(ShelfWindow *shelf)
{
    shelf_clear_selection(shelf);
    shelf_model_set_expanded(shelf->model, true);
}

static void select_all(ShelfWindow *shelf)
{
    if (shelf->model->count < 2) return;
    if (!shelf->model->expanded) expand(shelf);
    for (int index = 0; index < shelf->model->count; index++) shelf->model->items[index]->selected = true;
    shelf_repaint(shelf);
}

// ───────────── 提示 ─────────────

static void free_convert_items(ShelfWindow *shelf)
{
    for (int index = 0; index < shelf->convert_count; index++) shelf_item_release(shelf->convert_items[index]);
    free(shelf->convert_items);
    shelf->convert_items = NULL;
    shelf->convert_count = 0;
}

static void start_toast_timer(ShelfWindow *shelf)
{
    if (!shelf->handle) return;
    SetTimer(shelf->handle, SHELF_TIMER_TOAST, shelf->toast_action != TOAST_ACTION_NONE ? ACTION_TOAST_DURATION : TOAST_DURATION, NULL);
    shelf->toast_timer_running = true;
}

static void stop_toast_timer(ShelfWindow *shelf)
{
    if (shelf->handle) KillTimer(shelf->handle, SHELF_TIMER_TOAST);
    shelf->toast_timer_running = false;
}

void shelf_show_toast(ShelfWindow *shelf, const wchar_t *message, bool success, ToastAction action)
{
    if (shelf->closing || shelf->closed || !message) return;
    // 换成别的提示后“撤销”按钮不在了，撤销也随之失效：看得见按钮时才能撤销。
    shelf_removal_free(shelf->undo);
    shelf->undo = NULL;
    if (action != TOAST_ACTION_CONVERT) free_convert_items(shelf);
    free(shelf->toast_text);
    shelf->toast_text = wcs_dup(message);
    shelf->toast_success = success;
    shelf->toast_action = action;
    shelf->toast_visible = true;
    anim_start(&shelf->toast_opacity, 1, 0.140, EASE_OF(EASE_LINEAR));
    anim_set(&shelf->toast_offset, 8);
    anim_start(&shelf->toast_offset, 0, 0.200, EASE_OF(EASE_CUBIC_OUT));
    stop_toast_timer(shelf);
    start_toast_timer(shelf);
    shelf_request_frame(shelf);
    shelf_repaint(shelf);
}

void shelf_notify(void *context, const wchar_t *message, bool success)
{
    shelf_show_toast(context, message, success, TOAST_ACTION_NONE);
}

void shelf_hide_toast(ShelfWindow *shelf)
{
    stop_toast_timer(shelf);
    shelf_removal_free(shelf->undo);
    shelf->undo = NULL;
    free_convert_items(shelf);
    shelf->toast_action = TOAST_ACTION_NONE;
    shelf->toast_visible = false;
    anim_start(&shelf->toast_opacity, 0, 0.180, EASE_OF(EASE_LINEAR));
    shelf_request_frame(shelf);
    if (!shelf->close_after_toast) return;
    shelf->close_after_toast = false;
    ShelfModel *model = shelf->model;
    if (model->count == 0 && !model->busy && !shelf->dragging_out && !shelf->drag_inside && shelf->receiving == 0) shelf_window_close_animated(shelf);
}

static void offer_undo(ShelfWindow *shelf, ShelfRemoval *removal, bool expanded)
{
    wchar_t *message = str_removed_items(removal->count);
    shelf_show_toast(shelf, message, true, TOAST_ACTION_UNDO);
    free(message);
    shelf->undo = removal;
    shelf->undo_expanded = expanded && shelf->model->count == 0;
    shelf->close_after_toast = shelf->model->count == 0 && shelf_close_when_empty();
}

#ifdef _WIN32
/// 文本或链接拖到了不接收文字的地方：提示可以转成文件，转好后再拖就和别的文件一样。
void shelf_offer_convert(ShelfWindow *shelf, ShelfItem **items, int count)
{
    shelf_show_toast(shelf, S(STR_TextNotAccepted), false, TOAST_ACTION_CONVERT);
    free_convert_items(shelf);
    shelf->convert_items = xalloc((size_t)count * sizeof(ShelfItem *));
    for (int index = 0; index < count; index++) shelf->convert_items[index] = shelf_item_retain(items[index]);
    shelf->convert_count = count;
}
#endif

static void convert_to_files(ShelfWindow *shelf, ShelfItem **items, int count)
{
    int converted = shelf_model_convert_to_files(shelf->model, items, count);
    if (converted > 0)
    {
        wchar_t *message = str_converted_to_files(converted);
        shelf_notify(shelf, message, true);
        free(message);
    }
    else if (shelf->toast_action != TOAST_ACTION_NONE) shelf_hide_toast(shelf);
}

/// 放回刚才移除的项目：列表里各行从左边滑回来，堆叠里的牌从“撤销”按钮处飞回去。
static bool undo_removal(ShelfWindow *shelf)
{
    if (!shelf->undo || shelf->closing) return false;
    GfxRect button = shelf->layout.toast_action;
    float origin_x = (button.left + button.right) / 2, origin_y = (button.top + button.bottom) / 2;
    shelf->close_after_toast = false;
    ShelfRemoval *removal = shelf->undo;
    bool expanded = shelf->undo_expanded;
    shelf->undo = NULL;
    shelf_hide_toast(shelf);
    shelf_finish_motion(shelf);
    ShelfItem **restored = NULL;
    int count = shelf_model_restore(shelf->model, removal, &restored);
    shelf_removal_free(removal);
    if (count == 0) return true;
    if (expanded) shelf_model_set_expanded(shelf->model, true);
    for (int index = 0; index < shelf->returning_count; index++) shelf_item_release(shelf->returning_items[index]);
    free(shelf->returning_items);
    for (int index = 0; index < count; index++) shelf_item_retain(restored[index]);
    shelf->returning = true;
    shelf->returning_x = origin_x;
    shelf->returning_y = origin_y;
    shelf->returning_items = restored;
    shelf->returning_count = count;
    shelf_model_refresh_availability_in_background(shelf->model);
    return true;
}

static void on_toast_action(ShelfWindow *shelf)
{
    // 提示由各个操作自己收起或换掉：撤销要先取消“提示消失后关闭”，不能在这里先收起。
    if (shelf->toast_action == TOAST_ACTION_UNDO) undo_removal(shelf);
    else if (shelf->toast_action == TOAST_ACTION_CONVERT && shelf->convert_count > 0)
    {
        int count = shelf->convert_count;
        ShelfItem **items = xalloc((size_t)count * sizeof(ShelfItem *));
        for (int index = 0; index < count; index++) items[index] = shelf_item_retain(shelf->convert_items[index]);
        convert_to_files(shelf, items, count);
        for (int index = 0; index < count; index++) shelf_item_release(items[index]);
        free(items);
    }
}

// ───────────── 正在处理 ─────────────

/// 处理中的遮罩：很快就完成的操作不显示，免得一闪而过；显示出来以后至少停留一会儿再淡出。
void shelf_update_busy_overlay(ShelfWindow *shelf, bool waited)
{
    if (shelf->handle) KillTimer(shelf->handle, SHELF_TIMER_BUSY);
    bool busy = shelf->model->busy;
    if (busy == shelf->busy_shown || shelf->closing || shelf->closed) return;
    int wait = busy ? BUSY_DELAY : BUSY_MINIMUM - (int)(shelf_seconds_since(shelf->busy_shown_at) * 1000);
    if (!waited && wait > 0)
    {
        SetTimer(shelf->handle, SHELF_TIMER_BUSY, (UINT)wait, NULL);
        return;
    }
    shelf->busy_shown = busy;
    if (busy)
    {
        QueryPerformanceCounter(&shelf->busy_shown_at);
        shelf->ring_clock = (MotionClock){0};
        anim_set(&shelf->busy_opacity, 0);
        anim_start(&shelf->busy_opacity, 1, BUSY_FADE, EASE_OF(EASE_LINEAR));
    }
    else anim_start(&shelf->busy_opacity, 0, BUSY_FADE, EASE_OF(EASE_LINEAR));
    shelf_request_frame(shelf);
}

// ───────────── 移除 ─────────────

void shelf_perform_remove(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool expanded_before)
{
    ShelfRemoval *removal = count > 0 ? shelf_model_remove(shelf->model, items, count) : NULL;
    if (kind == REMOVE_BY_USER)
    {
        if (removal && !shelf->closing) offer_undo(shelf, removal, expanded_before);
        else shelf_removal_free(removal);
        return;
    }
    shelf_removal_free(removal);
    if (shelf->model->count == 0 && shelf_close_when_empty()) shelf_window_close_animated(shelf);
}

/// 移除项目并播放移除动画；全部移走且设置为自动关闭时不播放，直接关闭置物架。用于拖出之后。
void shelf_remove_items(ShelfWindow *shelf, ShelfItem **items, int count)
{
    if (count == 0) return;
    shelf->model->has_interacted = true;
    bool close_directly = shelf_close_when_empty() && shelf_model_all_items_in(shelf->model, items, count);
    shelf_remove_with_motion(shelf, items, count, REMOVE_VISIBLY, !close_directly);
}

void shelf_remove_visibly(void *context, ShelfItem **items, int count)
{
    shelf_remove_items(context, items, count);
}

/// 手动移除（行尾的按钮、Delete、菜单里的移除和清空）：移除后提示“已移除 N 项”，几秒内可以撤销。
static void remove_by_user(ShelfWindow *shelf, ShelfItem **items, int count)
{
    if (count == 0) return;
    shelf->model->has_interacted = true;
    shelf_remove_with_motion(shelf, items, count, REMOVE_BY_USER, true);
}

void shelf_close_if_still_empty(ShelfWindow *shelf)
{
    if (shelf->model->count > 0 || shelf->closing || !shelf_close_when_empty()) return;
    // 正显示着结果提示时等提示消失再关，提示能完整看完；没有提示时稍等一会儿再关。
    if (shelf->toast_timer_running)
    {
        shelf->close_after_toast = true;
        return;
    }
    SetTimer(shelf->handle, SHELF_TIMER_CLOSE_EMPTY, 1500, NULL);
}

// ───────────── 放入 ─────────────

// ───────────── 快速操作 ─────────────

void shelf_produced(void *context, const StrList *paths)
{
    shelves_open_beside(context, paths);
}

static void action_finished(ShelfModel *model, const QuickActionResult *result, void *context)
{
    ShelfWindow *shelf = context;
    // 置物架已在处理途中关闭：生成的文件放进新的置物架。
    if (model->released)
    {
        const StrList *produced = result->produced_new_items ? &result->outputs : &result->replaced_outputs;
        if (produced->count > 0) shelves_open_beside(NULL, produced);
    }
    shelf_window_release(shelf);
}

static void dropped_action_finished(ShelfModel *model, const QuickActionResult *result, void *context)
{
    ShelfWindow *shelf = context;
    if (!model->released && result->succeeded && !shelf->closing) shelf_close_if_still_empty(shelf);
    action_finished(model, result, context);
}

static void run_action(ShelfWindow *shelf, int index)
{
    ShelfModel *model = shelf->model;
    if (model->busy || index < 0 || index >= shelf->actions.count || shelf->closing) return;
    int count;
    ShelfItem **targets = target_items(shelf, &count);
    shelf_model_run_action(model, &shelf->actions.items[index], targets, count, shelf->handle, action_finished, shelf_window_retain(shelf));
    free(targets);
}

void shelf_show_receiving(ShelfWindow *shelf, const wchar_t *text)
{
    shelf_model_set_busy(shelf->model, text);
}

static float clamp_inside(float value, float size)
{
    float high = size - 16 > 16 ? size - 16 : 16;
    return value < 16 ? 16 : value > high ? high : value;
}

void shelf_received(DropReadResult *result, void *context)
{
    Receive *receive = context;
    ShelfWindow *shelf = receive->shelf;
    if (!receive->paths)
    {
        shelf->receiving--;
        shelf_show_receiving(shelf, NULL);
    }
    if (shelf->closing || shelf->closed)
    {
        drop_result_release_caches(result);
        drop_result_free(result);
        quick_action_free(&receive->action);
        shelf_window_release(shelf);
        free(receive);
        return;
    }
    if (receive->for_action)
    {
        shelf_model_run_action_on_dropped(shelf->model, &receive->action, result, shelf->handle, dropped_action_finished, shelf_window_retain(shelf));
        drop_result_free(result);
        quick_action_free(&receive->action);
        shelf_window_release(shelf);
        free(receive);
        return;
    }
    const wchar_t *error = result->error;
    int failed = result->failed;
    wchar_t *error_copy = wcs_dup(error);
    ShelfAddSummary summary = shelf_model_add(shelf->model, result);
    wchar_t *message = shelf_add_summary_message(summary);
    if (receive->restore)
    {
        wchar_t *restored = summary.added > 0 ? str_restored(summary.added, summary.unavailable) : wcs_dup(S(STR_RestoreUnavailable));
        shelf_notify(shelf, restored, summary.added > 0);
        free(restored);
    }
    else if (receive->paths)
    {
        // 命令行传来的文件：有重复或读不到的才提示。
        if (summary.duplicates > 0 || summary.unavailable > 0) shelf_notify(shelf, message, summary.added > 0);
    }
    else if (receive->paste)
    {
        // 和拖进来的一样落进堆叠或列表：从光标那里飞进来，光标在窗口外时从离它最近的窗口边上飞进来。
        if (summary.added > 0 && shelf_window_is_visible(shelf))
        {
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(shelf->handle, &cursor);
            float scale = shelf_scale(shelf);
            GfxSize size = ui_window_size(shelf->ui);
            shelf->landing = true;
            shelf->landing_x = clamp_inside(cursor.x / scale, size.width);
            shelf->landing_y = clamp_inside(cursor.y / scale, size.height);
            shelf->landing_added = summary.added;
        }
        shelf_notify(shelf, summary.added == 0 && error_copy ? error_copy : message, summary.added > 0);
    }
    else
    {
        if (summary.added > 0)
        {
            shelf->landing = true;
            shelf->landing_x = receive->x;
            shelf->landing_y = receive->y;
            shelf->landing_added = summary.added;
        }
#ifdef LOOK_VARIANT_CARDS
        // 都已在置物架里或读取失败：摊开的牌直接合拢。
        else shelf_close_fan(shelf);
#endif
        if (summary.added == 0) shelf_notify(shelf, error_copy ? error_copy : message, false);
        else if (summary.duplicates > 0 || summary.unavailable > 0 || failed > 0)
        {
            if (failed > 0)
            {
                wchar_t *combined = str_with_failed_attachments(message, failed);
                shelf_notify(shelf, combined, true);
                free(combined);
            }
            else shelf_notify(shelf, message, true);
        }
        if (shelf->model->count == 0) shelf_close_if_still_empty(shelf);
    }
    free(message);
    free(error_copy);
    drop_result_free(result);
    shelf_window_release(shelf);
    free(receive);
}

void shelf_add_paths(ShelfWindow *shelf, const StrList *paths)
{
    if (paths->count == 0) return;
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->paths = true;
    drop_read_paths(paths, shelf_received, receive);
}

void shelf_restore_paths(ShelfWindow *shelf, const StrList *paths)
{
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->paths = true;
    receive->restore = true;
    if (paths->count > 0) drop_read_paths(paths, shelf_received, receive);
    else
    {
        DropReadResult *empty = xcalloc(1, sizeof(DropReadResult));
        shelf_received(empty, receive);
    }
}

#ifdef _WIN32
static void paste(ShelfWindow *shelf)
{
    IDataObject *data = clipboard_get_data();
    if (!data)
    {
        shelf_notify(shelf, S(STR_ClipboardEmpty), false);
        return;
    }
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->paste = true;
    shelf->receiving++;
    drop_read(data, false, true, &shelf->model->lifetime_over, shelf_received, receive);
    IDataObject_Release(data);
}
#else

typedef struct PasteRead
{
    ShelfWindow *shelf;
    float x, y;
} PasteRead;

static void paste_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    PasteRead *read = data;
    ShelfWindow *shelf = read->shelf;
    GError *error = NULL;
    const GValue *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), result, &error);
    if (error)
    {
        log_info(L"粘贴：读取剪贴板失败：%hs", error->message);
        g_error_free(error);
    }
    // 这个 GValue 归 GdkClipboard 所有（transfer none），不能 unset/free；以前在这里释放会让
    // 类型系统在之后某次 g_value_unset 时读到坏掉的类型指针，按 Ctrl+V 后程序会崩。
    if (!shelf || shelf->closed)
    {
        log_trace(L"粘贴：置物架已经关闭，忽略剪贴板内容");
        shelf_window_release(shelf);
        free(read);
        return;
    }
    if (!value)
    {
        log_info(L"粘贴：剪贴板里没有可用的内容");
        shelf_notify(shelf, S(STR_ClipboardEmpty), false);
        shelf_window_release(shelf);
        free(read);
        return;
    }
    GType type = G_VALUE_TYPE(value);
    log_info(L"粘贴：剪贴板内容是 %hs", g_type_name(type) ? g_type_name(type) : "未知类型");
    if (type == GDK_TYPE_FILE_LIST)
    {
        StrList paths = {0};
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
        for (GSList *each = files; each; each = each->next)
        {
            char *path = g_file_get_path(G_FILE(each->data));
            if (!path) continue;
            wchar_t *wide = wide_from_utf8(path, strlen(path));
            if (wide)
            {
                strlist_add(&paths, wide);
                free(wide);
            }
            g_free(path);
        }
        g_slist_free(files);
        shelf_receive_paths_at(shelf, &paths, read->x, read->y);
        strlist_clear(&paths);
    }
    else if (type == G_TYPE_FILE)
    {
        StrList paths = {0};
        char *path = g_file_get_path(g_value_get_object(value));
        if (path)
        {
            wchar_t *wide = wide_from_utf8(path, strlen(path));
            if (wide)
            {
                strlist_add(&paths, wide);
                free(wide);
            }
            g_free(path);
        }
        shelf_receive_paths_at(shelf, &paths, read->x, read->y);
        strlist_clear(&paths);
    }
    else if (type == G_TYPE_STRING)
    {
        const char *utf8 = g_value_get_string(value);
        wchar_t *wide = utf8 ? wide_from_utf8(utf8, strlen(utf8)) : NULL;
        if (wide)
        {
            shelf_receive_text_at(shelf, wide, read->x, read->y);
            free(wide);
        }
    }
    else if (type == GDK_TYPE_TEXTURE) shelf_receive_texture_at(shelf, g_value_get_object(value), read->x, read->y);
    else log_info(L"粘贴：剪贴板里的 %hs 不是能放上置物架的内容", g_type_name(type) ? g_type_name(type) : "未知类型");
    shelf_window_release(shelf);
    free(read);
}

static void paste(ShelfWindow *shelf)
{
    GdkDisplay *display = gdk_display_get_default();
    GdkClipboard *clipboard = display ? gdk_display_get_clipboard(display) : NULL;
    if (!clipboard)
    {
        shelf_notify(shelf, S(STR_ClipboardEmpty), false);
        return;
    }
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    GType type = G_TYPE_NONE;
    if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST)) type = GDK_TYPE_FILE_LIST;
    else if (gdk_content_formats_contain_gtype(formats, G_TYPE_FILE)) type = G_TYPE_FILE;
    else if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) type = GDK_TYPE_TEXTURE;
    else if (gdk_content_formats_contain_gtype(formats, G_TYPE_STRING)) type = G_TYPE_STRING;
    char *format_text = formats ? gdk_content_formats_to_string(formats) : NULL;
    log_info(L"粘贴：开始读取剪贴板%s%hs", type == G_TYPE_NONE ? L"（没有支持的格式）" : L"",
        format_text ? format_text : "");
    g_free(format_text);
    if (type == G_TYPE_NONE)
    {
        shelf_notify(shelf, S(STR_ClipboardEmpty), false);
        return;
    }
    PasteRead *read = xcalloc(1, sizeof(PasteRead));
    read->shelf = shelf_window_retain(shelf);
    POINT cursor;
    GetCursorPos(&cursor);
    ScreenToClient(shelf->handle, &cursor);
    float scale = shelf_scale(shelf);
    read->x = cursor.x / scale;
    read->y = cursor.y / scale;
    gdk_clipboard_read_value_async(clipboard, type, G_PRIORITY_DEFAULT, NULL, paste_ready, read);
}
#endif

// ───────────── 菜单 ─────────────

/// 收下 items 数组本身（调用方不再释放），并持有其中每一项。
static void set_menu_items(ShelfWindow *shelf, ShelfItem **items, int count)
{
    for (int index = 0; index < shelf->menu_count; index++) shelf_item_release(shelf->menu_items[index]);
    free(shelf->menu_items);
    shelf->menu_items = items;
    shelf->menu_count = count;
    for (int index = 0; index < count; index++) shelf_item_retain(items[index]);
}

static void on_menu(int id, void *context)
{
    ShelfWindow *shelf = context;
    if (shelf->closing || shelf->closed) return;
    ShelfModel *model = shelf->model;
    // 菜单打开以后可能有项目已经被移走，只处理还在的。
    int count = 0;
    ShelfItem **items = xalloc((size_t)(shelf->menu_count ? shelf->menu_count : 1) * sizeof(ShelfItem *));
    for (int index = 0; index < shelf->menu_count; index++)
        if (shelf_model_contains(model, shelf->menu_items[index])) items[count++] = shelf->menu_items[index];
    switch (id)
    {
        case MENU_EXPAND: model->has_interacted = true; expand(shelf); break;
        case MENU_COLLAPSE: shelf_model_set_expanded(model, false); break;
        case MENU_SELECT_ALL: select_all(shelf); break;
        case MENU_COPY: shelf_model_copy(model, items, count); break;
        case MENU_PASTE: paste(shelf); break;
        case MENU_COPY_PATH: shelf_model_copy_paths(model, items, count); break;
        case MENU_REVEAL: shelf_model_reveal(model, items, count); break;
        case MENU_CONVERT: convert_to_files(shelf, items, count); break;
        case MENU_CLEAR:
        {
            int all = model->count;
            ShelfItem **everything = xalloc((size_t)(all ? all : 1) * sizeof(ShelfItem *));
            memcpy(everything, model->items, (size_t)all * sizeof(ShelfItem *));
            remove_by_user(shelf, everything, all);
            free(everything);
            break;
        }
        case MENU_SETTINGS: app_show_main_window(); break;
        case MENU_CLOSE: shelf_window_close_animated(shelf); break;
        case MENU_OPEN: if (count == 1) shelf_model_open(model, items[0]); break;
        case MENU_REMOVE: remove_by_user(shelf, items, count); break;
        case MENU_MOVE_TO_NEW_SHELF: if (count > 0 && count < model->count) shelf_split_off_from_menu(shelf, items, count); break;
        case MENU_CLEAR_HISTORY: shelf_history_clear(); break;
        // 恢复的项目放进当前的空置物架；当前置物架已有内容时另开一个。
        case MENU_RESTORE_LAST: shelves_restore(history_menu_entry(&history_menu, MENU_HISTORY_FIRST), model->count == 0 ? shelf : NULL); break;
        default:
        {
            ShelfHistoryEntry *entry = history_menu_entry(&history_menu, id);
            if (entry) shelves_restore(entry, model->count == 0 ? shelf : NULL);
            break;
        }
    }
    free(items);
}

static UiMenuItem menu_item(int id, StringId text, const wchar_t *glyph, const wchar_t *gesture, bool enabled)
{
    UiMenuItem item = {0};
    item.id = id;
    item.text = S(text);
    item.glyph = glyph;
    item.gesture = gesture;
    item.disabled = !enabled;
    return item;
}

static UiMenuItem separator(void)
{
    UiMenuItem item = {0};
    item.separator = true;
    return item;
}

/// 打开菜单的置物架拿着一份引用，直到下一次有菜单打开：菜单关着不选时不会回调，置物架先关掉了也不会访问到释放的内存。
static ShelfWindow *menu_owner;

static void show_menu(ShelfWindow *shelf, UiMenuItem *items, int count)
{
    POINT cursor;
    GetCursorPos(&cursor);
    shelf_window_retain(shelf);
    if (menu_owner) shelf_window_release(menu_owner);
    menu_owner = shelf;
    ui_menu_show(items, count, cursor.x, cursor.y, on_menu, shelf);
}

static void open_shelf_menu(ShelfWindow *shelf)
{
    ShelfModel *model = shelf->model;
    int count;
    ShelfItem **targets = target_items(shelf, &count);
    set_menu_items(shelf, targets, count);
    bool has_items = model->count > 0, file_system = false, text = false;
    for (int index = 0; index < shelf->menu_count; index++)
    {
        if (shelf_item_is_file_system(shelf->menu_items[index])) file_system = true;
        else text = true;
    }
    UiMenuItem items[16];
    int n = 0;
    if (model->expanded) items[n++] = menu_item(MENU_COLLAPSE, STR_MenuCollapse, L"\xE81E", L"Esc", true);
#ifdef LOOK_VARIANT_CARDS
    else if (has_items) items[n++] = menu_item(MENU_EXPAND, STR_MenuExpand, L"\xF0E2", NULL, true);
#else
    else if (has_items) items[n++] = menu_item(MENU_EXPAND, STR_MenuExpand, L"\xE8FD", NULL, true);
#endif
    items[n++] = menu_item(MENU_SELECT_ALL, STR_MenuSelectAll, L"\xE8B3", L"Ctrl+A", model->count > 1);
    items[n++] = menu_item(MENU_COPY, STR_MenuCopy, L"\xE8C8", L"Ctrl+C", has_items);
    items[n++] = menu_item(MENU_PASTE, STR_MenuPaste, L"\xE77F", L"Ctrl+V", true);
    items[n++] = menu_item(MENU_COPY_PATH, STR_MenuCopyPath, L"\xE71B", NULL, file_system);
    items[n++] = menu_item(MENU_REVEAL, STR_MenuReveal, L"\xE838", NULL, file_system);
    if (text) items[n++] = menu_item(MENU_CONVERT, STR_MenuConvertToFile, L"\xE8A5", NULL, true);
    items[n++] = separator();
    history_menu_build(&history_menu, MENU_HISTORY_FIRST, MENU_CLEAR_HISTORY);
    if (history_menu.count > 0)
    {
        items[n++] = menu_item(MENU_RESTORE_LAST, STR_RestoreLastShelf, L"\xE81C", NULL, true);
        items[n] = history_menu_root(&history_menu);
        items[n++].id = MENU_RECENT;
    }
    items[n++] = menu_item(MENU_CLEAR, STR_MenuClear, L"\xE894", NULL, has_items);
    items[n++] = menu_item(MENU_SETTINGS, STR_MenuSettings, L"\xE713", NULL, true);
    items[n++] = separator();
    // 展开着列表时 Esc 是收起，只在“收起”上标出来；收起后 Esc 才是关闭。
    items[n++] = menu_item(MENU_CLOSE, STR_CloseShelf, L"\xE711", model->expanded ? NULL : L"Esc", true);
    show_menu(shelf, items, n);
    history_menu_free_texts(&history_menu);
}

static void open_item_menu(ShelfWindow *shelf)
{
    int count;
    ShelfItem **selected = shelf_selected_items(shelf, &count);
    set_menu_items(shelf, selected, count);
    ShelfItem *single = count == 1 ? shelf->menu_items[0] : NULL;
    bool file_system = false, text = false;
    for (int index = 0; index < count; index++)
    {
        if (shelf_item_is_file_system(shelf->menu_items[index])) file_system = true;
        else text = true;
    }
    UiMenuItem items[10];
    int n = 0;
    items[n++] = menu_item(MENU_OPEN, single && single->kind == ITEM_TEXT ? STR_MenuCopyText : STR_MenuOpen, L"\xE8A7", L"Enter", single != NULL);
    items[n++] = menu_item(MENU_REVEAL, STR_MenuReveal, L"\xE838", NULL, file_system);
    items[n++] = menu_item(MENU_COPY, STR_MenuCopy, L"\xE8C8", L"Ctrl+C", true);
    items[n++] = menu_item(MENU_COPY_PATH, STR_MenuCopyPath, L"\xE71B", NULL, file_system);
    if (text) items[n++] = menu_item(MENU_CONVERT, STR_MenuConvertToFile, L"\xE8A5", NULL, true);
    if (count < shelf->model->count) items[n++] = menu_item(MENU_MOVE_TO_NEW_SHELF, STR_MenuMoveToNewShelf, L"\xE78B", NULL, true);
    items[n++] = separator();
    items[n++] = menu_item(MENU_REMOVE, STR_MenuRemove, L"\xE711", L"Delete", true);
    show_menu(shelf, items, n);
}

// ───────────── 鼠标 ─────────────

static bool in_rect(GfxRect rect, float x, float y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

ShelfTarget shelf_hit_test(ShelfWindow *shelf, float x, float y, int *row)
{
    *row = -1;
    const ShelfLayout *layout = &shelf->layout;
    // 带按钮的提示可以点，盖在别的内容上面。
    if (shelf->toast_visible && shelf->toast_action != TOAST_ACTION_NONE && in_rect(layout->toast, x, y))
        return in_rect(layout->toast_action, x, y) ? TARGET_TOAST_ACTION : TARGET_TOAST;
    if (shelf->busy_shown) return TARGET_BLOCKED;
    if (shelf->overlay_hit || shelf->closing) return TARGET_NONE;
    if (in_rect(layout->pin, x, y)) return TARGET_PIN;
    if (in_rect(layout->mode, x, y)) return TARGET_MODE;
    if (shelf->model->count > 0 && in_rect(layout->view, x, y)) return TARGET_VIEW;
    if (in_rect(layout->more, x, y)) return TARGET_MORE;
    if (in_rect(layout->close, x, y)) return TARGET_CLOSE;
    if (y >= 4 && y < SHELF_HEADER_HEIGHT && x >= 4 && x < layout->width - 4) return TARGET_HEADER;
    for (int index = 0; index < shelf->actions.count; index++)
    {
        if (!in_rect(layout->action_buttons[index], x, y)) continue;
        *row = index;
        return TARGET_ACTION;
    }
    if (!in_rect(layout->body, x, y)) return TARGET_NONE;
    if (shelf->model->count == 0) return TARGET_EMPTY;
    if (shelf->switching) return TARGET_NONE;
    if (!shelf->shown_expanded) return in_rect(layout->stack, x, y) ? TARGET_STACK : TARGET_NONE;
    if (!in_rect(layout->list, x, y)) return TARGET_NONE;
    if (layout->scrollable && x >= layout->scroll_track.left) return TARGET_SCROLLBAR;
#ifdef LOOK_VARIANT_CARDS
    // 悬停的牌抬起来，右上角的移除按钮伸出牌角，先看它。
    int hovered = shelf->hovered == TARGET_ROW || shelf->hovered == TARGET_ROW_REMOVE ? shelf->hovered_row : -1;
    if (hovered >= 0 && hovered < shelf->model->count)
    {
        GfxRect card = shelf_tile_card_rect(shelf, hovered);
        float lift = 4 * (float)shelf->model->items[hovered]->lift.value;
        GfxRect face = { card.left, card.top - lift, card.right, card.bottom - lift };
        if (in_rect(shelf_tile_remove_rect(face), x, y))
        {
            *row = hovered;
            return TARGET_ROW_REMOVE;
        }
    }
    int first, last;
    shelf_visible_tiles(shelf, &first, &last);
    for (int index = first; index <= last; index++)
    {
        if (!in_rect(shelf_tile_rect(shelf, index), x, y)) continue;
        *row = index;
        return TARGET_ROW;
    }
    // 牌之间的空白：双击收回堆叠。
    return TARGET_GRID;
#else
    int index = shelf->scroll_first + (int)floorf((y - layout->list.top - 1) / SHELF_ROW_PITCH);
    if (index < 0 || index >= shelf->model->count) return TARGET_NONE;
    GfxRect rect = shelf_row_rect(shelf, index);
    if (!in_rect(rect, x, y)) return TARGET_NONE;
    *row = index;
    GfxRect remove = { rect.right - 6 - 26, rect.top + 8, rect.right - 6, rect.top + 34 };
    return in_rect(remove, x, y) ? TARGET_ROW_REMOVE : TARGET_ROW;
#endif
}

/// 光标下的按钮的提示，和 C# 版各个按钮的 ToolTip 相同。
static const wchar_t *tooltip_for(ShelfWindow *shelf, ShelfTarget target, int row)
{
    switch (target)
    {
        case TARGET_PIN: return S(STR_PinTooltip);
        case TARGET_MODE: return S(shelf->move_mode ? STR_DragOutModeTooltipMove : STR_DragOutModeTooltipCopy);
        case TARGET_VIEW: return S(shelf->model->expanded ? STR_MenuCollapse : STR_MenuExpand);
        case TARGET_MORE: return S(STR_MoreTooltip);
        case TARGET_CLOSE: return S(STR_CloseShelfTooltip);
        case TARGET_ROW_REMOVE: return S(STR_RemoveItemTooltip);
        case TARGET_ACTION: return row >= 0 && row < SHELF_MAX_ACTIONS ? shelf->action_tooltips[row] : NULL;
        default: return NULL;
    }
}

static void update_tooltip(ShelfWindow *shelf)
{
    if (shelf->ui) ui_window_set_tooltip(shelf->ui, shelf->dragging_out ? NULL : tooltip_for(shelf, shelf->hovered, shelf->hovered_row));
}

#ifdef LOOK_VARIANT_CARDS
/// 光标下的那张牌（悬停时抬起），没有时为 -1。
static int hovered_tile(ShelfTarget target, int row)
{
    return target == TARGET_ROW || target == TARGET_ROW_REMOVE ? row : -1;
}

static void lift_tile(ShelfWindow *shelf, int index, bool up)
{
    if (index < 0 || index >= shelf->model->count) return;
    anim_start(&shelf->model->items[index]->lift, up ? 1 : 0, up ? 0.120 : 0.150, EASE_OF(EASE_CUBIC_OUT));
    if (shelf->ui) ui_window_request_frame(shelf->ui);
}

#endif
static void set_hovered(ShelfWindow *shelf, ShelfTarget target, int row, bool scroll)
{
    if (shelf->hovered == target && shelf->hovered_row == row && shelf->scroll_hovered == scroll) return;
#ifdef LOOK_VARIANT_CARDS
    // 悬停的牌抬起 4，移开时放下。
    int was_tile = hovered_tile(shelf->hovered, shelf->hovered_row), is_tile = hovered_tile(target, row);
    if (was_tile != is_tile)
    {
        lift_tile(shelf, was_tile, false);
        lift_tile(shelf, is_tile, true);
    }
#endif
    bool was_toast = shelf->hovered == TARGET_TOAST || shelf->hovered == TARGET_TOAST_ACTION;
    bool is_toast = target == TARGET_TOAST || target == TARGET_TOAST_ACTION;
    shelf->hovered = target;
    shelf->hovered_row = row;
    shelf->scroll_hovered = scroll;
    // 鼠标停在带按钮的提示上时不让它消失，移开后重新计时。
    if (shelf->toast_action != TOAST_ACTION_NONE && shelf->toast_visible)
    {
        if (is_toast && !was_toast) stop_toast_timer(shelf);
        else if (!is_toast && was_toast) start_toast_timer(shelf);
    }
    update_tooltip(shelf);
    shelf_repaint(shelf);
}

static bool passed_drag_threshold(ShelfWindow *shelf, float x, float y)
{
    return fabsf(x - shelf->press_x) >= GetSystemMetrics(SM_CXDRAG) || fabsf(y - shelf->press_y) >= GetSystemMetrics(SM_CYDRAG);
}

#ifdef LOOK_VARIANT_CARDS
/// 平铺的可见高度（DIP）。
static float grid_viewport(ShelfWindow *shelf)
{
    return shelf->layout.list.bottom - shelf->layout.list.top - 2;
}

#endif
static void scroll_to_thumb(ShelfWindow *shelf, float y)
{
    const ShelfLayout *layout = &shelf->layout;
    float track = layout->scroll_track.bottom - layout->scroll_track.top;
    float length = layout->scroll_thumb.bottom - layout->scroll_thumb.top;
#ifdef LOOK_VARIANT_CARDS
    float maximum = layout->extent - grid_viewport(shelf);
#else
    int maximum = shelf->model->count - shelf_visible_rows(shelf);
#endif
    if (maximum <= 0 || track - length <= 0) return;
    float position = (y - shelf->scroll_drag_offset - layout->scroll_track.top) / (track - length);
#ifdef LOOK_VARIANT_CARDS
    shelf_set_scroll_offset(shelf, position * maximum);
#else
    shelf_set_scroll(shelf, (int)lroundf(position * maximum));
#endif
}

static void press_row(ShelfWindow *shelf, int index, float x, float y)
{
    ShelfModel *model = shelf->model;
    ShelfItem *item = model->items[index];
    shelf->pressed_item = item;
    shelf->list_pressed = true;
    shelf->defer_selection = false;
    shelf->press_x = x;
    shelf->press_y = y;
    bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int selected = 0;
    for (int at = 0; at < model->count; at++) selected += model->items[at]->selected;
    if (!control && !shift && item->selected && selected > 1)
    {
        // 保留多选，便于把选中的多个项目一起拖出；未拖动时在松开后再改为单选。
        shelf->defer_selection = true;
        return;
    }
    if (shift && shelf->selection_anchor >= 0 && shelf->selection_anchor < model->count)
    {
        if (!control) shelf_clear_selection(shelf);
        int from = shelf->selection_anchor < index ? shelf->selection_anchor : index;
        int to = shelf->selection_anchor < index ? index : shelf->selection_anchor;
        for (int at = from; at <= to; at++) model->items[at]->selected = true;
    }
    else if (control)
    {
        item->selected = !item->selected;
        shelf->selection_anchor = index;
    }
    else
    {
        shelf_clear_selection(shelf);
        item->selected = true;
        shelf->selection_anchor = index;
    }
    shelf_repaint(shelf);
}

static void click(ShelfWindow *shelf, ShelfTarget target, int row)
{
    ShelfModel *model = shelf->model;
    switch (target)
    {
        case TARGET_PIN:
            model->pinned = !model->pinned;
            model->has_interacted = true;
            ui_window_set_topmost(shelf->ui, model->pinned);
            shelf_repaint(shelf);
            break;
        case TARGET_MODE:
        {
            model->has_interacted = true;
            bool move = !shelf->move_mode;
            int mode = move ? TRANSFER_MOVE : TRANSFER_COPY;
            app_update_drag_out_mode(mode);
            shelf_notify(shelf, S(move ? STR_DragOutModeChangedMove : STR_DragOutModeChangedCopy), true);
            break;
        }
        case TARGET_VIEW:
            model->has_interacted = true;
            if (model->expanded) shelf_model_set_expanded(model, false);
            else expand(shelf);
            break;
        case TARGET_MORE:
            open_shelf_menu(shelf);
            break;
        case TARGET_CLOSE:
            shelf_window_close_animated(shelf);
            break;
        case TARGET_ROW_REMOVE:
            if (row >= 0 && row < model->count)
            {
                ShelfItem *item = model->items[row];
                remove_by_user(shelf, &item, 1);
            }
            break;
        case TARGET_TOAST_ACTION:
            on_toast_action(shelf);
            break;
        case TARGET_ACTION:
            run_action(shelf, row);
            break;
        default:
            break;
    }
    // 切换了视图或移动/复制，按钮的提示跟着变。
    update_tooltip(shelf);
}

bool shelf_on_mouse(UiWindow *window, void *context, const UiMouseEvent *event)
{
    (void)window;
    ShelfWindow *shelf = context;
    if (shelf->closed) return true;
    ShelfModel *model = shelf->model;
    int row = -1;
    ShelfTarget target = event->kind == UI_MOUSE_LEAVE ? TARGET_NONE : shelf_hit_test(shelf, event->x, event->y, &row);
    bool scroll = target == TARGET_SCROLLBAR;
    switch (event->kind)
    {
        case UI_MOUSE_ENTER:
        case UI_MOUSE_MOVE:
            set_hovered(shelf, target, row, scroll);
            if (shelf->scroll_dragging)
            {
                scroll_to_thumb(shelf, event->y);
                break;
            }
            if (shelf->stack_pressed && GetKeyState(VK_LBUTTON) < 0 && passed_drag_threshold(shelf, event->x, event->y))
            {
                shelf->stack_pressed = false;
                shelf->pressed = TARGET_NONE;
                int count = model->count;
                ShelfItem **items = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
                memcpy(items, model->items, (size_t)count * sizeof(ShelfItem *));
                for (int index = 0; index < count; index++) shelf_item_retain(items[index]);
                shelf_begin_drag_out(shelf, items, count);
                for (int index = 0; index < count; index++) shelf_item_release(items[index]);
                free(items);
            }
            else if (shelf->list_pressed && shelf->pressed_item && GetKeyState(VK_LBUTTON) < 0 && passed_drag_threshold(shelf, event->x, event->y))
            {
                shelf->list_pressed = false;
                shelf->defer_selection = false;
                shelf->pressed = TARGET_NONE;
                if (!shelf->pressed_item->selected)
                {
                    shelf_clear_selection(shelf);
                    shelf->pressed_item->selected = true;
                }
                int count;
                ShelfItem **items = shelf_selected_items(shelf, &count);
                for (int index = 0; index < count; index++) shelf_item_retain(items[index]);
                shelf_begin_drag_out(shelf, items, count);
                for (int index = 0; index < count; index++) shelf_item_release(items[index]);
                free(items);
            }
            break;
        case UI_MOUSE_LEAVE:
            if (!shelf->scroll_dragging) set_hovered(shelf, TARGET_NONE, -1, false);
            break;
        case UI_MOUSE_DOWN:
        {
            bool double_click = shelf->double_click;
            shelf->double_click = false;
            set_hovered(shelf, target, row, scroll);
            if (event->right_button)
            {
                if (target == TARGET_STACK) open_shelf_menu(shelf);
                else if (target == TARGET_ROW || target == TARGET_ROW_REMOVE)
                {
                    ShelfItem *item = model->items[row];
                    if (!item->selected)
                    {
                        shelf_clear_selection(shelf);
                        item->selected = true;
                        shelf->selection_anchor = row;
                    }
                    shelf_repaint(shelf);
                    open_item_menu(shelf);
                }
                break;
            }
            shelf->pressed = target;
            shelf->pressed_row = row;
            if (target == TARGET_HEADER || target == TARGET_EMPTY)
            {
                // 拖动标题栏（或空置物架）移动窗口；拖到别的置物架上停一下再松手就合并进去。
                shelf->pressed = TARGET_NONE;
                shelf_stop_slide(shelf);
                model->has_interacted = true;
                shelf_move_by_hand(shelf);
                break;
            }
#ifdef LOOK_VARIANT_CARDS
            // 平铺里按下：单击时“刚由单击牌堆摊开”就过去了；双击的第二下落在刚摊开的牌桌上时不算数，不打开牌也不收回去。
            bool in_grid = target == TARGET_ROW || target == TARGET_ROW_REMOVE || target == TARGET_GRID || target == TARGET_SCROLLBAR;
            if (in_grid && !double_click) shelf->expanded_by_click = false;
            bool grid_double_click = double_click && !shelf->expanded_by_click;
#endif
            if (target == TARGET_STACK)
            {
#ifndef LOOK_VARIANT_CARDS
                if (double_click && model->count == 1)
                {
                    shelf->stack_pressed = false;
                    shelf->pressed = TARGET_NONE;
                    shelf_model_open(model, model->items[0]);
                    break;
                }
#endif
                shelf->stack_pressed = true;
                shelf->press_x = event->x;
                shelf->press_y = event->y;
            }
            else if (target == TARGET_ROW)
            {
#ifdef LOOK_VARIANT_CARDS
                if (double_click)
#else
                if (double_click && row >= 0)
#endif
                {
                    shelf->list_pressed = false;
                    shelf->pressed = TARGET_NONE;
#ifdef LOOK_VARIANT_CARDS
                    if (grid_double_click && row >= 0) shelf_model_open(model, model->items[row]);
#else
                    shelf_model_open(model, model->items[row]);
#endif
                    break;
                }
                press_row(shelf, row, event->x, event->y);
            }
#ifdef LOOK_VARIANT_CARDS
            else if (target == TARGET_GRID)
            {
                // 双击牌之间的空白处收回堆叠。
                shelf->pressed = TARGET_NONE;
                if (grid_double_click)
                {
                    model->has_interacted = true;
                    shelf_model_set_expanded(model, false);
                    break;
                }
            }
#endif
            else if (target == TARGET_SCROLLBAR)
            {
                const ShelfLayout *layout = &shelf->layout;
                if (event->y >= layout->scroll_thumb.top && event->y < layout->scroll_thumb.bottom)
                {
                    shelf->scroll_dragging = true;
                    shelf->scroll_drag_offset = event->y - layout->scroll_thumb.top;
                }
                else
                {
#ifdef LOOK_VARIANT_CARDS
                    float page = grid_viewport(shelf);
                    shelf_set_scroll_offset(shelf, shelf->scroll_offset + (event->y < layout->scroll_thumb.top ? -page : page));
#else
                    int page = shelf_visible_rows(shelf);
                    shelf_set_scroll(shelf, shelf->scroll_first + (event->y < layout->scroll_thumb.top ? -page : page));
#endif
                }
            }
            SetCapture(shelf->handle);
            shelf_repaint(shelf);
            break;
        }
        case UI_MOUSE_UP:
        {
            if (event->right_button) break;
            ShelfTarget pressed = shelf->pressed;
            int pressed_row = shelf->pressed_row;
            shelf->pressed = TARGET_NONE;
            shelf->scroll_dragging = false;
            if (GetCapture() == shelf->handle) ReleaseCapture();
            if (shelf->stack_pressed)
            {
                shelf->stack_pressed = false;
                model->has_interacted = true;
#ifdef LOOK_VARIANT_CARDS
                // 点一下牌堆（没有拖动）就摊开成平铺。这一下若是双击的第一下，第二下会落在刚摊开的牌桌上，不能再把它收回去。
                if (target == TARGET_STACK)
                {
                    shelf->expanded_by_click = true;
                    expand(shelf);
                }
#endif
            }
            if (shelf->list_pressed && shelf->defer_selection && shelf->pressed_item && shelf_model_contains(model, shelf->pressed_item))
            {
                shelf_clear_selection(shelf);
                shelf->pressed_item->selected = true;
                shelf->selection_anchor = shelf_model_index_of(model, shelf->pressed_item);
            }
            shelf->list_pressed = shelf->defer_selection = false;
            shelf->pressed_item = NULL;
            if (pressed != TARGET_NONE && pressed == target && (pressed != TARGET_ROW_REMOVE || pressed_row == row)) click(shelf, pressed, pressed_row);
            shelf_repaint(shelf);
            break;
        }
        case UI_MOUSE_WHEEL:
            if (shelf->shown_expanded && !shelf->switching && model->count > 0)
#ifdef LOOK_VARIANT_CARDS
                shelf_set_scroll_offset(shelf, shelf->scroll_offset - event->wheel * SHELF_ROW_PITCH);
#else
            {
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                if (lines == WHEEL_PAGESCROLL) lines = (UINT)shelf_visible_rows(shelf);
                shelf_set_scroll(shelf, shelf->scroll_first - (int)lroundf(event->wheel * (float)lines));
            }
#endif
            break;
    }
    return true;
}

LPCWSTR shelf_on_cursor(UiWindow *window, void *context, float x, float y)
{
    (void)window;
    ShelfWindow *shelf = context;
    int row = -1;
    switch (shelf_hit_test(shelf, x, y, &row))
    {
        case TARGET_PIN:
        case TARGET_MODE:
        case TARGET_VIEW:
        case TARGET_MORE:
        case TARGET_CLOSE:
        case TARGET_STACK:
        case TARGET_ROW_REMOVE:
        case TARGET_TOAST_ACTION:
        case TARGET_ACTION:
            return IDC_HAND;
        default:
            return IDC_ARROW;
    }
}

// ───────────── 键盘 ─────────────

bool shelf_on_key(UiWindow *window, void *context, unsigned key)
{
    (void)window;
    ShelfWindow *shelf = context;
    ShelfModel *model = shelf->model;
    bool control = GetKeyState(VK_CONTROL) < 0;
    log_trace(L"置物架按键：键码 %u，Ctrl %ls，展开 %ls，项目 %d", key, control ? L"按住" : L"松开",
        model->expanded ? L"是" : L"否", model->count);
    switch (key)
    {
        case VK_ESCAPE:
            if (model->expanded) shelf_model_set_expanded(model, false);
            else shelf_window_close_animated(shelf);
            return true;
        case VK_DELETE:
        {
            if (!model->expanded) return false;
            int count;
            ShelfItem **items = shelf_selected_items(shelf, &count);
            if (count > 0) remove_by_user(shelf, items, count);
            free(items);
            return count > 0;
        }
        case 'Z':
            return control && undo_removal(shelf);
        case VK_RETURN:
        {
            if (!model->expanded) return false;
            int count;
            ShelfItem **items = shelf_selected_items(shelf, &count);
            if (count == 1) shelf_model_open(model, items[0]);
            free(items);
            return count == 1;
        }
        case 'A':
            if (!control) return false;
            select_all(shelf);
            return true;
        case 'C':
        {
            if (!control) return false;
            int count;
            ShelfItem **items = target_items(shelf, &count);
            shelf_model_copy(model, items, count);
            free(items);
            return true;
        }
        case 'V':
            if (!control) return false;
            paste(shelf);
            return true;
    }
    return false;
}

// ───────────── 其他消息 ─────────────

bool shelf_on_message(UiWindow *window, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    (void)window, (void)lparam;
    ShelfWindow *shelf = context;
    if (message == WM_LBUTTONDBLCLK) shelf->double_click = true;
    // 拖着标题栏移动的途中：光标到了另一个置物架上先蓄力，停够了才可以合并。
    if (message == WM_MOVING && shelf->merge_watch) shelf_merge_on_moved(shelf);
    if (message != WM_TIMER) return false;
    *result = 0;
    switch (wparam)
    {
        case SHELF_TIMER_LEAVE:
            KillTimer(shelf->handle, SHELF_TIMER_LEAVE);
            if (!shelf->drag_inside || GetTickCount64() - shelf->last_drag_over < 200) return true;
            shelf_end_drag(shelf);
            drop_helper_leave(shelf->drop_helper);
            return true;
        case SHELF_TIMER_TOAST:
            shelf_hide_toast(shelf);
            return true;
        case SHELF_TIMER_BUSY:
            shelf_update_busy_overlay(shelf, true);
            return true;
        case SHELF_TIMER_MERGE:
            shelf_merge_on_dwell(shelf);
            return true;
        case SHELF_TIMER_CLOSE_EMPTY:
        {
            KillTimer(shelf->handle, SHELF_TIMER_CLOSE_EMPTY);
            ShelfModel *model = shelf->model;
            if (model->count == 0 && !model->busy && !shelf->dragging_out && !shelf->drag_inside && shelf->receiving == 0 && shelf_close_when_empty())
                shelf_window_close_animated(shelf);
            return true;
        }
    }
    return false;
}
