// 主窗口：标题栏、各层的排版、左侧状态栏、键盘和窗口位置。各页的内容见 main_pages.c，翻牌见 main_flip.c。
#include "main_window_internal.h"
#include "about_window.h"
#include "lang.h"
#include "power.h"
#include "shelf_history.h"
#include "shelf_window.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

#ifndef DROPSHELF_VERSION
#define DROPSHELF_VERSION L"0.0.0"
#endif

#define APP_ICON_RESOURCE 1

// 内容区宽到放得下状态栏和完整页面时显示；两个阈值之间不切换，拖动窗口边缘时不会反复出现、消失。
#define SIDE_PANEL_SHOW_WIDTH 1260.0f
#define SIDE_PANEL_HIDE_WIDTH 1160.0f

#define GLYPH_MINIMIZE L"\xE921"
#define GLYPH_MAXIMIZE L"\xE922"
#define GLYPH_RESTORE L"\xE923"

MainShell main_shell;

static UiElement *minimize_button, *maximize_button, *close_button;
static UiElement *status_element;
static GfxImage *app_icon;
static bool side_panel_wanted;
static bool side_panel_pending;     // 排版时发现要切换状态栏，下一帧再切
static float last_viewport;

// ───────────── 状态栏和页面标题的动画 ─────────────

typedef struct SidePanelState
{
    Anim opacity;           // 整个状态栏
    Anim shift;             // 状态栏的横向位置：收起时在左边 -24
    Anim page_shift;        // 页面横向让出位置
    Anim body_shift;        // 概览页标题行收起或出现时，下面的内容补上
    Anim title_x, title_y;  // 飞行中的“概览”标题的位移
    Anim title_scale;
    Anim title_opacity;
    bool title_on_panel;    // 飞的是状态栏里的标题（否则是概览页里的）
    Anim open_opacity;      // 概览页右上角的按钮
    Anim ghost_opacity, ghost_shift;    // 状态栏出现时右上角按钮留在原处淡出的样子
    GfxRect ghost_rect;
    // 状态栏顶部的页面标题
    Anim icon_opacity, icon_scale;
    Anim back_opacity;
    Anim name_opacity, name_shift;
    int header_page;
    bool panel_title_visible;
} SidePanelState;

static SidePanelState panel;

static UiElement *panel_main;       // 图标、标题、说明、按钮和呼出方式
static UiElement *panel_icon;
static UiElement *panel_title_label, *panel_body_label;
static UiElement *panel_ways[3];    // 晃动、边缘、快捷键
static UiElement *panel_keys;       // 快捷键的几个按键
static UiElement *panel_back;       // 返回概览的胶囊按钮
static UiElement *panel_name;       // 设置页的名称

StringId main_page_title(int page)
{
    switch (page)
    {
        case PAGE_GENERAL: return STR_General;
        case PAGE_SHELF: return STR_GesturesAndShelf;
        case PAGE_ACTIONS: return STR_QuickActionsTitle;
        case PAGE_APPEARANCE: return STR_Appearance;
        default: return STR_Overview;
    }
}

const wchar_t *main_page_glyph(int page)
{
    switch (page)
    {
        case PAGE_GENERAL: return GLYPH_SETTINGS;
        case PAGE_SHELF: return GLYPH_MOUSE;
        case PAGE_ACTIONS: return GLYPH_LIGHTNING;
        default: return GLYPH_COLOR;
    }
}

void main_request_frame(void)
{
    ui_window_request_frame(M->window);
}

void main_repaint(void)
{
    ui_window_repaint(M->window);
}

void main_relayout(void)
{
    ui_window_invalidate(M->window);
}

static float window_width(void)
{
    return M->root ? M->root->bounds.right : 0;
}

void main_content_columns(float *left, float *right)
{
    float available = window_width() - M->page_left - PAGE_MARGIN;
    float width = available < PAGE_MAX_WIDTH ? available : PAGE_MAX_WIDTH;
    *left = M->page_left + (available - width) / 2;
    *right = *left + width;
}

GfxRect main_sheet_area(void)
{
    float left = M->page_left - SHEET_PADDING;
    float available = window_width() - left - (PAGE_MARGIN - SHEET_PADDING);
    float width = available < SHEET_MAX_WIDTH ? available : SHEET_MAX_WIDTH;
    float x = left + (available - width) / 2;
    return (GfxRect){ x, M->stage.top + SHEET_TOP, x + width, M->stage.bottom - STACK_ROOM };
}

ShelfWindow *main_open_shelf(void)
{
    HWND handle = ui_window_handle(M->window);
    if (handle && IsWindowVisible(handle) && !IsIconic(handle)) return shelves_open_next_to(handle, true);
    return shelves_open_at_cursor(true);
}

// ───────────── 状态栏顶部的页面标题 ─────────────

static Ease roll_ease(void) { return EASE_BACK_OF(0.35); }
static Ease pop_ease(void) { return EASE_BACK_OF(0.5); }

/// 从 from 走到 to；延迟期间停在 from（和 C# 版的 Header 一样）。
static void header_anim(Anim *anim, double from, double to, Ease ease, double milliseconds, double delay)
{
    anim_set(anim, from);
    anim_start_delayed(anim, to, (milliseconds > 0 ? milliseconds : 340) / 1000.0, delay / 1000.0, ease);
}

/// “概览”标题在状态栏里的位置（字号 28，左上角），以及返回链接里“概览”的位置（字号 15）。
static void title_positions(float *title_x, float *title_y, float *crumb_x, float *crumb_y)
{
    GfxTextStyle title = { GFX_FONT_TEXT, 28, 600 };
    GfxTextStyle crumb = { GFX_FONT_TEXT, 15, 400 };
    GfxTextStyle glyph = { GFX_FONT_ICON, 13, 400 };
    float title_height = gfx_text_measure(S(STR_Overview), &title, 0).height;
    float crumb_height = gfx_text_measure(S(STR_Overview), &crumb, 0).height;
    float glyph_width = gfx_text_measure(GLYPH_BACK, &glyph, 0).width;
    *title_x = 98;
    *title_y = M->stage.top + 30 + (52 - title_height) / 2;
    // 返回按钮：状态栏左边距 30，按钮往左伸出 10，内边距 10，图标和文字之间隔 8。
    *crumb_x = 30 - 10 + 10 + glyph_width + 8;
    *crumb_y = M->stage.top + 20 + 5 + (crumb_height < 22 ? (22 - crumb_height) / 2 : 0);
}

/// 状态栏显示时，设置页的标题放在状态栏顶部：打开设置页时图标缩小消失，“概览”缩小飞进上方的返回链接，
/// 页面名称从上往下滚出；返回时页面名称继续向下滚走，“概览”从返回链接放大飞回，图标弹回。
void main_set_panel_header(int page, bool animate)
{
    bool on_overview = page == PAGE_OVERVIEW;
    // 从一个设置页抽到另一个设置页：返回链接和图标都不动，只换页名。
    bool swap = !on_overview && panel.header_page != PAGE_OVERVIEW;
    panel.header_page = page;
    if (!on_overview) ui_label_set_text(panel_name, UI_STR(main_page_title(page)));
    panel_back->hit_through = on_overview;

    // 先设好最终状态。
    anim_set(&panel.icon_opacity, on_overview ? 1 : 0);
    anim_set(&panel.icon_scale, 1);
    anim_set(&panel.back_opacity, on_overview ? 0 : 1);
    anim_set(&panel.name_opacity, on_overview ? 0 : 1);
    anim_set(&panel.name_shift, 0);
    if (!panel.title_on_panel || !anim_enabled() || !animate || !M->side_panel_shown)
    {
        anim_set(&panel.title_x, 0);
        anim_set(&panel.title_y, 0);
        anim_set(&panel.title_scale, 1);
        anim_set(&panel.title_opacity, 1);
    }
    panel.panel_title_visible = M->side_panel_shown && on_overview;
    main_request_frame();
    if (!animate || !M->side_panel_shown || !anim_enabled()) return;
    if (swap)
    {
        header_anim(&panel.name_shift, -40, 0, roll_ease(), 380, 0);
        header_anim(&panel.name_opacity, 0, 1, MOTION_EASE, 200, 0);
        return;
    }

    float title_x, title_y, crumb_x, crumb_y;
    title_positions(&title_x, &title_y, &crumb_x, &crumb_y);
    float dx = crumb_x - title_x, dy = crumb_y - title_y, ratio = 15.0f / 28.0f;
    panel.title_on_panel = true;
    panel.panel_title_visible = true;
    if (on_overview)
    {
        header_anim(&panel.title_x, dx, 0, MOTION_EASE, 0, 0);
        header_anim(&panel.title_y, dy, 0, MOTION_EASE, 0, 0);
        header_anim(&panel.title_scale, ratio, 1, MOTION_EASE, 0, 0);
        header_anim(&panel.name_shift, 0, 40, EASE_OF(EASE_CUBIC_IN), 200, 0);
        header_anim(&panel.name_opacity, 1, 0, EASE_OF(EASE_CUBIC_IN), 200, 0);
        header_anim(&panel.back_opacity, 1, 0, EASE_OF(EASE_CUBIC_IN), 120, 0);
        header_anim(&panel.title_opacity, 0, 1, MOTION_EASE, 140, 0);
        header_anim(&panel.icon_opacity, 0, 1, MOTION_EASE, 200, 120);
        header_anim(&panel.icon_scale, 0.5, 1, pop_ease(), 380, 120);
        return;
    }
    header_anim(&panel.icon_opacity, 1, 0, EASE_OF(EASE_CUBIC_IN), 180, 0);
    header_anim(&panel.icon_scale, 1, 0.5, EASE_OF(EASE_CUBIC_IN), 180, 0);
    header_anim(&panel.back_opacity, 0, 1, MOTION_EASE, 160, 220);
    header_anim(&panel.name_shift, -40, 0, roll_ease(), 380, 120);
    header_anim(&panel.name_opacity, 0, 1, MOTION_EASE, 200, 120);
    // “概览”飞到返回链接处淡出，之后收起。
    header_anim(&panel.title_x, 0, dx, MOTION_EASE, 0, 0);
    header_anim(&panel.title_y, 0, dy, MOTION_EASE, 0, 0);
    header_anim(&panel.title_scale, 1, ratio, MOTION_EASE, 0, 0);
    header_anim(&panel.title_opacity, 1, 0, MOTION_EASE, 120, 220);
}

// ───────────── 左侧状态栏的出现和收起 ─────────────

static void start_motion(Anim *anim, double from, double to)
{
    anim_set(anim, from);
    anim_start(anim, to, MOTION_SECONDS, MOTION_EASE);
}

/// 先记下各元素现在看到的位置，再一次性改成新布局，然后让它们从原位置滑到新位置：
/// 页面横向让出状态栏，概览标题飞到状态栏图标右侧（或飞回），下面的内容补上标题让出的空间。
static void set_side_panel(bool animate)
{
    bool show = side_panel_wanted;
    if (show == M->side_panel_shown) return;
    flip_cancel();
    bool on_overview = M->page == PAGE_OVERVIEW;
    float title_x, title_y, crumb_x, crumb_y;
    title_positions(&title_x, &title_y, &crumb_x, &crumb_y);
    float content_left, content_right;
    main_content_columns(&content_left, &content_right);
    // 概览页里的标题：页面内容左上角。
    float page_title_x = content_left, page_title_y = M->stage.top + PAGE_TOP - ui_scroll_offset(M->overview_scroll);
    float header_height = M->overview_header->desired.height + M->overview_header->margin_bottom;
    GfxRect open_rect = M->overview_header->child_count > 1 ? M->overview_header->children[1]->bounds : (GfxRect){ 0 };
    float panel_opacity = M->side_panel->hidden ? 0 : (float)panel.opacity.value;
    float panel_shift = M->side_panel->hidden ? -24 : (float)panel.shift.value;
    float before_left = M->page_left;

    M->side_panel_shown = show;
    M->page_left = PAGE_MARGIN + (show ? SIDE_PANEL_WIDTH : 0);
    M->overview_header->hidden = show;
    for (int page = 0; page < PAGE_COUNT; page++) M->pages[page].header->hidden = show;
    M->side_panel->hidden = false;
    M->side_panel->hit_through = !show;
    panel.panel_title_visible = show && on_overview;
    panel.title_on_panel = show;
    main_relayout();

    if (!animate || !anim_enabled())
    {
        anim_set(&panel.page_shift, 0);
        anim_set(&panel.body_shift, 0);
        anim_set(&panel.title_x, 0);
        anim_set(&panel.title_y, 0);
        anim_set(&panel.title_scale, 1);
        anim_set(&panel.title_opacity, 1);
        anim_set(&panel.open_opacity, 1);
        anim_set(&panel.ghost_opacity, 0);
        anim_set(&panel.shift, show ? 0 : -24);
        anim_set(&panel.opacity, show ? 1 : 0);
        M->side_panel->hidden = !show;
        M->side_panel_moving = false;
        main_set_panel_header(M->page, false);
        return;
    }

    // 页面让出（或收回）状态栏的宽度。概览页内容居中，按内容的实际位置算。
    float after_left;
    float saved = M->page_left;
    M->page_left = saved;
    float new_left, new_right;
    main_content_columns(&new_left, &new_right);
    after_left = new_left;
    M->page_left = before_left;
    float old_left, old_right;
    main_content_columns(&old_left, &old_right);
    M->page_left = saved;
    float page_offset = on_overview ? old_left - after_left : (before_left - SHEET_PADDING) - (saved - SHEET_PADDING);
    if (!on_overview)
    {
        // 设置页的牌可能被最大宽度限制，按牌的实际位置算。
        M->page_left = before_left;
        GfxRect before = main_sheet_area();
        M->page_left = saved;
        GfxRect after = main_sheet_area();
        page_offset = before.left - after.left;
    }
    start_motion(&panel.page_shift, page_offset, 0);
    if (on_overview)
    {
        // 标题行收起或出现，下面的内容上下补位。
        start_motion(&panel.body_shift, show ? header_height : -header_height, 0);
        if (show)
        {
            // 状态栏里的标题从页面左上角飞过来。
            start_motion(&panel.title_x, page_title_x - title_x, 0);
            start_motion(&panel.title_y, page_title_y - title_y, 0);
            panel.ghost_rect = open_rect;
            start_motion(&panel.ghost_shift, 0, -page_offset);
            anim_set(&panel.ghost_opacity, 1);
            anim_start(&panel.ghost_opacity, 0, 0.18, EASE_OF(EASE_LINEAR));
        }
        else
        {
            // 页面里的标题从状态栏飞回来；页面本身也在平移，要扣掉这一段。
            start_motion(&panel.title_x, title_x - new_left - page_offset, 0);
            start_motion(&panel.title_y, title_y - page_title_y, 0);
            start_motion(&panel.open_opacity, 0, 1);
        }
        anim_set(&panel.title_scale, 1);
        anim_set(&panel.title_opacity, 1);
    }
    start_motion(&panel.shift, panel_shift, show ? 0 : -24);
    start_motion(&panel.opacity, panel_opacity, show ? 1 : 0);
    M->side_panel_moving = true;
    if (!show) panel.header_page = PAGE_OVERVIEW;
    else main_set_panel_header(M->page, false);
    main_request_frame();
}

static void check_side_panel(float width)
{
    if (side_panel_wanted ? width >= SIDE_PANEL_HIDE_WIDTH : width < SIDE_PANEL_SHOW_WIDTH) return;
    side_panel_wanted = !side_panel_wanted;
    // 排版过程中不能再排版：等这一帧排完再切换。
    side_panel_pending = true;
    main_request_frame();
}

// ───────────── 状态栏的内容 ─────────────

typedef struct PanelIcon
{
    UiElement base;
} PanelIcon;

/// 线框版的应用图标（256 × 256 的画布缩进 52 × 52），跟随文字颜色。
static void panel_icon_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    float scale = (bounds.right - bounds.left) / 256;
    float center_x = (bounds.left + bounds.right) / 2, center_y = (bounds.top + bounds.bottom) / 2;
    float zoom = (float)panel.icon_scale.value;
    float opacity = (float)panel.icon_opacity.value;
    if (opacity <= 0.001f) return;
    gfx_push_transform(gfx_matrix_scale(zoom, zoom, center_x, center_y));
    gfx_push_opacity(opacity);
    Color color = theme_color(THEME_TEXT_PRIMARY);
    gfx_stroke_rounded((GfxRect){ bounds.left + 16 * scale, bounds.top + 16 * scale, bounds.left + 240 * scale, bounds.top + 240 * scale }, 50 * scale,
        color, 12 * scale);
    float x = bounds.left, y = bounds.top, s = scale;
    float stem[] = { x + 128 * s, y + 64 * s, x + 128 * s, y + 156 * s };
    float arrow[] = { x + 90 * s, y + 120 * s, x + 128 * s, y + 158 * s, x + 166 * s, y + 120 * s };
    float tray[] = { x + 68 * s, y + 170 * s, x + 68 * s, y + 190 * s, x + 188 * s, y + 190 * s, x + 188 * s, y + 170 * s };
    gfx_polyline(stem, 2, color, 12 * s);
    gfx_polyline(arrow, 3, color, 12 * s);
    gfx_polyline(tray, 4, color, 12 * s);
    gfx_pop_layer();
    gfx_pop_transform();
}

static const UiElementType panel_icon_type = { NULL, NULL, panel_icon_paint, NULL, NULL, false };

/// 状态栏里的“＋ 打开置物架”：强调色底，向右渐隐。
typedef struct SoftButton
{
    UiElement base;
} SoftButton;

static GfxSize soft_button_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 38 };
}

static void soft_button_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    bool pressed = element->pressed && element->hovered;
    Color fill = theme_color(pressed ? THEME_ACCENT_PRESSED : element->hovered ? THEME_ACCENT_HOVER : THEME_ACCENT);
    GfxStop stops[3] = { { 0, fill }, { 0.62f, fill }, { 1, color_with_alpha(fill, 0) } };
    gfx_push_layer(bounds, 6, 1);
    gfx_fill_rect_linear(bounds, bounds.left, bounds.top, bounds.right, bounds.top, stops, 3);
    gfx_pop_layer();
    Color text = theme_color(THEME_TEXT_ON_ACCENT);
    GfxTextStyle glyph = { GFX_FONT_ICON, 13, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    GfxTextStyle label = { GFX_FONT_TEXT, 13, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    gfx_text_draw(GLYPH_ADD, (GfxRect){ bounds.left + 14, bounds.top + 1, bounds.left + 40, bounds.bottom }, &glyph, text);
    gfx_text_draw(S(STR_OpenShelf), (GfxRect){ bounds.left + 40, bounds.top, bounds.right - 14, bounds.bottom }, &label, text);
}

static void soft_button_mouse(UiElement *element, const UiMouseEvent *event)
{
    (void)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) main_open_shelf();
}

static const UiElementType soft_button_type = { soft_button_measure, NULL, soft_button_paint, soft_button_mouse, NULL, true };

/// 返回概览的胶囊按钮：图标和“概览”，悬停时有底色。
static GfxSize back_pill_measure(UiElement *element, float available)
{
    (void)element;
    (void)available;
    GfxTextStyle crumb = { GFX_FONT_TEXT, 15, 400 };
    GfxTextStyle glyph = { GFX_FONT_ICON, 13, 400 };
    GfxSize text = gfx_text_measure(S(STR_Overview), &crumb, 0);
    return (GfxSize){ 10 + gfx_text_measure(GLYPH_BACK, &glyph, 0).width + 8 + text.width + 14, (text.height > 22 ? text.height : 22) + 11 };
}

static void back_pill_paint(UiElement *element, double time)
{
    (void)time;
    float opacity = (float)panel.back_opacity.value;
    if (opacity <= 0.001f) return;
    GfxRect bounds = element->bounds;
    gfx_push_opacity(opacity);
    bool pressed = element->pressed && element->hovered;
    gfx_fill_rounded(bounds, 6, theme_color(pressed ? THEME_SUBTLE_PRESSED : element->hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
    Color color = theme_color(element->hovered ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY);
    GfxTextStyle glyph = { GFX_FONT_ICON, 13, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    GfxTextStyle crumb = { GFX_FONT_TEXT, 15, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    float glyph_width = gfx_text_measure(GLYPH_BACK, &glyph, 0).width;
    gfx_text_draw(GLYPH_BACK, (GfxRect){ bounds.left + 10, bounds.top + 6, bounds.left + 10 + glyph_width + 1, bounds.bottom - 5 }, &glyph, color);
    gfx_text_draw(S(STR_Overview), (GfxRect){ bounds.left + 10 + glyph_width + 8, bounds.top + 5, bounds.right, bounds.bottom - 6 }, &crumb, color);
    gfx_pop_layer();
}

static void back_pill_mouse(UiElement *element, const UiMouseEvent *event)
{
    (void)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) flip_back();
}

static const UiElementType back_pill_type = { back_pill_measure, NULL, back_pill_paint, back_pill_mouse, NULL, true };

/// 状态栏背景：卡片色的椭圆渐隐，左上角一点强调色的光晕。
static void side_panel_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    Color card = theme_color(THEME_CARD);
    GfxStop fade[3] = { { 0, card }, { 0.6f, color_with_alpha(card, (uint8_t)(card.a * 0xB0 / 255)) }, { 1, color_with_alpha(card, 0) } };
    gfx_fill_rounded_radial(bounds, 0, bounds.left, bounds.top + height * 0.2f, width, height * 0.8f, fade, 3);
    Color accent = theme_color(THEME_ACCENT);
    accent.a = (uint8_t)(accent.a * 0.2f);
    GfxStop glow[2] = { { 0, accent }, { 1, color_with_alpha(accent, 0) } };
    gfx_fill_rounded_radial(bounds, 0, bounds.left, bounds.top, width, height * 0.5f, glow, 2);
}

static void side_panel_arrange(UiElement *element)
{
    GfxRect bounds = element->bounds;
    ui_measure(panel_main, SIDE_PANEL_WIDTH - 60);
    ui_arrange(panel_main, (GfxRect){ bounds.left + 30, bounds.top + 30, bounds.right - 30, bounds.top + 30 + panel_main->desired.height });
    ui_measure(panel_back, 200);
    float top = bounds.top + 20;
    ui_arrange(panel_back, (GfxRect){ bounds.left + 20, top, bounds.left + 20 + panel_back->desired.width, top + panel_back->desired.height });
    top += panel_back->desired.height + 2;
    ui_measure(panel_name, SIDE_PANEL_WIDTH - 60);
    ui_arrange(panel_name, (GfxRect){ bounds.left + 30, top, bounds.right - 30, top + panel_name->desired.height });
}

static void side_panel_paint_over(UiElement *element, double time)
{
    (void)element;
    (void)time;
    // 页面名称：裁在自己的一行里，从上方滚下来。
    float opacity = (float)panel.name_opacity.value;
    if (opacity <= 0.001f || panel.header_page == PAGE_OVERVIEW && opacity <= 0.001f) return;
    GfxRect rect = panel_name->bounds;
    gfx_push_clip(rect);
    GfxTextStyle style = { GFX_FONT_TEXT, 28, 600, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
    rect.top += (float)panel.name_shift.value;
    rect.bottom += (float)panel.name_shift.value;
    Color color = theme_color(THEME_TEXT_PRIMARY);
    color.a = (uint8_t)(color.a * (opacity > 1 ? 1 : opacity));
    gfx_text_draw(ui_label_text(panel_name), rect, &style, color);
    gfx_pop_clip();
}

static const UiElementType side_panel_type = { NULL, side_panel_arrange, side_panel_paint, NULL, NULL, false, NULL, NULL, NULL, false, side_panel_paint_over };

/// 一种呼出方式：图标和文字。
static UiElement *way_row(const wchar_t *glyph, UiElement *content)
{
    UiElement *row = ui_stack(true, 0);
    ui_set_margin(row, 2, 0, 0, 12);
    UiElement *icon = ui_label(UI_LIT(glyph), UI_TEXT_GLYPH);
    ui_label_set_size(icon, 15);
    ui_label_set_color(icon, THEME_ACCENT_TEXT);
    icon->width = 26;
    ui_add(row, icon);
    content->cross = UI_CROSS_CENTER;
    ui_add(row, content);
    return row;
}

static UiElement *way_text(StringId text)
{
    UiElement *label = ui_label(UI_STR(text), UI_TEXT_BODY);
    ui_label_set_size(label, 13);
    ui_label_style(label)->wrap = false;
    return label;
}

/// 快捷键的一个按键：小方块，下边框略粗。
static GfxSize key_cap_measure(UiElement *element, float available)
{
    (void)available;
    GfxTextStyle style = { GFX_FONT_TEXT, 11, 400 };
    GfxSize text = gfx_text_measure(element->tag, &style, 0);
    return (GfxSize){ text.width + 12, text.height + 3 + 3 };
}

static void key_cap_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    gfx_fill_rounded(bounds, 4, theme_color(THEME_CONTROL_BORDER));
    gfx_fill_rounded((GfxRect){ bounds.left + 1, bounds.top + 1, bounds.right - 1, bounds.bottom - 2 }, 3, theme_color(THEME_CONTROL));
    GfxTextStyle style = { GFX_FONT_TEXT, 11, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    gfx_text_draw(element->tag, (GfxRect){ bounds.left, bounds.top, bounds.right, bounds.bottom - 1 }, &style, theme_color(THEME_TEXT_PRIMARY));
}

static void key_cap_destroy(UiElement *element)
{
    free(element->tag);
}

static const UiElementType key_cap_type = { key_cap_measure, NULL, key_cap_paint, NULL, key_cap_destroy, false };

static void refresh_side_panel(void)
{
    const AppSettings *settings = app_settings();
    bool healthy = app_is_gesture_running();
    ui_label_set_text(panel_title_label, UI_STR(healthy ? STR_PanelReady : STR_StatusGestureFailed));
    ui_label_set_text(panel_body_label, UI_STR(healthy ? STR_PanelTagline : STR_StatusGestureFailedDetail));
    panel_ways[0]->hidden = !(healthy && auto_open_uses_shake(settings->auto_open_trigger));
    panel_ways[1]->hidden = !(healthy && auto_open_uses_edge(settings->auto_open_trigger));
    // 快捷键拆成一个个按键。
    ui_clear_children(panel_keys);
    bool has_hotkey = settings->hotkey && *settings->hotkey && app_is_hotkey_registered();
    if (has_hotkey)
    {
        const wchar_t *start = settings->hotkey;
        for (const wchar_t *cursor = start;; cursor++)
        {
            if (*cursor != L'+' && *cursor) continue;
            UiElement *cap = ui_element_new(&key_cap_type, sizeof(UiElement));
            cap->tag = wcs_dup_n(start, (size_t)(cursor - start));
            ui_set_margin(cap, 0, 0, 4, 0);
            cap->cross = UI_CROSS_CENTER;
            ui_add(panel_keys, cap);
            if (!*cursor) break;
            start = cursor + 1;
        }
    }
    panel_ways[2]->hidden = !has_hotkey;
}

static UiElement *build_side_panel(void)
{
    UiElement *side = ui_element_new(&side_panel_type, sizeof(UiElement));
    side->hidden = true;
    side->hit_through = true;

    panel_main = ui_stack(false, 0);
    panel_icon = ui_element_new(&panel_icon_type, sizeof(PanelIcon));
    panel_icon->width = panel_icon->height = 52;
    panel_icon->cross = UI_CROSS_START;
    ui_add(panel_main, panel_icon);
    panel_title_label = ui_label(UI_STR(STR_PanelReady), UI_TEXT_SUBTITLE);
    ui_label_style(panel_title_label)->wrap = true;
    ui_set_margin(panel_title_label, 0, 20, 0, 0);
    ui_add(panel_main, panel_title_label);
    panel_body_label = ui_label(UI_STR(STR_PanelTagline), UI_TEXT_CAPTION);
    ui_label_style(panel_body_label)->line_height = 19;
    ui_set_margin(panel_body_label, 0, 6, 0, 0);
    ui_add(panel_main, panel_body_label);
    UiElement *open = ui_element_new(&soft_button_type, sizeof(SoftButton));
    ui_set_margin(open, 0, 20, 0, 0);
    ui_add(panel_main, open);
    UiElement *ways = ui_label(UI_STR(STR_PanelWays), UI_TEXT_CAPTION);
    ui_label_set_color(ways, THEME_TEXT_TERTIARY);
    ui_set_margin(ways, 2, 26, 0, 8);
    ui_add(panel_main, ways);
    panel_ways[0] = way_row(GLYPH_MOVE, way_text(STR_WayShake));
    panel_ways[1] = way_row(GLYPH_EXPAND, way_text(STR_WayEdge));
    panel_keys = ui_stack(true, 0);
    panel_ways[2] = way_row(GLYPH_KEYBOARD, panel_keys);
    for (int index = 0; index < 3; index++) ui_add(panel_main, panel_ways[index]);
    ui_add(side, panel_main);

    panel_back = ui_element_new(&back_pill_type, sizeof(UiElement));
    panel_back->hit_through = true;
    ui_set_tooltip(panel_back, UI_STR(STR_BackTooltip));
    ui_add(side, panel_back);
    panel_name = ui_label(UI_STR(STR_General), UI_TEXT_TITLE);
    ui_label_style(panel_name)->ellipsis = true;
    // 名字由 side_panel_paint_over 自己画（带滚动和裁剪），标签只用来排版。
    panel_name->opacity = 0;
    ui_add(side, panel_name);
    return side;
}

/// 飞行中的“概览”标题：状态栏里的，或概览页标题行里的。
static void paint_flying_titles(void)
{
    float title_x, title_y, crumb_x, crumb_y;
    title_positions(&title_x, &title_y, &crumb_x, &crumb_y);
    GfxTextStyle style = { GFX_FONT_TEXT, 28, 600, false, false, GFX_ALIGN_START, GFX_ALIGN_START };
    float opacity = (float)panel.title_opacity.value;
    if (panel.title_on_panel && panel.panel_title_visible && opacity > 0.001f)
    {
        float scale = (float)panel.title_scale.value;
        GfxMatrix matrix = gfx_matrix_multiply(gfx_matrix_scale(scale, scale, title_x, title_y),
            gfx_matrix_translate((float)panel.title_x.value, (float)panel.title_y.value));
        gfx_push_transform(matrix);
        Color color = theme_color(THEME_TEXT_PRIMARY);
        color.a = (uint8_t)(color.a * (opacity > 1 ? 1 : opacity));
        gfx_text_draw(S(STR_Overview), (GfxRect){ title_x, title_y, title_x + 600, title_y + 60 }, &style, color);
        gfx_pop_transform();
    }
}

// ───────────── 标题栏 ─────────────

static void on_minimize(void *context)
{
    (void)context;
    ShowWindow(ui_window_handle(M->window), SW_MINIMIZE);
}

static void on_maximize(void *context)
{
    (void)context;
    HWND handle = ui_window_handle(M->window);
    ShowWindow(handle, IsZoomed(handle) ? SW_RESTORE : SW_MAXIMIZE);
}

static void on_close(void *context)
{
    (void)context;
    main_window_close();
}

static const GfxTextStyle caption_style = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

/// 标题栏里的运行状态：圆点和文字，光标停住时显示说明。
static GfxSize status_measure(UiElement *element, float available)
{
    (void)element;
    (void)available;
    StringId title = app_is_gesture_running() ? STR_StatusRunning : STR_StatusGestureFailed;
    return (GfxSize){ 13 + gfx_text_measure(S(title), &caption_style, 0).width, 20 };
}

static void status_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    bool healthy = app_is_gesture_running();
    float middle = (bounds.top + bounds.bottom) / 2;
    gfx_fill_ellipse(bounds.left + 3.5f, middle + 0.5f, 3.5f, 3.5f, theme_color(healthy ? THEME_SUCCESS : THEME_CAUTION));
    gfx_text_draw(S(healthy ? STR_StatusRunning : STR_StatusGestureFailed), (GfxRect){ bounds.left + 13, bounds.top, bounds.right + 20, bounds.bottom - 1 },
        &caption_style, theme_color(THEME_TEXT_SECONDARY));
}

static const UiElementType status_type = { status_measure, NULL, status_paint, NULL, NULL, true, NULL, NULL, NULL, false, NULL, IDC_ARROW };

static void refresh_status(void)
{
    const AppSettings *settings = app_settings();
    if (!app_is_gesture_running())
    {
        ui_set_tooltip(status_element, UI_STR(STR_StatusGestureFailedDetail));
        return;
    }
    bool has_hotkey = settings->hotkey && *settings->hotkey;
    wchar_t *detail = str_status_running_detail(settings->auto_open_trigger, has_hotkey ? settings->hotkey : NULL);
    ui_set_tooltip(status_element, UI_LIT(detail));
    free(detail);
}

// ───────────── 右下角的版本号 ─────────────

static GfxSize link_measure(UiElement *element, float available)
{
    (void)available;
    GfxTextStyle style = { GFX_FONT_TEXT, 12, 400 };
    wchar_t *text = str_version(DROPSHELF_VERSION);
    GfxSize size = gfx_text_measure(text, &style, 0);
    free(text);
    (void)element;
    return (GfxSize){ size.width + 12, size.height + 6 };
}

static void link_paint(UiElement *element, double time)
{
    (void)time;
    GfxTextStyle style = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    wchar_t *text = str_version(DROPSHELF_VERSION);
    gfx_text_draw(text, element->bounds, &style, theme_color(element->hovered ? THEME_ACCENT_TEXT : THEME_TEXT_SECONDARY));
    free(text);
}

static void link_mouse(UiElement *element, const UiMouseEvent *event)
{
    (void)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) about_window_show(ui_window_handle(M->window));
}

static const UiElementType link_type = { link_measure, NULL, link_paint, link_mouse, NULL, true };

// ───────────── 根元素：各层的排版和画的顺序 ─────────────

static GfxSize root_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 0 };
}

static void root_arrange(UiElement *element)
{
    GfxRect bounds = element->bounds;
    float width = bounds.right, height = bounds.bottom;
    M->stage = (GfxRect){ 0, CAPTION_HEIGHT, width, height };
    check_side_panel(width);

    // 标题栏。
    ui_measure(status_element, 300);
    float title_width = gfx_text_measure(L"DropShelf", &caption_style, 0).width;
    float status_left = 18 + 16 + 12 + title_width + 16;
    ui_arrange(status_element, (GfxRect){ status_left, 14, status_left + status_element->desired.width, 34 });
    float right = width;
    UiElement *captions[3] = { close_button, maximize_button, minimize_button };
    for (int index = 0; index < 3; index++)
    {
        ui_arrange(captions[index], (GfxRect){ right - 46, 0, right, 32 });
        right -= 46;
    }

    // 概览页的卡片高度跟着可见范围变化。
    float viewport = M->stage.bottom - M->stage.top;
    bool resized = fabsf(viewport - last_viewport) >= 1;
    last_viewport = viewport;
    ui_measure(M->overview_scroll, width);
    // 卡片高度按这次量出来的其余内容算；变了就再量一次，同一帧里就是新的高度，拖动窗口边缘时不会抖。
    if (pages_update_card_height(viewport, resized)) ui_measure(M->overview_scroll, width);
    ui_arrange(M->overview_scroll, M->stage);
    for (int page = 0; page < PAGE_COUNT; page++)
    {
        ui_measure(M->pages[page].host, width);
        ui_arrange(M->pages[page].host, M->stage);
    }
    ui_arrange(M->stack_layer, M->stage);
    flip_layout_edges();

    ui_measure(M->version_button, width);
    float link_width = M->version_button->desired.width, link_height = M->version_button->desired.height;
    ui_arrange(M->version_button, (GfxRect){ width - 10 - link_width, height - 6 - link_height, width - 10, height - 6 });

    ui_arrange(M->side_panel, (GfxRect){ 0, M->stage.top, SIDE_PANEL_WIDTH, height });
    ui_measure(M->dialogs, width);
    ui_arrange(M->dialogs, bounds);
}

Color main_stage_background(void)
{
    // 和下面 root_paint 画出来的一样：窗口先清成底色（有系统材质时全透明），内容区再盖一层 THEME_LAYER。
    Color window = ui_window_has_backdrop(M->window) ? COLOR_TRANSPARENT : theme_color(THEME_WINDOW_BACKGROUND);
    return color_over(theme_color(THEME_LAYER), window);
}

static void root_paint(UiElement *element, double time)
{
    GfxRect bounds = element->bounds;
    // 标题栏：图标、程序名和运行状态。
    if (app_icon) gfx_draw_image(app_icon, (GfxRect){ 18, 16, 34, 32 }, 1);
    gfx_text_draw(L"DropShelf", (GfxRect){ 46, 0, 200, 47 }, &caption_style, theme_color(THEME_TEXT_PRIMARY));
    ui_paint(status_element, time);
    ui_paint(minimize_button, time);
    ui_paint(maximize_button, time);
    ui_paint(close_button, time);

    // 内容区：底色和上边的分隔线。
    GfxRect stage = M->stage;
    gfx_fill_rect(stage, theme_color(THEME_LAYER));
    gfx_fill_rect((GfxRect){ 0, stage.top, bounds.right, stage.top + 1 }, theme_color(THEME_CARD_BORDER));
    gfx_push_clip((GfxRect){ 0, stage.top + 1, bounds.right, bounds.bottom });
    flip_paint_stage(time);
    // 状态栏出现时右上角按钮留在原处淡出。
    if (panel.ghost_opacity.value > 0.001 && M->overview_header->child_count > 1)
    {
        UiElement *button = M->overview_header->children[1];
        bool was_hidden = button->hidden;
        UiElement *header = M->overview_header;
        bool header_hidden = header->hidden;
        header->hidden = button->hidden = false;
        GfxRect saved = button->bounds;
        button->bounds = panel.ghost_rect;
        gfx_push_transform(gfx_matrix_translate((float)panel.ghost_shift.value, 0));
        gfx_push_opacity((float)panel.ghost_opacity.value);
        ui_paint(button, time);
        gfx_pop_layer();
        gfx_pop_transform();
        button->bounds = saved;
        button->hidden = was_hidden;
        header->hidden = header_hidden;
    }
    ui_paint(M->version_button, time);
    // 状态栏。
    M->side_panel->opacity = (float)panel.opacity.value;
    M->side_panel->transformed = true;
    M->side_panel->transform = gfx_matrix_translate((float)panel.shift.value, 0);
    ui_paint(M->side_panel, time);
    paint_flying_titles();
    gfx_pop_clip();
    actions_paint_overlay(time);
    ui_paint(M->dialogs, time);
    fan_paint_ghost(time);
}

static const UiElementType root_type = {
    root_measure, root_arrange, root_paint, NULL, NULL, false, NULL, NULL, NULL, false, NULL, NULL, false, true
};

// ───────────── 窗口的回调 ─────────────

static void change_bounds(AppSettings *settings, void *context)
{
    const double *bounds = context;
    settings->window_left = bounds[0];
    settings->window_top = bounds[1];
    settings->window_width = bounds[2];
    settings->window_height = bounds[3];
}

/// 把窗口的位置和大小记进设置（DIP），下次打开时还在原处。最大化、最小化时不记。
static void save_bounds(void)
{
    HWND handle = ui_window_handle(M->window);
    if (!handle || IsZoomed(handle) || IsIconic(handle)) return;
    RECT outer;
    GetWindowRect(handle, &outer);
    float scale = ui_window_scale(M->window);
    GfxSize size = ui_window_size(M->window);
    double bounds[4] = { outer.left / (double)scale, outer.top / (double)scale, size.width, size.height };
    const AppSettings *settings = app_settings();
    if (settings->window_left == bounds[0] && settings->window_top == bounds[1] && settings->window_width == bounds[2] &&
        settings->window_height == bounds[3])
        return;
    app_update_settings(change_bounds, bounds);
}

static void on_moved(UiWindow *moved, void *context)
{
    (void)moved;
    (void)context;
    // 窗口大小或状态变了，算好的翻牌位置已经失效。
    flip_cancel();
    if (maximize_button) ui_button_set_text(maximize_button, UI_LIT(IsZoomed(ui_window_handle(M->window)) ? GLYPH_RESTORE : GLYPH_MAXIMIZE));
    save_bounds();
}

static bool can_go_back(void)
{
    return M->page != PAGE_OVERVIEW && !dialogs_open() && !actions_dragging() && !fan_dragging();
}

static bool on_key(UiWindow *target, void *context, unsigned key)
{
    (void)target;
    (void)context;
    if (key == VK_ESCAPE)
    {
        if (flip_escape() || actions_escape() || dialogs_escape() || fan_escape()) return true;
    }
    // 翻牌期间页面跟着卡片变形，按键都先不处理（Alt+F4 等系统按键照常）。
    if (flip_morphing()) return (key & UI_KEY_ALT) == 0;
    bool back = key == VK_ESCAPE || key == (VK_LEFT | UI_KEY_ALT) || key == VK_BROWSER_BACK;
    if (!back || !can_go_back()) return false;
    flip_back();
    return true;
}

static bool on_mouse(UiWindow *target, void *context, const UiMouseEvent *event)
{
    (void)target;
    (void)context;
    if (dialogs_mouse(event)) return true;
    return flip_mouse(event);
}

static bool on_message(UiWindow *target, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    (void)target;
    (void)context;
    (void)lparam;
    // 鼠标的后退键回到概览。
    if (message == WM_XBUTTONDOWN && GET_XBUTTON_WPARAM(wparam) == XBUTTON1 && !flip_morphing() && can_go_back())
    {
        flip_back();
        *result = TRUE;
        return true;
    }
    if (message == WM_DPICHANGED) flip_cancel();
    if (message == WM_CAPTURECHANGED && (HWND)lparam != ui_window_handle(M->window)) flip_capture_lost();
    if (message == WM_SIZE && wparam == SIZE_MINIMIZED) flip_cancel();
    return false;
}

static void on_animate(UiWindow *target, void *context, double time)
{
    (void)target;
    (void)context;
    if (side_panel_pending)
    {
        side_panel_pending = false;
        set_side_panel(M->root->bounds.right > 0);
    }
    bool running = false;
    Anim *anims[] = {
        &panel.opacity, &panel.shift, &panel.page_shift, &panel.body_shift, &panel.title_x, &panel.title_y, &panel.title_scale,
        &panel.title_opacity, &panel.open_opacity, &panel.ghost_opacity, &panel.ghost_shift, &panel.icon_opacity, &panel.icon_scale,
        &panel.back_opacity, &panel.name_opacity, &panel.name_shift
    };
    for (size_t index = 0; index < sizeof(anims) / sizeof(anims[0]); index++) running |= anim_update(anims[index], time);
    if (M->side_panel_moving && !panel.opacity.running && !panel.page_shift.running)
    {
        // 动画结束：收起的状态栏不再参与画和点击。
        M->side_panel_moving = false;
        if (!M->side_panel_shown) M->side_panel->hidden = true;
    }
    if (panel.title_on_panel && panel.header_page != PAGE_OVERVIEW && !panel.title_opacity.running) panel.panel_title_visible = false;
    // 页面跟着状态栏让位：概览页内容、设置页和牌的边一起平移。
    M->page_shift = (float)panel.page_shift.value;
    M->overview_body->transformed = panel.body_shift.value != 0;
    M->overview_body->transform = gfx_matrix_translate(0, (float)panel.body_shift.value);
    // 概览页里的标题从状态栏飞回来，右上角的按钮淡入。
    if (M->overview_header->child_count > 1)
    {
        UiElement *title = M->overview_header->children[0];
        bool flying = !panel.title_on_panel && (panel.title_x.running || panel.title_y.running);
        title->transformed = flying;
        title->transform = gfx_matrix_translate((float)panel.title_x.value, (float)panel.title_y.value);
        M->overview_header->children[1]->opacity = (float)panel.open_opacity.value;
    }
    flip_animate(time);
    pages_animate(time);
    fan_animate(time);
    actions_animate(time);
    dialogs_animate(time);
    if (running) main_request_frame();
}

static void on_closed(UiWindow *closed, void *context)
{
    (void)closed;
    (void)context;
    // 窗口显示期间保持正常性能，关掉后交还系统决定。元素已经销毁，各部分只清掉自己记着的东西。
    power_hold(&M->window, false);
    M->window = NULL;
    pages_close();
    fan_close();
    actions_close();
    dialogs_close();
    flip_close();
    // 排好的几百段文字只有主窗口用，留着要占几兆内存；下次打开时第一帧重新排。
    gfx_release_text_cache();
    memset(M, 0, sizeof(*M));
    M->page = PAGE_OVERVIEW;
    minimize_button = maximize_button = close_button = status_element = NULL;
    side_panel_wanted = side_panel_pending = false;
    last_viewport = 0;
    memset(&panel, 0, sizeof(panel));
    app_main_window_closed();
}

static void on_history_changed(void *context)
{
    (void)context;
    if (!M->window) return;
    fan_refresh(true);
    pages_refresh();
}

static void on_language_changed(void *context)
{
    (void)context;
    if (!M->window) return;
    main_window_refresh();
    fan_refresh(false);
}

// ───────────── 打开和关闭 ─────────────

void main_window_refresh(void)
{
    if (!M->window) return;
    refresh_status();
    refresh_side_panel();
    pages_refresh();
    actions_refresh();
    dialogs_refresh();
    main_relayout();
}

bool main_window_is_open(void)
{
    return M->window != NULL;
}

static UiElement *build_root(void)
{
    UiElement *root = ui_element_new(&root_type, sizeof(UiElement));
    M->root = root;
    status_element = ui_element_new(&status_type, sizeof(UiElement));
    ui_add(root, status_element);
    minimize_button = ui_button(UI_BUTTON_CAPTION, UI_LIT(GLYPH_MINIMIZE), on_minimize, NULL);
    maximize_button = ui_button(UI_BUTTON_CAPTION, UI_LIT(GLYPH_MAXIMIZE), on_maximize, NULL);
    close_button = ui_button(UI_BUTTON_CAPTION_CLOSE, UI_LIT(GLYPH_CLOSE), on_close, NULL);
    ui_set_tooltip(minimize_button, UI_STR(STR_CaptionMinimize));
    ui_set_tooltip(close_button, UI_STR(STR_CaptionClose));
    ui_add(root, minimize_button);
    ui_add(root, maximize_button);
    ui_add(root, close_button);

    // 画和点击的顺序：概览页在最下面，然后是牌的边、各设置页、版本号、状态栏，对话框在最上面。
    pages_build();
    ui_add(root, M->overview_scroll);
    M->stack_layer = flip_create_stack_layer();
    ui_add(root, M->stack_layer);
    for (int page = 0; page < PAGE_COUNT; page++) ui_add(root, M->pages[page].host);
    M->version_button = ui_element_new(&link_type, sizeof(UiElement));
    ui_set_tooltip(M->version_button, UI_STR(STR_AboutTooltip));
    ui_add(root, M->version_button);
    M->side_panel = build_side_panel();
    ui_add(root, M->side_panel);
    M->dialogs = dialogs_create();
    ui_add(root, M->dialogs);
    return root;
}

void main_window_show(void)
{
    if (M->window)
    {
        ui_window_show(M->window);
        return;
    }
    static bool listening;
    if (!listening)
    {
        listening = true;
        shelf_history_add_listener(on_history_changed, NULL);
        language_add_listener(on_language_changed, NULL);
    }
    const AppSettings *settings = app_settings();
    UiWindowOptions options = { 0 };
    options.title = L"DropShelf";
    options.width = (float)settings->window_width;
    options.height = (float)settings->window_height;
    options.minimum_width = 640;
    options.minimum_height = 520;
    options.resizable = true;
    options.backdrop = UI_BACKDROP_MICA;
    options.background = THEME_WINDOW_BACKGROUND;
    options.caption_height = CAPTION_HEIGHT;
    options.icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE));
    options.key = on_key;
    options.mouse = on_mouse;
    options.message = on_message;
    options.moved = on_moved;
    options.closed = on_closed;
    options.animate = on_animate;
    M->page = PAGE_OVERVIEW;
    M->page_left = PAGE_MARGIN;
    panel.header_page = PAGE_OVERVIEW;
    anim_set(&panel.opacity, 0);
    anim_set(&panel.shift, -24);
    anim_set(&panel.title_scale, 1);
    anim_set(&panel.title_opacity, 1);
    anim_set(&panel.open_opacity, 1);
    anim_set(&panel.icon_opacity, 1);
    anim_set(&panel.icon_scale, 1);
    M->window = ui_window_create(&options);
    if (!M->window) return;
    if (!app_icon)
    {
        HICON icon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE), IMAGE_ICON, 32, 32, 0);
        app_icon = gfx_image_from_icon(icon);
        if (icon) DestroyIcon(icon);
    }

    ui_window_set_root(M->window, build_root());
    ui_window_set_maximize_button(M->window, maximize_button);
    main_window_refresh();
    fan_refresh(false);
    flip_show_page(PAGE_OVERVIEW);

    // 回到上次的位置；没有记录、或那个位置已经不在任何显示器上（换了显示器）时放在屏幕中央。
    bool placed = false;
    if (isfinite(settings->window_left) && isfinite(settings->window_top))
    {
        float scale = ui_window_scale(M->window);
        POINT corner = { (LONG)(settings->window_left * scale) + 40, (LONG)(settings->window_top * scale) + 20 };
        if (MonitorFromPoint(corner, MONITOR_DEFAULTTONULL))
        {
            ui_window_place(M->window, (int)(settings->window_left * scale), (int)(settings->window_top * scale), options.width, options.height);
            placed = true;
        }
    }
    if (!placed) ui_window_center(M->window);
    // 第一次排版前就按窗口的宽度定好状态栏显示与否，不播放动画。
    GfxSize size = ui_window_size(M->window);
    side_panel_wanted = size.width >= SIDE_PANEL_SHOW_WIDTH;
    if (side_panel_wanted)
    {
        M->root->bounds.right = size.width;
        set_side_panel(false);
    }
    power_hold(&M->window, true);
    ui_window_show(M->window);
}

void main_window_close(void)
{
    if (!M->window) return;
    save_bounds();
    flip_cancel();
    ui_window_close(M->window);
}

UiWindow *main_window_ui(void)
{
    return M->window;
}
