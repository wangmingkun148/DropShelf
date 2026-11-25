#include "ui.h"
#include "ui_internal.h"
#include "lang.h"
#include "log.h"
#include "task.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

#define UI_WINDOW_CLASS L"DropShelfWindow"

// 较新的 DWM 属性，旧的 SDK 头文件里可能没有。
#define ATTRIBUTE_TRANSITIONS_FORCEDISABLED 3
#define ATTRIBUTE_USE_IMMERSIVE_DARK_MODE_OLD 19
#define ATTRIBUTE_USE_IMMERSIVE_DARK_MODE 20
#define ATTRIBUTE_WINDOW_CORNER_PREFERENCE 33
#define ATTRIBUTE_BORDER_COLOR 34
#define ATTRIBUTE_SYSTEMBACKDROP_TYPE 38
#define CORNER_DO_NOT_ROUND 1
#define BORDER_COLOR_NONE 0xFFFFFFFE

/// 两帧之间隔了这么久（秒），算作新的一段绘制，统计分开记。
#define FRAME_BURST_GAP 0.25
/// 所有窗口都不在显示、也没有画过这么久（毫秒）以后，放掉显卡上留着下次用的缓存。
#define IDLE_TRIM_MILLISECONDS 3000

/// 一段连续的绘制（一段动画、一次拖动边框调整大小）用了多少时间。DROPSHELF_TRACE=1 时记进日志，用来比较优化前后。
typedef struct FrameStats
{
    int frames;
    int missed;                 // 和上一帧隔了一个半刷新周期以上（低性能模式隔帧画，按两个半）
    double first;               // 第一帧画了多久（秒）
    double total, longest;
    double began, last;         // 第一帧、最近一帧开始画的时刻
    float width, height;        // 最近一帧的窗口大小（DIP）
} FrameStats;

struct UiWindow
{
    HWND handle;
    UiWindowOptions options;
    GfxSurface *surface;
    UiElement *root;
    UiElement *hovered;         // 光标下的元素
    UiElement *captured;        // 按下鼠标时的元素，松开之前事件都交给它
    UiElement *focused;         // 有键盘焦点的元素
    UiElement *maximize_button; // 自己画的最大化按钮：Windows 11 上按系统的最大化按钮报告命中，悬停时照常弹出贴靠布局
    bool swallow_char;          // 按着 Alt 的按键已经由元素处理，随后的 WM_SYSCHAR 不要再交给系统（会响一声）
    bool tracking_mouse;        // 已经请求了“光标离开窗口”的通知
    bool tracking_nonclient;    // 已经请求了“光标离开最大化按钮”的通知（按钮上算非客户区）
    bool active;
    UiBackdrop applied_backdrop;
    bool backdrop_active;
    bool layout_dirty;
    bool frame_requested;       // 画的过程中有元素说还有动画在进行
    bool subscribed;            // 已经订阅逐帧回调
    bool stale;                 // 隐藏后还没画过：屏幕上还是上次显示时的画面
    bool rendering;
    bool animating;             // 正在调用 animate 回调（它可能改窗口大小，引起嵌套的重画）
    int message_depth;          // 正在处理的窗口消息有几层
    bool destroyed;             // 窗口已经销毁，等消息都处理完再释放这块内存
    HWND tooltip;               // 提示框（第一次需要时创建）
    wchar_t *tooltip_text;
    FrameStats stats;
    UiWindow *next;
};

static UiWindow *windows;
static UiWindow *creating;      // 正在 CreateWindowEx 里的窗口
static bool theme_listener_added;

// ───────────── 系统版本和窗口外观 ─────────────

static DWORD windows_build(void)
{
    static DWORD build;
    if (build) return build;
    // GetVersionEx 会按清单谎报版本，RtlGetVersion 给的是真的。
    typedef LONG (WINAPI *RtlGetVersionFunction)(OSVERSIONINFOW *);
    OSVERSIONINFOW version = { .dwOSVersionInfoSize = sizeof(version) };
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFunction get_version = ntdll ? (RtlGetVersionFunction)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    build = get_version && get_version(&version) == 0 ? version.dwBuildNumber : 19041;
    return build;
}

bool ui_is_windows_11(void)
{
    return windows_build() >= 22000;
}

static bool supports_system_backdrop(void)
{
    return windows_build() >= 22621;
}

/// 按主题设置窗口深浅色并启用系统材质。材质的深浅在启用时确定，之后只改深浅色不会重绘，
/// 所以先设深浅色，主题切换时（force）先去掉材质再重新启用。
static void apply_appearance(UiWindow *window, bool force)
{
    HWND handle = window->handle;
    BOOL dark = theme_is_dark();
    if (FAILED(DwmSetWindowAttribute(handle, ATTRIBUTE_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        DwmSetWindowAttribute(handle, ATTRIBUTE_USE_IMMERSIVE_DARK_MODE_OLD, &dark, sizeof(dark));

    if (window->options.popup)
    {
        // 显示和隐藏时不播放系统的淡入淡出；Windows 11 上不画圆角和边框，透明窗口里只看得到内容。
        BOOL disabled = TRUE;
        DwmSetWindowAttribute(handle, ATTRIBUTE_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
        if (ui_is_windows_11())
        {
            int corner = CORNER_DO_NOT_ROUND;
            DwmSetWindowAttribute(handle, ATTRIBUTE_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            COLORREF border = BORDER_COLOR_NONE;
            DwmSetWindowAttribute(handle, ATTRIBUTE_BORDER_COLOR, &border, sizeof(border));
        }
        return;
    }

    // 低性能模式下不用材质：半透明的材质要由 Windows 和窗口背后的内容一起合成，用电池时显卡跟不上，动画会卡。
    UiBackdrop wanted = low_performance_enabled() || window->options.backdrop == 0 ? UI_BACKDROP_NONE : window->options.backdrop;
    if (!force && wanted == window->applied_backdrop) return;
    window->applied_backdrop = wanted;
    window->backdrop_active = false;
    if (!supports_system_backdrop()) return;

    int none = UI_BACKDROP_NONE;
    DwmSetWindowAttribute(handle, ATTRIBUTE_SYSTEMBACKDROP_TYPE, &none, sizeof(none));
    MARGINS margins = { 0, 0, 0, 0 };
    if (wanted != UI_BACKDROP_NONE)
    {
        // 整个窗口都当作“玻璃”，材质才会铺满客户区。
        margins = (MARGINS){ -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(handle, &margins);
        int value = wanted;
        window->backdrop_active = SUCCEEDED(DwmSetWindowAttribute(handle, ATTRIBUTE_SYSTEMBACKDROP_TYPE, &value, sizeof(value)));
    }
    else
    {
        DwmExtendFrameIntoClientArea(handle, &margins);
    }
}

// ───────────── 帧耗时和空闲 ─────────────

static void flush_frame_stats(UiWindow *window)
{
    FrameStats *stats = &window->stats;
    if (stats->frames >= 2 && log_trace_enabled())
    {
        const wchar_t *title = window->options.title && window->options.title[0] ? window->options.title :
                               window->options.popup ? L"弹出窗口" : L"窗口";
        log_trace(L"绘制 %ls（%.0f×%.0f）：%d 帧，历时 %.0f 毫秒；每帧平均 %.2f、最长 %.2f、第一帧 %.2f 毫秒；掉帧 %d 次（刷新周期 %.2f 毫秒）",
            title, stats->width, stats->height, stats->frames, (stats->last - stats->began) * 1000, stats->total / stats->frames * 1000,
            stats->longest * 1000, stats->first * 1000, stats->missed, frames_refresh_period() * 1000);
    }
    *stats = (FrameStats){0};
}

/// 画完一帧：began、ended 是开始和画完的时刻（真实时间，不是动画用的显示时刻）。
static void record_frame(UiWindow *window, double began, double ended, float width, float height)
{
    FrameStats *stats = &window->stats;
    if (stats->frames > 0 && began - stats->last > FRAME_BURST_GAP) flush_frame_stats(window);
    double spent = ended - began;
    if (stats->frames == 0)
    {
        stats->began = began;
        stats->first = spent;
    }
    else if (began - stats->last > (low_performance_enabled() ? 2.5 : 1.5) * frames_refresh_period()) stats->missed++;
    stats->frames++;
    stats->total += spent;
    if (spent > stats->longest) stats->longest = spent;
    stats->last = began;
    stats->width = width;
    stats->height = height;
}

static void trace_memory(const wchar_t *moment)
{
    if (!log_trace_enabled()) return;
    PROCESS_MEMORY_COUNTERS_EX counters = { .cb = sizeof(counters) };
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters))) return;
    log_trace(L"内存（%ls）：私有 %.1f MB，工作集 %.1f MB", moment, counters.PrivateUsage / 1048576.0, counters.WorkingSetSize / 1048576.0);
}

static bool idle_check_scheduled;
static bool trimmed;                // 放掉显卡缓存以后还没有再画过
static ULONGLONG last_activity;

static void idle_check(void *data);

static void schedule_idle_check(unsigned milliseconds)
{
    if (idle_check_scheduled) return;
    idle_check_scheduled = true;
    task_delay(milliseconds, idle_check, NULL);
}

/// 画了一帧（drew），或者有窗口隐藏、关闭：空闲从现在起重新计时。
static void note_activity(bool drew)
{
    last_activity = GetTickCount64();
    if (drew) trimmed = false;
    schedule_idle_check(IDLE_TRIM_MILLISECONDS);
}

static bool any_window_showing(void)
{
    for (UiWindow *window = windows; window; window = window->next)
        if (!window->destroyed && window->handle && IsWindowVisible(window->handle) && !IsIconic(window->handle)) return true;
    return false;
}

/// 空闲了一阵：把各窗口还没记下的绘制统计记下；所有窗口都不在显示时（只剩托盘图标），放掉显卡上留着下次用的缓存。
/// 有窗口在显示时不放：放掉以后下一次重画（比如光标移过控件）要把窗口里的字全部重新栅格化，会顿一下。
static void idle_check(void *data)
{
    (void)data;
    idle_check_scheduled = false;
    bool animating = false;
    for (UiWindow *window = windows; window; window = window->next) animating |= window->subscribed;
    ULONGLONG idle = GetTickCount64() - last_activity;
    if (animating || idle < IDLE_TRIM_MILLISECONDS)
    {
        schedule_idle_check(animating ? IDLE_TRIM_MILLISECONDS : (unsigned)(IDLE_TRIM_MILLISECONDS - idle));
        return;
    }
    for (UiWindow *window = windows; window; window = window->next) flush_frame_stats(window);
    if (trimmed || any_window_showing()) return;
    trace_memory(L"放掉显卡缓存之前");
    gfx_trim();
    trimmed = true;
    trace_memory(L"放掉显卡缓存之后");
}

// ───────────── 画 ─────────────

static void on_frame(void *context, double time);

/// 隐藏或最小化的窗口不用逐帧回调（不然后台线程每次刷新都要叫醒界面线程）；再显示时系统会让它重画。
static void stop_frames(UiWindow *window)
{
    if (window->subscribed) frames_unsubscribe(on_frame, window);
    window->subscribed = false;
}

static void render(UiWindow *window, double time)
{
    if (window->rendering || window->animating) return;
    double began = frames_time();
    if (!IsWindowVisible(window->handle))
    {
        stop_frames(window);
        return;
    }
    // animate 里还有动画在进行时会请求下一帧，画的时候不能把这个请求清掉。
    bool animation_requested = false;
    if (window->options.animate)
    {
        // 动画先走到这一帧，窗口的位置和大小定下来以后再按新的大小画。
        window->frame_requested = false;
        window->animating = true;
        window->options.animate(window, window->options.context, time);
        window->animating = false;
        if (window->destroyed || !window->handle) return;
        if (!IsWindowVisible(window->handle))
        {
            stop_frames(window);
            return;
        }
        animation_requested = window->frame_requested;
    }
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        stop_frames(window);
        return;
    }

    window->rendering = true;
    window->frame_requested = animation_requested;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root && window->layout_dirty)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    window->layout_dirty = false;

    if (gfx_begin(window->surface, width, height, dpi))
    {
        bool transparent = window->options.popup || window->backdrop_active;
        gfx_clear(transparent ? COLOR_TRANSPARENT : theme_color(window->options.background));
        if (window->options.paint) window->options.paint(window, window->options.context, time);
        if (window->root) ui_paint(window->root, time);
        // 显卡设备丢失时已经重建，下一帧重画一次。
        if (!gfx_end(window->surface)) window->frame_requested = true;
        record_frame(window, began, frames_time(), width / scale, height / scale);
    }
    window->rendering = false;
    window->stale = false;

    // 还有动画在进行就接着订阅逐帧回调，否则退订，不占用处理器。
    if (window->frame_requested && !window->subscribed) frames_subscribe(on_frame, window);
    else if (!window->frame_requested && window->subscribed) frames_unsubscribe(on_frame, window);
    window->subscribed = window->frame_requested;
    note_activity(true);
}

static void on_frame(void *context, double time)
{
    UiWindow *window = context;
    // 低性能模式下高刷新率的屏幕隔一帧画一次。
    if (low_performance_skip(time)) return;
    render(window, time);
}

void ui_window_request_frame(UiWindow *window)
{
    if (!window || window->destroyed) return;
    window->frame_requested = true;
    // 不在画的过程中调用时（比如刚开始一段动画），直接订阅，下一帧就会来画。
    if (!window->rendering && !window->animating && !window->subscribed)
    {
        frames_subscribe(on_frame, window);
        window->subscribed = true;
    }
}

bool ui_window_is_animating(UiWindow *window)
{
    return window && !window->destroyed && (window->subscribed || window->frame_requested);
}

void ui_window_invalidate(UiWindow *window)
{
    if (!window || !window->handle) return;
    window->layout_dirty = true;
    InvalidateRect(window->handle, NULL, FALSE);
}

void ui_window_repaint(UiWindow *window)
{
    // 排到下一次刷新时画：一次刷新最多画一次，光标快速移过控件时不会每条鼠标消息都整窗重画。
    if (window && window->handle && IsWindowVisible(window->handle)) ui_window_request_frame(window);
}

// ───────────── 鼠标 ─────────────

static void send_mouse(UiElement *element, UiMouseKind kind, const UiMouseEvent *source)
{
    if (!element || !element->type->mouse) return;
    UiMouseEvent event = *source;
    event.kind = kind;
    // 交给元素的是它自己的坐标；光标离开窗口时（-1, -1）不用换算。
    if (event.x >= 0 || event.y >= 0) ui_element_to_local(element, &event.x, &event.y);
    element->type->mouse(element, &event);
}

static void set_hovered(UiWindow *window, UiElement *element, const UiMouseEvent *event)
{
    if (window->hovered == element) return;
    UiElement *previous = window->hovered;
    // 元素带提示时换成它的提示；两边都没有提示时不动，窗口自己设的提示（置物架）照常显示。
    const wchar_t *previous_tip = previous ? ui_element_tooltip(previous) : NULL;
    const wchar_t *next_tip = element ? ui_element_tooltip(element) : NULL;
    if (previous_tip || next_tip) ui_window_set_tooltip(window, next_tip);
    window->hovered = element;
    if (previous)
    {
        previous->hovered = false;
        send_mouse(previous, UI_MOUSE_LEAVE, event);
    }
    // 上面的回调可能改了光标下的元素（比如销毁了它），再确认一次。
    if (window->hovered == element && element)
    {
        element->hovered = true;
        send_mouse(element, UI_MOUSE_ENTER, event);
    }
    ui_window_repaint(window);
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
    // 告诉它不算点成（inside 为 false）。
    UiMouseEvent event = { .kind = UI_MOUSE_UP, .x = -1, .y = -1 };
    send_mouse(captured, UI_MOUSE_UP, &event);
    ui_window_repaint(window);
}

POINT ui_window_to_screen(UiWindow *window, float x, float y)
{
    float scale = ui_window_scale(window);
    POINT point = { (LONG)lroundf(x * scale), (LONG)lroundf(y * scale) };
    if (window && window->handle) ClientToScreen(window->handle, &point);
    return point;
}

void ui_window_from_screen(UiWindow *window, POINT point, float *x, float *y)
{
    if (window && window->handle) ScreenToClient(window->handle, &point);
    float scale = ui_window_scale(window);
    *x = point.x / scale;
    *y = point.y / scale;
}

static UiMouseEvent make_mouse_event(UiWindow *window, UiMouseKind kind, LPARAM lparam)
{
    float scale = ui_window_scale(window);
    UiMouseEvent event = { .kind = kind };
    event.x = GET_X_LPARAM(lparam) / scale;
    event.y = GET_Y_LPARAM(lparam) / scale;
    return event;
}

static UiElement *element_at(UiWindow *window, float x, float y)
{
    return window->root ? ui_hit_test(window->root, x, y) : NULL;
}

/// nonclient：光标在最大化按钮上，系统按非客户区发来的消息，离开时的通知也要按非客户区请求。
static void on_mouse_move(UiWindow *window, LPARAM lparam, bool nonclient)
{
    bool *tracking = nonclient ? &window->tracking_nonclient : &window->tracking_mouse;
    if (!*tracking)
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE | (nonclient ? TME_NONCLIENT : 0), window->handle, 0 };
        *tracking = TrackMouseEvent(&track) != 0;
    }
    UiMouseEvent event = make_mouse_event(window, UI_MOUSE_MOVE, lparam);
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    // 按住鼠标期间只有按下的那个元素算悬停，移出去就不算。
    if (window->captured) under = under == window->captured ? under : NULL;
    set_hovered(window, under, &event);
    send_mouse(window->captured ? window->captured : under, UI_MOUSE_MOVE, &event);
}

static void on_mouse_leave(UiWindow *window)
{
    window->tracking_mouse = false;
    UiMouseEvent event = { .kind = UI_MOUSE_LEAVE, .x = -1, .y = -1 };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    // 从客户区移到最大化按钮上时，离开的通知可能晚于按钮上的移动，这时按钮仍算悬停。
    if (!window->captured && !(window->tracking_nonclient && window->hovered && window->hovered == window->maximize_button))
        set_hovered(window, NULL, &event);
}

/// 光标离开最大化按钮（非客户区），或在非客户区里移到了按钮以外。
static void leave_maximize_button(UiWindow *window)
{
    if (window->captured || !window->hovered || window->hovered != window->maximize_button) return;
    UiMouseEvent event = { .kind = UI_MOUSE_LEAVE, .x = -1, .y = -1 };
    set_hovered(window, NULL, &event);
}

/// 非客户区消息带的是屏幕坐标，换成客户区坐标交给和客户区一样的处理。
static LPARAM client_lparam(HWND handle, LPARAM lparam)
{
    POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    ScreenToClient(handle, &point);
    return MAKELPARAM((WORD)(short)point.x, (WORD)(short)point.y);
}

static void on_mouse_down(UiWindow *window, LPARAM lparam, bool right)
{
    UiMouseEvent event = make_mouse_event(window, UI_MOUSE_DOWN, lparam);
    event.right_button = right;
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    set_hovered(window, under, &event);
    // 点到能接收键盘的元素（或它里面）时把焦点给它，点到别处时谁都没有。
    UiElement *focusable = under;
    while (focusable && !focusable->type->focusable) focusable = focusable->parent;
    if (!right) ui_window_set_focus(window, focusable && !focusable->disabled ? focusable : NULL);
    if (!under || under->disabled || window->hovered != under) return;
    if (!right)
    {
        window->captured = under;
        under->pressed = true;
        SetCapture(window->handle);
    }
    send_mouse(under, UI_MOUSE_DOWN, &event);
    ui_window_repaint(window);
}

static void on_mouse_up(UiWindow *window, LPARAM lparam, bool right)
{
    UiMouseEvent event = make_mouse_event(window, UI_MOUSE_UP, lparam);
    event.right_button = right;
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    if (right)
    {
        event.inside = true;
        if (under && !under->disabled) send_mouse(under, UI_MOUSE_UP, &event);
        return;
    }
    UiElement *captured = window->captured;
    if (!captured) return;
    window->captured = NULL;
    captured->pressed = false;
    ReleaseCapture();
    event.inside = under == captured;
    send_mouse(captured, UI_MOUSE_UP, &event);
    // 点击的处理里窗口可能已经关闭。
    if (!window->handle) return;
    set_hovered(window, element_at(window, event.x, event.y), &event);
    ui_window_repaint(window);
}

void ui_window_layout(UiWindow *window)
{
    if (!window || !window->handle || !window->root || !window->layout_dirty || window->rendering) return;
    RECT client;
    GetClientRect(window->handle, &client);
    float scale = ui_window_scale(window);
    if (client.right <= 0 || client.bottom <= 0) return;
    ui_measure(window->root, client.right / scale);
    ui_arrange(window->root, (GfxRect){ 0, 0, client.right / scale, client.bottom / scale });
    window->layout_dirty = false;
}

void ui_window_update_hover(UiWindow *window)
{
    if (!window || !window->handle || window->captured || window->rendering || window->animating) return;
    POINT cursor;
    GetCursorPos(&cursor);
    float x, y;
    ui_window_from_screen(window, cursor, &x, &y);
    RECT client;
    GetClientRect(window->handle, &client);
    float scale = ui_window_scale(window);
    bool inside = WindowFromPoint(cursor) == window->handle && x >= 0 && y >= 0 && x < client.right / scale && y < client.bottom / scale;
    UiMouseEvent event = { .kind = UI_MOUSE_MOVE, .x = x, .y = y };
    // 排版要是最新的，才找得对光标下的元素。
    if (window->root && window->layout_dirty)
    {
        ui_measure(window->root, client.right / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, client.right / scale, client.bottom / scale });
        window->layout_dirty = false;
    }
    set_hovered(window, inside ? element_at(window, x, y) : NULL, &event);
}

void ui_window_forget(UiWindow *window, UiElement *element)
{
    if (!window) return;
    if (window->focused == element) window->focused = NULL;
    if (window->hovered == element) window->hovered = NULL;
    if (window->maximize_button == element) window->maximize_button = NULL;
    if (window->captured == element)
    {
        window->captured = NULL;
        if (GetCapture() == window->handle) ReleaseCapture();
    }
    if (window->root == element) window->root = NULL;
}

// ───────────── 窗口消息 ─────────────

static int frame_thickness(HWND handle)
{
    UINT dpi = GetDpiForWindow(handle);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

/// 没有系统标题栏的窗口：顶边可以调整大小，顶部一条可以拖动。
static LRESULT hit_test(UiWindow *window, LPARAM lparam)
{
    LRESULT result = DefWindowProcW(window->handle, WM_NCHITTEST, 0, lparam);
    if (result != HTCLIENT) return result;
    POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    ScreenToClient(window->handle, &point);
    RECT client;
    GetClientRect(window->handle, &client);
    if (window->options.resizable && !IsZoomed(window->handle))
    {
        int border = frame_thickness(window->handle);
        if (point.y < border)
        {
            if (point.x < border) return HTTOPLEFT;
            if (point.x >= client.right - border) return HTTOPRIGHT;
            return HTTOP;
        }
    }
    float scale = ui_window_scale(window);
    float x = point.x / scale, y = point.y / scale;
    UiElement *under = element_at(window, x, y);
    if (under && under == window->maximize_button && !under->disabled && ui_is_windows_11()) return HTMAXBUTTON;
    if (y < window->options.caption_height && !under) return HTCAPTION;
    return HTCLIENT;
}

static void destroy_window_state(UiWindow *window)
{
    flush_frame_stats(window);
    // 提示框属于这个窗口，系统随窗口一起销毁。
    window->tooltip = NULL;
    free(window->tooltip_text);
    window->tooltip_text = NULL;
    if (window->subscribed) frames_unsubscribe(on_frame, window);
    window->subscribed = false;
    for (UiWindow **link = &windows; *link; link = &(*link)->next)
    {
        if (*link != window) continue;
        *link = window->next;
        break;
    }
    UiElement *root = window->root;
    window->root = NULL;
    window->hovered = window->captured = window->focused = window->maximize_button = NULL;
    window->handle = NULL;
    if (root) ui_element_destroy(root);
    gfx_surface_destroy(window->surface);
    window->surface = NULL;
    if (creating == window) creating = NULL;
    if (window->options.closed) window->options.closed(window, window->options.context);
    note_activity(false);
    // 点击按钮关掉窗口时，外层还在处理这次点击的消息，等它们都返回了再释放。
    window->destroyed = true;
    if (window->message_depth == 0) free(window);
}

static LRESULT handle_message(UiWindow *window, HWND handle, UINT message, WPARAM wparam, LPARAM lparam);

static LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    UiWindow *window = (UiWindow *)GetWindowLongPtrW(handle, GWLP_USERDATA);
    if (message == WM_NCCREATE)
    {
        window = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        window->handle = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)window);
    }
    if (!window) return DefWindowProcW(handle, message, wparam, lparam);

    window->message_depth++;
    LRESULT result = handle_message(window, handle, message, wparam, lparam);
    if (--window->message_depth == 0 && window->destroyed) free(window);
    return result;
}

static LRESULT handle_message(UiWindow *window, HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    LRESULT hooked = 0;
    if (window->options.message && message != WM_NCDESTROY &&
        window->options.message(window, window->options.context, message, wparam, lparam, &hooked))
        return hooked;
    switch (message)
    {
        case WM_NCACTIVATE:
            // 没有边框的窗口不让系统重画非客户区，否则激活时会闪一下标题栏。
            if (window->options.frameless) return DefWindowProcW(handle, message, wparam, -1);
            break;
        case WM_NCCALCSIZE:
            // 没有边框：整个窗口都是客户区。
            if (window->options.frameless && wparam) return 0;
            if (!window->options.popup && wparam)
            {
                // 去掉系统的标题栏：左、右、下的边框照旧（调整大小用），顶上不留。
                NCCALCSIZE_PARAMS *parameters = (NCCALCSIZE_PARAMS *)lparam;
                LONG top = parameters->rgrc[0].top;
                DefWindowProcW(handle, message, wparam, lparam);
                parameters->rgrc[0].top = top;
                // 最大化时窗口比屏幕大出一圈边框，顶上也要让出来，否则内容被切掉一条。
                if (IsZoomed(handle)) parameters->rgrc[0].top += frame_thickness(handle);
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (window->options.popup || window->options.frameless) return HTCLIENT;
            return hit_test(window, lparam);
        case WM_MOUSEACTIVATE:
            if (window->options.no_activate) return MA_NOACTIVATE;
            break;
        case WM_GETMINMAXINFO:
            if (window->options.minimum_width > 0 || window->options.minimum_height > 0)
            {
                RECT outer, client;
                GetWindowRect(handle, &outer);
                GetClientRect(handle, &client);
                float scale = ui_window_scale(window);
                MINMAXINFO *info = (MINMAXINFO *)lparam;
                info->ptMinTrackSize.x = (LONG)(window->options.minimum_width * scale) + (outer.right - outer.left) - client.right;
                info->ptMinTrackSize.y = (LONG)(window->options.minimum_height * scale) + (outer.bottom - outer.top) - client.bottom;
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(handle, NULL);
            // 有动画在进行时下一次刷新就会画，这里不再多画一次；刚显示、屏幕上还是旧画面时马上画。
            if (!window->subscribed || window->stale) render(window, frames_time());
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (((const WINDOWPOS *)lparam)->flags & SWP_HIDEWINDOW)
            {
                window->stale = true;
                note_activity(false);
            }
            break;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) note_activity(false);
            if (wparam != SIZE_MINIMIZED)
            {
                // 拖动边框调整大小时系统在自己的循环里，直接画，内容才跟得上窗口。
                window->layout_dirty = true;
                render(window, frames_time());
                if (window->options.moved && (wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED)) window->options.moved(window, window->options.context);
            }
            return 0;
        case WM_EXITSIZEMOVE:
            if (window->options.moved) window->options.moved(window, window->options.context);
            return 0;
        case WM_DPICHANGED:
        {
            // 挪到了缩放比例不同的显示器：按系统建议的位置和大小调整。
            const RECT *suggested = (const RECT *)lparam;
            SetWindowPos(handle, NULL, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            ui_window_invalidate(window);
            if (window->options.moved) window->options.moved(window, window->options.context);
            return 0;
        }
        case WM_ACTIVATE:
            window->active = LOWORD(wparam) != WA_INACTIVE;
            // 低性能模式的自动档随电源状态变化，窗口激活时再查一次要不要用材质。
            if (window->active) apply_appearance(window, false);
            ui_window_repaint(window);
            if (window->options.activated) window->options.activated(window, window->options.context, window->active);
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(window, lparam, false);
            return 0;
        case WM_MOUSELEAVE:
            on_mouse_leave(window);
            return 0;
        // 光标在最大化按钮上时（见 hit_test）收到的是非客户区消息，悬停、按下和点击照客户区处理。
        case WM_NCMOUSEMOVE:
            if (wparam == HTMAXBUTTON) on_mouse_move(window, client_lparam(handle, lparam), true);
            else leave_maximize_button(window);
            // 照常交给系统，Windows 11 据此弹出贴靠布局。
            break;
        case WM_NCMOUSELEAVE:
            window->tracking_nonclient = false;
            leave_maximize_button(window);
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (wparam != HTMAXBUTTON) break;
            on_mouse_down(window, client_lparam(handle, lparam), false);
            return 0;
        case WM_NCLBUTTONUP:
            if (wparam != HTMAXBUTTON) break;
            on_mouse_up(window, client_lparam(handle, lparam), false);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            on_mouse_down(window, lparam, false);
            return 0;
        case WM_LBUTTONUP:
            on_mouse_up(window, lparam, false);
            return 0;
        case WM_RBUTTONDOWN:
            on_mouse_down(window, lparam, true);
            return 0;
        case WM_RBUTTONUP:
            on_mouse_up(window, lparam, true);
            return 0;
        case WM_CAPTURECHANGED:
            if (window->captured && (HWND)lparam != handle)
            {
                // 鼠标被别的窗口拿走：当作没有点成。
                UiElement *captured = window->captured;
                UiMouseEvent event = { .kind = UI_MOUSE_UP, .x = -1, .y = -1 };
                window->captured = NULL;
                captured->pressed = false;
                send_mouse(captured, UI_MOUSE_UP, &event);
                ui_window_repaint(window);
            }
            return 0;
        case WM_MOUSEWHEEL:
        {
            POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(handle, &point);
            float scale = ui_window_scale(window);
            UiMouseEvent event = { .kind = UI_MOUSE_WHEEL, .x = point.x / scale, .y = point.y / scale };
            event.wheel = GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA;
            if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return 0;
            // 滚轮交给光标下的元素，它不接收就一级级往上找。先找能接收鼠标的：盖在页面上的透明的一层
            // （设置页下面露出的牌边所在的那层铺满整个窗口）不接收鼠标，滚轮要穿过它交给下面的页面。
            UiElement *under = window->root ? ui_hit_test(window->root, event.x, event.y) : NULL;
            if (!under && window->root) under = ui_hit_test_any(window->root, event.x, event.y);
            for (UiElement *element = under; element; element = element->parent)
            {
                if (!element->type->mouse || !element->type->wheel) continue;
                send_mouse(element, UI_MOUSE_WHEEL, &event);
                break;
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT || LOWORD(lparam) == HTMAXBUTTON)
            {
                if (window->options.cursor)
                {
                    POINT point;
                    GetCursorPos(&point);
                    ScreenToClient(handle, &point);
                    float scale = ui_window_scale(window);
                    LPCWSTR cursor = window->options.cursor(window, window->options.context, point.x / scale, point.y / scale);
                    if (cursor)
                    {
                        SetCursor(LoadCursorW(NULL, cursor));
                        return TRUE;
                    }
                }
                // 能点的控件上显示手形光标，输入框上是文字光标。
                UiElement *hovered = window->hovered;
                bool clickable = hovered && !hovered->disabled;
                LPCWSTR cursor = clickable ? hovered->type->cursor ? hovered->type->cursor : IDC_HAND : IDC_ARROW;
                SetCursor(LoadCursorW(NULL, cursor));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            unsigned key = (unsigned)wparam | (message == WM_SYSKEYDOWN ? UI_KEY_ALT : 0);
            // 先交给有焦点的元素（输入框、快捷键框），它不要再交给窗口。
            UiElement *focused = window->focused;
            if (focused && !focused->hidden && focused->type->key && focused->type->key(focused, key))
            {
                window->swallow_char = message == WM_SYSKEYDOWN;
                return 0;
            }
            if (window->options.key && window->options.key(window, window->options.context, key))
            {
                window->swallow_char = message == WM_SYSKEYDOWN;
                return 0;
            }
            window->swallow_char = false;
            break;
        }
        case WM_CHAR:
            if (window->focused && window->focused->type->character)
            {
                window->focused->type->character(window->focused, (wchar_t)wparam);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (window->swallow_char) return 0;
            break;
        case WM_CLOSE:
            DestroyWindow(handle);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
            destroy_window_state(window);
            return DefWindowProcW(handle, message, wparam, lparam);
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

// ───────────── 创建和放置 ─────────────

/// 界面语言变了：控件上的文字是现取的，重新排版并重画就是新的语言。
static void on_language_changed(void *context)
{
    (void)context;
    for (UiWindow *window = windows; window; window = window->next) ui_window_invalidate(window);
}

static void on_theme_changed(void *context)
{
    (void)context;
    for (UiWindow *window = windows; window; window = window->next)
    {
        apply_appearance(window, true);
        ui_window_invalidate(window);
    }
}

bool ui_window_snapshot(UiWindow *window, const wchar_t *path)
{
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || window->rendering) return false;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    if (!gfx_begin_image(width, height, dpi)) return false;
    window->rendering = true;
    double time = frames_time();
    gfx_clear(window->options.popup ? COLOR_TRANSPARENT : theme_color(window->options.background));
    if (window->options.paint) window->options.paint(window, window->options.context, time);
    if (window->root) ui_paint(window->root, time);
    window->rendering = false;
    return gfx_end_image(path);
}

/// 按窗口现在的大小排版，画进一张图片（gfx_begin_image）；画不了时返回 false。
static bool begin_window_image(UiWindow *window, double *time)
{
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || window->rendering) return false;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    if (!gfx_begin_image(width, height, dpi)) return false;
    *time = frames_time();
    return true;
}

GfxImage *ui_window_capture(UiWindow *window)
{
    double time;
    if (!window || !window->handle || !begin_window_image(window, &time)) return NULL;
    window->rendering = true;
    gfx_clear(window->options.popup ? COLOR_TRANSPARENT : theme_color(window->options.background));
    if (window->options.paint) window->options.paint(window, window->options.context, time);
    if (window->root) ui_paint(window->root, time);
    window->rendering = false;
    return gfx_end_image_capture();
}

void ui_window_warm(UiWindow *window)
{
    if (!window || !window->handle || window->rendering) return;
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    if (!gfx_begin_image(width, height, dpi)) return;
    window->rendering = true;
    double time = frames_time();
    gfx_clear(COLOR_TRANSPARENT);
    if (window->options.paint) window->options.paint(window, window->options.context, time);
    if (window->root) ui_paint(window->root, time);
    window->rendering = false;
    HBITMAP bitmap = gfx_end_image_bitmap();
    if (bitmap) DeleteObject(bitmap);
}

void ui_snapshot_windows(const wchar_t *directory)
{
    directory_create(directory);
    int number = 1;
    for (UiWindow *window = windows; window; window = window->next, number++)
    {
        wchar_t name[32];
        swprintf(name, 32, L"window-%d.png", number);
        wchar_t *path = path_join(directory, name);
        ui_window_snapshot(window, path);
        free(path);
    }
}

void ui_refresh_window_appearance(void)
{
    for (UiWindow *window = windows; window; window = window->next)
    {
        apply_appearance(window, false);
        ui_window_invalidate(window);
    }
}

UiWindow *ui_window_create(const UiWindowOptions *options)
{
    if (!gfx_initialize()) return NULL;
    HINSTANCE instance = GetModuleHandleW(NULL);
    static bool registered;
    if (!registered)
    {
        WNDCLASSEXW class = { .cbSize = sizeof(class) };
        class.style = CS_DBLCLKS;
        class.lpfnWndProc = window_procedure;
        class.hInstance = instance;
        class.hCursor = LoadCursorW(NULL, IDC_ARROW);
        class.lpszClassName = UI_WINDOW_CLASS;
        if (!RegisterClassExW(&class)) return NULL;
        registered = true;
    }
    if (!theme_listener_added)
    {
        theme_add_listener(on_theme_changed, NULL);
        language_add_listener(on_language_changed, NULL);
        theme_listener_added = true;
    }

    UiWindow *window = xcalloc(1, sizeof(UiWindow));
    window->options = *options;
    window->layout_dirty = true;
    window->stale = true;
    DWORD style, extended = WS_EX_NOREDIRECTIONBITMAP;
    if (options->popup)
    {
        style = WS_POPUP;
        extended |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        if (options->no_activate) extended |= WS_EX_NOACTIVATE;
    }
    else if (options->frameless)
    {
        // 带标题栏样式系统才画圆角和阴影；WM_NCCALCSIZE 里把整个窗口都算作客户区，标题栏并不显示。
        style = WS_OVERLAPPED | WS_CAPTION;
        extended |= WS_EX_TOOLWINDOW;
        if (options->no_activate) extended |= WS_EX_NOACTIVATE;
    }
    else
    {
        style = WS_OVERLAPPEDWINDOW;
        if (!options->resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    creating = window;
    HWND handle = CreateWindowExW(extended, UI_WINDOW_CLASS, options->title ? options->title : L"", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, instance, window);
    if (!handle)
    {
        log_win32_error(L"创建窗口失败", GetLastError());
        // 创建到一半失败时窗口已经走过销毁的流程，那里释放过了。
        if (creating == window) free(window);
        creating = NULL;
        return NULL;
    }
    creating = NULL;
    window->surface = gfx_surface_create(handle);
    window->next = windows;
    windows = window;
    if (options->icon)
    {
        SendMessageW(handle, WM_SETICON, ICON_BIG, (LPARAM)options->icon);
        SendMessageW(handle, WM_SETICON, ICON_SMALL, (LPARAM)options->icon);
    }
    apply_appearance(window, true);
    // 让系统按 WM_NCCALCSIZE 的新结果重新算一遍边框。
    SetWindowPos(handle, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (options->width > 0 && options->height > 0)
    {
        RECT outer;
        GetWindowRect(handle, &outer);
        ui_window_place(window, outer.left, outer.top, options->width, options->height);
    }
    return window;
}

HWND ui_window_handle(UiWindow *window)
{
    return window ? window->handle : NULL;
}

void ui_window_set_root(UiWindow *window, UiElement *root)
{
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
    return window->root;
}

void ui_window_show(UiWindow *window)
{
    if (window->options.no_activate)
    {
        ShowWindow(window->handle, SW_SHOWNOACTIVATE);
    }
    else
    {
        if (IsIconic(window->handle)) ShowWindow(window->handle, SW_RESTORE);
        else ShowWindow(window->handle, SW_SHOW);
        SetForegroundWindow(window->handle);
    }
    ui_window_invalidate(window);
}

void ui_window_show_with(UiWindow *window, bool activate)
{
    if (!activate || window->options.no_activate)
    {
        ShowWindow(window->handle, SW_SHOWNOACTIVATE);
    }
    else
    {
        ShowWindow(window->handle, SW_SHOW);
        SetForegroundWindow(window->handle);
    }
    ui_window_invalidate(window);
}

void ui_window_set_topmost(UiWindow *window, bool topmost)
{
    if (!window || !window->handle) return;
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    SetWindowPos(window->handle, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    // 系统有时把这次调整当成次序没变，返回成功却没有置顶（程序不在前台、刚显示过不抢焦点的窗口之后会遇到）：
    // 先排到普通窗口的最前面，再置顶一次。
    if (topmost && !(GetWindowLongW(window->handle, GWL_EXSTYLE) & WS_EX_TOPMOST))
    {
        SetWindowPos(window->handle, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        SetWindowPos(window->handle, HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
}

void ui_window_set_handlers(UiWindow *window, const UiWindowOptions *handlers)
{
    if (!window) return;
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
    if (!window || !window->handle || window->options.backdrop == backdrop) return;
    window->options.backdrop = backdrop;
    apply_appearance(window, true);
    ui_window_repaint(window);
}

void ui_window_set_icon(UiWindow *window, HICON icon)
{
    if (!window || !window->handle || !icon) return;
    window->options.icon = icon;
    SendMessageW(window->handle, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessageW(window->handle, WM_SETICON, ICON_SMALL, (LPARAM)icon);
}

void ui_window_set_border_color(UiWindow *window, const Color *color)
{
    if (!window || !window->handle || !ui_is_windows_11()) return;
    COLORREF value = color ? RGB(color->r, color->g, color->b) : 0xFFFFFFFF;    // DWMWA_COLOR_DEFAULT
    DwmSetWindowAttribute(window->handle, ATTRIBUTE_BORDER_COLOR, &value, sizeof(value));
}

void ui_window_set_maximize_button(UiWindow *window, UiElement *button)
{
    if (window) window->maximize_button = button;
}

void ui_window_begin_move(UiWindow *window)
{
    if (!window || !window->handle) return;
    // 和 WPF 的 DragMove 一样：放开鼠标捕获，假装按在标题栏上，系统接着拖动窗口。
    if (window->captured)
    {
        window->captured->pressed = false;
        window->captured = NULL;
    }
    ReleaseCapture();
    SendMessageW(window->handle, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
}

// ───────────── 提示 ─────────────

/// 整个窗口作为一个提示区域：光标停住一会儿就显示当前的文字，文字换了先收起，光标再停住时显示新的。
static bool ensure_tooltip(UiWindow *window)
{
    if (window->tooltip) return true;
    static bool initialized;
    if (!initialized)
    {
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&controls);
        initialized = true;
    }
    window->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window->handle, NULL, GetModuleHandleW(NULL), NULL);
    if (!window->tooltip) return false;
    TTTOOLINFOW tool = { sizeof(tool) };
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = window->handle;
    tool.uId = (UINT_PTR)window->handle;
    tool.lpszText = L"";
    SendMessageW(window->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
    // 长的说明折成几行，宽度和 WPF 的提示差不多。
    SendMessageW(window->tooltip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)(int)(320 * ui_window_scale(window)));
    return true;
}

void ui_window_set_tooltip(UiWindow *window, const wchar_t *text)
{
    if (!window || !window->handle || wcs_equal(window->tooltip_text, text)) return;
    free(window->tooltip_text);
    window->tooltip_text = wcs_dup(text);
    if (!text || !*text)
    {
        if (window->tooltip)
        {
            SendMessageW(window->tooltip, TTM_POP, 0, 0);
            SendMessageW(window->tooltip, TTM_ACTIVATE, FALSE, 0);
        }
        return;
    }
    if (!ensure_tooltip(window)) return;
    SendMessageW(window->tooltip, TTM_POP, 0, 0);
    TTTOOLINFOW tool = { sizeof(tool) };
    tool.hwnd = window->handle;
    tool.uId = (UINT_PTR)window->handle;
    tool.lpszText = window->tooltip_text;
    SendMessageW(window->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
    SendMessageW(window->tooltip, TTM_ACTIVATE, TRUE, 0);
}

void ui_window_close(UiWindow *window)
{
    if (window && window->handle) DestroyWindow(window->handle);
}

void ui_close_all_windows(void)
{
    while (windows) DestroyWindow(windows->handle);
}

float ui_window_scale(UiWindow *window)
{
    UINT dpi = window && window->handle ? GetDpiForWindow(window->handle) : 96;
    return dpi > 0 ? dpi / 96.0f : 1.0f;
}

GfxSize ui_window_size(UiWindow *window)
{
    RECT client;
    GetClientRect(window->handle, &client);
    float scale = ui_window_scale(window);
    return (GfxSize){ client.right / scale, client.bottom / scale };
}

bool ui_window_is_active(UiWindow *window)
{
    return window->active;
}

bool ui_window_has_backdrop(UiWindow *window)
{
    return window->backdrop_active;
}

void ui_window_place(UiWindow *window, int x, int y, float width, float height)
{
    HWND handle = window->handle;
    if (width <= 0 || height <= 0)
    {
        SetWindowPos(handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    // 先挪过去，窗口才知道自己在哪个显示器上、缩放比例是多少；再按那里的比例定大小。
    SetWindowPos(handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT outer, client;
    GetWindowRect(handle, &outer);
    GetClientRect(handle, &client);
    float scale = ui_window_scale(window);
    int frame_width = (outer.right - outer.left) - client.right;
    int frame_height = (outer.bottom - outer.top) - client.bottom;
    SetWindowPos(handle, NULL, 0, 0, (int)(width * scale + 0.5f) + frame_width, (int)(height * scale + 0.5f) + frame_height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ui_window_center(UiWindow *window)
{
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    // 先挪到那个显示器上，让大小按它的缩放比例调整好，再居中。
    GfxSize size = ui_window_size(window);
    ui_window_place(window, monitor.rcWork.left, monitor.rcWork.top, size.width, size.height);
    RECT outer;
    GetWindowRect(window->handle, &outer);
    int x = monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - (outer.right - outer.left)) / 2;
    int y = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - (outer.bottom - outer.top)) / 2;
    SetWindowPos(window->handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
