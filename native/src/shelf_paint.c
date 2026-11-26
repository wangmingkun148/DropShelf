// 置物架窗口的绘制逻辑（从 shelf_window.c 拆出），按风格各编译一遍（见 look_variant.h）。
#include "look_variant.h"
#include "shelf_window_internal.h"
#ifdef LOOK_VARIANT_CARDS
#include "shelf_card.h"
#include "card_face.h"
#endif
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef LOOK_VARIANT_CARDS
static GfxRect row_remove_rect(GfxRect row)
{
    return shelf_rect_of(row.right - 6 - 26, row.top + 5 + 3, 26, 26);
}
#endif

// ───────────── 画 ─────────────

static Color faded(Color color, float opacity)
{
    if (opacity >= 1) return color;
    if (opacity <= 0) return COLOR_TRANSPARENT;
    color.a = (uint8_t)lroundf(color.a * opacity);
    return color;
}

/// 置物架上的图标按钮：悬停时有淡淡的底色，按下时更淡。
static void paint_icon_button(ShelfWindow *shelf, GfxRect rect, ShelfTarget target, const wchar_t *glyph, float size, Color text)
{
    bool hovered = shelf->hovered == target, pressed = hovered && shelf->pressed == target;
#ifdef LOOK_VARIANT_CARDS
    gfx_fill_rounded(rect, 6, theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
#else
    gfx_fill_rounded(rect, 4, theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
#endif
    GfxTextStyle style = shelf_glyph_style(size);
    gfx_text_draw(glyph, rect, &style, text);
}

#ifdef LOOK_VARIANT_CARDS
/// 一张有底色、有描边的小牌（标题栏的“移动/复制”、底部的快速操作）：悬停时提亮、描边换成强调色，按下时变淡。
static void paint_chip_card(GfxRect rect, float radius, ThemeColor border, bool hovered, bool pressed, float opacity)
{
    if (pressed) opacity *= 0.8f;
    gfx_fill_rounded(rect, radius, faded(theme_color(hovered ? THEME_CARD_HOVER : THEME_CARD), opacity));
    gfx_stroke_rounded(rect, radius, faded(theme_color(hovered ? THEME_ACCENT : border), opacity), 1);
}

#endif
static void paint_header(ShelfWindow *shelf)
{
    const ShelfLayout *layout = &shelf->layout;
    // 置顶：开着时图标是强调色。（Wayland 版没有这个按钮，矩形是空的。）
    if (layout->pin.right > layout->pin.left)
        paint_icon_button(shelf, layout->pin, TARGET_PIN, shelf->model->pinned ? GLYPH_PINNED : GLYPH_PIN, 14,
            theme_color(shelf->model->pinned ? THEME_ACCENT_TEXT : THEME_TEXT_PRIMARY));

    // 移动/复制：和普通按钮一样有底色和边框，一眼看出能点。
    bool hovered = shelf->hovered == TARGET_MODE, pressed = hovered && shelf->pressed == TARGET_MODE;
#ifdef LOOK_VARIANT_CARDS
    paint_chip_card(layout->mode, 4, THEME_CONTROL_STRONG_BORDER, hovered, pressed, 1);
    Color text = faded(theme_color(THEME_TEXT_PRIMARY), pressed ? 0.8f : 1);
#else
    gfx_fill_rounded(layout->mode, 4, theme_color(pressed ? THEME_CONTROL_PRESSED : hovered ? THEME_CONTROL_HOVER : THEME_CONTROL));
    gfx_stroke_rounded(layout->mode, 4, theme_color(THEME_CONTROL_BORDER), 1);
    Color text = theme_color(pressed ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY);
#endif
    const wchar_t *glyph = shelf->move_mode ? GLYPH_MOVE : GLYPH_COPY;
    float glyph_width = shelf_text_width(glyph, 12, 400, GFX_FONT_ICON);
    GfxTextStyle icon = shelf_glyph_style(12);
#ifdef LOOK_VARIANT_CARDS
    gfx_text_draw(glyph, shelf_rect_of(layout->mode.left + 8, layout->mode.top + 1, glyph_width, 23), &icon, text);
#else
    gfx_text_draw(glyph, shelf_rect_of(layout->mode.left + 7, layout->mode.top + 1, glyph_width, 23), &icon, text);
#endif
    GfxTextStyle label = shelf_text_style(12, 400, GFX_ALIGN_START);
    gfx_text_draw(S(shelf->move_mode ? STR_TransferMove : STR_TransferCopy),
#ifdef LOOK_VARIANT_CARDS
        (GfxRect){ layout->mode.left + 8 + glyph_width + 5, layout->mode.top, layout->mode.right, layout->mode.bottom - 1 }, &label, text);
#else
        (GfxRect){ layout->mode.left + 7 + glyph_width + 5, layout->mode.top, layout->mode.right, layout->mode.bottom - 1 }, &label, text);
#endif

    // 收起时堆叠下方已显示名称和数量，标题只在展开列表时显示。
    if (shelf->model->expanded)
    {
        wchar_t *title = shelf_model_title(shelf->model);
        GfxTextStyle style = shelf_text_style(12, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(title, layout->title, &style, theme_color(THEME_TEXT_SECONDARY));
        free(title);
    }

    Color primary = theme_color(THEME_TEXT_PRIMARY);
    if (shelf->model->count > 0)
#ifdef LOOK_VARIANT_CARDS
        paint_icon_button(shelf, layout->view, TARGET_VIEW, shelf->model->expanded ? GLYPH_COLLAPSE : GLYPH_SPREAD, 14,
#else
        paint_icon_button(shelf, layout->view, TARGET_VIEW, shelf->model->expanded ? GLYPH_COLLAPSE : GLYPH_LIST, 14,
#endif
            shelf->pressed == TARGET_VIEW && shelf->hovered == TARGET_VIEW ? theme_color(THEME_TEXT_SECONDARY) : primary);
    paint_icon_button(shelf, layout->more, TARGET_MORE, GLYPH_MORE, 14,
        shelf->pressed == TARGET_MORE && shelf->hovered == TARGET_MORE ? theme_color(THEME_TEXT_SECONDARY) : primary);
    paint_icon_button(shelf, layout->close, TARGET_CLOSE, GLYPH_CLOSE, 11,
        shelf->pressed == TARGET_CLOSE && shelf->hovered == TARGET_CLOSE ? theme_color(THEME_TEXT_SECONDARY) : primary);
}

static void paint_empty(ShelfWindow *shelf, float opacity)
{
    GfxRect body = shelf->layout.body;
#ifdef LOOK_VARIANT_CARDS
    float first = shelf_line_height(13, 400), second = shelf_line_height(11, 400);
    float total = 72 + 10 + first + 3 + second;
    float top = (body.top + body.bottom - total) / 2, center = (body.left + body.right) / 2;
    Color ink = faded(theme_color(THEME_TEXT_PRIMARY), opacity);
    GfxRect slot = { center - 27, top, center + 27, top + 72 };
    gfx_stroke_rounded_dashed(slot, 5, ink, 1.5f, 2, 1.3333f);
    float middle = top + 36;
    gfx_line(center, middle - 7, center, middle + 7, ink, 1.4f);
    gfx_line(center - 7, middle, center + 7, middle, ink, 1.4f);
    top += 72 + 10;
#else
    gfx_stroke_rounded_dashed(body, 10, faded(theme_color(THEME_CONTROL_STRONG_BORDER), 0.45f * opacity), 1.2f, 4, 3);
    float glyph = shelf_line_height(26, 400) , first = shelf_line_height(13, 400), second = shelf_line_height(11, 400);
    GfxTextStyle icon = shelf_glyph_style(26);
    GfxSize glyph_size = gfx_text_measure(GLYPH_DOWNLOAD, &icon, 0);
    glyph = glyph_size.height > 0 ? glyph_size.height : glyph;
    float total = glyph + 10 + first + 3 + second;
    float top = (body.top + body.bottom - total) / 2;
    gfx_text_draw(GLYPH_DOWNLOAD, (GfxRect){ body.left, top, body.right, top + glyph }, &icon, faded(theme_color(THEME_TEXT_SECONDARY), opacity));
    top += glyph + 10;
#endif
    GfxTextStyle style = shelf_text_style(13, 400, GFX_ALIGN_CENTER);
#ifdef LOOK_VARIANT_CARDS
    gfx_text_draw(S(STR_DropHere), (GfxRect){ body.left + 8, top, body.right - 8, top + first }, &style, ink);
#else
    gfx_text_draw(S(STR_DropHere), (GfxRect){ body.left + 8, top, body.right - 8, top + first }, &style, faded(theme_color(THEME_TEXT_PRIMARY), opacity));
#endif
    top += first + 3;
    style.size = 11;
#ifdef LOOK_VARIANT_CARDS
    gfx_text_draw(S(STR_OrPaste), (GfxRect){ body.left + 8, top, body.right - 8, top + second }, &style, faded(theme_color(THEME_TEXT_SECONDARY), opacity));
#else
    gfx_text_draw(S(STR_OrPaste), (GfxRect){ body.left + 8, top, body.right - 8, top + second }, &style, faded(theme_color(THEME_TEXT_TERTIARY), opacity));
#endif
}

/// 牌上的内容：缩略图按比例放进去，没有缩略图时是类型图标；已删除或移动的项目内容变淡。
static void paint_item_picture(ShelfItem *item, GfxRect area, float glyph_size, float opacity)
{
    float content = item->available ? opacity : opacity * 0.5f;
    if (item->thumbnail)
    {
        float width = (float)gfx_image_width(item->thumbnail), height = (float)gfx_image_height(item->thumbnail);
        float ratio = fminf((area.right - area.left) / width, (area.bottom - area.top) / height);
        float w = width * ratio, h = height * ratio;
        float x = (area.left + area.right - w) / 2, y = (area.top + area.bottom - h) / 2;
        gfx_draw_image(item->thumbnail, (GfxRect){ x, y, x + w, y + h }, content);
        return;
    }
    GfxTextStyle style = shelf_glyph_style(glyph_size);
    gfx_text_draw(shelf_item_glyph(item), area, &style, faded(theme_color(THEME_TEXT_SECONDARY), content));
}

#ifdef LOOK_VARIANT_CARDS
/// 一张竖版的牌，中心在 (0, 0)：牌面上印着缩略图。下面几张换成牌桌上的牌面底色和下层牌的描边。
static void paint_card_face(ShelfItem *item, bool top)
{
    float half_width = SHELF_CARD_WIDTH / 2, half_height = SHELF_CARD_HEIGHT / 2;
    GfxRect rect = { -half_width, -half_height, half_width, half_height };
    gfx_fill_rounded(rect, SHELF_CARD_RADIUS, theme_color(top ? THEME_SHELF_TILE : THEME_CARD));
    gfx_stroke_rounded(rect, SHELF_CARD_RADIUS, theme_color(top ? THEME_SHELF_TILE_BORDER : THEME_SHELF_UNDER_CARD_BORDER), 1.2f);
    if (item) paint_item_picture(item, (GfxRect){ rect.left + 7, rect.top + 20, rect.right - 7, rect.bottom - 16 }, 26, 1);
}

static GfxMatrix canvas_matrix(const ShelfLayout *layout, CardPose pose, float canvas_x, float canvas_y, float canvas_scale)
{
    (void)layout;
    GfxMatrix matrix = gfx_matrix_scale(pose.scale, pose.scale, 0, 0);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(pose.angle, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(pose.x, pose.y));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_scale(canvas_scale, canvas_scale, 0, 0));
    return gfx_matrix_multiply(matrix, gfx_matrix_translate(canvas_x, canvas_y));
}

#endif
static void paint_card(ShelfWindow *shelf, int card, ShelfItem *item)
{
    CardPose pose = shelf->card_posed[card] ? shelf->card_pose[card] : shelf_card_home(card);
    float opacity = pose.opacity * (float)item->dim.value;
    if (opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
#ifdef LOOK_VARIANT_CARDS
    gfx_push_transform(canvas_matrix(layout, pose, layout->canvas_x, layout->canvas_y, layout->canvas_scale));
#else
    GfxMatrix matrix = gfx_matrix_scale(pose.scale, pose.scale, 0, 0);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(pose.angle, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(pose.x, pose.y));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_scale(layout->canvas_scale, layout->canvas_scale, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(layout->canvas_x, layout->canvas_y));
    gfx_push_transform(matrix);
#endif
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
#ifdef LOOK_VARIANT_CARDS
    paint_card_face(item, card == 0);
#else
    float half = SHELF_CARD_SIZE / 2;
    GfxRect rect = { -half, -half, half, half };
    gfx_shadow(rect, 10, 10, 0, 1.5f, COLOR_ARGB(41, 0, 0, 0));
    gfx_fill_rounded(rect, 10, theme_color(THEME_SHELF_TILE));
    gfx_stroke_rounded(rect, 10, theme_color(THEME_SHELF_TILE_BORDER), 1);
    paint_item_picture(item, (GfxRect){ -half + 10, -half + 10, half - 10, half - 10 }, 34, 1);
#endif
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

#ifdef LOOK_VARIANT_CARDS
/// 最上面一张的牌角：项目数和数量最多的文件类型，右下角倒过来再印一个；裁在牌面以内。
static void paint_rank(ShelfWindow *shelf, float opacity)
#else
static void paint_badge(ShelfWindow *shelf, float opacity)
#endif
{
#ifdef LOOK_VARIANT_CARDS
    if (shelf->rank_hidden || opacity <= 0.001f || shelf->model->count == 0) return;
#else
    int count = shelf->mark_override ? shelf->mark_count : shelf->model->count;
    if (count < 2 || opacity <= 0) return;
#endif
    const ShelfLayout *layout = &shelf->layout;
#ifdef LOOK_VARIANT_CARDS
    wchar_t count[16];
    swprintf(count, ARRAYSIZE(count), L"%d", shelf->model->count);
    if (!shelf->rank_suit_known)
    {
        shelf->rank_suit = shelf_model_suit(shelf->model);
        shelf->rank_suit_known = true;
    }
    FileSuit suit = shelf->rank_suit;
    gfx_push_transform(gfx_matrix_multiply(gfx_matrix_scale(layout->canvas_scale, layout->canvas_scale, 0, 0),
        gfx_matrix_translate(layout->canvas_x, layout->canvas_y)));
    GfxRect card = { 48, 12, 48 + SHELF_CARD_WIDTH, 12 + SHELF_CARD_HEIGHT };
    gfx_push_clip(card);
    static const CardCorner corner = { 15, 15, 6, 2 };
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    // 新牌落进来时数字从下面滚上来；花色和倒过来的那一个不动。
    card_paint_corner_rolling(count, suit, &corner, card.left + 5, card.top + 4, ink, opacity, shelf->rank_shift, shelf->rank_opacity);
    card_paint_corner_inverted(count, suit, &corner, card, 5, 4, ink, opacity);
    gfx_pop_clip();
#else
    wchar_t text[16];
    swprintf(text, 16, L"%d", count);
    float width = fmaxf(24, shelf_text_width(text, 11, 600, GFX_FONT_TEXT) + 12 + 4);
    GfxRect rect = shelf_rect_of(106, 6, width, 24);
    float center_x = 106 + width / 2, center_y = 18;
    GfxMatrix matrix = gfx_matrix_scale(shelf->badge_scale, shelf->badge_scale, center_x, center_y);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_scale(layout->canvas_scale, layout->canvas_scale, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(layout->canvas_x, layout->canvas_y));
    gfx_push_transform(matrix);
    gfx_fill_rounded(rect, 12, faded(theme_color(THEME_ACCENT), opacity));
    gfx_stroke_rounded(rect, 12, faded(theme_color(THEME_SHELF_TILE), opacity), 2);
    GfxTextStyle style = shelf_text_style(11, 600, GFX_ALIGN_CENTER);
    style.ellipsis = false;
    gfx_text_draw(text, rect, &style, faded(theme_color(THEME_TEXT_ON_ACCENT), opacity));
#endif
    gfx_pop_transform();
}

static void paint_stack(ShelfWindow *shelf, float opacity)
{
    if (opacity < 0.999f) gfx_push_opacity(opacity);
    for (int card = 2; card >= 0; card--)
    {
        ShelfItem *item = shelf_model_from_end(shelf->model, card + 1);
        if (item) paint_card(shelf, card, item);
    }
#ifdef LOOK_VARIANT_CARDS
    paint_rank(shelf, shelf->mark_opacity);
#else
    paint_badge(shelf, shelf->mark_opacity);
#endif
    if (shelf->mark_opacity > 0)
    {
        const wchar_t *caption, *detail;
        if (shelf->model->count == 1)
        {
            ShelfItem *item = shelf->model->items[0];
            caption = shelf_item_display_name(item);
            detail = shelf_item_detail(item);
        }
        else
        {
            if (!shelf->stack_caption)
            {
                shelf->stack_caption = shelf_model_stack_caption(shelf->model);
                shelf->stack_detail = shelf_model_stack_detail(shelf->model);
            }
            caption = shelf->stack_caption;
            detail = shelf->stack_detail;
        }
        GfxTextStyle style = shelf_text_style(13, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(caption, shelf->layout.caption, &style, faded(theme_color(THEME_TEXT_PRIMARY), shelf->mark_opacity));
        style = shelf_text_style(11, 400, GFX_ALIGN_CENTER);
        gfx_text_draw(detail, shelf->layout.detail, &style, faded(theme_color(THEME_TEXT_SECONDARY), shelf->mark_opacity));
    }
    if (opacity < 0.999f) gfx_pop_layer();
}

#ifdef LOOK_VARIANT_CARDS
/// 平铺里的一张牌：竖版小牌印着缩略图，牌角印扩展名和花色，名字最多两行写在下面；悬停时抬起，选中时换成强调色描边。
static void paint_tile(ShelfWindow *shelf, int index, ShelfItem *item)
#else
static void paint_row(ShelfWindow *shelf, int index, ShelfItem *item)
#endif
{
    const ShelfItemMotion *motion = &item->motion;
#ifdef LOOK_VARIANT_CARDS
    float opacity = motion->opacity * (float)item->dim.value;
#else
    float opacity = motion->row_opacity * (float)item->dim.value * (item->available ? 1.0f : 0.5f);
#endif
    if (opacity <= 0.001f) return;
#ifdef LOOK_VARIANT_CARDS
    GfxRect slot = shelf_tile_rect(shelf, index);
    GfxRect card = shelf_tile_card_rect(shelf, index);
    // 以牌面中心为原点缩放、转动，再平移；名字跟着一起动。
    float cx = (card.left + card.right) / 2, cy = (card.top + card.bottom) / 2;
    GfxMatrix matrix = gfx_matrix_scale(motion->scale, motion->scale, cx, cy);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(motion->angle, cx, cy));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(motion->dx, motion->dy));
    gfx_push_transform(matrix);
#else
    GfxRect row = shelf_row_rect(shelf, index);
    gfx_push_transform(gfx_matrix_translate(motion->row_dx, motion->row_dy));
#endif
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
#ifdef LOOK_VARIANT_CARDS
    bool hovered = shelf->hovered_row == index && (shelf->hovered == TARGET_ROW || shelf->hovered == TARGET_ROW_REMOVE) && !shelf->switching;
    // 牌抬起时露出的桌面投影。
    float lift = (float)item->lift.value;
    if (hovered) gfx_fill_rounded(card, SHELF_CARD_RADIUS, faded(theme_color(THEME_SHELF_UNDER_CARD_BORDER), 0.18f));
    GfxRect face = { card.left, card.top - 4 * lift, card.right, card.bottom - 4 * lift };
    gfx_fill_rounded(face, SHELF_CARD_RADIUS, theme_color(THEME_SHELF_TILE));
    if (item->selected) gfx_stroke_rounded(face, SHELF_CARD_RADIUS, theme_color(THEME_ACCENT), 2);
    else gfx_stroke_rounded(face, SHELF_CARD_RADIUS, theme_color(THEME_SHELF_TILE_BORDER), 1.2f);
    paint_item_picture(item, (GfxRect){ face.left + 7, face.top + 20, face.right - 7, face.bottom - 16 }, 26, 1);
    wchar_t *mark = shelf_item_card_mark(item);
    static const CardCorner corner = { 10, 10, 5, 2 };
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    FileSuit suit = shelf_item_suit(item);
    card_paint_corner(mark, suit, &corner, face.left + 4, face.top + 4, ink, 1);
    card_paint_corner_inverted(mark, suit, &corner, face, 4, 4, ink, 1);
    free(mark);
    // 右上角的移除：墨色小圆里一个叉，悬停时变红。
    if (hovered)
#else
    bool hovered = shelf->hovered_row == index && (shelf->hovered == TARGET_ROW || shelf->hovered == TARGET_ROW_REMOVE);
    ThemeColor fill = item->selected ? THEME_ACCENT_SUBTLE : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT;
    gfx_fill_rounded(row, 6, theme_color(fill));

    // 行首图标：落下的动画里它单独飞进来。
    GfxRect thumb = shelf_rect_of(row.left + 6, row.top + 5, 32, 32);
    if (motion->thumb_opacity > 0.001f)
    {
        float cx = (thumb.left + thumb.right) / 2, cy = (thumb.top + thumb.bottom) / 2;
        GfxMatrix matrix = gfx_matrix_scale(motion->thumb_scale, motion->thumb_scale, cx, cy);
        matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(motion->thumb_angle, cx, cy));
        matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(motion->thumb_dx, motion->thumb_dy));
        gfx_push_transform(matrix);
        paint_item_picture(item, thumb, 20, motion->thumb_opacity);
        gfx_pop_transform();
    }

    float text_right = row.right - 6 - 26 - 4;
    if (motion->text_opacity > 0.001f)
#endif
    {
#ifdef LOOK_VARIANT_CARDS
        GfxRect remove = shelf_tile_remove_rect(face);
        float x = (remove.left + remove.right) / 2, y = (remove.top + remove.bottom) / 2;
#else
        float name_height = shelf_line_height(13, 400), detail_height = shelf_line_height(11, 400);
        float top = thumb.top + (32 - name_height - detail_height) / 2;
        float left = row.left + 6 + 36 + 6 + motion->text_dx;
        GfxTextStyle style = shelf_text_style(13, 400, GFX_ALIGN_START);
        gfx_text_draw(shelf_item_display_name(item), (GfxRect){ left, top, text_right + motion->text_dx, top + name_height }, &style,
            faded(theme_color(THEME_TEXT_PRIMARY), motion->text_opacity));
        style = shelf_text_style(11, 400, GFX_ALIGN_START);
        gfx_text_draw(shelf_item_detail(item), (GfxRect){ left, top + name_height, text_right + motion->text_dx, top + name_height + detail_height },
            &style, faded(theme_color(THEME_TEXT_SECONDARY), motion->text_opacity));
    }

    // 行尾的移除按钮，悬停在这一行上时才显示。
    if (hovered && !shelf->switching)
    {
        GfxRect remove = row_remove_rect(row);
#endif
        bool over = shelf->hovered == TARGET_ROW_REMOVE;
#ifdef LOOK_VARIANT_CARDS
        Color paper = theme_color(THEME_SHELF_SOLID);
        gfx_fill_ellipse(x, y, 9, 9, theme_color(over ? THEME_CRITICAL : THEME_TEXT_PRIMARY));
        gfx_stroke_ellipse(x, y, 8.25f, 8.25f, paper, 1.5f);
        gfx_line(x - 3, y - 3, x + 3, y + 3, paper, 1.4f);
        gfx_line(x + 3, y - 3, x - 3, y + 3, paper, 1.4f);
    }
    // 名字：牌下面，最多两行；选中时铺一层淡淡的强调色。
    float name_opacity = motion->name_opacity * (item->available ? 1.0f : 0.5f);
    if (name_opacity > 0.001f)
    {
        GfxTextStyle name = { GFX_FONT_TEXT, 11, 400, true, true, GFX_ALIGN_CENTER, GFX_ALIGN_START, 14 };
        GfxSize size = gfx_text_measure(shelf_item_display_name(item), &name, SHELF_SLOT_WIDTH - 6);
        float height = fminf(size.height, 28), width = fminf(size.width, SHELF_SLOT_WIDTH - 6);
        float top = card.bottom + 5;
        if (item->selected)
        {
            float middle = (slot.left + slot.right) / 2;
            gfx_fill_rounded((GfxRect){ middle - width / 2 - 3, top, middle + width / 2 + 3, top + height }, 4,
                faded(theme_color(THEME_ACCENT_SUBTLE), name_opacity));
        }
        gfx_text_draw(shelf_item_display_name(item), (GfxRect){ slot.left + 3, top, slot.right - 3, top + height }, &name,
            faded(theme_color(THEME_TEXT_PRIMARY), name_opacity));
#else
        bool down = over && shelf->pressed == TARGET_ROW_REMOVE;
        gfx_fill_rounded(remove, 4, theme_color(down ? THEME_SUBTLE_PRESSED : over ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
        GfxTextStyle style = shelf_glyph_style(10);
        gfx_text_draw(GLYPH_CLOSE, remove, &style, theme_color(down ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY));
#endif
    }
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

#ifdef LOOK_VARIANT_CARDS
static void paint_grid(ShelfWindow *shelf, float opacity)
#else
static void paint_list(ShelfWindow *shelf, float opacity)
#endif
{
    const ShelfLayout *layout = &shelf->layout;
    GfxRect list = layout->list;
    // 列表里的内容只显示在列表范围以内。
    gfx_push_clip((GfxRect){ list.left + 1, list.top + 1, list.right - 1, list.bottom - 1 });
    if (opacity < 0.999f) gfx_push_opacity(opacity);
#ifdef LOOK_VARIANT_CARDS
    int first, last;
    shelf_visible_tiles(shelf, &first, &last);
    for (int index = first; index <= last; index++) paint_tile(shelf, index, shelf->model->items[index]);
#else
    for (int index = shelf->scroll_first > 0 ? shelf->scroll_first - 1 : 0; index < shelf->model->count; index++)
    {
        GfxRect row = shelf_row_rect(shelf, index);
        if (row.top > list.bottom + 60) break;
        paint_row(shelf, index, shelf->model->items[index]);
    }
#endif
    if (layout->scrollable)
    {
        float alpha = shelf->scroll_hovered || shelf->scroll_dragging ? 1.0f : 0.55f;
        gfx_fill_rounded(layout->scroll_thumb, 3, faded(theme_color(THEME_SCROLL_THUMB), alpha));
    }
    if (opacity < 0.999f) gfx_pop_layer();
    gfx_pop_clip();
}

/// 快速操作按钮栏：上面一条分隔线；只有两个操作时按钮上写名称，四个时只有图标。
static void paint_action_bar(ShelfWindow *shelf, float opacity)
{
    int count = shelf->actions.count;
    if (count == 0 || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
#ifndef LOOK_VARIANT_CARDS
    gfx_fill_rect(layout->action_divider, faded(theme_color(THEME_DIVIDER), opacity));
#endif
    bool large = count <= 2;
    for (int index = 0; index < count; index++)
    {
        GfxRect button = layout->action_buttons[index];
        bool hovered = shelf->hovered == TARGET_ACTION && shelf->hovered_row == index;
        bool pressed = hovered && shelf->pressed == TARGET_ACTION && shelf->pressed_row == index;
#ifdef LOOK_VARIANT_CARDS
        paint_chip_card(button, 5, THEME_SHELF_UNDER_CARD_BORDER, hovered, pressed, opacity);
        Color text = faded(theme_color(THEME_TEXT_PRIMARY), opacity * (pressed ? 0.8f : 1));
#else
        gfx_fill_rounded(button, 4, faded(theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT), opacity));
        Color text = faded(theme_color(pressed ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY), opacity);
#endif
        const wchar_t *glyph = quick_action_glyph(&shelf->actions.items[index]);
        if (!large)
        {
            GfxTextStyle icon = shelf_glyph_style(16);
            gfx_text_draw(glyph, button, &icon, text);
            continue;
        }
        // 图标和名称作为一组居中，放不下时名称末尾省略。
        const wchar_t *label = shelf->action_labels[index] ? shelf->action_labels[index] : L"";
        float glyph_width = shelf_text_width(glyph, 15, 400, GFX_FONT_ICON);
        float available = button.right - button.left - 8 - glyph_width - 6;
        float label_width = fminf(shelf_text_width(label, 12, 400, GFX_FONT_TEXT), available > 0 ? available : 0);
        float left = (button.left + button.right - glyph_width - 6 - label_width) / 2;
        GfxTextStyle icon = shelf_glyph_style(15);
        gfx_text_draw(glyph, (GfxRect){ left, button.top + 1, left + glyph_width, button.bottom + 1 }, &icon, text);
        GfxTextStyle style = shelf_text_style(12, 400, GFX_ALIGN_START);
        gfx_text_draw(label, (GfxRect){ left + glyph_width + 6, button.top, left + glyph_width + 6 + label_width + 1, button.bottom }, &style, text);
    }
}

/// 拖入时展开的快速操作卡片：图标在上，名称在下（最多两行）；拖到上面的那张铺上强调色、略微放大。
static void paint_tiles(ShelfWindow *shelf, float opacity)
{
    int count = shelf->actions.count;
    if (count == 0 || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    bool large = count <= 2;
    gfx_push_clip(layout->tiles_host);
    gfx_push_opacity(opacity);
    for (int index = 0; index < count; index++)
    {
        GfxRect tile = layout->tiles[index];
        float active = (float)shelf->tile_highlight[index].value;
        float scale = 1 + 0.04f * active;
        float cx = (tile.left + tile.right) / 2, cy = (tile.top + tile.bottom) / 2;
        gfx_push_transform(gfx_matrix_scale(scale, scale, cx, cy));
#ifdef LOOK_VARIANT_CARDS
        gfx_fill_rounded(tile, 6, theme_color(THEME_CARD));
        gfx_stroke_rounded(tile, 6, theme_color(THEME_SHELF_UNDER_CARD_BORDER), 1);
#else
        gfx_fill_rounded(tile, 10, theme_color(THEME_CARD));
        gfx_stroke_rounded(tile, 10, theme_color(THEME_CARD_BORDER), 1);
#endif
        if (active > 0.001f)
        {
            // 强调色的一层连边框一起盖住。
            GfxRect cover = { tile.left - 0.5f, tile.top - 0.5f, tile.right + 0.5f, tile.bottom + 0.5f };
#ifdef LOOK_VARIANT_CARDS
            gfx_fill_rounded(cover, 6.5f, faded(theme_color(THEME_ACCENT), active));
#else
            gfx_fill_rounded(cover, 10.5f, faded(theme_color(THEME_ACCENT), active));
#endif
        }
        Color text = color_lerp(theme_color(THEME_TEXT_PRIMARY), theme_color(THEME_TEXT_ON_ACCENT), active);
        const wchar_t *glyph = quick_action_glyph(&shelf->actions.items[index]);
        const wchar_t *label = shelf->action_labels[index] ? shelf->action_labels[index] : L"";
        GfxTextStyle icon = shelf_glyph_style(large ? 22.0f : 20.0f);
        float glyph_height = gfx_text_measure(glyph, &icon, 0).height;
        float padding = large ? 6.0f : 2.0f;
        float label_width = tile.right - tile.left - 2 - padding * 2;
        GfxTextStyle wrap = { GFX_FONT_TEXT, 12, 400, true, true, GFX_ALIGN_CENTER, GFX_ALIGN_START, 15 };
        float label_height = fminf(gfx_text_measure(label, &wrap, label_width > 1 ? label_width : 1).height, 30);
        float top = cy - (glyph_height + 8 + label_height) / 2;
        gfx_text_draw(glyph, (GfxRect){ tile.left, top, tile.right, top + glyph_height }, &icon, text);
        top += glyph_height + 8;
        gfx_text_draw(label, (GfxRect){ tile.left + 1 + padding, top, tile.right - 1 - padding, top + label_height }, &wrap, text);
        gfx_pop_transform();
    }
    gfx_pop_layer();
    gfx_pop_clip();
}

static void paint_drop_overlay(ShelfWindow *shelf, float opacity)
{
#ifdef LOOK_VARIANT_CARDS
    const ShelfLayout *layout = &shelf->layout;
    GfxRect zone = layout->drop_zone;
#else
    GfxRect zone = shelf->layout.drop_zone;
#endif
    float cx = (zone.left + zone.right) / 2, cy = (zone.top + zone.bottom) / 2;
    // 可以合并时整个放置区域轻轻弹一下。
    float pulse = (float)shelf->drop_zone_scale.value;
    bool pulsing = fabsf(pulse - 1) > 0.0005f;
    if (pulsing) gfx_push_transform(gfx_matrix_scale(pulse, pulse, cx, cy));
    // 合并前的蓄力：拖着别的置物架停在这里时，底色从中间涨满。
    float charge = opacity * (float)shelf->merge_charge_opacity.value;
    if (charge > 0.001f)
    {
        float scale = (float)shelf->merge_charge_scale.value;
        float half_width = (zone.right - zone.left) / 2 * scale, half_height = (zone.bottom - zone.top) / 2 * scale;
#ifdef LOOK_VARIANT_CARDS
        gfx_fill_rounded((GfxRect){ cx - half_width, cy - half_height, cx + half_width, cy + half_height }, 8 * scale,
#else
        gfx_fill_rounded((GfxRect){ cx - half_width, cy - half_height, cx + half_width, cy + half_height }, 10 * scale,
#endif
            faded(theme_color(THEME_ACCENT_SUBTLE), charge));
    }
#ifndef LOOK_VARIANT_CARDS
    // 拖到快速操作上时虚线框和底色变淡，文字不变。
    float frame = opacity * (float)shelf->frame_opacity.value;
    gfx_fill_rounded(zone, 10, faded(theme_color(THEME_ACCENT_FAINT), frame));
    gfx_stroke_rounded_dashed(zone, 10, faded(theme_color(THEME_ACCENT), frame), 1.6f, 4, 3);
#endif
    float label = opacity * (float)shelf->drop_label_opacity.value;
    if (label > 0.001f)
    {
#ifdef LOOK_VARIANT_CARDS
        // 拖到快速操作上时整组牌变淡，文字不变。
        float group = label * (float)shelf->frame_opacity.value;
        float progress = (float)shelf->fan.value;
        gfx_push_opacity(group);
        for (int index = 1; index >= 0; index--)
        {
            ShelfItem *item = shelf_model_from_end(shelf->model, index + 1);
            if (!item) continue;
            CardPose pose = shelf_fan_pose(index, progress);
            gfx_push_transform(canvas_matrix(layout, pose, layout->drop_canvas_x, layout->drop_canvas_y, layout->drop_canvas_scale));
            paint_card_face(item, index == 0);
            gfx_pop_transform();
        }
        // 虚线空位：从略小、略低的地方浮起来。
        float slot_scale = 0.9f + 0.1f * progress, slot_shift = 6 - 9 * progress;
        CardPose slot = { 56 + 32, 10 + 43 + slot_shift, 0, slot_scale, 1 };
        gfx_push_transform(canvas_matrix(layout, slot, layout->drop_canvas_x, layout->drop_canvas_y, layout->drop_canvas_scale));
        gfx_push_opacity(progress);
        GfxRect ghost = { -SHELF_CARD_WIDTH / 2, -SHELF_CARD_HEIGHT / 2, SHELF_CARD_WIDTH / 2, SHELF_CARD_HEIGHT / 2 };
        gfx_fill_rounded(ghost, SHELF_CARD_RADIUS, theme_color(THEME_ACCENT_FAINT));
        gfx_stroke_rounded_dashed(ghost, SHELF_CARD_RADIUS, theme_color(THEME_ACCENT), 1.5f, 2, 1.3333f);
        gfx_pop_layer();
        gfx_pop_transform();
        if (shelf->incoming > 0)
        {
            wchar_t text[16];
            swprintf(text, ARRAYSIZE(text), L"+%d", shelf->incoming);
            GfxTextStyle number = { GFX_FONT_NUMBER, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
            float width = shelf_text_width(text, 12, 400, GFX_FONT_NUMBER) + 14;
            gfx_push_transform(gfx_matrix_multiply(gfx_matrix_scale(layout->drop_canvas_scale, layout->drop_canvas_scale, 0, 0),
                gfx_matrix_translate(layout->drop_canvas_x, layout->drop_canvas_y)));
            GfxRect badge = { 132, 0, 132 + width, 20 };
            gfx_fill_rounded(badge, 10, theme_color(THEME_ACCENT));
            gfx_text_draw(text, badge, &number, theme_color(THEME_TEXT_ON_ACCENT));
            gfx_pop_transform();
        }
        gfx_pop_layer();
        GfxTextStyle style = shelf_text_style(12, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(S(shelf->drop_zone_text), layout->drop_text, &style, faded(theme_color(THEME_TEXT_PRIMARY), label));
#else
        GfxTextStyle icon = shelf_glyph_style(22);
        float glyph = gfx_text_measure(GLYPH_DOWNLOAD, &icon, 0).height;
        float text = shelf_line_height(12, 400);
        float top = (zone.top + zone.bottom - glyph - 6 - text) / 2;
        Color color = faded(theme_color(THEME_ACCENT_TEXT), label);
        gfx_text_draw(GLYPH_DOWNLOAD, (GfxRect){ zone.left, top, zone.right, top + glyph }, &icon, color);
        GfxTextStyle style = shelf_text_style(12, 400, GFX_ALIGN_CENTER);
        gfx_text_draw(S(shelf->drop_zone_text), (GfxRect){ zone.left, top + glyph + 6, zone.right, top + glyph + 6 + text }, &style, color);
#endif
    }
    if (pulsing) gfx_pop_transform();
    paint_tiles(shelf, opacity * (float)shelf->tiles_opacity.value);
}

static void paint_busy(ShelfWindow *shelf, double time)
{
    float opacity = (float)shelf->busy_opacity.value;
    if (opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    gfx_fill_rect((GfxRect){ 0, 0, layout->width, layout->height }, faded(theme_color(THEME_SMOKE), opacity));
    float text = shelf_line_height(12, 400);
    float top = (layout->height - 30 - 12 - text) / 2;
    float cx = layout->width / 2, cy = top + 15;
    // 不确定进度的环：0.9 秒转一圈。
    clock_tick(&shelf->ring_clock, time);
    float angle = (float)fmod(shelf->ring_clock.elapsed / 0.9 * 360.0, 360.0);
    gfx_stroke_ellipse(cx, cy, 13.5f, 13.5f, faded(theme_color(THEME_CONTROL_ALT), opacity), 3);
    gfx_arc(cx, cy, 13.5f, angle - 90, 90, faded(theme_color(THEME_ACCENT), opacity), 3);
    GfxTextStyle style = shelf_text_style(12, 400, GFX_ALIGN_CENTER);
    gfx_text_draw(shelf->model->busy_text, (GfxRect){ 0, top + 30 + 12, layout->width, top + 30 + 12 + text }, &style,
        faded(theme_color(THEME_TEXT_SECONDARY), opacity));
    ui_window_request_frame(shelf->ui);
}

static void paint_toast(ShelfWindow *shelf)
{
    float opacity = (float)shelf->toast_opacity.value;
    if (!shelf->toast_text || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    gfx_push_transform(gfx_matrix_translate(0, (float)shelf->toast_offset.value));
    gfx_push_opacity(opacity);
    gfx_fill_rounded(layout->toast, 8, theme_color(THEME_FLYOUT));
    gfx_stroke_rounded(layout->toast, 8, theme_color(THEME_FLYOUT_BORDER), 1);
    GfxTextStyle icon = shelf_glyph_style(14);
    gfx_text_draw(shelf->toast_success ? GLYPH_COMPLETED : GLYPH_WARNING, layout->toast_glyph, &icon,
        theme_color(shelf->toast_success ? THEME_SUCCESS : THEME_CAUTION));
    GfxTextStyle wrap = { GFX_FONT_TEXT, 12, 400, true, false, GFX_ALIGN_START, GFX_ALIGN_START };
    gfx_text_draw(shelf->toast_text, layout->toast_text, &wrap, theme_color(THEME_TEXT_PRIMARY));
    if (shelf->toast_action != TOAST_ACTION_NONE)
    {
        bool hovered = shelf->hovered == TARGET_TOAST_ACTION, pressed = hovered && shelf->pressed == TARGET_TOAST_ACTION;
        gfx_fill_rounded(layout->toast_action, 4, theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
        GfxTextStyle style = shelf_text_style(12, 600, GFX_ALIGN_CENTER);
        style.ellipsis = false;
        GfxRect label = layout->toast_action;
        label.bottom -= 1;
        gfx_text_draw(S(shelf->toast_action == TOAST_ACTION_UNDO ? STR_Undo : STR_MenuConvertToFile), label, &style,
            theme_color(pressed ? THEME_TEXT_SECONDARY : THEME_ACCENT_TEXT));
    }
    gfx_pop_layer();
    gfx_pop_transform();
}

void shelf_paint(UiWindow *window, void *context, double time)
{
    ShelfWindow *shelf = context;
    (void)window;
    shelf_layout(shelf);
    const ShelfLayout *layout = &shelf->layout;
#ifndef LOOK_VARIANT_CARDS
    if (ui_window_has_backdrop(shelf->ui))
        gfx_fill_rect((GfxRect){ 0, 0, layout->width, layout->height }, theme_color(THEME_SHELF_ACRYLIC_TINT));
#endif

    // 整个内容一起缩放、淡入淡出（出场和离场），背景不动。
    float origin_x = layout->width * shelf->origin_x, origin_y = layout->height * shelf->origin_y;
    GfxMatrix surface = gfx_matrix_scale(shelf->surface_scale, shelf->surface_scale, origin_x, origin_y);
    surface = gfx_matrix_multiply(surface, gfx_matrix_translate(shelf->surface_dx, shelf->surface_dy));
    gfx_push_transform(surface);
    // 拖到别的置物架上、松手就合并时内容变淡。
    float surface_opacity = shelf->surface_opacity * (float)shelf->merge_dim.value;
    bool layered = surface_opacity < 0.999f;
    if (layered) gfx_push_opacity(surface_opacity);

    paint_header(shelf);
    float body = (float)shelf->body_opacity.value;
    if (body > 0.001f)
    {
        if (shelf->model->count == 0) paint_empty(shelf, body);
        bool show_list = shelf->switching || shelf->shown_expanded;
        bool show_stack = shelf->model->count > 0 && (shelf->switching || !shelf->shown_expanded);
#ifdef LOOK_VARIANT_CARDS
        // 平铺压在堆叠上面，切换时牌飞过堆叠不会被挡住。
#else
        if (show_list) paint_list(shelf, body);
#endif
        if (show_stack) paint_stack(shelf, body);
#ifdef LOOK_VARIANT_CARDS
        if (show_list) paint_grid(shelf, body);
#endif
    }
    paint_action_bar(shelf, (float)shelf->bar_opacity.value);
    float overlay = (float)shelf->overlay_opacity.value;
    if (shelf->overlay_visible && overlay > 0.001f) paint_drop_overlay(shelf, overlay);
    paint_busy(shelf, time);
    paint_toast(shelf);

    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

