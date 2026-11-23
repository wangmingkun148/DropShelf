#include "shelf_peek.h"
#include "app.h"
#include "shelf_window.h"
#include "strings.h"
#include "task.h"
#include "theme.h"
#include "thumbnail.h"
#include "ui.h"

#include <math.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <stdlib.h>

// 尺寸都是 DIP。窗口比角大一圈，留出阴影和悬停时多伸出的地方。
#define TAB_WIDTH 44.0f
#define TAB_HEIGHT 72.0f
#define HOVER_EXTRA 8.0f
#define MARGIN 14.0f
#define RADIUS 10.0f
#define WINDOW_WIDTH (TAB_WIDTH + HOVER_EXTRA + MARGIN)
#define WINDOW_HEIGHT (TAB_HEIGHT + 2 * MARGIN)
#define ICON_SIZE 28.0f
#define HIDE_TIMER 1
#define BUSY_RETRY 2000             // 全屏或演示时每隔这么久看一次
#define RESUME_MINIMUM 1500         // 光标移开后至少再留这么久，不会一移开就没了

static UiWindow *window;
static StrList paths;               // 角里的文件
static StrList waiting;             // 全屏时攒着、还没探出的
static bool waiting_scheduled;
static ScreenEdge edge = EDGE_RIGHT;
static bool shown;                  // 探出着（包括正在探出），leaving 时为 false
static bool leaving;
static bool hovering, pressed;
static ULONGLONG deadline;
static DWORD remaining;
static Anim slide, hover, badge;
static GfxImage *icon;
static unsigned icon_generation;
static bool ignore_busy;

void shelf_peek_ignore_busy(bool ignore)
{
    ignore_busy = ignore;
}

bool shelf_peek_is_shown(void)
{
    return shown;
}

int shelf_peek_count(void)
{
    return shown ? paths.count : 0;
}

ScreenEdge shelf_peek_edge(void)
{
    return edge;
}

POINT shelf_peek_place(POINT cursor, RECT work, int width, int height, ScreenEdge *side)
{
    ScreenEdge chosen = cursor.x < work.left + (work.right - work.left) / 2 ? EDGE_LEFT : EDGE_RIGHT;
    if (side) *side = chosen;
    LONG top = cursor.y - height / 2;
    if (top > work.bottom - height) top = work.bottom - height;
    if (top < work.top) top = work.top;
    return (POINT){ chosen == EDGE_LEFT ? work.left : work.right - width, top };
}

// ───────────── 画 ─────────────

static float visible_width(void)
{
    return (float)slide.value * TAB_WIDTH + (float)hover.value * HOVER_EXTRA;
}

/// 角的外框：贴边的一侧伸到窗口外面，看起来是从屏幕边缘后面探出来的。
static GfxRect tab_rect(GfxSize size)
{
    float visible = visible_width();
    float top = (size.height - TAB_HEIGHT) / 2;
    if (edge == EDGE_LEFT) return (GfxRect){ -RADIUS - 2, top, visible, top + TAB_HEIGHT };
    return (GfxRect){ size.width - visible, top, size.width + RADIUS + 2, top + TAB_HEIGHT };
}

static void paint_icon(GfxRect tab)
{
    // 图标在露出来的那一截里居中，跟着角一起滑。
    float inner = edge == EDGE_LEFT ? tab.right : tab.left;
    float center_x = edge == EDGE_LEFT ? inner - TAB_WIDTH / 2 + 1 : inner + TAB_WIDTH / 2 - 1;
    float center_y = (tab.top + tab.bottom) / 2 + 5;
    if (icon)
    {
        float width = (float)gfx_image_width(icon), height = (float)gfx_image_height(icon);
        float fit = width > 0 && height > 0 ? fminf(ICON_SIZE / width, ICON_SIZE / height) : 1;
        width *= fit;
        height *= fit;
        gfx_draw_image(icon, (GfxRect){ center_x - width / 2, center_y - height / 2, center_x + width / 2, center_y + height / 2 }, 1);
        return;
    }
    GfxTextStyle style = { GFX_FONT_ICON, 20, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER, 0 };
    gfx_text_draw(L"\xE896", (GfxRect){ center_x - 16, center_y - 16, center_x + 16, center_y + 16 }, &style, theme_color(THEME_ACCENT_TEXT));
}

static void paint_badge(GfxRect tab)
{
    wchar_t text[8];
    if (paths.count > 99) wcscpy(text, L"99+");
    else swprintf(text, ARRAYSIZE(text), L"%d", paths.count);
    GfxTextStyle style = { GFX_FONT_TEXT, 11, 600, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER, 0 };
    float width = fmaxf(18, ceilf(gfx_text_measure(text, &style, 0).width) + 10);
    float inner = edge == EDGE_LEFT ? tab.right : tab.left;
    float center_x = edge == EDGE_LEFT ? inner - width / 2 - 5 : inner + width / 2 + 5;
    float center_y = tab.top + 14;
    float scale = (float)badge.value;
    gfx_push_transform(gfx_matrix_scale(scale, scale, center_x, center_y));
    GfxRect pill = { center_x - width / 2, center_y - 9, center_x + width / 2, center_y + 9 };
    gfx_fill_rounded(pill, 9, theme_color(THEME_ACCENT));
    gfx_text_draw(text, pill, &style, theme_color(THEME_TEXT_ON_ACCENT));
    gfx_pop_transform();
}

static void peek_paint(UiWindow *target, void *context, double time)
{
    (void)context;
    (void)time;
    if (slide.value <= 0.001) return;
    GfxSize size = ui_window_size(target);
    GfxRect tab = tab_rect(size);
    float opacity = (float)fmin(1.0, slide.value * 2);
    gfx_push_opacity(opacity);
    gfx_shadow(tab, RADIUS, 12, 0, 2, COLOR_ARGB(70, 0, 0, 0));
    Color fill = theme_color(THEME_SHELF_SOLID);
    fill = color_over(color_with_alpha(theme_color(THEME_SUBTLE_HOVER), (uint8_t)(theme_color(THEME_SUBTLE_HOVER).a * hover.value)), fill);
    gfx_fill_rounded(tab, RADIUS, fill);
    gfx_stroke_rounded(tab, RADIUS, theme_color(THEME_FLYOUT_BORDER), 1);
    paint_icon(tab);
    paint_badge(tab);
    gfx_pop_layer();
}

// ───────────── 计时 ─────────────

static void start_timer(DWORD milliseconds)
{
    if (!window) return;
    deadline = GetTickCount64() + milliseconds;
    SetTimer(ui_window_handle(window), HIDE_TIMER, milliseconds, NULL);
}

static DWORD stay_milliseconds(void)
{
    return (DWORD)app_settings()->watch_peek_seconds * 1000;
}

static void forget_icon(void)
{
    icon_generation++;
    if (icon) gfx_image_release(icon);
    icon = NULL;
}

static void icon_loaded(GfxImage *image, void *context)
{
    if ((unsigned)(uintptr_t)context != icon_generation || !window)
    {
        if (image) gfx_image_release(image);
        return;
    }
    if (icon) gfx_image_release(icon);
    icon = image;
    ui_window_repaint(window);
}

static void load_icon(void)
{
    forget_icon();
    if (paths.count == 0 || !window) return;
    int size = (int)ceilf(ICON_SIZE * ui_window_scale(window));
    thumbnail_load(paths.items[paths.count - 1], size, icon_loaded, (void *)(uintptr_t)icon_generation);
}

static void update_tooltip(void)
{
    if (!window) return;
    wchar_t *text = str_watch_peek_tooltip(paths.count);
    ui_window_set_tooltip(window, text);
    free(text);
}

static void hide_now(void)
{
    if (!window) return;
    KillTimer(ui_window_handle(window), HIDE_TIMER);
    ui_window_set_tooltip(window, NULL);
    ShowWindow(ui_window_handle(window), SW_HIDE);
    anim_set(&slide, 0);
    anim_set(&hover, 0);
    leaving = false;
    hovering = pressed = false;
}

void shelf_peek_dismiss(void)
{
    if (!shown) return;
    shown = false;
    leaving = true;
    strlist_clear(&paths);
    forget_icon();
    KillTimer(ui_window_handle(window), HIDE_TIMER);
    ui_window_set_tooltip(window, NULL);
    anim_start(&hover, 0, 0.16, EASE_OF(EASE_CUBIC_OUT));
    anim_start(&slide, 0, 0.22, EASE_OF(EASE_CUBIC_IN));
    if (!anim_enabled()) hide_now();
    else ui_window_request_frame(window);
}

// ───────────── 窗口 ─────────────

static void peek_animate(UiWindow *target, void *context, double time)
{
    (void)context;
    bool running = anim_update(&slide, time) | anim_update(&hover, time) | anim_update(&badge, time);
    if (running) ui_window_request_frame(target);
    else if (leaving) hide_now();
}

static bool peek_mouse(UiWindow *target, void *context, const UiMouseEvent *event)
{
    (void)target;
    (void)context;
    if (!shown) return true;
    switch (event->kind)
    {
        case UI_MOUSE_MOVE:
            if (!hovering)
            {
                // 停在上面时再往外伸一点，计时先停下。
                hovering = true;
                ULONGLONG now = GetTickCount64();
                remaining = deadline > now ? (DWORD)(deadline - now) : 0;
                KillTimer(ui_window_handle(window), HIDE_TIMER);
                anim_start(&hover, 1, 0.18, EASE_OF(EASE_CUBIC_OUT));
                ui_window_request_frame(window);
            }
            break;
        case UI_MOUSE_LEAVE:
            hovering = pressed = false;
            anim_start(&hover, 0, 0.18, EASE_OF(EASE_CUBIC_OUT));
            start_timer(remaining > RESUME_MINIMUM ? remaining : RESUME_MINIMUM);
            ui_window_request_frame(window);
            break;
        case UI_MOUSE_DOWN:
            pressed = true;
            break;
        case UI_MOUSE_UP:
            if (pressed && event->right_button) shelf_peek_dismiss();
            else if (pressed) shelf_peek_open();
            pressed = false;
            break;
        default:
            break;
    }
    return true;
}

static LPCWSTR peek_cursor(UiWindow *target, void *context, float x, float y)
{
    (void)target;
    (void)context;
    (void)x;
    (void)y;
    return IDC_HAND;
}

static bool peek_message(UiWindow *target, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    (void)target;
    (void)context;
    (void)lparam;
    if (message != WM_TIMER || wparam != HIDE_TIMER) return false;
    KillTimer(ui_window_handle(window), HIDE_TIMER);
    if (!hovering) shelf_peek_dismiss();
    *result = 0;
    return true;
}

static void peek_closed(UiWindow *target, void *context)
{
    (void)target;
    (void)context;
    window = NULL;
    shown = leaving = hovering = pressed = false;
    strlist_clear(&paths);
    forget_icon();
}

static bool ensure_window(void)
{
    if (window) return true;
    UiWindowOptions options = { 0 };
    options.title = L"DropShelf";
    options.popup = true;
    options.no_activate = true;
    options.width = WINDOW_WIDTH;
    options.height = WINDOW_HEIGHT;
    options.paint = peek_paint;
    options.mouse = peek_mouse;
    options.animate = peek_animate;
    options.cursor = peek_cursor;
    options.message = peek_message;
    options.closed = peek_closed;
    window = ui_window_create(&options);
    return window != NULL;
}

/// 全屏游戏、演示、锁屏时不打扰。
static bool user_is_busy(void)
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (ignore_busy || FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_NOT_PRESENT || state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE ||
           state == QUNS_APP;
}

static void show(void)
{
    if (!ensure_window()) return;
    bool was_shown = shown;
    shown = true;
    if (!was_shown && !leaving)
    {
        // 从光标那一侧的边缘探出来，和光标一样高。
        POINT cursor = { 0, 0 };
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { .cbSize = sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        UINT dpi_x = 96, dpi_y = 96;
        double scale = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x > 0 ? dpi_x / 96.0 : 1.0;
        int width = (int)lround(WINDOW_WIDTH * scale), height = (int)lround(WINDOW_HEIGHT * scale);
        POINT place = shelf_peek_place(cursor, info.rcWork, width, height, &edge);
        anim_set(&slide, 0);
        anim_set(&hover, 0);
        hovering = pressed = false;
        ui_window_place(window, place.x, place.y, WINDOW_WIDTH, WINDOW_HEIGHT);
        ui_window_set_topmost(window, true);
        ui_window_show_with(window, false);
    }
    leaving = false;
    anim_start(&slide, 1, 0.34, EASE_BACK_OF(0.35));
    // 新来的文件让数字跳一下。
    if (was_shown)
    {
        anim_set(&badge, 1.3);
        anim_start(&badge, 1, 0.3, EASE_BACK_OF(0.4));
    }
    else anim_set(&badge, 1);
    if (hovering) remaining = stay_milliseconds();
    else start_timer(stay_milliseconds());
    update_tooltip();
    load_icon();
    ui_window_repaint(window);
    ui_window_request_frame(window);
}

static void add_to_corner(const StrList *added)
{
    for (int index = 0; index < added->count; index++)
        if (strlist_index_of(&paths, added->items[index], true) < 0) strlist_add(&paths, added->items[index]);
    if (paths.count > 0) show();
}

static void retry_waiting(void *data)
{
    (void)data;
    waiting_scheduled = false;
    if (waiting.count == 0) return;
    if (user_is_busy())
    {
        waiting_scheduled = true;
        task_delay(BUSY_RETRY, retry_waiting, NULL);
        return;
    }
    StrList ready = waiting;
    waiting = (StrList){0};
    add_to_corner(&ready);
    strlist_clear(&ready);
}

void shelf_peek_add(const StrList *added)
{
    if (!added || added->count == 0) return;
    // 已经探出着就直接加进去；没探出时先看用户是不是在全屏。
    if (!shown && user_is_busy())
    {
        for (int index = 0; index < added->count; index++)
            if (strlist_index_of(&waiting, added->items[index], true) < 0) strlist_add(&waiting, added->items[index]);
        if (!waiting_scheduled)
        {
            waiting_scheduled = true;
            task_delay(BUSY_RETRY, retry_waiting, NULL);
        }
        return;
    }
    add_to_corner(added);
}

bool shelf_peek_open(void)
{
    if (!shown || !window || paths.count == 0) return false;
    RECT rect;
    GetWindowRect(ui_window_handle(window), &rect);
    // 置物架从角所在的那条边伸出来，和角一样高。
    POINT center = { edge == EDGE_LEFT ? rect.left : rect.right - 1, rect.top + (rect.bottom - rect.top) / 2 };
    StrList opened = paths;
    paths = (StrList){0};
    shown = false;
    forget_icon();
    hide_now();
    shelves_open_with_paths(center, edge, &opened);
    strlist_clear(&opened);
    return true;
}

void shelf_peek_close(void)
{
    strlist_clear(&waiting);
    if (window) ui_window_close(window);
    window = NULL;
    shown = leaving = false;
    strlist_clear(&paths);
    forget_icon();
}
