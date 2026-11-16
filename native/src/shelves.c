// 管理同时打开的多个置物架，并预先准备一个隐藏的窗口，让晃动后能立即弹出。和 C# 版的 ShelfManager 对应。
#include "shelf_window_internal.h"
#include "shelf_card.h"
#include "lang.h"
#include "shelf_history.h"
#include "log.h"
#include "task.h"

#include <math.h>
#include <shellscalingapi.h>
#include <stdlib.h>

static ShelfWindow *open_shelves;   // 打开着的置物架
static ShelfWindow *spare;          // 隐藏着待命的窗口
static bool shutting_down;
static bool listener_added;
static bool prewarm_scheduled;

static void on_language_changed(void *context)
{
    (void)context;
    // 项目的说明文字跟着语言走，下次画的时候重新生成；窗口本身由界面底座重画。
    shelf_items_language_changed();
    for (ShelfWindow *shelf = open_shelves; shelf; shelf = shelf->next)
    {
        shelf_update_title(shelf);
        // 快速操作的名称和提示是生成好的；正在拖入时换掉卡片的文字也没关系，排版不变。
        shelf_rebuild_actions(shelf);
    }
    if (spare) shelf_rebuild_actions(spare);
}

static void ensure_listener(void)
{
    if (listener_added) return;
    listener_added = true;
    language_add_listener(on_language_changed, NULL);
}

ShelfWindow *shelves_first(void)
{
    return open_shelves;
}

int shelves_open_count(void)
{
    int count = 0;
    for (ShelfWindow *shelf = open_shelves; shelf; shelf = shelf->next) count++;
    return count;
}

bool shelves_contain_point(POINT point)
{
    for (ShelfWindow *shelf = open_shelves; shelf; shelf = shelf->next)
    {
        RECT rect;
        if (shelf_window_is_visible(shelf) && GetWindowRect(shelf->handle, &rect) && PtInRect(&rect, point)) return true;
    }
    return false;
}

void shelves_on_closed(ShelfWindow *shelf)
{
    if (spare == shelf)
    {
        spare = NULL;
        shelf_window_release(shelf);
    }
    for (ShelfWindow **link = &open_shelves; *link; link = &(*link)->next)
    {
        if (*link != shelf) continue;
        *link = shelf->next;
        shelf->next = NULL;
        shelf_window_release(shelf);
        break;
    }
}

// ───────────── 备用窗口 ─────────────

static bool any_busy(void)
{
    for (ShelfWindow *shelf = open_shelves; shelf; shelf = shelf->next)
        if (shelf_window_is_animating(shelf) || (shelf->model->opened_by_gesture && shelf_window_is_reusable(shelf))) return true;
    return false;
}

/// 创建窗口会占用界面线程一点时间，在动画或拖动途中进行会造成卡顿，所以等到空闲再创建。
static void prewarm_tick(void *data)
{
    (void)data;
    prewarm_scheduled = false;
    if (shutting_down || spare) return;
    if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || any_busy())
    {
        shelves_prewarm();
        return;
    }
    spare = shelf_window_create();
    // 在屏幕外画一帧：字体、文字排版和绘制效果都在这时准备好，晃动弹出时显示窗口只剩少量工作。
    if (spare) ui_window_warm(spare->ui);
    // 替置物架出场的那张牌也先备好。
    shelf_card_prewarm();
}

void shelves_prewarm(void)
{
    ensure_listener();
    if (shutting_down || spare || prewarm_scheduled) return;
    prewarm_scheduled = true;
    task_delay(600, prewarm_tick, NULL);
}

// ───────────── 打开 ─────────────

ShelfWindow *shelves_open_at(POINT center, bool by_gesture, bool activate, ScreenEdge edge)
{
    if (shutting_down) return NULL;
    ensure_listener();
    // 晃动打开的空置物架会被复用，避免越开越多。
    ShelfWindow *shelf = NULL;
    for (ShelfWindow *candidate = open_shelves; candidate && !shelf; candidate = candidate->next)
        if (shelf_window_is_reusable(candidate) && shelf_window_is_visible(candidate)) shelf = candidate;
    if (!shelf)
    {
        shelf = spare ? spare : shelf_window_create();
        spare = NULL;
        if (!shelf) return NULL;
        shelf->next = open_shelves;
        open_shelves = shelf;
    }
    shelf->model->opened_by_gesture = by_gesture;
    ShelfEntranceKind entrance = !by_gesture ? ENTRANCE_PLAIN : edge == EDGE_NONE ? ENTRANCE_SHAKE : ENTRANCE_EDGE;
    shelf_window_show_at(shelf, center, activate, entrance, edge);
    shelves_prewarm();
    return shelf;
}

ShelfWindow *shelves_open_at_cursor(bool activate)
{
    POINT cursor = {0};
    GetCursorPos(&cursor);
    return shelves_open_at(cursor, false, activate, EDGE_NONE);
}

/// 紧挨着 anchor 窗口的位置：放在右边，右边放不下就放左边，两边都不行时叠在它右侧靠里一点。
static POINT next_to(HWND anchor)
{
    POINT center = {0};
    RECT rect;
    if (anchor && GetWindowRect(anchor, &rect))
    {
        // 放在右边，右边放不下就放左边，两边都不行时叠在它右侧靠里一点。
        POINT middle = { rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
        HMONITOR monitor = MonitorFromPoint(middle, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { .cbSize = sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        UINT dpi_x = 96, dpi_y = 96;
        double scale = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x > 0 ? dpi_x / 96.0 : 1.0;
        RECT work = info.rcWork;
        int width = (int)lround(shelf_width_for(app_settings()->quick_shelf_size) * scale);
        int gap = (int)lround(12 * scale);
        center.x = rect.right + gap + width <= work.right ? rect.right + gap + width / 2
                 : rect.left - gap - width >= work.left ? rect.left - gap - width / 2
                 : rect.right - gap - width / 2;
        center.y = middle.y;
    }
    else GetCursorPos(&center);
    return center;
}

ShelfWindow *shelves_open_next_to(HWND anchor, bool activate)
{
    if (shutting_down) return NULL;
    return shelves_open_at(next_to(anchor), false, activate, EDGE_NONE);
}

void shelves_open_beside(ShelfWindow *source, const StrList *paths)
{
    if (shutting_down || paths->count == 0) return;
    HWND anchor = source && !source->closed ? source->handle : NULL;
    ShelfWindow *shelf = shelves_open_at(next_to(anchor), false, false, EDGE_NONE);
    if (shelf) shelf_model_add_files(shelf->model, paths);
}

void shelves_restore(ShelfHistoryEntry *entry, ShelfWindow *target)
{
    if (shutting_down || !entry) return;
    ShelfWindow *shelf = target && !target->closing && !target->closed ? target : shelves_open_at_cursor(true);
    if (!shelf) return;
    // 先移出历史并标记为已使用：读取期间再次点击不会重复恢复，空置物架也不会被自动关闭或复用。
    shelf_history_entry_retain(entry);
    shelf_history_remove(entry);
    shelf->model->has_interacted = true;
    StrList paths = {0};
    for (int index = 0; index < entry->count; index++) strlist_add(&paths, entry->items[index].path);
    shelf_restore_paths(shelf, &paths);
    strlist_clear(&paths);
    shelf_history_entry_release(entry);
}

static void close_unused(void *data)
{
    (void)data;
    // 放下操作的数据交接可能晚于松开鼠标到达，稍等片刻再判断。
    ShelfWindow *closing[32];
    int count = 0;
    for (ShelfWindow *shelf = open_shelves; shelf && count < 32; shelf = shelf->next)
        if (shelf->model->opened_by_gesture && shelf_window_is_reusable(shelf) && !shelf->dragging_out) closing[count++] = shelf_window_retain(shelf);
    for (int index = 0; index < count; index++)
    {
        shelf_window_close_animated(closing[index]);
        shelf_window_release(closing[index]);
    }
}

void shelves_drag_released(void)
{
    task_delay(350, close_unused, NULL);
}

void shelves_apply_settings(void)
{
    for (ShelfWindow *shelf = open_shelves; shelf; shelf = shelf->next) shelf_window_apply_settings(shelf);
    if (spare) shelf_window_apply_settings(spare);
}

void shelves_close_all(void)
{
    shutting_down = true;
    while (open_shelves)
    {
        ShelfWindow *shelf = open_shelves;
        shelf_window_close_now(shelf);
        // 关闭时已经从列表里摘掉；窗口没能正常关闭时也不要卡在这里。
        if (open_shelves == shelf)
        {
            open_shelves = shelf->next;
            shelf_window_release(shelf);
        }
    }
    if (spare) shelf_window_close_now(spare);
    if (spare)
    {
        shelf_window_release(spare);
        spare = NULL;
    }
    split_pad_close_shared();
    // 关完了：之后（测试里，或者退出前又有人要打开）照常可以打开。
    shutting_down = false;
}
