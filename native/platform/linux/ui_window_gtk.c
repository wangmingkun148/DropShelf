// ui_window.c 的 GTK4 + wlr-layer-shell 后端。
//
// 元素树、布局、动画和绘制都还是共享代码；这里只负责窗口本身：
// 普通窗口用 GtkWindow，置物架/菜单这类 popup 用 layer-shell（天然置顶、可以精确贴边），
// 鼠标、滚轮、键盘、焦点和逐帧动画都转成 ui.h 里的回调。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ui.h"
#include "ui_internal.h"
#include "ui_linux.h"
#include "gfx_gtk.h"
#include "gesture_linux.h"
#include "xdnd_linux.h"
#include "lang.h"
#include "log.h"

#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct UiWindow
{
    GtkWidget *window;
    GtkWidget *overlay;             // 包住自绘区域；编辑输入框时在上面盖一个 GtkEntry
    GtkWidget *area;
    GtkWidget *editor;              // 正在编辑的输入框对应的 GtkEntry，输入法要用真正的控件
    UiElement *editing;             // 这个 GtkEntry 对应的元素
    GfxSurface *surface;
    UiWindowOptions options;
    UiElement *root;
    UiElement *hovered;
    UiElement *captured;
    UiElement *focused;
    UiElement *maximize_button;
    int x, y;                       // 屏幕上的位置（逻辑像素）
    float width, height;            // 客户区（DIP）
    bool layer;
    bool keyboard_deferred;         // 开着的时候 X11 正在拖东西，键盘先没要，点一下再要
    bool visible;
    bool active;
    bool layout_dirty;
    int layout_width, layout_height;    // 上次排版用的大小：GTK 改变窗口大小时要重新排版
    bool frame_requested;
    bool subscribed;
    bool rendering;
    bool destroyed;
    bool cloaked;                       // 隐身：照常推进动画，只是什么都不画（见 ui_window_set_cloaked）
    bool hide_posted;                   // 绘制回调里要求隐藏，等这一帧画完再藏（见 ShowWindow）
    bool moving;
    bool move_by_cursor;                // 层壳窗口拖动时用全局光标定位（Hyprland IPC / 已知指针位置）
    bool move_button_seen;              // 这次拖动里 evdev 至少看到过一次左键按下
    int move_blind_ticks;               // evdev 还没看到按下时按下去的轮数，用来兜底
    guint move_source;                  // 拖动期间按固定间隔查光标；pointer 离开窗口后 GTK 不再送事件，靠它继续跟
    int move_offset_x, move_offset_y;   // 按下时全局光标相对窗口左上角的偏移
    // 上次设过的层壳位置：值没变就不再设，省掉每帧一次 configure 往返。
    bool layer_placed;
    GdkMonitor *layer_monitor;          // GdkDisplay 持有，这里只借来比较
    int layer_margin_x, layer_margin_y;
    bool caption_press;                 // 在自绘标题栏上按下了，还没拖动：等移动超过阈值再开始拖窗口
    float caption_press_x, caption_press_y;
    float move_last_x, move_last_y;
    float pointer_x, pointer_y;
    bool pointer_inside;
    wchar_t *tooltip_text;
    UiDropHandler drop_handler;
    void *drop_context;
    UiDragPrepare drag_prepare;
    UiDragEnd drag_end;
    void *drag_context;
    void *drag_data;
    GtkDragSource *drag_source;         // 拖出用的拖动源
    GdkDrag *drag;                      // 这次拖出的拖动；GTK 等不到结束通知时由我们替它收尾
    guint drag_guard;                   // 拖动期间定时确认左键还按着
    gint64 drag_began_at;
    gint64 drag_released_at;            // 第一次看到左键松开的时间，0 表示还没松开
    bool drag_active;
    bool drag_saw_button;               // 这次拖动期间至少看到过一次左键按着
    bool drag_stale;                    // 已经替 GTK 收过尾、GTK 那边还挂着：下次从这里拖出前取消掉
    guint stale_cancel_source;          // 过一会儿还没收到 drag-end，就把挂着的拖动取消掉
    bool drag_taken_over;               // 这次拖出已经交给 X11 拖动源，收尾由它来做
    bool drag_cancelled;                // GTK 报过这次拖动失败（松手处没有接收方、被拒）
    GtkWidget *drag_icon_label;         // 文字拖动图标里那个截短了的标签，拖动结束前要留着
    UiWindow *next;
};

static UiWindow *windows;
// 本程序正在进行的拖出；拖动结束时在 drag-end 里清掉，窗口关掉时也会清掉。
static GdkDrag *active_drag;
static bool css_installed;
static GtkCssProvider *editor_css;

// 输入框的 GtkEntry 覆盖层：焦点、位置和文字同步见文件后面的实现。
static void update_editor(UiWindow *window);
static void drag_guard_stop(UiWindow *window);
static void finish_stale_drag(UiWindow *window);
static void release_active_drag(UiWindow *window);
static void cancel_stale_drags(void);
static void schedule_stale_cancel(UiWindow *window);
static void stop_stale_cancel(UiWindow *window);
static void release_drag_icon_label(UiWindow *window);

// Win32 的鼠标捕获和按键状态。GTK 的鼠标手势本来就会抓住指针，这里只把状态记下来给共享代码看。
static HWND capture_window;
static bool left_button_down, right_button_down;
static bool control_down, shift_down, alt_down, super_down;

typedef struct UiTimer
{
    HWND handle;
    UINT_PTR id;
    guint source;
    bool active;
} UiTimer;

static UiTimer timers[64];

static gboolean timer_fired(gpointer data)
{
    UiTimer *timer = data;
    timer->source = 0;
    timer->active = false;
    UiWindow *window = (UiWindow *)timer->handle;
    if (!window || window->destroyed || !window->options.message) return G_SOURCE_REMOVE;
    LRESULT result = 0;
    window->options.message(window, window->options.context, WM_TIMER, (WPARAM)timer->id, 0, &result);
    return G_SOURCE_REMOVE;
}

UINT_PTR SetTimer(HWND handle, UINT_PTR id, UINT elapse, void *procedure)
{
    (void)procedure;
    UiTimer *timer = NULL;
    for (int index = 0; index < 64; index++)
    {
        if (timers[index].handle == handle && timers[index].id == id) timer = &timers[index];
        else if (!timer && !timers[index].handle) timer = &timers[index];
    }
    if (!timer) return 0;
    if (timer->active && timer->source) g_source_remove(timer->source);
    timer->handle = handle;
    timer->id = id;
    timer->active = true;
    timer->source = g_timeout_add(elapse ? elapse : 1, timer_fired, timer);
    return id;
}

BOOL KillTimer(HWND handle, UINT_PTR id)
{
    for (int index = 0; index < 64; index++)
    {
        if (timers[index].handle != handle || timers[index].id != id) continue;
        if (timers[index].active && timers[index].source) g_source_remove(timers[index].source);
        timers[index] = (UiTimer){0};
        return TRUE;
    }
    return FALSE;
}

static void drop_timers(HWND handle)
{
    for (int index = 0; index < 64; index++)
    {
        if (timers[index].handle != handle) continue;
        if (timers[index].active && timers[index].source) g_source_remove(timers[index].source);
        timers[index] = (UiTimer){0};
    }
}

// ───────────── 投递消息 ─────────────

typedef struct PostedMessage
{
    HWND handle;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
} PostedMessage;

static gboolean posted_message(gpointer data)
{
    PostedMessage *posted = data;
    UiWindow *window = NULL;
    for (UiWindow *each = windows; each; each = each->next)
        if ((HWND)each == posted->handle)
        {
            window = each;
            break;
        }
    // 窗口可能在消息到达前就关掉了；只在列表里找到才碰它。
    if (window && !window->destroyed && window->options.message)
    {
        LRESULT result = 0;
        window->options.message(window, window->options.context, posted->message, posted->wparam, posted->lparam, &result);
    }
    free(posted);
    return G_SOURCE_REMOVE;
}

/// 别的线程（比如手势识别）往界面线程投递消息，和 Windows 的 PostMessage 一样不等处理完就返回。
BOOL PostMessageW(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (!handle) return FALSE;
    PostedMessage *posted = xcalloc(1, sizeof(PostedMessage));
    posted->handle = handle;
    posted->message = message;
    posted->wparam = wparam;
    posted->lparam = lparam;
    g_idle_add_full(G_PRIORITY_DEFAULT, posted_message, posted, NULL);
    return TRUE;
}

static UiWindow *window_of(HWND handle)
{
    UiWindow *window = (UiWindow *)handle;
    return window && !window->destroyed ? window : NULL;
}

short GetKeyState(int key)
{
    if (key == VK_LBUTTON) return left_button_down ? (short)-32768 : 0;
    if (key == VK_RBUTTON) return right_button_down ? (short)-32768 : 0;
    if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL) return control_down ? (short)-32768 : 0;
    if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) return shift_down ? (short)-32768 : 0;
    if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU) return alt_down ? (short)-32768 : 0;
    if (key == VK_LWIN || key == VK_RWIN) return super_down ? (short)-32768 : 0;
    return 0;
}

short GetAsyncKeyState(int key)
{
    return GetKeyState(key);
}

HWND SetCapture(HWND handle)
{
    HWND previous = capture_window;
    capture_window = handle;
    return previous;
}

HWND GetCapture(void)
{
    return capture_window;
}

BOOL ReleaseCapture(void)
{
    if (capture_window)
    {
        UiWindow *window = window_of(capture_window);
        capture_window = NULL;
        if (window) ui_window_cancel_press(window);
    }
    return TRUE;
}

BOOL ScreenToClient(HWND handle, POINT *point)
{
    UiWindow *window = window_of(handle);
    if (!window || !point) return FALSE;
    point->x -= window->x;
    point->y -= window->y;
    return TRUE;
}

BOOL ClientToScreen(HWND handle, POINT *point)
{
    UiWindow *window = window_of(handle);
    if (!window || !point) return FALSE;
    point->x += window->x;
    point->y += window->y;
    return TRUE;
}

/// 客户区的大小（物理像素）：我们的窗口没有标题栏，客户区就是整个窗口。
BOOL GetClientRect(HWND handle, RECT *rect)
{
    UiWindow *window = window_of(handle);
    if (!window || !rect) return FALSE;
    float scale = ui_window_scale(window);
    rect->left = 0;
    rect->top = 0;
    rect->right = (LONG)lroundf(window->width * scale);
    rect->bottom = (LONG)lroundf(window->height * scale);
    return TRUE;
}

UINT GetDpiForWindow(HWND handle)
{
    UiWindow *window = window_of(handle);
    float scale = window ? ui_window_scale(window) : 1;
    if (scale <= 0) scale = 1;
    return (UINT)lroundf(scale * 96.0f);
}

BOOL SetWindowTextW(HWND handle, LPCWSTR text)
{
    (void)handle;
    (void)text;
    return TRUE;
}

BOOL SetForegroundWindow(HWND handle)
{
    UiWindow *window = window_of(handle);
    if (!window) return FALSE;
    gtk_window_present(GTK_WINDOW(window->window));
    return TRUE;
}

static char *utf8(const wchar_t *text)
{
    return text ? utf8_from_wide(text, NULL) : NULL;
}

static void install_css(void)
{
    if (css_installed) return;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, ".dropshelf-transparent { background-color: transparent; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    css_installed = true;
}

// ───────────── 帧 ─────────────

static void on_frame(void *context, double time)
{
    UiWindow *window = context;
    if (!window || window->destroyed || !window->visible) return;
    if (low_performance_skip(time)) return;
    gtk_widget_queue_draw(window->area);
}

static void ensure_frames(UiWindow *window)
{
    if (window->subscribed) return;
    frames_subscribe(on_frame, window);
    window->subscribed = true;
}

static void stop_frames(UiWindow *window)
{
    if (!window->subscribed) return;
    frames_unsubscribe(on_frame, window);
    window->subscribed = false;
}

void ui_window_request_frame(UiWindow *window)
{
    if (!window || window->destroyed) return;
    window->frame_requested = true;
    ensure_frames(window);
    gtk_widget_queue_draw(window->area);
}

bool ui_window_is_animating(UiWindow *window)
{
    return window && !window->destroyed && (window->subscribed || window->frame_requested);
}

void ui_window_repaint(UiWindow *window)
{
    if (!window || window->destroyed || !window->visible) return;
    gtk_widget_queue_draw(window->area);
}

void ui_window_invalidate(UiWindow *window)
{
    if (!window || window->destroyed) return;
    window->layout_dirty = true;
    gtk_widget_queue_draw(window->area);
}

// ───────────── 绘制 ─────────────

static void layout_now(UiWindow *window, int width, int height)
{
    if (!window->root || !window->layout_dirty) return;
    ui_measure(window->root, width);
    ui_arrange(window->root, (GfxRect){ 0, 0, width, height });
    window->layout_width = width;
    window->layout_height = height;
    window->layout_dirty = false;
}

static void draw_frame(UiWindow *window, cairo_t *cairo, int width, int height)
{
    if (!window || window->destroyed || window->rendering) return;
    window->rendering = true;
    window->width = width;
    window->height = height;
    window->frame_requested = false;

    if (window->options.animate)
        window->options.animate(window, window->options.context, frames_time());
    // animate 里可能已经关掉了窗口：销毁回调把窗口从列表里摘掉，结构体由空闲回调释放，这里读 destroyed 仍然安全。
    if (window->destroyed)
    {
        window->rendering = false;
        return;
    }

    // animate 可能改了窗口大小；用 GTK 现在给的大小。
    width = gtk_widget_get_width(window->area);
    height = gtk_widget_get_height(window->area);
    if (width <= 0 || height <= 0)
    {
        window->rendering = false;
        return;
    }
    window->width = width;
    window->height = height;
    // 显示后窗口大小才最后定下来（还有用户拖动边缘、最大化）：大小变了就重新排版。
    if (width != window->layout_width || height != window->layout_height) window->layout_dirty = true;
    layout_now(window, width, height);
    if (g_getenv("DROPSHELF_DEBUG_SIZE"))
        fprintf(stderr, "size: area=%dx%d root=%gx%g scale=%g\n", width, height,
            window->root ? window->root->bounds.right : 0, window->root ? window->root->bounds.bottom : 0,
            (double)ui_window_scale(window));
    // 排版后输入框可能挪了位置；编辑中的 GtkEntry 跟着走。
    if (window->editing) update_editor(window);

    gfx_surface_set_cairo(window->surface, cairo);
    if (gfx_begin(window->surface, width, height, 96.0f))
    {
        bool transparent = window->options.popup || window->cloaked;
        gfx_clear(transparent ? COLOR_TRANSPARENT : theme_color(window->options.background));
        if (window->options.paint && !window->cloaked) window->options.paint(window, window->options.context, frames_time());
        if (window->root && !window->cloaked) ui_paint(window->root, frames_time());
        gfx_end(window->surface);
    }
    window->rendering = false;

    if (window->frame_requested) ensure_frames(window);
    else stop_frames(window);
}

static void on_draw(GtkDrawingArea *area, cairo_t *cairo, int width, int height, gpointer data)
{
    (void)area;
    draw_frame(data, cairo, width, height);
}

// ───────────── 鼠标 ─────────────

static void send_mouse(UiElement *element, UiMouseKind kind, const UiMouseEvent *source)
{
    if (!element || !element->type->mouse) return;
    UiMouseEvent event = *source;
    event.kind = kind;
    if (event.x >= 0 || event.y >= 0) ui_element_to_local(element, &event.x, &event.y);
    element->type->mouse(element, &event);
}

static void set_hovered(UiWindow *window, UiElement *element, const UiMouseEvent *event)
{
    if (window->hovered == element) return;
    UiElement *previous = window->hovered;
    const wchar_t *previous_tip = previous ? ui_element_tooltip(previous) : NULL;
    const wchar_t *next_tip = element ? ui_element_tooltip(element) : NULL;
    if (previous_tip || next_tip) ui_window_set_tooltip(window, next_tip);
    window->hovered = element;
    if (previous)
    {
        previous->hovered = false;
        send_mouse(previous, UI_MOUSE_LEAVE, event);
    }
    if (window->hovered == element && element)
    {
        element->hovered = true;
        send_mouse(element, UI_MOUSE_ENTER, event);
    }
    ui_window_repaint(window);
}

static UiElement *element_at(UiWindow *window, float x, float y)
{
    return window->root ? ui_hit_test(window->root, x, y) : NULL;
}

static void set_cursor_for(UiWindow *window, float x, float y)
{
    const wchar_t *cursor = NULL;
    if (window->options.cursor) cursor = window->options.cursor(window, window->options.context, x, y);
    if (!cursor) cursor = window->hovered && !window->hovered->disabled
        ? (window->hovered->type->cursor ? window->hovered->type->cursor : IDC_HAND)
        : IDC_ARROW;
    const char *name = "default";
    if (cursor && wcscmp(cursor, IDC_HAND) == 0) name = "pointer";
    else if (cursor && wcscmp(cursor, IDC_IBEAM) == 0) name = "text";
    gtk_widget_set_cursor_from_name(window->area, name);
}

// 拖动窗口：层壳窗口按全局光标定位，普通窗口按本地增量。
//
// 层壳窗口的位置由合成器按 margin 摆放，窗口跟着光标动之后，鼠标事件的本地坐标里就
// 包含窗口自己移动的那一段：只按本地增量算，每隔一次事件才会挪一下，拖起来发飘。
// 所以按下时记下全局光标和窗口左上角的差，之后每次事件都用全局光标反推窗口位置。

/// 把“指针离开了窗口”送给界面。
static void send_leave_event(UiWindow *window)
{
    UiMouseEvent event = { .kind = UI_MOUSE_LEAVE, .x = -1, .y = -1 };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    if (!window->captured) set_hovered(window, NULL, &event);
}

/// 结束拖动：停掉跟光标的定时器，清掉拖动状态。
static void end_move(UiWindow *window, const wchar_t *reason)
{
    if (window->move_source)
    {
        g_source_remove(window->move_source);
        window->move_source = 0;
    }
    window->caption_press = false;
    if (!window->moving) return;
    log_info(L"窗口拖动结束：%ls，停在 (%d,%d)", reason, window->x, window->y);
    window->moving = false;
    window->move_by_cursor = false;
    window->move_button_seen = false;
    window->move_blind_ticks = 0;
    // 拖动期间指针跑到窗口外面去了，收尾时补一次“离开”，界面上的悬停状态才不会停住。
    if (!window->pointer_inside && !window->destroyed) send_leave_event(window);
}

/// 拖动期间固定间隔跟一次光标。GTK 的事件只在指针位于窗口内时才送来，窗口跟得慢一点
/// 指针就跑到窗口外面，之后既收不到移动也收不到松手；这里直接问全局光标，
/// 松手用 evdev 的全局左键状态判断，窗口就能一直贴着指针走。
#define MOVE_FOLLOW_MILLISECONDS 8
/// evdev 最多等这么久才把“一直没看到按下”当成松手（设备刚插上时要过一会儿才开始监听）。
#define MOVE_BLIND_MILLISECONDS 1500

/// 这次拖动该不该按“左键松开了”收尾。刚按下时 evdev 可能还没轮到新设备（先返回 0），
/// 这时不能立刻当成松手，否则拖一下就断。
static bool move_should_release(UiWindow *window)
{
    int buttons = gesture_left_button_state();
    if (buttons > 0)
    {
        window->move_button_seen = true;
        window->move_blind_ticks = 0;
        return false;
    }
    if (buttons < 0) return false;
    if (window->move_button_seen) return true;
    return ++window->move_blind_ticks * MOVE_FOLLOW_MILLISECONDS >= MOVE_BLIND_MILLISECONDS;
}

static gboolean move_follow_tick(gpointer data)
{
    UiWindow *window = data;
    if (window->destroyed || !window->moving || !window->move_by_cursor)
    {
        window->move_source = 0;
        return G_SOURCE_REMOVE;
    }
    if (move_should_release(window))
    {
        window->move_source = 0;
        end_move(window, L"全局左键已经松开");
        return G_SOURCE_REMOVE;
    }
    if (gesture_left_button_state() < 0)
    {
        // 没有 evdev 权限时看不到全局按键，只能退回 GTK 的 release/leave。
        window->move_source = 0;
        return G_SOURCE_REMOVE;
    }
    POINT cursor;
    if (GetCursorPos(&cursor))
        ui_window_place(window, cursor.x - window->move_offset_x, cursor.y - window->move_offset_y, 0, 0);
    return G_SOURCE_CONTINUE;
}

static void move_window_to_pointer(UiWindow *window, float local_x, float local_y)
{
    int old_x = window->x;
    int old_y = window->y;
    POINT cursor;
    gint64 query_started = g_get_monotonic_time();
    bool have_cursor = window->layer && window->move_by_cursor && GetCursorPos(&cursor);
    if (log_trace_enabled() && g_get_monotonic_time() - query_started > 2000)
        log_trace(L"窗口拖动：向 Hyprland 询问光标用了 %.1f 毫秒", (g_get_monotonic_time() - query_started) / 1000.0);
    if (have_cursor)
        ui_window_place(window, cursor.x - window->move_offset_x, cursor.y - window->move_offset_y, 0, 0);
    else
    {
        float delta_x = local_x - window->move_last_x;
        float delta_y = local_y - window->move_last_y;
        ui_window_place(window, old_x + (int)lroundf(delta_x), old_y + (int)lroundf(delta_y), 0, 0);
    }
    // 本地坐标扣掉窗口这次移动的量，下一帧的增量才是光标真正走的路程。
    window->move_last_x = local_x - (float)(window->x - old_x);
    window->move_last_y = local_y - (float)(window->y - old_y);
}

static void on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
    (void)controller;
    UiWindow *window = data;
    if (window->destroyed) return;
    window->pointer_x = (float)x;
    window->pointer_y = (float)y;
    window->pointer_inside = true;
    ui_pointer_seen_global(window->x + (int)lroundf((float)x), window->y + (int)lroundf((float)y));
    if (window->caption_press && left_button_down)
    {
        // 标题栏上按着拖动：超过几个像素才算拖动，这样双击不会被第一次按下吃掉。
        float delta_x = (float)x - window->caption_press_x;
        float delta_y = (float)y - window->caption_press_y;
        if (delta_x * delta_x + delta_y * delta_y > 9.0f)
        {
            window->caption_press = false;
            ui_window_begin_move(window);
            return;
        }
    }
    if (window->moving)
    {
        // GTK 偶尔会在窗口跟手移动的途中漏掉 release（指针刚好离开 surface 的时候），
        // 只信点击手势的话窗口会一直粘着鼠标。evdev 能看到全局左键，松开了就收尾。
        if (move_should_release(window))
        {
            end_move(window, L"全局左键已经松开");
            return;
        }
        // 有定时器在按全局光标摆窗口时，这里不要再摆一次：两个来源的时机不同，
        // 一帧里挪两下反而会看出抖动。
        if (!(window->move_by_cursor && window->move_source)) move_window_to_pointer(window, (float)x, (float)y);
        return;
    }
    UiMouseEvent event = { .kind = UI_MOUSE_MOVE, .x = (float)x, .y = (float)y };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    if (window->captured) under = under == window->captured ? under : NULL;
    set_hovered(window, under, &event);
    send_mouse(window->captured ? window->captured : under, UI_MOUSE_MOVE, &event);
    set_cursor_for(window, event.x, event.y);
}

static void on_enter(GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
    (void)controller;
    (void)x;
    (void)y;
    UiWindow *window = data;
    if (window->destroyed) return;
    window->pointer_inside = true;
}

static void on_leave(GtkEventControllerMotion *controller, gpointer data)
{
    (void)controller;
    UiWindow *window = data;
    if (window->destroyed) return;
    window->pointer_inside = false;
    if (window->moving && window->move_by_cursor)
    {
        // 窗口正跟着全局光标走，指针暂时跑到窗口外面是正常的，定时器会把它追回来。
        // 这时不结束拖动，也不把“离开”送给界面，免得置物架自己收起来。
        log_trace(L"窗口拖动：指针暂时离开窗口，继续跟着光标");
        return;
    }
    end_move(window, L"指针离开了窗口");
    send_leave_event(window);
}

static void on_pressed(GtkGestureClick *gesture, int press_count, double x, double y, gpointer data)
{
    UiWindow *window = data;
    if (window->destroyed) return;
    window->pointer_x = (float)x;
    window->pointer_y = (float)y;
    window->pointer_inside = true;
    ui_pointer_seen_global(window->x + (int)lroundf((float)x), window->y + (int)lroundf((float)y));
    // 新的一次按下：先把上一轮已经收过尾、GTK 那边还挂着的拖动取消掉。
    // 要在这里取消，不能等 drag-prepare——那时候 GTK 已经在准备这次新拖动了，容易把新的取消掉。
    cancel_stale_drags();
    // 新的一次按下：上一次拖动不管因为什么没收到 release，都在这里结束掉。
    end_move(window, L"新的按下");
    // 点一下就把键盘焦点收到自绘区域：层壳窗口靠“点击后按需获得键盘”才收得到快捷键。
    // 开着的时候 X11 正在拖东西、当时没要键盘的，这时候再要。
    if (window->keyboard_deferred)
    {
        window->keyboard_deferred = false;
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window->window), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        gtk_window_present(GTK_WINDOW(window->window));
    }
    gtk_widget_grab_focus(window->area);
    bool right = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == 3;
    if (right) right_button_down = true;
    else left_button_down = true;
    capture_window = (HWND)window;
    UiMouseEvent event = { .kind = UI_MOUSE_DOWN, .x = (float)x, .y = (float)y, .right_button = right };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    // 自绘标题栏的空白处：按住拖动窗口，双击切换最大化（Windows 上由系统的非客户区完成）。
    if (!right && !under && !window->layer && window->options.caption_height > 0
        && event.y < (float)window->options.caption_height)
    {
        if (press_count >= 2)
        {
            window->caption_press = false;
            GtkWindow *gtk = GTK_WINDOW(window->window);
            if (gtk_window_is_maximized(gtk)) gtk_window_unmaximize(gtk);
            else if (window->options.resizable) gtk_window_maximize(gtk);
            return;
        }
        window->caption_press = true;
        window->caption_press_x = (float)x;
        window->caption_press_y = (float)y;
        return;
    }
    set_hovered(window, under, &event);
    UiElement *focusable = under;
    while (focusable && !focusable->type->focusable) focusable = focusable->parent;
    if (!right) ui_window_set_focus(window, focusable && !focusable->disabled ? focusable : NULL);
    if (!under || under->disabled || window->hovered != under) return;
    if (!right)
    {
        window->captured = under;
        under->pressed = true;
    }
    send_mouse(under, UI_MOUSE_DOWN, &event);
    ui_window_repaint(window);
}

static void on_released(GtkGestureClick *gesture, int press_count, double x, double y, gpointer data)
{
    (void)press_count;
    UiWindow *window = data;
    if (window->destroyed) return;
    bool right = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == 3;
    if (right) right_button_down = false;
    else left_button_down = false;
    if (capture_window == (HWND)window) capture_window = NULL;
    UiMouseEvent event = { .kind = UI_MOUSE_UP, .x = (float)x, .y = (float)y, .right_button = right };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    if (right)
    {
        event.inside = true;
        if (under && !under->disabled) send_mouse(under, UI_MOUSE_UP, &event);
        return;
    }
    UiElement *captured = window->captured;
    window->captured = NULL;
    end_move(window, L"松开左键");
    if (!captured) return;
    captured->pressed = false;
    event.inside = under == captured;
    send_mouse(captured, UI_MOUSE_UP, &event);
    if (window->destroyed) return;
    set_hovered(window, element_at(window, event.x, event.y), &event);
    ui_window_repaint(window);
}

static gboolean on_scroll(GtkEventControllerScroll *controller, double delta_x, double delta_y, gpointer data)
{
    (void)controller;
    (void)delta_x;
    UiWindow *window = data;
    if (window->destroyed) return FALSE;
    UiMouseEvent event = { .kind = UI_MOUSE_WHEEL, .x = window->pointer_x, .y = window->pointer_y, .wheel = (float)-delta_y };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return TRUE;
    UiElement *under = window->root ? ui_hit_test(window->root, event.x, event.y) : NULL;
    if (!under && window->root) under = ui_hit_test_any(window->root, event.x, event.y);
    for (UiElement *element = under; element; element = element->parent)
    {
        if (!element->type->mouse || !element->type->wheel) continue;
        send_mouse(element, UI_MOUSE_WHEEL, &event);
        return TRUE;
    }
    return TRUE;
}

// ───────────── 键盘 ─────────────

static unsigned key_to_vk(guint keyval)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) return (unsigned)(L'A' + (keyval - GDK_KEY_a));
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) return (unsigned)keyval;
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) return (unsigned)keyval;
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24) return VK_F1 + (unsigned)(keyval - GDK_KEY_F1);
    if (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9) return VK_NUMPAD0 + (unsigned)(keyval - GDK_KEY_KP_0);
    switch (keyval)
    {
        case GDK_KEY_Escape: return VK_ESCAPE;
        case GDK_KEY_Return: return VK_RETURN;
        case GDK_KEY_KP_Enter: return VK_RETURN;
        case GDK_KEY_Tab: return VK_TAB;
        case GDK_KEY_BackSpace: return VK_BACK;
        case GDK_KEY_Delete: return VK_DELETE;
        case GDK_KEY_Insert: return VK_INSERT;
        case GDK_KEY_Home: return VK_HOME;
        case GDK_KEY_End: return VK_END;
        case GDK_KEY_Page_Up: return VK_PRIOR;
        case GDK_KEY_Page_Down: return VK_NEXT;
        case GDK_KEY_Left: return VK_LEFT;
        case GDK_KEY_Right: return VK_RIGHT;
        case GDK_KEY_Up: return VK_UP;
        case GDK_KEY_Down: return VK_DOWN;
        case GDK_KEY_space: return VK_SPACE;
        case GDK_KEY_Shift_L: return VK_LSHIFT;
        case GDK_KEY_Shift_R: return VK_RSHIFT;
        case GDK_KEY_Control_L: return VK_LCONTROL;
        case GDK_KEY_Control_R: return VK_RCONTROL;
        case GDK_KEY_Alt_L: return VK_LMENU;
        case GDK_KEY_Alt_R: return VK_RMENU;
        case GDK_KEY_Super_L: return VK_LWIN;
        case GDK_KEY_Super_R: return VK_RWIN;
        case GDK_KEY_Caps_Lock: return VK_CAPITAL;
        case GDK_KEY_Num_Lock: return VK_NUMLOCK;
        case GDK_KEY_Scroll_Lock: return VK_SCROLL;
        case GDK_KEY_Pause: return VK_PAUSE;
        case GDK_KEY_Print: return VK_SNAPSHOT;
        case GDK_KEY_grave: return VK_OEM_3;
        case GDK_KEY_minus: return VK_OEM_MINUS;
        case GDK_KEY_equal: return VK_OEM_PLUS;
        case GDK_KEY_bracketleft: return VK_OEM_4;
        case GDK_KEY_bracketright: return VK_OEM_6;
        case GDK_KEY_backslash: return VK_OEM_5;
        case GDK_KEY_semicolon: return VK_OEM_1;
        case GDK_KEY_apostrophe: return VK_OEM_7;
        case GDK_KEY_comma: return VK_OEM_COMMA;
        case GDK_KEY_period: return VK_OEM_PERIOD;
        case GDK_KEY_slash: return VK_OEM_2;
        default: return 0;
    }
}

static void update_modifiers(GdkModifierType state)
{
    control_down = (state & GDK_CONTROL_MASK) != 0;
    shift_down = (state & GDK_SHIFT_MASK) != 0;
    alt_down = (state & GDK_ALT_MASK) != 0;
    super_down = (state & GDK_SUPER_MASK) != 0;
}

static void release_modifier(guint keyval)
{
    switch (keyval)
    {
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R: control_down = false; break;
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R: shift_down = false; break;
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R: alt_down = false; break;
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R: super_down = false; break;
        default: break;
    }
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
    (void)controller;
    (void)keycode;
    UiWindow *window = data;
    if (window->destroyed) return FALSE;
    update_modifiers(state);
    switch (keyval)
    {
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R: control_down = true; break;
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R: shift_down = true; break;
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R: alt_down = true; break;
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R: super_down = true; break;
        default: break;
    }
    unsigned key = key_to_vk(keyval);
    if (!key) return FALSE;
    if (state & GDK_ALT_MASK) key |= UI_KEY_ALT;
    log_trace(L"窗口按键：键码 %u（%hs），修饰键 0x%x", key, gdk_keyval_name(keyval) ? gdk_keyval_name(keyval) : "?", (unsigned)state);

    UiElement *focused = window->focused;
    if (focused && !focused->hidden && focused->type->key && focused->type->key(focused, key)) return TRUE;
    if (window->options.key && window->options.key(window, window->options.context, key)) return TRUE;

    if (!(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)))
    {
        gunichar character = gdk_keyval_to_unicode(keyval);
        if (character && focused && focused->type->character)
        {
            focused->type->character(focused, (wchar_t)character);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
    (void)controller;
    (void)keycode;
    (void)data;
    update_modifiers(state);
    release_modifier(keyval);
}

static void on_focus_enter(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller;
    UiWindow *window = data;
    if (window->destroyed) return;
    window->active = true;
    ui_window_repaint(window);
    if (window->options.activated) window->options.activated(window, window->options.context, true);
}

static void on_focus_leave(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller;
    UiWindow *window = data;
    if (window->destroyed) return;
    window->active = false;
    // 失焦后收不到松开修饰键的事件，留着按住的状态会让下一次点击被当成 Ctrl+点击。
    control_down = shift_down = alt_down = super_down = false;
    ui_window_repaint(window);
    if (window->options.activated) window->options.activated(window, window->options.context, false);
}

// ───────────── 生命周期 ─────────────

static gboolean free_window(gpointer data)
{
    free(data);
    return G_SOURCE_REMOVE;
}

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    UiWindow *window = data;
    if (window->destroyed) return;
    window->destroyed = true;
    // 窗口在拖动中被关掉：X11 拖动源按取消收尾，置物架那边才能把项目放回去。
    if (window->drag_taken_over) xdnd_source_abort();
    // 拖动还挂着（GTK 没收到结束通知，或者窗口在拖动中被关掉）时，把挂在这个源上的回调断开：
    // 窗口马上要释放，迟到的结束通知不能再回调进来。拖动源这时还被 GdkDrag 撑着（GTK 为拖动多引用了一份），
    // 断开是安全的；拖动和数据源本身不碰，接收方还能把数据取完。
    if ((window->drag_active || window->drag_stale) && window->drag_source)
        g_signal_handlers_disconnect_by_data(window->drag_source, window);
    if (window->drag_active) active_drag = NULL;
    window->drag_active = false;
    window->drag_stale = false;
    window->drag = NULL;
    drag_guard_stop(window);
    stop_stale_cancel(window);
    xdnd_source_assist_finish();
    release_drag_icon_label(window);
    window->drag_source = NULL;
    stop_frames(window);
    if (window->move_source)
    {
        g_source_remove(window->move_source);
        window->move_source = 0;
    }
    drop_timers((HWND)window);
    if (capture_window == (HWND)window) capture_window = NULL;
    for (UiWindow **link = &windows; *link; link = &(*link)->next)
    {
        if (*link != window) continue;
        *link = window->next;
        break;
    }
    UiElement *root = window->root;
    window->root = NULL;
    window->hovered = window->captured = window->focused = window->maximize_button = NULL;
    if (root) ui_element_destroy(root);
    gfx_surface_destroy(window->surface);
    window->surface = NULL;
    free(window->tooltip_text);
    window->tooltip_text = NULL;
    if (window->options.closed) window->options.closed(window, window->options.context);
    // 不在 destroy 信号里直接 free：信号可能是从 animate/paint 里同步触发的，调用栈还没退回去。
    // 挂到空闲回调里释放，之后再有任何回调进来都能先看到 destroyed 标志。
    g_idle_add(free_window, window);
}

// GTK 4.24 去掉了 gdk_display_get_monitor() 和 get_monitor_at_point()，只剩 GListModel。
// 返回的 monitor 带一个新的引用。
static GdkMonitor *monitor_at_point(int x, int y)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return NULL;
    GListModel *monitors = gdk_display_get_monitors(display);
    GdkMonitor *fallback = NULL;
    guint count = g_list_model_get_n_items(monitors);
    for (guint index = 0; index < count; index++)
    {
        GdkMonitor *monitor = g_list_model_get_item(monitors, index);
        if (!monitor) continue;
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);
        if (x >= geometry.x && x < geometry.x + geometry.width && y >= geometry.y && y < geometry.y + geometry.height)
            return monitor;
        if (!fallback) fallback = monitor;
        else g_object_unref(monitor);
    }
    return fallback;
}

static GdkMonitor *primary_monitor(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return NULL;
    GListModel *monitors = gdk_display_get_monitors(display);
    if (g_list_model_get_n_items(monitors) == 0) return NULL;
    return g_list_model_get_item(monitors, 0);
}

static void update_layer_position(UiWindow *window)
{
    if (!window->layer || !gtk_layer_is_supported()) return;
    GtkWindow *gtk = GTK_WINDOW(window->window);
    GdkMonitor *monitor = monitor_at_point(window->x, window->y);
    if (!monitor) return;
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    int margin_x = window->x - geometry.x;
    int margin_y = window->y - geometry.y;
    if (margin_x < 0) margin_x = 0;
    if (margin_y < 0) margin_y = 0;
    // 记回合成器实际会给的位置，拖动时按增量算的下一次位置才和光标对得上。
    window->x = geometry.x + margin_x;
    window->y = geometry.y + margin_y;
    if (window->layer_placed && window->layer_monitor == monitor
        && window->layer_margin_x == margin_x && window->layer_margin_y == margin_y)
    {
        g_object_unref(monitor);
        return;
    }
    if (window->layer_placed && window->layer_monitor != monitor)
        log_info(L"窗口跨屏：换到显示器 %hs（%d,%d %dx%d），窗口在 (%d,%d)，margin %d,%d",
            gdk_monitor_get_model(monitor) ? gdk_monitor_get_model(monitor) : "?",
            geometry.x, geometry.y, geometry.width, geometry.height, window->x, window->y, margin_x, margin_y);
    window->layer_placed = true;
    window->layer_monitor = monitor;
    window->layer_margin_x = margin_x;
    window->layer_margin_y = margin_y;
    gtk_layer_set_monitor(gtk, monitor);
    gtk_layer_set_margin(gtk, GTK_LAYER_SHELL_EDGE_LEFT, margin_x);
    gtk_layer_set_margin(gtk, GTK_LAYER_SHELL_EDGE_TOP, margin_y);
    g_object_unref(monitor);
}

// ───────────── 文件拖入 ─────────────

static GdkDragAction drop_enter_motion(UiWindow *window, GdkDrop *drop, double x, double y, bool enter)
{
    if (!window->drop_handler) return 0;
    GdkDragAction action = gdk_drop_get_actions(drop);
    if (!(action & (GDK_ACTION_COPY | GDK_ACTION_MOVE))) action = GDK_ACTION_COPY | GDK_ACTION_MOVE;
    if (!window->drop_handler(window, window->drop_context, enter ? UI_DROP_ENTER : UI_DROP_MOTION, drop, NULL, x, y, &action)) return 0;
    return action;
}

static GdkDragAction on_drop_enter(GtkDropTarget *target, double x, double y, gpointer data)
{
    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    GdkContentFormats *formats = drop ? gdk_drop_get_formats(drop) : NULL;
    char *text = formats ? gdk_content_formats_to_string(formats) : NULL;
    log_info(L"GTK 拖放：进入窗口（%g, %g），格式 %hs，允许动作 0x%x", x, y, text ? text : "",
        (unsigned)(drop ? gdk_drop_get_actions(drop) : 0));
    g_free(text);
    GdkDragAction action = drop_enter_motion(data, drop, x, y, true);
    log_info(L"GTK 拖放：进入结果 0x%x（0 表示不接收）", (unsigned)action);
    return action;
}

static GdkDragAction on_drop_motion(GtkDropTarget *target, double x, double y, gpointer data)
{
    return drop_enter_motion(data, gtk_drop_target_get_current_drop(target), x, y, false);
}

static void on_drop_leave(GtkDropTarget *target, gpointer data)
{
    UiWindow *window = data;
    log_info(L"GTK 拖放：离开窗口");
    if (!window->drop_handler || window->destroyed) return;
    GdkDragAction action = 0;
    window->drop_handler(window, window->drop_context, UI_DROP_LEAVE, gtk_drop_target_get_current_drop(target), NULL, 0, 0, &action);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
    UiWindow *window = data;
    GType type = value ? G_VALUE_TYPE(value) : G_TYPE_INVALID;
    log_info(L"GTK 拖放：在（%g, %g）松开，数据是 %hs", x, y,
        type != G_TYPE_INVALID && g_type_name(type) ? g_type_name(type) : "无");
    if (!window->drop_handler || window->destroyed) return FALSE;
    GdkDragAction action = gdk_drop_get_actions(gtk_drop_target_get_current_drop(target));
    bool accepted = window->drop_handler(window, window->drop_context, UI_DROP_DROP, gtk_drop_target_get_current_drop(target), value, x, y, &action);
    log_info(L"GTK 拖放：松开结果 %ls", accepted ? L"已接收" : L"未接收");
    return accepted;
}

bool ui_drag_is_local(GdkDrag *drag)
{
    if (!drag) return false;
    GdkSurface *surface = gdk_drag_get_surface(drag);
    if (!surface) return false;
    for (UiWindow *window = windows; window; window = window->next)
    {
        if (window->destroyed || !window->window) continue;
        if (gtk_native_get_surface(GTK_NATIVE(window->window)) == surface) return true;
    }
    return false;
}

GdkDrag *ui_active_drag(void)
{
    return active_drag;
}

/// 合成器不一定会把拖动的结束事件送回来（Hyprland 上拖到没有窗口的地方就是这样），
/// GTK 的拖动会一直挂着：之后从别的程序拖进来的东西都会被当成自己的拖动，怎么都放不进置物架。
/// 这里盯着全局左键：已经松开而 GTK 还没结束，就主动取消这次拖动。
#define DRAG_GUARD_POLL_MILLISECONDS 50
#define DRAG_GUARD_GRACE_MICROSECONDS 100000
// 松开以后还等一会儿：给合成器和接收方正常结束的机会，迟迟等不到才当作漏发。
#define DRAG_GUARD_RELEASE_DELAY_MICROSECONDS 1000000

/// 从置物架往 XWayland 程序（WPS 这类）拖的时候，Hyprland 转过去的那次拖动对面拿不到数据：
/// 拖动期间它不让 X11 程序读 selection，对面只能退回粘贴剪贴板，或者干脆没反应。
/// 光标停在 XWayland 窗口上够久，就把这次拖出换成自己的 Xdnd，数据直接由我们给。
static bool take_over_for_x11(void)
{
    if (xdnd_source_active()) return false;
    UiWindow *owner = NULL;
    for (UiWindow *window = windows; window; window = window->next)
    {
        if (window->destroyed || !window->drag_active || !window->drag_data) continue;
        owner = window;
        break;
    }
    if (!owner) return false;
    // 光标还在置物架上时不算。置物架是层壳，不在合成器的窗口列表里，底下那个 X11 窗口会被
    // 当成“光标下的程序”——刚拖着没离开置物架就把 GTK 的拖动掐掉就错了。
    POINT cursor;
    int frame_x = 0, frame_y = 0, frame_width = 0, frame_height = 0;
    if (GetCursorPos(&cursor) && ui_window_frame(owner, &frame_x, &frame_y, &frame_width, &frame_height)
        && cursor.x >= frame_x && cursor.x < frame_x + frame_width && cursor.y >= frame_y
        && cursor.y < frame_y + frame_height)
        return false;
    if (!xdnd_source_should_take_over()) return false;
    // 复制模式（以及文字、链接这种本来就不能移走的）只接管旧拖动的数据 selection：
    // Hyprland 的拖动还留着，目标程序不会同时看到两个拖动源，也就不会把剪贴板里的东西再放一遍。
    // 移动模式要答应“移动”，旧拖动做不到，仍由自己的 X11 拖动源负责。
    if (xdnd_source_copy_only() && xdnd_source_assist_begin())
    {
        log_info(L"拖出：保留 Hyprland 的拖动，只替 X11 接收方提供数据");
        return false;
    }
    if (!xdnd_source_begin()) return false;
    if (!ui_drag_take_over(owner))
    {
        xdnd_source_abort();
        return false;
    }
    log_info(L"拖出：改由自己的 X11 拖动源送到光标下的程序");
    return true;
}

/// 这次拖出的结果。拖动失败时 GDK 留着的还是最后协商出的动作——拖动一开始先经过置物架自己，
/// 那时就定成了复制——不能当成送到了，否则在空白处松手，项目也会从置物架上消失。
static GdkDragAction drag_result(UiWindow *window, GdkDrag *drag)
{
    return drag && !window->drag_cancelled ? gdk_drag_get_selected_action(drag) : 0;
}

static gboolean drag_guard_tick(gpointer data)
{
    UiWindow *window = data;
    if (window->destroyed || !window->drag_active)
    {
        window->drag_guard = 0;
        return G_SOURCE_REMOVE;
    }
    if (g_get_monotonic_time() - window->drag_began_at < DRAG_GUARD_GRACE_MICROSECONDS) return G_SOURCE_CONTINUE;
    if (take_over_for_x11())
    {
        window->drag_guard = 0;
        return G_SOURCE_REMOVE;
    }
    int buttons = gesture_left_button_state();
    // 没看到过按下就松开的不算：可能是触摸板这种看不到左键的设备，宁可不收也不能把正常拖动掐掉。
    if (buttons == 1)
    {
        window->drag_saw_button = true;
        window->drag_released_at = 0;
        return G_SOURCE_CONTINUE;
    }
    if (buttons != 0 || !window->drag_saw_button) return G_SOURCE_CONTINUE;
    if (!window->drag_released_at) window->drag_released_at = g_get_monotonic_time();
    if (g_get_monotonic_time() - window->drag_released_at < DRAG_GUARD_RELEASE_DELAY_MICROSECONDS) return G_SOURCE_CONTINUE;
    log_info(L"GTK 拖动没有收到结束通知：左键已经松开，按拖动结束收尾（动作 0x%x，数据源留着让接收方取完）",
        (unsigned)drag_result(window, window->drag));
    window->drag_guard = 0;
    finish_stale_drag(window);
    return G_SOURCE_REMOVE;
}

static void drag_guard_stop(UiWindow *window)
{
    if (!window->drag_guard) return;
    g_source_remove(window->drag_guard);
    window->drag_guard = 0;
}

static void stop_stale_cancel(UiWindow *window)
{
    if (!window->stale_cancel_source) return;
    g_source_remove(window->stale_cancel_source);
    window->stale_cancel_source = 0;
}

static gboolean stale_cancel_fired(gpointer data)
{
    UiWindow *window = data;
    window->stale_cancel_source = 0;
    if (window->destroyed || !window->drag_stale) return G_SOURCE_REMOVE;
    GtkDragSource *source = window->drag_source;
    window->drag_stale = false;
    window->drag = NULL;
    if (!source) return G_SOURCE_REMOVE;
    log_info(L"拖出：GTK 那边一直没结束，取消挂着的拖动");
    gtk_drag_source_drag_cancel(source);
    return G_SOURCE_REMOVE;
}

/// 收过尾以后 GdkDrag 还挂在合成器那边：取消得太早可能把接收方还在读的数据掐掉，
/// 太晚又会让 Hyprland 以为我们还在拖，拖进来的东西进不来。等一小会儿再取消。
#define STALE_CANCEL_MILLISECONDS 1500

static void schedule_stale_cancel(UiWindow *window)
{
    if (!window->drag_stale || window->stale_cancel_source) return;
    window->stale_cancel_source = g_timeout_add(STALE_CANCEL_MILLISECONDS, stale_cancel_fired, window);
}

/// 新的按下：不用再等，马上把挂着的拖动取消掉。
static void cancel_stale_drags(void)
{
    for (UiWindow *window = windows; window; window = window->next)
    {
        if (!window->drag_stale) continue;
        GtkDragSource *source = window->drag_source;
        stop_stale_cancel(window);
        window->drag_stale = false;
        window->drag = NULL;
        if (!source) continue;
        log_info(L"拖出：新的按下，取消上一轮挂着的 GTK 拖动");
        gtk_drag_source_drag_cancel(source);
    }
}

/// 合成器迟迟不把拖动的结束送回来时，替 GTK 按“这次拖动已经结束”收尾：
/// 通知 GDK 到此为止（藏起跟着光标的拖动图标、清掉全局的拖动状态），置物架这边也恢复原样。
/// 但 GdkDrag 和它的数据源故意留着不取消——接收方这时可能还在取数据，
/// 提前取消会把内容掐断（拖到 WPS 里就是内容不全、图片放不进去）。留着的那次拖动等下次从这里拖出时再真正取消。
static void finish_stale_drag(UiWindow *window)
{
    if (!window->drag_active) return;
    GdkDrag *drag = window->drag;
    void *data = window->drag_data;
    window->drag_active = false;
    window->drag_stale = true;
    window->drag_data = NULL;
    active_drag = NULL;
    drag_guard_stop(window);
    schedule_stale_cancel(window);
    xdnd_source_assist_finish();
    if (drag) gdk_drag_drop_done(drag, TRUE);
    release_drag_icon_label(window);
    if (window->drag_end && data) window->drag_end(window, window->drag_context, data, drag_result(window, drag));
}

static void release_drag_icon_label(UiWindow *window)
{
    if (!window->drag_icon_label) return;
    g_object_unref(window->drag_icon_label);
    window->drag_icon_label = NULL;
}

/// 窗口关掉或不再管拖放时，GTK 不会替我们结束进行中的拖动，先取消掉，别让拖动状态卡在全局。
static void release_active_drag(UiWindow *window)
{
    if (!window->drag_active) return;
    window->drag_active = false;
    window->drag_stale = false;
    window->drag = NULL;
    active_drag = NULL;
    drag_guard_stop(window);
    stop_stale_cancel(window);
    xdnd_source_assist_finish();
    if (window->drag_source) gtk_drag_source_drag_cancel(window->drag_source);
    release_drag_icon_label(window);
}

bool ui_drag_take_over(UiWindow *window)
{
    if (!window || window->destroyed || !window->drag_active || !window->drag_data || !window->drag_source) return false;
    GtkDragSource *drag_source = window->drag_source;
    window->drag_taken_over = true;
    window->drag_active = false;
    window->drag = NULL;
    window->drag_stale = false;
    if (active_drag) active_drag = NULL;
    drag_guard_stop(window);
    // 取消 GTK 的拖动：Hyprland 那边那次拖动跟着结束，对方也就不会再收到它发过去的 Xdnd，
    // 免得两个拖动源同时往一个窗口上放。drag-end 可能紧接着就到，收尾留给 X11 拖动源。
    gtk_drag_source_drag_cancel(drag_source);
    release_drag_icon_label(window);
    return true;
}

void ui_drag_taken_over_finished(GdkDragAction action)
{
    for (UiWindow *window = windows; window; window = window->next)
    {
        if (!window->drag_taken_over) continue;
        window->drag_taken_over = false;
        void *data = window->drag_data;
        window->drag_data = NULL;
        if (!data || !window->drag_end) return;
        log_info(L"拖出：X11 拖动源收尾，动作 0x%x", (unsigned)action);
        window->drag_end(window, window->drag_context, data, action);
        return;
    }
}

void ui_window_set_drop_handler(UiWindow *window, UiDropHandler handler, void *context)
{
    if (!window) return;
    window->drop_handler = handler;
    window->drop_context = context;
    log_info(L"拖放：窗口%ls接收文件拖入（层壳 %ls）", handler ? L"开始" : L"不再", window->layer ? L"是" : L"否");
}

void ui_window_set_drag_handler(UiWindow *window, UiDragPrepare prepare, UiDragEnd end, void *context)
{
    if (!window) return;
    // 收掉处理器时如果还在拖动，先让它按取消收尾，再换掉回调，否则这次拖动永远结束不了。
    if (!prepare)
    {
        release_active_drag(window);
        // 交给 X11 拖动源的那次拖出不归 GTK 管：这里也要让它收尾，不然置物架一直停在“正在拖”。
        if (window->drag_taken_over) xdnd_source_abort();
    }
    window->drag_prepare = prepare;
    window->drag_end = end;
    window->drag_context = context;
}

/// GTK 默认把拖出去的文字整段做成拖动图标里的一个 GtkLabel。文字很长时这个标签会宽得
/// 离谱（很多行时还会非常高），拖动图标窗口跟着变得巨大：GL 渲染时 NVIDIA 会报
/// Xid 13“3D HEIGHT CT Violation”，显卡通道被重置，严重时整个桌面一起崩。
/// 图标只需要让人看出拖的是什么，这里只留开头几十个字符。
#define DRAG_ICON_TEXT_CHARACTERS 32

static char *drag_icon_preview(const char *text)
{
    GString *out = g_string_sized_new(4 * DRAG_ICON_TEXT_CHARACTERS);
    int characters = 0;
    bool space = false;
    const char *cursor = text;
    while (*cursor && characters < DRAG_ICON_TEXT_CHARACTERS)
    {
        gunichar code = g_utf8_get_char(cursor);
        if (g_unichar_isspace(code))
        {
            if (out->len > 0) space = true;
        }
        else
        {
            if (space)
            {
                g_string_append_c(out, ' ');
                characters++;
                space = false;
                if (characters >= DRAG_ICON_TEXT_CHARACTERS) break;
            }
            char bytes[8];
            int length = g_unichar_to_utf8(code, bytes);
            g_string_append_len(out, bytes, length);
            characters++;
        }
        cursor = g_utf8_next_char(cursor);
    }
    if (*cursor) g_string_append(out, "…");
    return g_string_free(out, FALSE);
}

/// 文字拖动时换成一个截短的、单行的拖动图标；文件等其它内容仍用 GTK 自己的默认图标。
static void set_drag_icon(UiWindow *window, GtkDragSource *source, GdkContentProvider *provider)
{
    bool text = false;
    GdkContentFormats *formats = gdk_content_provider_ref_formats(provider);
    if (formats) text = gdk_content_formats_contain_gtype(formats, G_TYPE_STRING);
    gdk_content_formats_unref(formats);
    if (!text)
    {
        // 上一次拖文字设过的图标不能用在下一次拖文件上。
        release_drag_icon_label(window);
        gtk_drag_source_set_icon(source, NULL, 0, 0);
        return;
    }
    char *preview = NULL;
    GValue value = G_VALUE_INIT;
    if (gdk_content_provider_get_value(provider, &value, NULL))
    {
        if (G_VALUE_HOLDS_STRING(&value)) preview = drag_icon_preview(g_value_get_string(&value));
        g_value_unset(&value);
    }
    // 取不到文字（或者全是空白）也不能退回 GTK 的默认图标：那正是照整段文字生成大标签的路径。
    if (preview && !*preview)
    {
        g_free(preview);
        preview = NULL;
    }
    // GTK 看到没有图标就会照整段文字生成一个标签。先放一个 1x1 的透明图标挡住它，
    // 真正的截短标签在 drag-begin 里换进拖动图标（见 on_drag_begin）。
    static const guint8 transparent[4] = { 0, 0, 0, 0 };
    GBytes *bytes = g_bytes_new_static(transparent, sizeof transparent);
    GdkTexture *texture = gdk_memory_texture_new(1, 1, GDK_MEMORY_DEFAULT, bytes, 4);
    g_bytes_unref(bytes);
    gtk_drag_source_set_icon(source, GDK_PAINTABLE(texture), 0, 0);
    g_object_unref(texture);
    release_drag_icon_label(window);
    if (!preview) return;
    GtkWidget *label = gtk_label_new(preview);
    g_free(preview);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(label), 1);
    gtk_label_set_max_width_chars(GTK_LABEL(label), DRAG_ICON_TEXT_CHARACTERS);
    window->drag_icon_label = label;
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y, gpointer data)
{
    UiWindow *window = data;
    if (!window->drag_prepare || window->destroyed) return NULL;
    // GTK 已经在准备新一次拖动了，说明上一次拖动对它来说早该结束了。可 WPS 这类程序常常
    // 不回结束通知，置物架还停在“正在拖出”，新的拖出就会被自己挡回去。这里先按漏发收尾。
    // 不先收掉的话，用户越急着连续拖，左键一直按着，拖动的兜底收尾也永远等不到松开。
    if (window->drag_active)
    {
        log_info(L"拖出：上一次拖动还挂着，先收尾再开始新的拖动");
        finish_stale_drag(window);
    }
    // 正常情况下按下时已经取消过上一轮挂着的拖动了（见 cancel_stale_drags）。这里只把指针放掉，
    // 不能再调 gtk_drag_source_drag_cancel：这时候 GTK 正在准备当前这次拖动，会把新的取消掉。
    if (window->drag_stale)
    {
        stop_stale_cancel(window);
        window->drag_stale = false;
        window->drag = NULL;
    }
    GdkDragAction actions = GDK_ACTION_COPY;
    window->drag_data = NULL;
    GdkContentProvider *provider = window->drag_prepare(window, window->drag_context, x, y, &actions, &window->drag_data);
    if (!provider)
    {
        window->drag_data = NULL;
        return NULL;
    }
    gtk_drag_source_set_actions(source, actions ? actions : GDK_ACTION_COPY);
    set_drag_icon(window, source, provider);
    return provider;
}

static void on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer data)
{
    UiWindow *window = data;
    // 上一轮拖动没收到结束通知的话，新的拖动一开就把旧的那次忘掉。
    if (active_drag && active_drag != drag) log_info(L"上一次拖动没有收到结束通知，新的拖动开始时忘掉它");
    log_info(L"拖出：GTK 拖动开始");
    stop_stale_cancel(window);
    active_drag = drag;
    window->drag_source = source;
    window->drag = drag;
    window->drag_active = true;
    window->drag_stale = false;
    window->drag_cancelled = false;
    window->drag_saw_button = gesture_left_button_state() == 1;
    window->drag_released_at = 0;
    window->drag_began_at = g_get_monotonic_time();
    // 文字拖动：把截短的标签换进拖动图标（prepare 里只放了个透明占位）。
    if (window->drag_icon_label)
    {
        GtkWidget *icon = gtk_drag_icon_get_for_drag(drag);
        if (icon) gtk_drag_icon_set_child(GTK_DRAG_ICON(icon), window->drag_icon_label);
    }
    if (!window->drag_guard) window->drag_guard = g_timeout_add(DRAG_GUARD_POLL_MILLISECONDS, drag_guard_tick, window);
}

static gboolean on_drag_cancel(GtkDragSource *source, GdkDrag *drag, GdkDragCancelReason reason, gpointer data)
{
    (void)source;
    UiWindow *window = data;
    if (!window->destroyed && window->drag == drag)
    {
        window->drag_cancelled = true;
        log_info(L"拖出：GTK 拖动没有送出去（原因 %d）", (int)reason);
    }
    return FALSE;
}

static void on_drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data, gpointer data)
{
    (void)source;
    (void)delete_data;
    UiWindow *window = data;
    if (window->destroyed) return;
    if (active_drag == drag) active_drag = NULL;
    // 迟到的结束通知：可能是更早一次拖动的，也可能已经由 finish_stale_drag 收过尾了。
    // 不认准是哪一个就收尾，会把正在进行的拖动或置物架状态弄乱。
    if (window->drag != drag)
    {
        log_info(L"拖出结束：收到更早一次拖动的结束通知，忽略");
        return;
    }
    bool was_stale = window->drag_stale;
    window->drag = NULL;
    window->drag_active = false;
    window->drag_stale = false;
    drag_guard_stop(window);
    stop_stale_cancel(window);
    // 复制模式接管过 selection 的话，等接收方把数据取完再交回去。
    xdnd_source_assist_finish();
    release_drag_icon_label(window);
    if (window->drag_taken_over)
    {
        // 这次拖出已经交给 X11 拖动源：GTK 的拖动只是被取消了，收尾等 X11 那边谈完再说。
        log_info(L"拖出结束：GTK 的拖动收起来了，由 X11 拖动源继续");
        return;
    }
    if (was_stale)
    {
        // 已经按漏发收过尾、也回调过置物架了，这里只把 GTK 拖动的残留清掉。
        log_info(L"拖出结束：收到迟到的 GTK 结束通知");
        return;
    }
    GdkDragAction action = drag_result(window, drag);
    log_info(L"拖出结束：收到 GTK 的结束通知，动作 0x%x", (unsigned)action);
    if (window->drag_end) window->drag_end(window, window->drag_context, window->drag_data, action);
    window->drag_data = NULL;
}

UiWindow *ui_window_create(const UiWindowOptions *options)
{
    if (!options) return NULL;
    if (!gfx_initialize()) return NULL;
    install_css();

    UiWindow *window = xcalloc(1, sizeof(*window));
    window->options = *options;
    window->width = options->width > 0 ? options->width : 640;
    window->height = options->height > 0 ? options->height : 480;
    window->layout_dirty = true;
    window->surface = gfx_surface_create(NULL);

    window->window = gtk_window_new();
    GtkWindow *gtk = GTK_WINDOW(window->window);
    // 挂到当前的 GtkApplication 上；不然 GtkApplication 数不到这个窗口，一启动就会因为“没有窗口”退出。
    GApplication *application = g_application_get_default();
    if (GTK_IS_APPLICATION(application))
        gtk_window_set_application(gtk, GTK_APPLICATION(application));
    char *title = options->title ? utf8(options->title) : NULL;
    if (title) gtk_window_set_title(gtk, title);
    free(title);
    gtk_window_set_default_size(gtk, (int)lroundf(window->width), (int)lroundf(window->height));
    gtk_window_set_resizable(gtk, options->resizable);
    bool wants_layer = options->popup || options->frameless;
    gtk_window_set_decorated(gtk, !wants_layer);
    if (!wants_layer)
    {
        // 界面自己画标题栏（最小化、最大化、关闭都在里面）；GTK 再画一条会重复。
        // 留一个看不见的标题栏给 CSD，阴影和边缘缩放还归 GTK 管。
        GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_visible(titlebar, FALSE);
        gtk_window_set_titlebar(gtk, titlebar);
    }
    gtk_widget_add_css_class(window->window, "dropshelf-transparent");

    window->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(window->area, TRUE);
    gtk_widget_set_vexpand(window->area, TRUE);
    gtk_widget_set_focusable(window->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(window->area), on_draw, window, NULL);

    if (wants_layer && gtk_layer_is_supported())
    {
        window->layer = true;
        gtk_layer_init_for_window(gtk);
        gtk_layer_set_namespace(gtk, "dropshelf");
        gtk_layer_set_layer(gtk, options->popup ? GTK_LAYER_SHELL_LAYER_OVERLAY : GTK_LAYER_SHELL_LAYER_TOP);
        gtk_layer_set_anchor(gtk, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(gtk, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_keyboard_mode(gtk, options->no_activate ? GTK_LAYER_SHELL_KEYBOARD_MODE_NONE
            : GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        gtk_layer_set_exclusive_zone(gtk, -1);
    }

    // 自绘区域放在 GtkOverlay 里：输入框有焦点时把 GtkEntry 盖在它上面。
    window->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(window->overlay), window->area);
    gtk_window_set_child(gtk, window->overlay);

    GtkGesture *left = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left), 1);
    g_signal_connect(left, "pressed", G_CALLBACK(on_pressed), window);
    g_signal_connect(left, "released", G_CALLBACK(on_released), window);
    gtk_widget_add_controller(window->area, GTK_EVENT_CONTROLLER(left));

    GtkGesture *right = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), 3);
    g_signal_connect(right, "pressed", G_CALLBACK(on_pressed), window);
    g_signal_connect(right, "released", G_CALLBACK(on_released), window);
    gtk_widget_add_controller(window->area, GTK_EVENT_CONTROLLER(right));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), window);
    g_signal_connect(motion, "enter", G_CALLBACK(on_enter), window);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), window);
    gtk_widget_add_controller(window->area, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), window);
    gtk_widget_add_controller(window->area, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), window);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), window);
    gtk_widget_add_controller(window->area, key);

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), window);
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), window);
    gtk_widget_add_controller(window->area, focus);

    // 从别的程序拖进来：文件、文字、图片。数据在松开时由 GTK 读好，交给窗口的拖放回调。
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    // 顺序就是偏好：浏览器拖图片时同时给 GdkFileList（里面是网页地址，没有本地路径）
// 和 image/png，优先要图片本身，落下来才是一张能看的图。
    GType drop_types[] = { GDK_TYPE_TEXTURE, GDK_TYPE_FILE_LIST, G_TYPE_FILE, G_TYPE_STRING };
    gtk_drop_target_set_gtypes(drop, drop_types, G_N_ELEMENTS(drop_types));
    // 捕获阶段先拿到拖动：别的控制器（编辑框、拖动源）在冒泡阶段可能先把它吃掉。
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drop), GTK_PHASE_CAPTURE);
    g_signal_connect(drop, "enter", G_CALLBACK(on_drop_enter), window);
    g_signal_connect(drop, "motion", G_CALLBACK(on_drop_motion), window);
    g_signal_connect(drop, "leave", G_CALLBACK(on_drop_leave), window);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), window);
    gtk_widget_add_controller(window->area, GTK_EVENT_CONTROLLER(drop));

    // 从置物架拖出：GTK4 只在拖动达到阈值时通过 prepare 问内容，置物架在这里决定拖哪些项目。
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(source, "prepare", G_CALLBACK(on_drag_prepare), window);
    g_signal_connect(source, "drag-begin", G_CALLBACK(on_drag_begin), window);
    g_signal_connect(source, "drag-cancel", G_CALLBACK(on_drag_cancel), window);
    g_signal_connect(source, "drag-end", G_CALLBACK(on_drag_end), window);
    gtk_widget_add_controller(window->area, GTK_EVENT_CONTROLLER(source));

    g_signal_connect(window->window, "destroy", G_CALLBACK(on_window_destroy), window);

    window->next = windows;
    windows = window;
    return window;
}

HWND ui_window_handle(UiWindow *window)
{
    return window ? (HWND)window : NULL;
}

// Win32 形状的窗口位置接口。位置是屏幕像素，大小换算回 DIP 再交给窗口后端。
BOOL GetWindowRect(HWND handle, RECT *rect)
{
    UiWindow *window = (UiWindow *)handle;
    if (!window || window->destroyed || !rect) return FALSE;
    float scale = ui_window_scale(window);
    rect->left = window->x;
    rect->top = window->y;
    rect->right = window->x + (LONG)lroundf(window->width * scale);
    rect->bottom = window->y + (LONG)lroundf(window->height * scale);
    return TRUE;
}

BOOL SetWindowPos(HWND handle, HWND insert_after, int x, int y, int width, int height, UINT flags)
{
    (void)insert_after;
    UiWindow *window = (UiWindow *)handle;
    if (!window || window->destroyed) return FALSE;
    int left = (flags & SWP_NOMOVE) ? window->x : x;
    int top = (flags & SWP_NOMOVE) ? window->y : y;
    float logical_width = 0, logical_height = 0;
    if (!(flags & SWP_NOSIZE) && width > 0 && height > 0)
    {
        float scale = ui_window_scale(window);
        if (scale <= 0) scale = 1;
        logical_width = width / scale;
        logical_height = height / scale;
    }
    ui_window_place(window, left, top, logical_width, logical_height);
    return TRUE;
}

BOOL IsWindowVisible(HWND handle)
{
    UiWindow *window = (UiWindow *)handle;
    return window && !window->destroyed && window->visible;
}

BOOL IsWindow(HWND handle)
{
    UiWindow *window = (UiWindow *)handle;
    return window && !window->destroyed;
}

BOOL IsIconic(HWND handle)
{
    // Wayland 不告诉客户端窗口是不是最小化了；主窗口这时候也不会被当成可见。
    (void)handle;
    return FALSE;
}

BOOL IsZoomed(HWND handle)
{
    UiWindow *window = (UiWindow *)handle;
    return window && !window->destroyed && gtk_window_is_maximized(GTK_WINDOW(window->window));
}

BOOL EnableWindow(HWND handle, BOOL enable)
{
    (void)handle;
    (void)enable;
    return TRUE;
}

BOOL DestroyIcon(HICON icon)
{
    (void)icon;
    return TRUE;
}

static gboolean hide_after_frame(gpointer data)
{
    UiWindow *window = data;
    window->hide_posted = false;
    // 这期间又显示出来或已经关掉了，就不用再藏。关掉的窗口由排在这之后的空闲回调释放，这里还能读。
    if (!window->destroyed && !window->visible) gtk_widget_set_visible(window->window, FALSE);
    return G_SOURCE_REMOVE;
}

/// 共享代码里的 ShowWindow：隐藏、显示、最小化、最大化和还原。
BOOL ShowWindow(HWND handle, int command)
{
    UiWindow *window = (UiWindow *)handle;
    if (!window || window->destroyed) return FALSE;
    if (command == SW_HIDE)
    {
        window->visible = false;
        window->hovered = window->captured = NULL;
        // 动画播完时是在绘制回调里藏的：当场藏起来的话，GTK 还会把这一帧提交到已经撤掉层壳的 surface 上，
        // 下次再显示时合成器按协议错误断开连接。等这一帧画完再藏。
        if (!window->rendering) gtk_widget_set_visible(window->window, FALSE);
        else if (!window->hide_posted)
        {
            window->hide_posted = true;
            g_idle_add(hide_after_frame, window);
        }
        return TRUE;
    }
    if (command == SW_MINIMIZE)
    {
        gtk_window_minimize(GTK_WINDOW(window->window));
        return TRUE;
    }
    if (command == SW_MAXIMIZE)
    {
        gtk_window_maximize(GTK_WINDOW(window->window));
        return TRUE;
    }
    if (command == SW_RESTORE)
    {
        gtk_window_unmaximize(GTK_WINDOW(window->window));
        return TRUE;
    }
    ui_window_show_with(window, command != SW_SHOWNOACTIVATE);
    return TRUE;
}

/// 隐身：Wayland 没有 DWM 那样的 cloak。窗口照样映射、照样出帧，只是画成全透明，取消隐身后下一帧就是画好的样子。
/// 不用透明度 0：GTK 不给透明度为 0 的窗口调绘制回调，而动画是在绘制里推进的，牌和置物架的出场会停住。
void ui_window_set_cloaked(HWND handle, bool cloaked)
{
    UiWindow *window = (UiWindow *)handle;
    if (!window || window->destroyed || window->cloaked == cloaked) return;
    window->cloaked = cloaked;
    gtk_widget_queue_draw(window->area);
}

void ui_window_set_click_through(UiWindow *window, bool on)
{
    if (!window || window->destroyed) return;
    GtkNative *native = gtk_widget_get_native(window->window);
    if (!native) return;
    GdkSurface *surface = gtk_native_get_surface(native);
    if (!surface) return;
    // 空输入区域：鼠标事件直接落到下面的窗口上。
    cairo_region_t *region = on ? cairo_region_create() : NULL;
    gdk_surface_set_input_region(surface, region);
    if (region) cairo_region_destroy(region);
}

void ui_window_set_root(UiWindow *window, UiElement *root)
{
    if (!window || window->destroyed) return;
    UiElement *previous = window->root;
    window->root = NULL;
    window->hovered = window->captured = window->focused = window->maximize_button = NULL;
    if (previous) ui_element_destroy(previous);
    window->root = root;
    if (root) ui_element_attach(root, window);
    ui_window_invalidate(window);
}

UiElement *ui_window_root(UiWindow *window)
{
    return window ? window->root : NULL;
}

void ui_window_show(UiWindow *window)
{
    ui_window_show_with(window, !window->options.no_activate);
}

/// 层壳窗口要不要键盘：平时按需（点一下才收），X11 应用正拖着东西时先不要——
/// 键盘一换到我们这边，Hyprland 就把键盘从 XWayland 收走，X 那边的拖动会被直接结束、东西放不下来。
/// 这种时候先记着，第一次点它再打开（见 on_pressed）。
static void update_layer_keyboard(UiWindow *window)
{
    if (!window->layer || window->options.no_activate) return;
    window->keyboard_deferred = xdnd_drag_in_progress();
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window->window),
        window->keyboard_deferred ? GTK_LAYER_SHELL_KEYBOARD_MODE_NONE : GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
}

void ui_window_show_with(UiWindow *window, bool activate)
{
    if (!window || window->destroyed) return;
    update_layer_keyboard(window);
    update_layer_position(window);
    window->visible = true;
    gtk_widget_set_visible(window->window, TRUE);
    if (window->layer && window->drop_handler)
        log_info(L"窗口显示：接收拖入的层壳窗口在 (%d,%d)，%.0fx%.0f", window->x, window->y, window->width, window->height);
    if (activate && !window->options.no_activate)
    {
        gtk_window_present(GTK_WINDOW(window->window));
        gtk_widget_grab_focus(window->area);
    }
    ui_window_invalidate(window);
}

void ui_window_close(UiWindow *window)
{
    if (!window || window->destroyed) return;
    release_active_drag(window);
    gtk_window_destroy(GTK_WINDOW(window->window));
}

void ui_close_all_windows(void)
{
    UiWindow *window = windows;
    while (window)
    {
        UiWindow *next = window->next;
        ui_window_close(window);
        window = next;
    }
    windows = NULL;
}

bool ui_window_frame(UiWindow *window, int *x, int *y, int *width, int *height)
{
    if (!window || window->destroyed || !window->visible) return false;
    if (x) *x = window->x;
    if (y) *y = window->y;
    if (width) *width = (int)lroundf(window->width);
    if (height) *height = (int)lroundf(window->height);
    return true;
}

void ui_window_place(UiWindow *window, int x, int y, float width, float height)
{
    if (!window || window->destroyed) return;
    window->x = x;
    window->y = y;
    if (width > 0) window->width = width;
    if (height > 0) window->height = height;
    if (width > 0 || height > 0)
        gtk_window_set_default_size(GTK_WINDOW(window->window), (int)lroundf(window->width), (int)lroundf(window->height));
    update_layer_position(window);
}

void ui_window_center(UiWindow *window)
{
    if (!window || window->destroyed) return;
    GdkMonitor *monitor = primary_monitor();
    if (!monitor) return;
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    g_object_unref(monitor);
    int x = geometry.x + (geometry.width - (int)lroundf(window->width)) / 2;
    int y = geometry.y + (geometry.height - (int)lroundf(window->height)) / 2;
    ui_window_place(window, x, y, 0, 0);
}

void ui_window_set_topmost(UiWindow *window, bool topmost)
{
    if (!window || !window->layer || !gtk_layer_is_supported()) return;
    // Wayland 上没有真正的“取消置顶”：普通窗口永远在 overlay/top 层下面。共享代码里的
    // 置顶开关在 Linux 版去掉了，这里一律留在 overlay 层（值是判断一下再设，避免多余的重映射）。
    (void)topmost;
    GtkWindow *gtk = GTK_WINDOW(window->window);
    if (gtk_layer_get_layer(gtk) != GTK_LAYER_SHELL_LAYER_OVERLAY)
        gtk_layer_set_layer(gtk, GTK_LAYER_SHELL_LAYER_OVERLAY);
}

void ui_window_set_border_color(UiWindow *window, const Color *color)
{
    (void)window;
    (void)color;
}

/// 换成另一份回调（换风格时置物架从一种画法换到另一种）。其余选项不变。
void ui_window_set_handlers(UiWindow *window, const UiWindowOptions *handlers)
{
    if (!window || window->destroyed || !handlers) return;
    window->options.paint = handlers->paint;
    window->options.mouse = handlers->mouse;
    window->options.key = handlers->key;
    window->options.activated = handlers->activated;
    window->options.moved = handlers->moved;
    window->options.closed = handlers->closed;
    window->options.animate = handlers->animate;
    window->options.cursor = handlers->cursor;
    window->options.message = handlers->message;
    ui_window_repaint(window);
}

void ui_window_set_backdrop(UiWindow *window, UiBackdrop backdrop)
{
    if (!window || window->destroyed) return;
    // Wayland 没有 Mica/Acrylic；深浅两种风格都用自己的纯色背景，这里只记下选项。
    window->options.backdrop = backdrop;
    ui_window_repaint(window);
}

void ui_window_set_icon(UiWindow *window, HICON icon)
{
    (void)window;
    (void)icon;
    // Linux 的窗口图标跟着 .desktop 的 app id 走，切换风格时不用换。
}

/// Wayland 窗口没有系统画的圆角（Windows 11 的 DWM 会给），置物架是方角的，替它表演的牌也用方角（见 shelf_card.c）。
bool ui_is_windows_11(void)
{
    return false;
}

void ui_window_set_maximize_button(UiWindow *window, UiElement *button)
{
    if (window) window->maximize_button = button;
}

void ui_window_begin_move(UiWindow *window)
{
    if (!window || window->destroyed) return;
    if (window->layer)
    {
        // layer-shell 没有系统拖动；记下起点，移动时改 margin。
        window->moving = true;
        window->move_last_x = window->pointer_x;
        window->move_last_y = window->pointer_y;
        window->move_button_seen = gesture_left_button_state() > 0;
        window->move_blind_ticks = 0;
        POINT cursor;
        window->move_by_cursor = GetCursorPos(&cursor);
        if (window->move_by_cursor)
        {
            window->move_offset_x = cursor.x - window->x;
            window->move_offset_y = cursor.y - window->y;
        }
        log_info(L"窗口拖动开始：层壳窗口在 (%d,%d)，%ls", window->x, window->y,
            window->move_by_cursor ? L"按全局光标定位" : L"按本地增量移动");
        // 能看到全局左键时才用定时器跟光标：它同时负责判断松手。
        if (window->move_by_cursor && gesture_left_button_state() >= 0 && !window->move_source)
            window->move_source = g_timeout_add(MOVE_FOLLOW_MILLISECONDS, move_follow_tick, window);
        return;
    }
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window->window));
    if (!surface || !GDK_IS_TOPLEVEL(surface)) return;
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    if (pointer) gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), pointer, 1, window->pointer_x, window->pointer_y, GDK_CURRENT_TIME);
}

void ui_window_set_tooltip(UiWindow *window, const wchar_t *text)
{
    if (!window || window->destroyed) return;
    free(window->tooltip_text);
    window->tooltip_text = text ? wcs_dup(text) : NULL;
    if (!window->tooltip_text)
    {
        gtk_widget_set_has_tooltip(window->area, FALSE);
        return;
    }
    char *bytes = utf8(window->tooltip_text);
    gtk_widget_set_tooltip_text(window->area, bytes);
    free(bytes);
}

bool ui_window_snapshot(UiWindow *window, const wchar_t *path)
{
    if (!window || window->destroyed || !path) return false;
    int scale = (int)lroundf(ui_window_scale(window));
    if (scale < 1) scale = 1;
    int width = (int)lroundf(window->width) * scale, height = (int)lroundf(window->height) * scale;
    if (width <= 0 || height <= 0) return false;
    if (!gfx_begin_image(width, height, 96.0f * scale)) return false;
    gfx_clear(window->options.popup ? COLOR_TRANSPARENT : theme_color(window->options.background));
    if (window->root)
    {
        ui_measure(window->root, window->width);
        ui_arrange(window->root, (GfxRect){ 0, 0, window->width, window->height });
        ui_paint(window->root, frames_time());
    }
    if (window->options.paint) window->options.paint(window, window->options.context, frames_time());
    return gfx_end_image(path);
}

/// 把窗口现在画的样子截成一张图片（换风格时盖在窗口上的那张牌正面就是它）。和 ui_window_snapshot 一样先离屏画一遍。
GfxImage *ui_window_capture(UiWindow *window)
{
    if (!window || window->destroyed) return NULL;
    int scale = (int)lroundf(ui_window_scale(window));
    if (scale < 1) scale = 1;
    int width = (int)lroundf(window->width) * scale, height = (int)lroundf(window->height) * scale;
    if (width <= 0 || height <= 0) return NULL;
    if (!gfx_begin_image(width, height, 96.0f * scale)) return NULL;
    gfx_clear(window->options.popup ? COLOR_TRANSPARENT : theme_color(window->options.background));
    if (window->options.paint) window->options.paint(window, window->options.context, frames_time());
    if (window->root) ui_paint(window->root, frames_time());
    return gfx_end_image_capture();
}

void ui_window_warm(UiWindow *window)
{
    (void)window;
}

void ui_window_layout(UiWindow *window)
{
    if (!window || window->destroyed || !window->root || window->rendering) return;
    int width = gtk_widget_get_width(window->area);
    int height = gtk_widget_get_height(window->area);
    if (width > 0 && height > 0) layout_now(window, width, height);
}

void ui_window_update_hover(UiWindow *window)
{
    if (!window || window->destroyed || window->captured || window->rendering) return;
    UiMouseEvent event = { .kind = UI_MOUSE_MOVE, .x = window->pointer_x, .y = window->pointer_y };
    set_hovered(window, window->pointer_inside ? element_at(window, event.x, event.y) : NULL, &event);
}

void ui_window_forget(UiWindow *window, UiElement *element)
{
    if (!window) return;
    if (window->focused == element) window->focused = NULL;
    if (window->hovered == element) window->hovered = NULL;
    if (window->maximize_button == element) window->maximize_button = NULL;
    if (window->captured == element) window->captured = NULL;
    if (window->root == element) window->root = NULL;
}

// ───────────── 输入框的 GtkEntry 覆盖层 ─────────────
// 自绘的输入框自己处理按键，但输入法（中文、日文…）只对真正的 GTK 控件生效。
// 有焦点时在输入框上面盖一个同样位置的 GtkEntry：文字、焦点和回车都同步，外观用主题色。

static void editor_css_install(void)
{
    if (!editor_css)
    {
        editor_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(editor_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    Color background = theme_color(THEME_FLYOUT);
    Color text = theme_color(THEME_TEXT_PRIMARY);
    char *css = g_strdup_printf(
        ".dropshelf-editor { background-color: rgba(%u,%u,%u,%.3f); color: rgba(%u,%u,%u,%.3f);"
        " border: none; border-radius: 4px; padding: 0 10px; font-size: 14px; min-height: 0;"
        " box-shadow: none; outline: none; }",
        background.r, background.g, background.b, background.a / 255.0, text.r, text.g, text.b, text.a / 255.0);
    gtk_css_provider_load_from_string(editor_css, css);
    g_free(css);
}

static void editor_apply_text(UiWindow *window)
{
    if (!window->editing || !window->editor) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(window->editor));
    wchar_t *wide = wide_from_utf8(text ? text : "", text ? strlen(text) : 0);
    if (wide)
    {
        ui_textbox_type(window->editing, wide);
        free(wide);
    }
}

static void editor_changed(GtkEditable *editable, gpointer data)
{
    (void)editable;
    editor_apply_text(data);
}

static void editor_activated(GtkEntry *entry, gpointer data)
{
    (void)entry;
    UiWindow *window = data;
    if (window->editing && window->editing->type->key) window->editing->type->key(window->editing, VK_RETURN);
}

static gboolean editor_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    UiWindow *window = data;
    if (!window->editing || !window->options.key) return FALSE;
    unsigned key = 0;
    switch (keyval)
    {
        case GDK_KEY_Escape: key = VK_ESCAPE; break;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab: key = VK_TAB; break;
        case GDK_KEY_Up: key = VK_UP; break;
        case GDK_KEY_Down: key = VK_DOWN; break;
        case GDK_KEY_Page_Up: key = VK_PRIOR; break;
        case GDK_KEY_Page_Down: key = VK_NEXT; break;
        default: return FALSE;
    }
    return window->options.key(window, window->options.context, key);
}

static void editor_create(UiWindow *window)
{
    if (window->editor || !window->overlay) return;
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "dropshelf-editor");
    gtk_widget_set_halign(entry, GTK_ALIGN_START);
    gtk_widget_set_valign(entry, GTK_ALIGN_START);
    gtk_widget_set_visible(entry, FALSE);
    g_signal_connect(entry, "changed", G_CALLBACK(editor_changed), window);
    g_signal_connect(entry, "activate", G_CALLBACK(editor_activated), window);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(editor_key_pressed), window);
    gtk_widget_add_controller(entry, keys);
    gtk_overlay_add_overlay(GTK_OVERLAY(window->overlay), entry);
    window->editor = entry;
}

static void editor_place(UiWindow *window)
{
    if (!window->editor || !window->editing) return;
    GfxRect bounds = window->editing->bounds;
    int left = (int)lroundf(bounds.left) + 1, top = (int)lroundf(bounds.top) + 1;
    int width = (int)lroundf(bounds.right - bounds.left) - 2;
    int height = (int)lroundf(bounds.bottom - bounds.top) - 2;
    gtk_widget_set_margin_start(window->editor, left);
    gtk_widget_set_margin_top(window->editor, top);
    gtk_widget_set_size_request(window->editor, width > 10 ? width : 10, height > 10 ? height : 10);
}

static void update_editor(UiWindow *window)
{
    if (!window || window->destroyed) return;
    UiElement *focused = window->focused;
    bool wanted = focused && focused->type->text_edit && !focused->disabled;
    if (!wanted)
    {
        if (window->editing)
        {
            window->editing = NULL;
            if (window->editor) gtk_widget_set_visible(window->editor, FALSE);
            if (window->area) gtk_widget_grab_focus(window->area);
        }
        return;
    }
    editor_create(window);
    if (!window->editor) return;
    if (window->editing != focused)
    {
        window->editing = focused;
        editor_css_install();
        const wchar_t *text = ui_textbox_text(focused);
        char *utf8 = utf8_from_wide(text ? text : L"", NULL);
        gtk_editable_set_text(GTK_EDITABLE(window->editor), utf8 ? utf8 : "");
        free(utf8);
        const wchar_t *placeholder = ui_textbox_placeholder(focused);
        char *hint = placeholder && *placeholder ? utf8_from_wide(placeholder, NULL) : NULL;
        gtk_entry_set_placeholder_text(GTK_ENTRY(window->editor), hint);
        free(hint);
        gtk_widget_set_visible(window->editor, TRUE);
        gtk_widget_grab_focus(window->editor);
    }
    else
    {
        // 程序改了文字（比如校验后纠正）：让 GtkEntry 跟上；内容一样时不动，不影响输入法的组合。
        const wchar_t *text = ui_textbox_text(focused);
        char *utf8 = utf8_from_wide(text ? text : L"", NULL);
        const char *current = gtk_editable_get_text(GTK_EDITABLE(window->editor));
        if (utf8 && (!current || strcmp(current, utf8) != 0)) gtk_editable_set_text(GTK_EDITABLE(window->editor), utf8);
        free(utf8);
    }
    editor_place(window);
}

void ui_window_set_focus(UiWindow *window, UiElement *element)
{
    if (!window || window->focused == element) return;
    UiElement *previous = window->focused;
    window->focused = element;
    if (previous)
    {
        previous->focused = false;
        if (previous->type->focus) previous->type->focus(previous, false);
    }
    if (element && window->focused == element)
    {
        element->focused = true;
        if (element->type->focus) element->type->focus(element, true);
    }
    update_editor(window);
    ui_window_repaint(window);
}

UiElement *ui_window_focus(UiWindow *window)
{
    return window ? window->focused : NULL;
}

void ui_window_cancel_press(UiWindow *window)
{
    if (!window || !window->captured) return;
    UiElement *captured = window->captured;
    window->captured = NULL;
    captured->pressed = false;
    UiMouseEvent event = { .kind = UI_MOUSE_UP, .x = -1, .y = -1 };
    send_mouse(captured, UI_MOUSE_UP, &event);
    ui_window_repaint(window);
}

POINT ui_window_to_screen(UiWindow *window, float x, float y)
{
    POINT point = { 0, 0 };
    if (!window) return point;
    point.x = window->x + (LONG)lroundf(x);
    point.y = window->y + (LONG)lroundf(y);
    return point;
}

void ui_window_from_screen(UiWindow *window, POINT point, float *x, float *y)
{
    if (!window)
    {
        if (x) *x = point.x;
        if (y) *y = point.y;
        return;
    }
    if (x) *x = (float)(point.x - window->x);
    if (y) *y = (float)(point.y - window->y);
}

float ui_window_scale(UiWindow *window)
{
    if (!window || window->destroyed) return 1;
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window->window));
    if (surface)
    {
        GdkDisplay *display = gdk_surface_get_display(surface);
        GdkMonitor *monitor = gdk_display_get_monitor_at_surface(display, surface);
        if (monitor)
        {
            int scale = gdk_monitor_get_scale_factor(monitor);
            if (scale > 0) return (float)scale;
        }
    }
    return 1;
}

GfxSize ui_window_size(UiWindow *window)
{
    if (!window || window->destroyed) return (GfxSize){ 0, 0 };
    int width = gtk_widget_get_width(window->area);
    int height = gtk_widget_get_height(window->area);
    if (width <= 0 || height <= 0) return (GfxSize){ window->width, window->height };
    return (GfxSize){ (float)width, (float)height };
}

bool ui_window_is_active(UiWindow *window)
{
    return window && window->active;
}

bool ui_window_has_backdrop(UiWindow *window)
{
    (void)window;
    return false;
}

void ui_refresh_window_appearance(void)
{
    for (UiWindow *window = windows; window; window = window->next) ui_window_repaint(window);
}

void ui_snapshot_windows(const wchar_t *directory)
{
    int index = 1;
    for (UiWindow *window = windows; window; window = window->next)
    {
        wchar_t name[64];
        swprintf(name, 64, L"window-%d.png", index++);
        wchar_t *path = path_join(directory, name);
        ui_window_snapshot(window, path);
        free(path);
    }
}