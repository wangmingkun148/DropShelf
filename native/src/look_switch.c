// 换风格：先让各部分按原来的风格收尾，换好颜色，再按新风格重建。有动画效果时，主窗口、欢迎引导和开着的置物架
// 像一串多米诺一样依次翻成新风格：每个窗口上盖一张牌（shelf_card_flip），正面是截下的旧样子；牌都盖好以后才真正换风格，
// 截下新样子贴到牌背；离光标最近的那张先翻，后面的等前一张翻到一半再翻。窗口在牌下面隐身，牌翻完再现身。
#include "look_switch.h"
#include "about_window.h"
#include "anim.h"
#include "main_window.h"
#include "onboarding_window.h"
#include "shelf_card.h"
#include "shelf_peek.h"
#include "shelf_window_internal.h"
#include "shortcut_icons.h"
#include "tray.h"

#include <math.h>
#include <stdlib.h>

#define FLIP_MILLISECONDS 520.0
/// 第一张先停一下：牌都盖好、新样子截好之后再翻。
#define FIRST_DELAY 140.0
/// 后一张等前一张翻到这么多再开始。
#define DOMINO_STEP 0.42
/// 窗口再多，整串也在这么久之内翻完。
#define MAXIMUM_SPREAD 1100.0
#define MAXIMUM_FLIPS 32

typedef struct Flip
{
    UiWindow *ui;
    HWND handle;
    ShelfWindow *shelf;         // 置物架的一份引用；主窗口和欢迎引导是 NULL
    ShelfCard *card;
    RECT from;
    double delay;
} Flip;

static Flip flips[MAXIMUM_FLIPS];
static int flip_count, shown_count, finished_count;
static bool switching;
static Look target_look, queued_look;
static bool has_queued;

/// 不播动画，直接换。
static void apply(Look look)
{
    // 原来的风格先收尾：置物架的动画走到终点，主窗口的内容拆掉（两种风格各有一份代码，各自清掉自己的东西）。
    shelves_leave_look();
    main_window_prepare_look();
    theme_set_look(look);
    // 新风格接手：置物架按新的大小重新排版，主窗口重建并回到原来那一页，其余窗口换上新的材质和图标。
    shelves_adopt_look();
    main_window_apply_look();
    shelf_peek_apply_look();
    onboarding_window_apply_look();
    about_window_apply_look();
    tray_refresh_icon();
    shortcut_icons_sync(look);
}

/// 窗口的内容区在屏幕上的位置（截图就是这一块）。
static bool client_on_screen(HWND handle, RECT *rect)
{
    RECT client;
    if (!handle || !GetClientRect(handle, &client) || client.right <= 0 || client.bottom <= 0) return false;
    POINT origin = { 0, 0 };
    ClientToScreen(handle, &origin);
    *rect = (RECT){ origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
    return true;
}

static double dpi_scale_of(HWND handle)
{
    UINT dpi = GetDpiForWindow(handle);
    return dpi > 0 ? dpi / 96.0 : 1.0;
}

static void add_flip(UiWindow *ui, ShelfWindow *shelf)
{
    HWND handle = ui_window_handle(ui);
    if (flip_count >= MAXIMUM_FLIPS || !handle || !IsWindowVisible(handle) || IsIconic(handle)) return;
    Flip *flip = &flips[flip_count];
    if (!client_on_screen(handle, &flip->from)) return;
    *flip = (Flip){ ui, handle, shelf_window_retain(shelf), NULL, flip->from, 0 };
    flip_count++;
}

static double distance_to(const RECT *rect, POINT point)
{
    double x = (rect->left + rect->right) / 2.0 - point.x, y = (rect->top + rect->bottom) / 2.0 - point.y;
    return sqrt(x * x + y * y);
}

static int by_delay(const void *a, const void *b)
{
    double first = ((const Flip *)a)->delay, second = ((const Flip *)b)->delay;
    return first < second ? -1 : first > second;
}

static void finish_switch(void)
{
    for (int index = 0; index < flip_count; index++) shelf_window_release(flips[index].shelf);
    flip_count = shown_count = finished_count = 0;
    switching = false;
    if (has_queued)
    {
        has_queued = false;
        look_switch_to(queued_look);
    }
}

/// 牌盖着的窗口还在（盖牌的这两帧里可能被关掉）。
static bool still_open(const Flip *flip)
{
    if (flip->shelf) return !flip->shelf->closed && flip->shelf->ui == flip->ui;
    return flip->ui == main_window_ui() || flip->ui == onboarding_window_ui();
}

/// 牌都盖好了：真正换风格，截下每个窗口的新样子贴到牌背。
static void apply_under_cards(void)
{
    apply(target_look);
    for (int index = 0; index < flip_count; index++)
    {
        Flip *flip = &flips[index];
        if (!still_open(flip)) continue;
        RECT to = flip->from;
        client_on_screen(flip->handle, &to);
        GfxImage *back = ui_window_capture(flip->ui);
        shelf_card_set_back(flip->card, back, to, dpi_scale_of(flip->handle));
        gfx_image_release(back);
    }
}

static void on_card_shown(void *context)
{
    Flip *flip = context;
    window_set_cloaked(flip->handle, true);
    if (++shown_count == flip_count) apply_under_cards();
}

static void on_card_finished(void *context)
{
    Flip *flip = context;
    window_set_cloaked(flip->handle, false);
    if (++finished_count == flip_count) finish_switch();
}

void look_switch_to(Look look)
{
    if (switching)
    {
        // 正在翻：翻完再换到最后选的那种。
        queued_look = look;
        has_queued = look != target_look;
        return;
    }
    if (look == theme_look()) return;
    if (!anim_enabled() || low_performance_enabled())
    {
        apply(look);
        return;
    }
    flip_count = shown_count = finished_count = 0;
    if (main_window_is_open()) add_flip(main_window_ui(), NULL);
    if (onboarding_window_ui()) add_flip(onboarding_window_ui(), NULL);
    for (ShelfWindow *shelf = shelves_first(); shelf; shelf = shelf->next)
        if (!shelf->closing) add_flip(shelf->ui, shelf);
    if (flip_count == 0)
    {
        apply(look);
        return;
    }

    // 离光标最近的先翻（一般就是刚点了“风格”的主窗口），后面的依次跟上。
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    for (int index = 0; index < flip_count; index++) flips[index].delay = distance_to(&flips[index].from, cursor);
    qsort(flips, (size_t)flip_count, sizeof(Flip), by_delay);
    double step = flip_count > 1 ? fmin(FLIP_MILLISECONDS * DOMINO_STEP, MAXIMUM_SPREAD / (flip_count - 1)) : 0;
    switching = true;
    target_look = look;
    has_queued = false;
    int playing = 0;
    for (int index = 0; index < flip_count; index++)
    {
        Flip *flip = &flips[playing];
        *flip = flips[index];
        flip->delay = FIRST_DELAY + step * playing;
        GfxImage *front = ui_window_capture(flip->ui);
        flip->card = front ? shelf_card_rent() : NULL;
        if (!flip->card)
        {
            gfx_image_release(front);
            shelf_window_release(flip->shelf);
            continue;
        }
        playing++;
        shelf_card_flip(flip->card, flip->from, dpi_scale_of(flip->handle), front, flip->delay, FLIP_MILLISECONDS, on_card_shown, on_card_finished, flip);
        gfx_image_release(front);
    }
    flip_count = playing;
    if (flip_count == 0)
    {
        switching = false;
        apply(look);
    }
}

bool look_switch_busy(void)
{
    return switching;
}
