#include "shelf_peek.h"
#include "app.h"
#include "card_face.h"
#include "shelf_window.h"
#include "strings.h"
#include "task.h"
#include "theme.h"
#include "thumbnail.h"
#include "ui.h"

#include <math.h>
#ifdef _WIN32
#include <shellapi.h>
#include <shellscalingapi.h>
#endif
#include <stdlib.h>

#define PI_F 3.14159265f
// 尺寸都是 DIP。
// 简洁：一个小置物架斜着藏在屏幕边缘后面，只露出一个圆角；悬停时转正、往外伸，露出 HOVER_OUT 宽的一条。
#define SHELF_WIDTH 104.0f
#define SHELF_HEIGHT 88.0f
#define RADIUS 12.0f
#define REST_ANGLE 32.0f            // 平时斜多少度
#define REST_OUT 36.0f              // 平时露出的圆角离屏幕边缘多远
#define HOVER_OUT 50.0f
#define CORNER_INSET 15.5f          // 平时图标的中心离露出的那个角（没有圆角时的尖角）多远
#define ICON_SIZE 22.0f
#define MARGIN 14.0f                // 窗口比露出的部分大一圈，留出阴影
// 纸牌：探出来的是一张竖版的牌，斜着露出靠里的那一截；窗口比露出的部分大一圈，留出阴影、倾斜和悬停时多探出的地方。
#define CARD_WIDTH 60.0f
#define CARD_HEIGHT 84.0f
#define CARD_RADIUS 5.0f
#define PEEK_WIDTH 38.0f            // 探出时露出多宽
#define CARD_HOVER_EXTRA 10.0f
#define TILT 6.0f                   // 牌斜着探出，露出的那个角更突出；悬停时再斜一点
#define HOVER_TILT 3.0f
#define CARD_MARGIN 18.0f
#define WINDOW_WIDTH (theme_is_cards() ? PEEK_WIDTH + CARD_HOVER_EXTRA + CARD_MARGIN : HOVER_OUT + MARGIN)
#define WINDOW_HEIGHT (theme_is_cards() ? CARD_HEIGHT + 2 * CARD_MARGIN : SHELF_HEIGHT + 2 * MARGIN)
#define ICON_LOAD_SIZE 28.0f
#define HIDE_TIMER 1
#define BUSY_RETRY 2000             // 全屏或演示时每隔这么久看一次
#define RESUME_MINIMUM 1500         // 光标移开后至少再留这么久，不会一移开就没了

typedef struct Peek
{
    UiWindow *window;
    StrList paths;              // 角里的文件
    ScreenEdge edge;
    bool lower;                 // 露出下面的角（否则是上面的角）
    bool shown;                 // 探出着（包括正在探出），缩回和点开后为 false
    bool leaving;               // 正在缩回
    bool opening;               // 点开了，置物架正转出来，角淡出
    bool hovering, pressed;
    unsigned order;             // 越大越新
    ULONGLONG deadline;
    DWORD remaining;
    Anim slide, hover, badge, fade;
    GfxImage *icon;
    unsigned icon_generation;
    FileSuit suit;              // 纸牌：角里的文件数量最多的类型，印在牌角上
} Peek;

static Peek peeks[SHELF_PEEK_MAXIMUM];
static unsigned next_order;
static StrList waiting;             // 全屏时攒着、还没探出的
static bool waiting_scheduled;
static bool ignore_busy;

static void shelf_peek_open_one(Peek *peek);

void shelf_peek_ignore_busy(bool ignore)
{
    ignore_busy = ignore;
}

static Peek *newest_shown(void)
{
    Peek *newest = NULL;
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
        if (peeks[index].shown && (!newest || peeks[index].order > newest->order)) newest = &peeks[index];
    return newest;
}

bool shelf_peek_is_shown(void)
{
    return newest_shown() != NULL;
}

int shelf_peek_corner_count(void)
{
    int count = 0;
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++) count += peeks[index].shown;
    return count;
}

int shelf_peek_count(void)
{
    int count = 0;
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
        if (peeks[index].shown) count += peeks[index].paths.count;
    return count;
}

ScreenEdge shelf_peek_edge(void)
{
    Peek *newest = newest_shown();
    return newest ? newest->edge : EDGE_RIGHT;
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

int shelf_peek_stack(int top, int height, RECT work, const int *busy, int busy_count)
{
    // 候选：top 本身，和紧贴每一段上面、下面的位置；取不重叠、不超出工作区、离 top 最近的，一样近时取下面的。
    int best = top, best_distance = -1;
    for (int index = -1; index < 2 * busy_count; index++)
    {
        int candidate = index < 0 ? top : index % 2 ? busy[index] : busy[index] - height;
        if (candidate < work.top || candidate + height > work.bottom) continue;
        bool free_here = true;
        for (int other = 0; other < busy_count && free_here; other++)
            free_here = candidate >= busy[2 * other + 1] || candidate + height <= busy[2 * other];
        int distance = abs(candidate - top);
        if (!free_here || (best_distance >= 0 && (distance > best_distance || (distance == best_distance && candidate < best)))) continue;
        best = candidate;
        best_distance = distance;
    }
    return best;
}

static unsigned next_random(void)
{
    static unsigned state;
    if (!state)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        state = (unsigned)now.QuadPart | 1;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// ───────────── 画 ─────────────

typedef struct Pose
{
    float x, y, angle;          // 小置物架（或牌）的中心（窗口里的 DIP）和转角（度，顺时针为正）
} Pose;

/// 简洁：小置物架现在的位置。先按贴右边、露出下面的角算，再按实际的边和角镜像。
static Pose shelf_pose(const Peek *peek, GfxSize size)
{
    float a = SHELF_WIDTH / 2, b = SHELF_HEIGHT / 2, r = RADIUS;
    float c = cosf(REST_ANGLE * PI_F / 180), s = sinf(REST_ANGLE * PI_F / 180);
    float edge_x = size.width, middle = size.height / 2;
    // 平时：转过 REST_ANGLE，左下的圆角露出 REST_OUT；露出的三角形（按没有圆角算）上下居中。
    float sharp_out = REST_OUT + r * (c + s - 1);
    float corner_y = middle + sharp_out * (c / s - s / c) / 2;
    float rest_x = edge_x - REST_OUT + r + (a - r) * c + (b - r) * s, rest_y = corner_y + a * s - b * c;
    // 悬停：转正，露出 HOVER_OUT 宽的一条，上下居中。中间那几帧露出来的是梯形。
    float hover_x = edge_x - HOVER_OUT + a;
    float t = (float)peek->hover.value;
    Pose pose = { rest_x + (hover_x - rest_x) * t, rest_y + (middle - rest_y) * t, REST_ANGLE * (1 - t) };
    // 探出和缩回：整个往屏幕外退。
    pose.x += (1 - (float)peek->slide.value) * (REST_OUT + (HOVER_OUT - REST_OUT) * t + MARGIN);
    if (!peek->lower)
    {
        pose.y = size.height - pose.y;
        pose.angle = -pose.angle;
    }
    if (peek->edge == EDGE_LEFT)
    {
        pose.x = size.width - pose.x;
        pose.angle = -pose.angle;
    }
    return pose;
}

/// 纸牌：牌的中心放在屏幕边缘外面，露出靠里的那一截。
static Pose card_pose(const Peek *peek, GfxSize size)
{
    float visible = (float)peek->slide.value * PEEK_WIDTH + (float)peek->hover.value * CARD_HOVER_EXTRA;
    float tilt = (TILT + (float)peek->hover.value * HOVER_TILT) * (peek->edge == EDGE_LEFT ? 1 : -1) * (peek->lower ? -1 : 1);
    return (Pose){ peek->edge == EDGE_LEFT ? visible - CARD_WIDTH / 2 : size.width - visible + CARD_WIDTH / 2, size.height / 2, tilt };
}

static GfxMatrix pose_matrix(Pose pose)
{
    return gfx_matrix_multiply(gfx_matrix_rotate(pose.angle, 0, 0), gfx_matrix_translate(pose.x, pose.y));
}

static void count_text(const Peek *peek, wchar_t *text, size_t size)
{
    if (peek->paths.count > 99) wcscpy(text, L"99+");
    else swprintf(text, size, L"%d", peek->paths.count);
}

static void draw_fitted(GfxImage *image, float center_x, float center_y, float box)
{
    float width = (float)gfx_image_width(image), height = (float)gfx_image_height(image);
    float fit = width > 0 && height > 0 ? fminf(box / width, box / height) : 1;
    width *= fit;
    height *= fit;
    gfx_draw_image(image, (GfxRect){ center_x - width / 2, center_y - height / 2, center_x + width / 2, center_y + height / 2 }, 1);
}

/// 简洁：数量（两个以上才显示），正着写，跟着置物架走。
static void paint_badge(const Peek *peek, float center_x, float center_y)
{
    if (peek->paths.count < 2) return;
    wchar_t text[8];
    count_text(peek, text, ARRAYSIZE(text));
    GfxTextStyle style = { GFX_FONT_TEXT, 11, 600, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER, 0 };
    float width = fmaxf(18, ceilf(gfx_text_measure(text, &style, 0).width) + 10);
    float scale = (float)peek->badge.value;
    gfx_push_transform(gfx_matrix_scale(scale, scale, center_x, center_y));
    GfxRect pill = { center_x - width / 2, center_y - 9, center_x + width / 2, center_y + 9 };
    gfx_fill_rounded(pill, 9, theme_color(THEME_ACCENT));
    gfx_text_draw(text, pill, &style, theme_color(THEME_TEXT_ON_ACCENT));
    gfx_pop_transform();
}

static void paint_shelf_corner(const Peek *peek, GfxSize size)
{
    Pose pose = shelf_pose(peek, size);
    GfxMatrix matrix = pose_matrix(pose);
    gfx_push_transform(matrix);
    GfxRect rect = { -SHELF_WIDTH / 2, -SHELF_HEIGHT / 2, SHELF_WIDTH / 2, SHELF_HEIGHT / 2 };
    gfx_shadow(rect, RADIUS, 12, 0, 2, COLOR_ARGB(70, 0, 0, 0));
    Color fill = theme_color(THEME_SHELF_SOLID);
    Color tint = theme_color(THEME_SUBTLE_HOVER);
    fill = color_over(color_with_alpha(tint, (uint8_t)(tint.a * peek->hover.value)), fill);
    gfx_fill_rounded(rect, RADIUS, fill);
    gfx_stroke_rounded(rect, RADIUS, theme_color(THEME_FLYOUT_BORDER), 1);
    // 图标：平时在露出来的那个圆角里，转正后在露出来的那一条中间。先按贴右边、露出下面的角算，再镜像。
    float t = (float)peek->hover.value;
    float x = -SHELF_WIDTH / 2 + CORNER_INSET + (HOVER_OUT / 2 - CORNER_INSET) * t, y = (SHELF_HEIGHT / 2 - CORNER_INSET) * (1 - t);
    // 数量在图标靠露出的那条长边的一侧：平时三角形沿着这条边伸得最长。
    float badge_x = x - 3, badge_y = y - 20;
    if (!peek->lower) y = -y, badge_y = -badge_y;
    if (peek->edge == EDGE_LEFT) x = -x, badge_x = -badge_x;
    if (peek->icon) draw_fitted(peek->icon, x, y, ICON_SIZE);
    else
    {
        GfxTextStyle style = { GFX_FONT_ICON, 18, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER, 0 };
        gfx_text_draw(L"\xE896", (GfxRect){ x - 14, y - 14, x + 14, y + 14 }, &style, theme_color(THEME_ACCENT_TEXT));
    }
    gfx_pop_transform();
    gfx_matrix_apply(matrix, &badge_x, &badge_y);
    paint_badge(peek, badge_x, badge_y);
}

/// 牌面：中间是最新那个文件的缩略图（露出来多少算多少），没有缩略图时是文档图标。
static void paint_card_picture(const Peek *peek, GfxRect card)
{
    GfxRect area = { card.left + 7, card.top + 20, card.right - 7, card.bottom - 16 };
    if (peek->icon)
    {
        draw_fitted(peek->icon, (area.left + area.right) / 2, (area.top + area.bottom) / 2, fminf(area.right - area.left, area.bottom - area.top));
        return;
    }
    GfxTextStyle style = { GFX_FONT_ICON, 26, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER, 0 };
    gfx_text_draw(L"\xE8A5", area, &style, theme_color(THEME_TEXT_SECONDARY));
}

/// 牌角印在露出来的那一侧：张数在上、花色在下。来了新文件时数字从下面滚上来。
static void paint_card_corner(const Peek *peek, GfxRect card)
{
    wchar_t rank[8];
    count_text(peek, rank, ARRAYSIZE(rank));
    static const CardCorner corner = { 15, 15, 6, 2 };
    float left = peek->edge == EDGE_LEFT ? card.right - 5 - card_corner_size(rank, &corner).width : card.left + 5;
    float roll = (float)peek->badge.value;
    card_paint_corner_rolling(rank, peek->suit, &corner, left, card.top + 4, theme_color(THEME_TEXT_PRIMARY), 1, (1 - roll) * 10, roll);
}

static void paint_card(const Peek *peek, GfxSize size)
{
    gfx_push_transform(pose_matrix(card_pose(peek, size)));
    GfxRect card = { -CARD_WIDTH / 2, -CARD_HEIGHT / 2, CARD_WIDTH / 2, CARD_HEIGHT / 2 };
    gfx_shadow(card, CARD_RADIUS, 12, 0, 3, COLOR_ARGB(70, 0, 0, 0));
    gfx_fill_rounded(card, CARD_RADIUS, theme_color(THEME_SHELF_TILE));
    gfx_stroke_rounded(card, CARD_RADIUS, theme_color(THEME_SHELF_TILE_BORDER), 1.2f);
    gfx_push_clip(card);
    paint_card_picture(peek, card);
    paint_card_corner(peek, card);
    gfx_pop_clip();
    gfx_pop_transform();
}

static void peek_paint(UiWindow *target, void *context, double time)
{
    (void)time;
    Peek *peek = context;
    float opacity = (float)(fmin(1.0, peek->slide.value * 2) * peek->fade.value);
    if (opacity <= 0.001f) return;
    GfxSize size = ui_window_size(target);
    gfx_push_opacity(opacity);
    if (theme_is_cards()) paint_card(peek, size);
    else paint_shelf_corner(peek, size);
    gfx_pop_layer();
}

// ───────────── 计时 ─────────────

static void start_timer(Peek *peek, DWORD milliseconds)
{
    if (!peek->window) return;
    peek->deadline = GetTickCount64() + milliseconds;
    SetTimer(ui_window_handle(peek->window), HIDE_TIMER, milliseconds, NULL);
}

static DWORD stay_milliseconds(void)
{
    return (DWORD)app_settings()->watch_peek_seconds * 1000;
}

static void forget_icon(Peek *peek)
{
    peek->icon_generation++;
    if (peek->icon) gfx_image_release(peek->icon);
    peek->icon = NULL;
}

/// 缩略图读完时用：低 3 位是第几个角，其余是读的时候那个角的 icon_generation。
static void icon_loaded(GfxImage *image, void *context)
{
    uintptr_t value = (uintptr_t)context;
    Peek *peek = &peeks[value & 7];
    if ((unsigned)(value >> 3) != (peek->icon_generation & 0x1FFFFFFF) || !peek->window)
    {
        if (image) gfx_image_release(image);
        return;
    }
    if (peek->icon) gfx_image_release(peek->icon);
    peek->icon = image;
    ui_window_repaint(peek->window);
}

static void load_icon(Peek *peek)
{
    forget_icon(peek);
    if (peek->paths.count == 0 || !peek->window) return;
    int size = (int)ceilf(ICON_LOAD_SIZE * ui_window_scale(peek->window));
    uintptr_t context = ((uintptr_t)(peek->icon_generation & 0x1FFFFFFF) << 3) | (uintptr_t)(peek - peeks);
    thumbnail_load(peek->paths.items[peek->paths.count - 1], size, icon_loaded, (void *)context);
}

static void update_tooltip(Peek *peek)
{
    if (!peek->window) return;
    wchar_t *text = str_watch_peek_tooltip(peek->paths.count);
    ui_window_set_tooltip(peek->window, text);
    free(text);
}

static void hide_now(Peek *peek)
{
    if (!peek->window) return;
    KillTimer(ui_window_handle(peek->window), HIDE_TIMER);
    ui_window_set_tooltip(peek->window, NULL);
    ShowWindow(ui_window_handle(peek->window), SW_HIDE);
    anim_set(&peek->slide, 0);
    anim_set(&peek->hover, 0);
    anim_set(&peek->fade, 1);
    peek->shown = peek->leaving = peek->opening = false;
    peek->hovering = peek->pressed = false;
    strlist_clear(&peek->paths);
    forget_icon(peek);
}

static void dismiss(Peek *peek)
{
    if (!peek->shown) return;
    peek->shown = false;
    peek->leaving = true;
    KillTimer(ui_window_handle(peek->window), HIDE_TIMER);
    ui_window_set_tooltip(peek->window, NULL);
    anim_start(&peek->hover, 0, 0.16, EASE_OF(EASE_CUBIC_OUT));
    anim_start(&peek->slide, 0, 0.22, EASE_OF(EASE_CUBIC_IN));
    if (!anim_enabled()) hide_now(peek);
    else ui_window_request_frame(peek->window);
}

void shelf_peek_dismiss(void)
{
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++) dismiss(&peeks[index]);
}

// ───────────── 窗口 ─────────────

static void peek_animate(UiWindow *target, void *context, double time)
{
    Peek *peek = context;
    bool running = anim_update(&peek->slide, time) | anim_update(&peek->hover, time) | anim_update(&peek->badge, time) |
                   anim_update(&peek->fade, time);
    if (running) ui_window_request_frame(target);
    else if (peek->leaving || peek->opening) hide_now(peek);
}

static bool peek_mouse(UiWindow *target, void *context, const UiMouseEvent *event)
{
    (void)target;
    Peek *peek = context;
    if (!peek->shown) return true;
    switch (event->kind)
    {
        case UI_MOUSE_MOVE:
            if (!peek->hovering)
            {
                // 停在上面时转正、往外伸，计时先停下。
                peek->hovering = true;
                ULONGLONG now = GetTickCount64();
                peek->remaining = peek->deadline > now ? (DWORD)(peek->deadline - now) : 0;
                KillTimer(ui_window_handle(peek->window), HIDE_TIMER);
                anim_start(&peek->hover, 1, theme_is_cards() ? 0.18 : 0.26, EASE_OF(EASE_CUBIC_OUT));
                ui_window_request_frame(peek->window);
            }
            break;
        case UI_MOUSE_LEAVE:
            peek->hovering = peek->pressed = false;
            anim_start(&peek->hover, 0, theme_is_cards() ? 0.18 : 0.26, EASE_OF(EASE_CUBIC_OUT));
            start_timer(peek, peek->remaining > RESUME_MINIMUM ? peek->remaining : RESUME_MINIMUM);
            ui_window_request_frame(peek->window);
            break;
        case UI_MOUSE_DOWN:
            peek->pressed = true;
            break;
        case UI_MOUSE_UP:
            if (peek->pressed && event->right_button) dismiss(peek);
            else if (peek->pressed) shelf_peek_open_one(peek);
            peek->pressed = false;
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
    (void)lparam;
    Peek *peek = context;
    if (message != WM_TIMER || wparam != HIDE_TIMER) return false;
    KillTimer(ui_window_handle(peek->window), HIDE_TIMER);
    if (!peek->hovering) dismiss(peek);
    *result = 0;
    return true;
}

static void peek_closed(UiWindow *target, void *context)
{
    (void)target;
    Peek *peek = context;
    peek->window = NULL;
    peek->shown = peek->leaving = peek->opening = peek->hovering = peek->pressed = false;
    strlist_clear(&peek->paths);
    forget_icon(peek);
}

static bool ensure_window(Peek *peek)
{
    if (peek->window) return true;
    UiWindowOptions options = { 0 };
    options.title = L"DropShelf";
    options.popup = true;
    options.no_activate = true;
    options.width = WINDOW_WIDTH;
    options.height = WINDOW_HEIGHT;
    options.context = peek;
    options.paint = peek_paint;
    options.mouse = peek_mouse;
    options.animate = peek_animate;
    options.cursor = peek_cursor;
    options.message = peek_message;
    options.closed = peek_closed;
    peek->window = ui_window_create(&options);
    return peek->window != NULL;
}

/// 全屏游戏、演示、锁屏时不打扰。
static bool user_is_busy(void)
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (ignore_busy || FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_NOT_PRESENT || state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE ||
           state == QUNS_APP;
}

static double scale_of(HMONITOR monitor)
{
    UINT dpi_x = 96, dpi_y = 96;
    return GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x > 0 ? dpi_x / 96.0 : 1.0;
}

/// 新探出的角放在哪里：已经有角探出着时贴同一条边，否则贴光标那一侧；和光标一样高，被别的角占着时挪到最近的空处。
static void place(Peek *peek)
{
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    Peek *anchor = newest_shown();
    RECT anchor_rect = {0};
    POINT probe = cursor;
    if (anchor && GetWindowRect(ui_window_handle(anchor->window), &anchor_rect))
    {
        probe = (POINT){ anchor_rect.left + (anchor_rect.right - anchor_rect.left) / 2, anchor_rect.top + (anchor_rect.bottom - anchor_rect.top) / 2 };
        // 光标在同一块屏幕上时还是和光标一样高。
        if (MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST) == MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST)) probe.y = cursor.y;
    }
    else anchor = NULL;
    HMONITOR monitor = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    double scale = scale_of(monitor);
    int width = (int)lround(WINDOW_WIDTH * scale), height = (int)lround(WINDOW_HEIGHT * scale);
    POINT at = shelf_peek_place(probe, info.rcWork, width, height, &peek->edge);
    if (anchor)
    {
        peek->edge = anchor->edge;
        at.x = anchor->edge == EDGE_LEFT ? info.rcWork.left : info.rcWork.right - width;
    }
    int busy[2 * SHELF_PEEK_MAXIMUM], busy_count = 0;
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
    {
        Peek *other = &peeks[index];
        RECT rect;
        if (other == peek || !other->shown || other->edge != peek->edge || !GetWindowRect(ui_window_handle(other->window), &rect)) continue;
        if (MonitorFromPoint((POINT){ rect.left, rect.top }, MONITOR_DEFAULTTONEAREST) != monitor) continue;
        busy[2 * busy_count] = rect.top;
        busy[2 * busy_count++ + 1] = rect.bottom;
    }
    at.y = shelf_peek_stack(at.y, height, info.rcWork, busy, busy_count);
    ui_window_place(peek->window, at.x, at.y, WINDOW_WIDTH, WINDOW_HEIGHT);
}

static void show(Peek *peek)
{
    if (!ensure_window(peek)) return;
    bool was_shown = peek->shown;
    if (!was_shown)
    {
        anim_set(&peek->slide, 0);
        anim_set(&peek->hover, 0);
        anim_set(&peek->fade, 1);
        peek->hovering = peek->pressed = false;
        peek->lower = next_random() & 1;
        place(peek);    // 先放好再算作探出，免得把自己当成已经探出的角来对齐
        ui_window_set_topmost(peek->window, true);
        ui_window_show_with(peek->window, false);
    }
    peek->shown = true;
    peek->order = ++next_order;
    anim_start(&peek->slide, 1, 0.34, EASE_BACK_OF(0.35));
    // 新来的文件让数字跳一下（简洁从第二个起才有数字，第一次出现时弹出来）；纸牌是牌角的数字从下面滚上来。
    if (was_shown && theme_is_cards())
    {
        anim_set(&peek->badge, 0);
        anim_start(&peek->badge, 1, 0.3, EASE_OF(EASE_CUBIC_OUT));
    }
    else if (was_shown)
    {
        anim_set(&peek->badge, peek->paths.count == 2 ? 0 : 1.3);
        anim_start(&peek->badge, 1, 0.3, EASE_BACK_OF(0.4));
    }
    else anim_set(&peek->badge, 1);
    if (peek->hovering) peek->remaining = stay_milliseconds();
    else start_timer(peek, stay_milliseconds());
    update_tooltip(peek);
    load_icon(peek);
    ui_window_repaint(peek->window);
    ui_window_request_frame(peek->window);
}

static void add_to(Peek *peek, const StrList *added)
{
    int before = peek->paths.count;
    for (int index = 0; index < added->count; index++)
        if (strlist_index_of(&peek->paths, added->items[index], true) < 0) strlist_add(&peek->paths, added->items[index]);
    if (peek->paths.count == 0 || (peek->shown && peek->paths.count == before)) return;
    FileSuit *suits = xalloc((size_t)peek->paths.count * sizeof(FileSuit));
    for (int index = 0; index < peek->paths.count; index++) suits[index] = file_suit_of_path(peek->paths.items[index]);
    peek->suit = file_suit_dominant(suits, peek->paths.count);
    free(suits);
    show(peek);
}

/// 空着的角：先找没在用的，再找正在缩回的（直接收起、清空，重新探出）。
static Peek *free_peek(void)
{
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
        if (!peeks[index].shown && !peeks[index].leaving && !peeks[index].opening) return &peeks[index];
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
        if (peeks[index].leaving)
        {
            hide_now(&peeks[index]);
            return &peeks[index];
        }
    return NULL;
}

static bool already_shown(const wchar_t *path)
{
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
        if (peeks[index].shown && strlist_index_of(&peeks[index].paths, path, true) >= 0) return true;
    return false;
}

static void deliver(const StrList *added)
{
    if (!app_settings()->watch_separate_shelves)
    {
        Peek *peek = newest_shown();
        if (!peek) peek = free_peek();
        if (peek) add_to(peek, added);
        return;
    }
    // 每个文件一个角；角都用上了，再来的并进最新的那个。
    for (int index = 0; index < added->count; index++)
    {
        if (already_shown(added->items[index])) continue;
        Peek *peek = shelf_peek_corner_count() < SHELF_PEEK_MAXIMUM ? free_peek() : NULL;
        if (!peek) peek = newest_shown();
        if (!peek) continue;
        StrList one = {0};
        strlist_add(&one, added->items[index]);
        add_to(peek, &one);
        strlist_clear(&one);
    }
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
    deliver(&ready);
    strlist_clear(&ready);
}

void shelf_peek_add(const StrList *added)
{
    if (!added || added->count == 0) return;
    // 已经有角探出着就直接加；都没探出时先看用户是不是在全屏。
    if (!shelf_peek_is_shown() && user_is_busy())
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
    deliver(added);
}

/// 角现在的样子（屏幕像素的中心、DIP 的大小和转角），置物架从这里转出来。
static ShelfPopOrigin pop_origin(Peek *peek)
{
    RECT rect = {0};
    GetWindowRect(ui_window_handle(peek->window), &rect);
    GfxSize size = ui_window_size(peek->window);
    float scale = ui_window_scale(peek->window);
    bool cards = theme_is_cards();
    Pose pose = cards ? card_pose(peek, size) : shelf_pose(peek, size);
    ShelfPopOrigin origin = { { rect.left + (LONG)lroundf(pose.x * scale), rect.top + (LONG)lroundf(pose.y * scale) },
        cards ? CARD_WIDTH : SHELF_WIDTH, cards ? CARD_HEIGHT : SHELF_HEIGHT, pose.angle };
    return origin;
}

static void shelf_peek_open_one(Peek *peek)
{
    if (!peek->shown || !peek->window || peek->paths.count == 0) return;
    ShelfPopOrigin origin = pop_origin(peek);
    StrList opened = {0};
    strlist_clone(&opened, &peek->paths);
    // 角原地淡出，同时置物架从它那里转出来。
    peek->shown = false;
    peek->opening = true;
    KillTimer(ui_window_handle(peek->window), HIDE_TIMER);
    ui_window_set_tooltip(peek->window, NULL);
    anim_start(&peek->fade, 0, 0.14, EASE_OF(EASE_CUBIC_OUT));
    if (!anim_enabled()) hide_now(peek);
    else ui_window_request_frame(peek->window);
    shelves_open_from_peek(peek->edge, &origin, &opened);
    strlist_clear(&opened);
}

bool shelf_peek_open(void)
{
    Peek *peek = newest_shown();
    if (!peek) return false;
    shelf_peek_open_one(peek);
    return true;
}

void shelf_peek_close(void)
{
    strlist_clear(&waiting);
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
    {
        Peek *peek = &peeks[index];
        if (peek->window) ui_window_close(peek->window);
        peek->window = NULL;
        peek->shown = peek->leaving = peek->opening = false;
        strlist_clear(&peek->paths);
        forget_icon(peek);
    }
}

void shelf_peek_apply_look(void)
{
    for (int index = 0; index < SHELF_PEEK_MAXIMUM; index++)
    {
        Peek *peek = &peeks[index];
        if (!peek->window) continue;
        if (peek->shown)
        {
            RECT rect;
            GetWindowRect(ui_window_handle(peek->window), &rect);
            float scale = ui_window_scale(peek->window);
            int width = (int)lround(WINDOW_WIDTH * scale), height = (int)lround(WINDOW_HEIGHT * scale);
            int x = peek->edge == EDGE_LEFT ? rect.left : rect.right - width;
            int y = (rect.top + rect.bottom) / 2 - height / 2;
            ui_window_place(peek->window, x, y, WINDOW_WIDTH, WINDOW_HEIGHT);
        }
        ui_window_repaint(peek->window);
    }
}
