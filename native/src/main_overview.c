// 主窗口的概览页与卡片绘制（从 main_pages.c 拆出），按风格各编译一遍（见 look_variant.h）。
#include "look_variant.h"
#include "main_window_internal.h"
#include "ui_internal.h"
#include "quick_action.h"
#include "shelf_history.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_CARD_HEIGHT 70.0f
#define MAX_CARD_HEIGHT 230.0f
/// 牌抽出后会超出牌堆标题约这么高；窗口够高时在卡片和牌堆之间留出这段空间。
#define FAN_HEADROOM 72.0f
#define TILE_GAP 6.0f
/// 卡片够高时换成大卡片的样式；回差让高度停在分界附近时不会来回切换。
#define TALL_HEIGHT 138.0f
#define TALL_HYSTERESIS 6.0f

static struct
{
    UiElement *getting_started;
    Anim card_height;
    float card_target;
} O;

// ───────────── 概览页的卡片 ─────────────

typedef struct PageTile
{
    UiElement base;
    int page;
    wchar_t *summary;
    StrList items;          // 摘要拆成的各项，大卡片上显示为标签
#ifdef LOOK_VARIANT_CARDS
    StrList glance;         // 卡片右侧画出的当前设置：常规是快捷键的各个按键，快速操作是底部各个按钮的图标
#endif
    bool tall;
    Anim tall_anim;         // 0 是一行的样式，1 是大卡片
#ifdef LOOK_VARIANT_CARDS
    Anim lift;              // 悬停时卡片抬起、底下的牌背多露出一点（0 到 1）
#endif
} PageTile;

static void tile_split_summary(PageTile *tile)
{
    strlist_clear(&tile->items);
    const wchar_t *text = tile->summary ? tile->summary : L"";
    static const wchar_t *const separators[] = { L" · ", L"、", L", " };
    const wchar_t *start = text;
    while (*start)
    {
        const wchar_t *next = NULL;
        size_t length = 0;
        for (int index = 0; index < 3; index++)
        {
            const wchar_t *found = wcsstr(start, separators[index]);
            if (found && (!next || found < next))
            {
                next = found;
                length = wcslen(separators[index]);
            }
        }
        size_t part = next ? (size_t)(next - start) : wcslen(start);
        wchar_t *item = wcs_dup_n(start, part);
        wchar_t *trimmed = wcs_trim_dup(item);
        free(item);
        if (*trimmed) strlist_add_owned(&tile->items, trimmed);
        else free(trimmed);
        if (!next) break;
        start = next + length;
    }
}

#ifdef LOOK_VARIANT_CARDS
// ───────────── 卡片右侧的当前设置 ─────────────

/// 键帽：一键一格，下沿加粗，数字用 Bahnschrift。返回画完后的右边。
static float keycaps_width(const StrList *keys)
{
    GfxTextStyle style = { GFX_FONT_NUMBER, 11, 400 };
    float width = 0;
    for (int index = 0; index < keys->count; index++) width += gfx_text_measure(keys->items[index], &style, 0).width + 12 + 3;
    return width;
}

static void paint_keycaps(const StrList *keys, float left, float middle, Color color)
{
    GfxTextStyle style = { GFX_FONT_NUMBER, 11, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    float height = gfx_text_measure(L"0", &style, 0).height + 1 + 3;
    float top = middle - height / 2;
    for (int index = 0; index < keys->count; index++)
    {
        float width = gfx_text_measure(keys->items[index], &style, 0).width + 12;
        GfxRect cap = { left, top, left + width, top + height };
        gfx_stroke_rounded(cap, 3, color, 1);
        gfx_fill_rect((GfxRect){ cap.left + 2, cap.bottom - 2, cap.right - 2, cap.bottom - 1 }, color);
        gfx_text_draw(keys->items[index], (GfxRect){ cap.left, cap.top + 1, cap.right, cap.bottom - 2 }, &style, color);
        left += width + 3;
    }
}

/// 把快捷键拆成一个个按键；没有快捷键或没注册上时为空，和 C# 版的 HotkeyKeys 相同。
static void hotkey_keys(StrList *keys)
{
    strlist_clear(keys);
    const AppSettings *settings = app_settings();
    if (!settings->hotkey || !*settings->hotkey || !app_is_hotkey_registered()) return;
    const wchar_t *start = settings->hotkey;
    for (const wchar_t *cursor = start;; cursor++)
    {
        if (*cursor && *cursor != L'+') continue;
        strlist_add_owned(keys, wcs_dup_n(start, (size_t)(cursor - start)));
        if (!*cursor) break;
        start = cursor + 1;
    }
}

/// 一段折线，线条用墨色、圆角连接（入门提示和卡片上的小图共用）。
static void line_art(const float *points, int count, float x, float y, float opacity)
{
    float moved[16];
    for (int index = 0; index < count * 2 && index < 16; index += 2)
    {
        moved[index] = points[index] + x;
        moved[index + 1] = points[index + 1] + y;
    }
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    ink.a = (uint8_t)(ink.a * opacity);
    gfx_polyline(moved, count, ink, 1.4f);
}

static float glance_width(const PageTile *tile)
{
    switch (tile->page)
    {
        case PAGE_GENERAL: return keycaps_width(&tile->glance);
        case PAGE_SHELF: return 74;
        case PAGE_ACTIONS: return tile->glance.count * 31.0f;
        default: return 3 * 22.0f;
    }
}

/// 卡片右侧直接画出当前设置：快捷键、呼出方式、底部按钮、主题。left 是左边，middle 是竖直的中线。
static void paint_glance(const PageTile *tile, float left, float middle)
{
    const AppSettings *settings = app_settings();
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    switch (tile->page)
    {
        case PAGE_GENERAL:
            paint_keycaps(&tile->glance, left, middle, ink);
            break;
        case PAGE_SHELF:
        {
            // 左边的折线是晃动，右边的箭头和竖线是屏幕边缘；没用到的那种变淡。
            float top = middle - 13;
            float shake[] = { 2, 13, 8, 5, 15, 21, 22, 5, 29, 21, 35, 13 };
            line_art(shake, 6, left, top, settings->auto_open_trigger == TRIGGER_EDGE ? 0.25f : 1);
            float edge_opacity = settings->auto_open_trigger == TRIGGER_SHAKE ? 0.25f : 1;
            float shaft[] = { 48, 13, 62, 13 }, head[] = { 57, 8, 62, 13, 57, 18 };
            line_art(shaft, 2, left, top, edge_opacity);
            line_art(head, 3, left, top, edge_opacity);
            Color accent = theme_color(THEME_ACCENT);
            accent.a = (uint8_t)(accent.a * edge_opacity);
            gfx_line(left + 71, top + 1, left + 71, top + 25, accent, 2.4f);
            break;
        }
        case PAGE_ACTIONS:
        {
            GfxTextStyle glyph = { GFX_FONT_ICON, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
            for (int index = 0; index < tile->glance.count; index++)
            {
                GfxRect slot = { left + index * 31.0f, middle - 13, left + index * 31.0f + 28, middle + 13 };
                gfx_stroke_rounded(slot, 4, ink, 1);
                gfx_text_draw(tile->glance.items[index], slot, &glyph, ink);
            }
            break;
        }
        default:
        {
            // 浅色、深色两张牌和一张强调色的牌；没选的主题变淡。
            Color border = theme_color(THEME_CONTROL_BORDER), accent = theme_color(THEME_ACCENT);
            GfxRect chip = { left, middle - 12, left + 18, middle + 12 };
            gfx_push_opacity(settings->theme == THEME_DARK ? 0.45f : 1);
            gfx_fill_rounded(chip, 3, COLOR_RGB(0xFB, 0xFA, 0xF6));
            gfx_stroke_rounded(chip, 3, border, 1);
            gfx_pop_layer();
            chip.left += 22, chip.right += 22;
            gfx_push_opacity(settings->theme == THEME_LIGHT ? 0.45f : 1);
            gfx_fill_rounded(chip, 3, COLOR_RGB(0x26, 0x25, 0x22));
            gfx_stroke_rounded(chip, 3, border, 1);
            gfx_pop_layer();
            chip.left += 22, chip.right += 22;
            gfx_fill_rounded(chip, 3, accent);
            break;
        }
    }
}

/// 卡片左边的图标框：一张描边的小牌，悬停时铺满强调色。
static void paint_emblem(GfxRect box, const wchar_t *glyph, float glyph_size, bool hovered)
{
    if (hovered) gfx_fill_rounded(box, 6, theme_color(THEME_ACCENT));
    else gfx_stroke_rounded(box, 6, theme_color(THEME_TEXT_PRIMARY), 1);
    GfxTextStyle icon = { GFX_FONT_ICON, glyph_size, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    gfx_text_draw(glyph, box, &icon, theme_color(hovered ? THEME_TEXT_ON_ACCENT : THEME_TEXT_PRIMARY));
}

/// 卡片就是点开后会翻过来的那张牌：bounds 是卡片的位置（悬停抬起之后的），翻牌时也按这个画。
#endif
static void tile_paint_at(PageTile *tile, GfxRect bounds, bool hovered, bool pressed)
{
    UiElement *element = &tile->base;
    float height = bounds.bottom - bounds.top;
#ifdef LOOK_VARIANT_CARDS
    if (pressed) gfx_push_opacity(0.85f);
#else
    if (pressed) gfx_push_opacity(0.8f);
#endif
    gfx_fill_rounded(bounds, TILE_RADIUS, theme_color(hovered ? THEME_CARD_HOVER : THEME_CARD));
#ifdef LOOK_VARIANT_CARDS
    gfx_stroke_rounded(bounds, TILE_RADIUS, theme_color(hovered ? THEME_ACCENT : THEME_CARD_BORDER), 1);
#else
    gfx_stroke_rounded(bounds, TILE_RADIUS, theme_color(THEME_CARD_BORDER), 1);
#endif
    gfx_push_layer((GfxRect){ bounds.left + 1, bounds.top + 1, bounds.right - 1, bounds.bottom - 1 }, TILE_RADIUS - 1, 1);
    const wchar_t *glyph = main_page_glyph(tile->page);
#ifdef LOOK_VARIANT_CARDS
    Color primary = theme_color(THEME_TEXT_PRIMARY), secondary = theme_color(THEME_TEXT_SECONDARY);
#else
    Color accent = theme_color(THEME_ACCENT_TEXT);
#endif

    // 背景的大图标随高度渐显。
    float ramp = (height - 140) / (230 - 140);
    ramp = ramp < 0 ? 0 : ramp > 1 ? 1 : ramp;
    if (ramp > 0)
    {
        GfxTextStyle big = { GFX_FONT_ICON, 150, 400, false, false, GFX_ALIGN_END, GFX_ALIGN_END };
#ifdef LOOK_VARIANT_CARDS
        Color faint = primary;
        faint.a = (uint8_t)(faint.a * 0.06f * ramp);
#else
        Color faint = accent;
        faint.a = (uint8_t)(faint.a * 0.07f * ramp);
#endif
        gfx_text_draw(glyph, (GfxRect){ bounds.left, bounds.top, bounds.right - 1 + 16, bounds.bottom - 1 + 24 }, &big, faint);
    }
    float tall = (float)tile->tall_anim.value;
#ifndef LOOK_VARIANT_CARDS
    Color primary = theme_color(THEME_TEXT_PRIMARY), secondary = theme_color(THEME_TEXT_SECONDARY);
#endif
    if (tall < 0.999f)
    {
        // 一行的样式：图标在左，标题和摘要在右，竖直居中。
        gfx_push_opacity(1 - tall);
#ifdef LOOK_VARIANT_CARDS
        float middle = (bounds.top + bounds.bottom) / 2;
        GfxRect box = { bounds.left + 14, middle - 20, bounds.left + 14 + 40, middle + 20 };
        paint_emblem(box, glyph, 19, hovered);
        float glance = glance_width(tile);
        float glance_left = bounds.right - 16 - glance;
        if (glance > 0) paint_glance(tile, glance_left, middle);
        float left = box.right + 14, right = glance > 0 ? glance_left - 12 : bounds.right - 16;
#else
        GfxTextStyle icon = { GFX_FONT_ICON, 22, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        float icon_width = gfx_text_measure(glyph, &icon, 0).width;
        gfx_text_draw(glyph, (GfxRect){ bounds.left + 1 + 18, bounds.top, bounds.left + 19 + icon_width + 1, bounds.bottom }, &icon, accent);
        float left = bounds.left + 1 + 18 + icon_width + 16, right = bounds.right - 1 - 16;
#endif
        GfxTextStyle header = { GFX_FONT_TEXT, 14, 600, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
        GfxTextStyle summary = { GFX_FONT_TEXT, 12, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
        float header_height = gfx_text_measure(S(main_page_title(tile->page)), &header, 0).height;
        float summary_height = gfx_text_measure(L"Ag", &summary, 0).height;
        float top = (bounds.top + bounds.bottom - header_height - 3 - summary_height) / 2;
        gfx_text_draw(S(main_page_title(tile->page)), (GfxRect){ left, top, right, top + header_height }, &header, primary);
        top += header_height + 3;
        gfx_text_draw(tile->summary ? tile->summary : L"", (GfxRect){ left, top, right, top + summary_height }, &summary, secondary);
        gfx_pop_layer();
    }
    if (tall > 0.001f)
    {
        // 大卡片：图标方块在左上，标题和标签在下。
        gfx_push_opacity(tall);
#ifdef LOOK_VARIANT_CARDS
        GfxRect box = { bounds.left + 20, bounds.top + 20, bounds.left + 20 + 44, bounds.top + 20 + 44 };
        paint_emblem(box, glyph, 21, hovered);
        float left = bounds.left + 20, right = bounds.right - 18, bottom = bounds.bottom - 20;
#else
        GfxRect box = { bounds.left + 1 + 22, bounds.top + 1 + 22, bounds.left + 1 + 22 + 44, bounds.top + 1 + 22 + 44 };
        gfx_fill_rounded(box, 10, theme_color(THEME_ACCENT_FAINT));
        GfxTextStyle icon = { GFX_FONT_ICON, 22, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        gfx_text_draw(glyph, box, &icon, accent);
        float left = bounds.left + 1 + 22, right = bounds.right - 1 - 18, bottom = bounds.bottom - 1 - 20;
#endif
        GfxTextStyle chip = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
#ifdef LOOK_VARIANT_CARDS
        // 标签从左往右排，排不下换行，每个标签右边和下边留 6。先算出有几行，从下往上摆。
        float chip_height = gfx_text_measure(L"Ag", &chip, 0).height + 9;
#else
        // 标签从左往右排，排不下换行。先算出有几行，从下往上摆。
        float chip_height = gfx_text_measure(L"Ag", &chip, 0).height + 7;
#endif
        int rows = 1;
        float x = left;
        for (int index = 0; index < tile->items.count; index++)
        {
#ifdef LOOK_VARIANT_CARDS
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 20;
#else
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 18;
#endif
            if (x > left && x + width > right)
            {
                rows++;
                x = left;
            }
            x += width + 6;
        }
#ifdef LOOK_VARIANT_CARDS
        float chips_top = bottom - rows * (chip_height + 6);
#else
        float chips_top = bottom - rows * chip_height;
#endif
        GfxTextStyle header = { GFX_FONT_TEXT, 16, 600, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
        float header_height = gfx_text_measure(S(main_page_title(tile->page)), &header, 0).height;
        float header_top = chips_top - 10 - header_height;
        gfx_text_draw(S(main_page_title(tile->page)), (GfxRect){ left, header_top, right, header_top + header_height }, &header, primary);
        x = left;
        float y = chips_top;
        for (int index = 0; index < tile->items.count; index++)
        {
#ifdef LOOK_VARIANT_CARDS
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 20;
#else
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 18;
#endif
            if (x > left && x + width > right)
            {
#ifdef LOOK_VARIANT_CARDS
                y += chip_height + 6;
#else
                y += chip_height;
#endif
                x = left;
            }
            GfxRect pill = { x, y, x + width, y + chip_height };
#ifdef LOOK_VARIANT_CARDS
            gfx_fill_rounded(pill, 4, theme_color(THEME_SETTING_ROW));
            gfx_stroke_rounded(pill, 4, theme_color(THEME_CARD_BORDER), 1);
#else
            gfx_fill_rounded(pill, 11, theme_color(THEME_SUBTLE_SELECTED));
#endif
            gfx_text_draw(tile->items.items[index], (GfxRect){ pill.left, pill.top, pill.right, pill.bottom - 1 }, &chip, secondary);
            x += width + 6;
        }
        gfx_pop_layer();
    }
    gfx_pop_layer();
    if (pressed) gfx_pop_layer();
    (void)element;
}

static void tile_paint(UiElement *element, double time)
{
    PageTile *tile = (PageTile *)element;
    if (anim_update(&tile->tall_anim, time)) ui_window_request_frame(element->window);
#ifdef LOOK_VARIANT_CARDS
    if (anim_update(&tile->lift, time)) ui_window_request_frame(element->window);
    // 底下露出一道牌背，悬停时卡片微微抬起，牌背多露出一点。
    float lift = (float)tile->lift.value;
    GfxRect bounds = element->bounds;
    ThemePattern back = theme_pattern(THEME_PATTERN_STACK_EDGE);
    gfx_fill_rounded_pattern((GfxRect){ bounds.left + 3 + 2 * lift, bounds.top + 3 + 2 * lift, bounds.right + 3 + 2 * lift, bounds.bottom + 3 + 2 * lift },
        TILE_RADIUS, &back);
    bounds.top -= 2 * lift;
    bounds.bottom -= 2 * lift;
    tile_paint_at(tile, bounds, element->hovered, element->pressed && element->hovered);
#else
    tile_paint_at(tile, element->bounds, element->hovered, element->pressed && element->hovered);
#endif
}

static void tile_arrange(UiElement *element)
{
    PageTile *tile = (PageTile *)element;
    float height = element->bounds.bottom - element->bounds.top;
    if (tile->tall ? height < TALL_HEIGHT - TALL_HYSTERESIS : height >= TALL_HEIGHT + TALL_HYSTERESIS)
    {
        tile->tall = !tile->tall;
        // 两种样式交叉淡入，不随高度连续变化。
        anim_start(&tile->tall_anim, tile->tall ? 1 : 0, 0.2, EASE_OF(EASE_LINEAR));
        ui_window_request_frame(element->window);
    }
}

static void tile_mouse(UiElement *element, const UiMouseEvent *event)
{
    PageTile *tile = (PageTile *)element;
#ifdef LOOK_VARIANT_CARDS
    if (event->kind == UI_MOUSE_ENTER || event->kind == UI_MOUSE_LEAVE)
    {
        anim_start(&tile->lift, event->kind == UI_MOUSE_ENTER ? 1 : 0, 0.12, EASE_OF(EASE_LINEAR));
        ui_window_request_frame(element->window);
    }
#endif
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) flip_open(tile->page);
}

static void tile_destroy(UiElement *element)
{
    PageTile *tile = (PageTile *)element;
    free(tile->summary);
    strlist_clear(&tile->items);
#ifdef LOOK_VARIANT_CARDS
    strlist_clear(&tile->glance);
#endif
}

static const UiElementType tile_type = { NULL, tile_arrange, tile_paint, tile_mouse, tile_destroy, true };

void pages_paint_tile_face(UiElement *element, GfxMatrix matrix, float opacity)
{
    if (opacity <= 0.001f) return;
    PageTile *tile = (PageTile *)element;
    gfx_push_transform(matrix);
    if (opacity < 0.999f) gfx_push_opacity(opacity);
    tile_paint_at(tile, element->bounds, false, false);
    if (opacity < 0.999f) gfx_pop_layer();
    gfx_pop_transform();
}

static void tile_set_summary(UiElement *element, wchar_t *summary)
{
    PageTile *tile = (PageTile *)element;
    if (wcs_equal(tile->summary, summary))
    {
        free(summary);
        return;
    }
    free(tile->summary);
    tile->summary = summary;
    tile_split_summary(tile);
    ui_window_repaint(element->window);
}

bool pages_update_card_height(float viewport, bool resized)
{
    UiElement *page = M->overview_page;
    if (!page || page->desired.height <= 0 || viewport <= 0) return false;
    // 用量出来的高度减去卡片区域本身，结果与卡片当前的高度无关。
    float grid = M->tile_grid->desired.height + M->tile_grid->margin_top + M->tile_grid->margin_bottom;
    float rest = page->desired.height - grid;
    float spare = viewport - PAGE_TOP - PAGE_MARGIN - rest;
    float headroom = M->history_section->hidden ? 0 : FAN_HEADROOM;
    float cards = (spare - headroom) / 2 - TILE_GAP;
    cards = floorf(cards < MIN_CARD_HEIGHT ? MIN_CARD_HEIGHT : cards > MAX_CARD_HEIGHT ? MAX_CARD_HEIGHT : cards);
    if (fabsf(cards - O.card_target) < 1) return false;
    O.card_target = cards;
    // 拖动窗口边缘时直接跟随；只有页面内容变了（如收起入门提示）才平滑过渡，否则动画落后于窗口边缘，整页都在抖。
    if (!resized && M->tiles[0]->height > 0 && anim_enabled())
    {
        anim_start(&O.card_height, cards, MOTION_SECONDS, MOTION_EASE);
        main_request_frame();
        return false;
    }
    anim_set(&O.card_height, cards);
    for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->height = cards;
    return true;
}


// ───────────── 概览页 ─────────────

#ifdef LOOK_VARIANT_CARDS
/// 入门提示顶上的一行：左边是第几步，右上角的小图画出这一步怎么做（线条用墨色，重点用强调色）。tag 是第几步（1 到 3）。
static GfxSize tip_header_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 26 };
}

static void tip_rect(GfxRect rect, float angle, Color fill, bool dashed)
{
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    if (angle != 0) gfx_push_transform(gfx_matrix_rotate(angle, (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2));
    gfx_fill_rounded(rect, 2, fill);
    if (dashed) gfx_stroke_rounded_dashed(rect, 2, ink, 1, 2, 2);
    else gfx_stroke_rounded(rect, 2, ink, 1);
    if (angle != 0) gfx_pop_transform();
}

static void tip_header_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    int step = (int)(intptr_t)element->tag;
    wchar_t rank[2] = { (wchar_t)(L'0' + step), 0 };
    GfxTextStyle style = { GFX_FONT_RANK, 24, 600, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    gfx_text_draw(rank, bounds, &style, theme_color(THEME_TEXT_PRIMARY));
    // 72 × 24 的小图靠右，竖直居中。
    float x = bounds.right - 72, y = (bounds.top + bounds.bottom) / 2 - 12;
    Color accent = theme_color(THEME_ACCENT), card = theme_color(THEME_CARD);
    switch (step)
    {
        case 1:
        {
            float shake[] = { 2, 12, 9, 4, 17, 20, 25, 4, 33, 20, 40, 12 }, shaft[] = { 50, 12, 62, 12 }, head[] = { 58, 8, 62, 12, 58, 16 };
            line_art(shake, 6, x, y, 1);
            line_art(shaft, 2, x, y, 1);
            line_art(head, 3, x, y, 1);
            gfx_line(x + 69, y + 1, x + 69, y + 23, accent, 2.4f);
            break;
        }
        case 2:
        {
            tip_rect((GfxRect){ x + 2.5f, y + 5.5f, x + 16.5f, y + 22.5f }, -8, card, false);
            tip_rect((GfxRect){ x + 8.5f, y + 4.5f, x + 22.5f, y + 21.5f }, 6, card, false);
            tip_rect((GfxRect){ x + 5.5f, y + 4.5f, x + 19.5f, y + 21.5f }, 0, accent, false);
            float shaft[] = { 30, 13, 64, 13 }, head[] = { 60, 9, 64, 13, 60, 17 };
            line_art(shaft, 2, x, y, 1);
            line_art(head, 3, x, y, 1);
            break;
        }
        default:
        {
            float shaft[] = { 20, 1, 20, 10 }, head[] = { 16, 7, 20, 11, 24, 7 };
            line_art(shaft, 2, x, y, 1);
            line_art(head, 3, x, y, 1);
            tip_rect((GfxRect){ x + 4.5f, y + 13.5f, x + 35.5f, y + 23.5f }, 0, accent, false);
            tip_rect((GfxRect){ x + 38.5f, y + 13.5f, x + 69.5f, y + 23.5f }, 0, COLOR_TRANSPARENT, true);
            break;
        }
    }
}

static const UiElementType tip_header_type = { tip_header_measure, NULL, tip_header_paint, NULL, NULL, false };

/// 三张入门提示按顺序编号，右上角的小图画出这一步怎么做。
static UiElement *tip_card(int step, StringId title, StringId body)
#else
static UiElement *tip_card(const wchar_t *glyph, StringId title, StringId body)
#endif
{
    UiElement *card = ui_stack(false, 0);
#ifdef LOOK_VARIANT_CARDS
    ui_stack_set_padding(card, 14, 12, 16, 16);
#else
    ui_stack_set_padding(card, 16, 16, 16, 18);
#endif
    ui_stack_set_background(card, THEME_CARD, THEME_CARD_BORDER, 8);
#ifdef LOOK_VARIANT_CARDS
    UiElement *top = ui_element_new(&tip_header_type, sizeof(UiElement));
    top->tag = (void *)(intptr_t)step;
    ui_add(card, top);
#else
    UiElement *icon = ui_label(UI_LIT(glyph), UI_TEXT_GLYPH);
    ui_label_set_size(icon, 22);
    ui_label_set_color(icon, THEME_ACCENT_TEXT);
    icon->cross = UI_CROSS_START;
    ui_add(card, icon);
#endif
    UiElement *header = ui_label(UI_STR(title), UI_TEXT_BODY_STRONG);
    ui_label_style(header)->wrap = true;
#ifdef LOOK_VARIANT_CARDS
    ui_set_margin(header, 0, 8, 0, 4);
#else
    ui_set_margin(header, 0, 12, 0, 4);
#endif
    ui_add(card, header);
    ui_add(card, ui_label(UI_STR(body), UI_TEXT_CAPTION));
    return card;
}

#ifdef LOOK_VARIANT_CARDS
/// 概览页右上角的“打开置物架”：强调色按钮，文字后面跟着快捷键的键帽（淡一些）。
typedef struct OpenButton
{
    UiElement base;
    StrList keys;
} OpenButton;

static const GfxTextStyle open_label_style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static GfxSize open_button_measure(UiElement *element, float available)
{
    (void)available;
    OpenButton *button = (OpenButton *)element;
    GfxSize text = gfx_text_measure(S(STR_OpenShelf), &open_label_style, 0);
    float keys = button->keys.count > 0 ? 12 + keycaps_width(&button->keys) - 3 : 0;
    return (GfxSize){ 16 + text.width + keys + 10, text.height + 6 + 7 };
}

static void open_button_paint(UiElement *element, double time)
{
    (void)time;
    OpenButton *button = (OpenButton *)element;
    GfxRect bounds = element->bounds;
    bool pressed = element->pressed && element->hovered;
    gfx_fill_rounded(bounds, 6, theme_color(pressed ? THEME_ACCENT_PRESSED : element->hovered ? THEME_ACCENT_HOVER : THEME_ACCENT));
    Color text = theme_color(THEME_TEXT_ON_ACCENT);
    float width = gfx_text_measure(S(STR_OpenShelf), &open_label_style, 0).width;
    gfx_text_draw(S(STR_OpenShelf), (GfxRect){ bounds.left + 16, bounds.top, bounds.left + 16 + width + 1, bounds.bottom - 1 }, &open_label_style, text);
    if (button->keys.count == 0) return;
    gfx_push_opacity(0.75f);
    paint_keycaps(&button->keys, bounds.left + 16 + width + 12, (bounds.top + bounds.bottom) / 2, text);
    gfx_pop_layer();
}

static void open_button_mouse(UiElement *element, const UiMouseEvent *event)
{
    (void)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) pages_open_shelf(NULL);
}

static void open_button_destroy(UiElement *element)
{
    strlist_clear(&((OpenButton *)element)->keys);
}

static const UiElementType open_button_type = { open_button_measure, NULL, open_button_paint, open_button_mouse, open_button_destroy, true };
static UiElement *open_button;

/// 最近关闭的置物架下面的牌桌：铺满窗口宽度，从标题上方一点一直铺到窗口底边（被窗口底边裁掉）。
static void table_paint(UiElement *element, double time)
{
    (void)time;
    ThemePattern table = theme_pattern(THEME_PATTERN_TABLE);
    gfx_fill_rounded_pattern((GfxRect){ M->stage.left, element->bounds.top + 12, M->stage.right, element->bounds.top + 12 + 4000 }, 0, &table);
}

static GfxSize table_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 0 };
}

static const UiElementType table_type = { table_measure, NULL, table_paint, NULL, NULL, false };
static UiElement *history_tally;

#endif
void pages_overview_build(void)
{
    UiElement *page = ui_stack(false, 0);
    M->overview_page = page;

    // 标题行：窗口宽到显示左侧状态栏时，标题移到状态栏图标右侧，这一行收起。
    UiElement *header = ui_stack(true, 0);
    M->overview_header = header;
    UiElement *title = ui_label(UI_STR(STR_Overview), UI_TEXT_TITLE);
    title->cross = UI_CROSS_CENTER;
    title->grow = 1;
    ui_add(header, title);
#ifdef LOOK_VARIANT_CARDS
    UiElement *open = ui_element_new(&open_button_type, sizeof(OpenButton));
#else
    UiElement *open = ui_button(UI_BUTTON_ACCENT, UI_STR(STR_OpenShelf), pages_open_shelf, NULL);
    ui_button_set_glyph(open, GLYPH_ADD, 13);
    ui_button_set_padding(open, 16, 6, 16, 7);
#endif
    open->cross = UI_CROSS_CENTER;
#ifdef LOOK_VARIANT_CARDS
    open_button = open;
#endif
    ui_add(header, open);
    ui_add(page, header);

    UiElement *body = ui_stack(false, 0);
    M->overview_body = body;
    O.getting_started = ui_stack(false, 0);
    UiElement *getting_header = ui_stack(true, 0);
    UiElement *getting_title = ui_label(UI_STR(STR_GettingStarted), UI_TEXT_BODY_STRONG);
    ui_set_margin(getting_title, 2, 24, 0, 8);
    getting_title->grow = 1;
    ui_add(getting_header, getting_title);
    UiElement *dismiss = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_CLOSE), pages_dismiss_getting_started, NULL);
    dismiss->width = dismiss->height = 28;
    ui_button_set_font_size(dismiss, 10);
    dismiss->cross = UI_CROSS_END;
    ui_set_margin(dismiss, 0, 0, 0, 4);
    ui_set_tooltip(dismiss, UI_STR(STR_DismissTooltip));
    ui_add(getting_header, dismiss);
    ui_add(O.getting_started, getting_header);
    UiElement *tips = ui_grid(3, 9, 0);
#ifdef LOOK_VARIANT_CARDS
    ui_add(tips, tip_card(1, STR_TipShakeTitle, STR_TipShakeBody));
    ui_add(tips, tip_card(2, STR_TipStackTitle, STR_TipStackBody));
    ui_add(tips, tip_card(3, STR_QuickActionsTitle, STR_TipActionsBody));
#else
    ui_add(tips, tip_card(GLYPH_MOUSE, STR_TipShakeTitle, STR_TipShakeBody));
    ui_add(tips, tip_card(GLYPH_LIBRARY, STR_TipStackTitle, STR_TipStackBody));
    ui_add(tips, tip_card(GLYPH_LIGHTNING, STR_QuickActionsTitle, STR_TipActionsBody));
#endif
    ui_add(O.getting_started, tips);
    ui_add(body, O.getting_started);

    pages_section_header(body, STR_SettingsSection, 24, 8);
    M->tile_grid = ui_grid(2, TILE_GAP, TILE_GAP);
    ui_set_margin(M->tile_grid, 0, 0, 0, TILE_GAP);
    for (int index = 0; index < PAGE_COUNT; index++)
    {
        PageTile *tile = (PageTile *)ui_element_new(&tile_type, sizeof(PageTile));
        tile->page = index;
        tile->base.height = MIN_CARD_HEIGHT;
        M->tiles[index] = &tile->base;
        ui_add(M->tile_grid, &tile->base);
    }
    ui_add(body, M->tile_grid);
    ui_add(page, body);
    anim_set(&O.card_height, MIN_CARD_HEIGHT);
    O.card_target = MIN_CARD_HEIGHT;

    // 中间留空，最近关闭的置物架总是贴着窗口底边；下半截伸出页面，被窗口底边裁掉。
    ui_add(page, ui_spacer());
    M->history_section = ui_stack(false, 0);
#ifdef LOOK_VARIANT_CARDS
    ui_add(M->history_section, ui_element_new(&table_type, sizeof(UiElement)));
    // 标题和右侧“已保留几个 / 最多几个”都用牌桌上的文字颜色。
    UiElement *history_header = ui_stack(true, 0);
    ui_set_margin(history_header, 2, 24, 0, 8);
    UiElement *history_title = ui_label(UI_STR(STR_RecentShelves), UI_TEXT_BODY_STRONG);
    ui_label_set_color(history_title, THEME_TABLE_TEXT);
    history_title->grow = 1;
    ui_add(history_header, history_title);
    history_tally = ui_label(UI_LIT(L""), UI_TEXT_BODY);
    ui_label_style(history_tally)->font = GFX_FONT_NUMBER;
    ui_label_set_size(history_tally, 13);
    ui_label_set_color(history_tally, THEME_TABLE_TEXT);
    history_tally->cross = UI_CROSS_END;
    ui_set_margin(history_tally, 0, 0, 0, 1);
    ui_add(history_header, history_tally);
    ui_add(M->history_section, history_header);
#else
    pages_section_header(M->history_section, STR_RecentShelves, 24, 8);
#endif
    M->fan = fan_create();
    M->fan->height = 165;
    ui_set_margin(M->fan, 0, 4, 0, -36);
    ui_add(M->history_section, M->fan);
    ui_add(page, M->history_section);

    UiElement *column = pages_column_create();
    ui_add(column, page);
    M->overview_scroll = ui_scroll(column);
}

void pages_overview_refresh(const AppSettings *settings, bool has_hotkey, int history_count)
{
    O.getting_started->hidden = !settings->show_getting_started;

    // 概览页四张卡片上的当前设置。
    tile_set_summary(M->tiles[PAGE_GENERAL], str_general_summary(settings->language, settings->start_with_windows, has_hotkey ? settings->hotkey : NULL));
    tile_set_summary(M->tiles[PAGE_SHELF], str_shelf_summary(settings->auto_open_trigger, settings->drag_out_mode, settings->watched_folders.count));
    QuickActionList visible = quick_actions_visible(settings);
    StrList labels = {0};
#ifdef LOOK_VARIANT_CARDS
    PageTile *general = (PageTile *)M->tiles[PAGE_GENERAL], *actions = (PageTile *)M->tiles[PAGE_ACTIONS];
    hotkey_keys(&general->glance);
    strlist_clear(&actions->glance);
    for (int index = 0; index < visible.count; index++)
    {
        strlist_add_owned(&labels, quick_action_label(&visible.items[index]));
        strlist_add(&actions->glance, quick_action_glyph(&visible.items[index]));
    }
#else
    for (int index = 0; index < visible.count; index++) strlist_add_owned(&labels, quick_action_label(&visible.items[index]));
#endif
    quick_action_list_free(&visible);
    tile_set_summary(M->tiles[PAGE_ACTIONS], str_actions_summary(&labels));
    strlist_clear(&labels);
    tile_set_summary(M->tiles[PAGE_APPEARANCE], str_appearance_summary(settings->look, settings->theme, settings->accent_color));

#ifdef LOOK_VARIANT_CARDS
    wchar_t tally[32];
    swprintf(tally, ARRAYSIZE(tally), L"%d / %d", history_count, SHELF_HISTORY_CAPACITY);
    ui_label_set_text(history_tally, UI_LIT(tally));
    if (open_button)
    {
        OpenButton *button = (OpenButton *)open_button;
        StrList keys = {0};
        hotkey_keys(&keys);
        if (!strlist_equal(&keys, &button->keys, false))
        {
            strlist_clear(&button->keys);
            button->keys = keys;
            ui_window_invalidate(M->window);
        }
        else strlist_clear(&keys);
    }
#else
    (void)history_count;
#endif
}

void pages_overview_animate(double time)
{
    if (anim_update(&O.card_height, time))
    {
        main_request_frame();
        for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->height = floorf((float)O.card_height.value);
        main_relayout();
    }
    else if (M->tiles[0] && M->tiles[0]->height != (float)O.card_height.value)
    {
        for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->height = (float)O.card_height.value;
        main_relayout();
    }
}

void pages_overview_close(void)
{
    memset(&O, 0, sizeof(O));
#ifdef LOOK_VARIANT_CARDS
    open_button = history_tally = NULL;
#endif
}

UiElement *pages_overview_getting_started(void)
{
    return O.getting_started;
}

const wchar_t *pages_tile_summary(int page)
{
    return page >= 0 && page < PAGE_COUNT && M->tiles[page] ? ((PageTile *)M->tiles[page])->summary : NULL;
}

bool pages_tile_is_tall(int page)
{
    return page >= 0 && page < PAGE_COUNT && M->tiles[page] && ((PageTile *)M->tiles[page])->tall;
}

float pages_card_height(void)
{
    return (float)O.card_height.value;
}
