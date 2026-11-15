// 管理同时打开的多个置物架，并预先准备一个隐藏的窗口，让晃动后能立即弹出。和 C# 版的 ShelfManager 对应。
#include "shelf_window_internal.h"
#include "lang.h"
#include "log.h"
#include "task.h"

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
    for (ShelfWindow *shelf = open_shelves; shelf; shelf = shelf->next) shelf_update_title(shelf);
}

static void ensure_listener(void)
{
    if (listener_added) return;
    listener_added = true;
    language_add_listener(on_language_changed, NULL);
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
}
