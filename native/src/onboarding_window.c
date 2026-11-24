// 第一次运行时的引导：五步，每步上面是一段循环播放的演示，下面是说明。和 C# 版 OnboardingWindow 对应，前四步演示的关键帧相同。
// 最后一步问要不要观察文件夹；升级来的老用户只看这一步（onboarding_window_show_watch）。
#include "onboarding_window.h"
#include "app.h"
#include "card_face.h"
#include "main_window.h"
#include "power.h"
#include "strings.h"
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 600.0f
#define WINDOW_HEIGHT 560.0f
#define CAPTION_HEIGHT 40.0f
#define STEP_COUNT 5
#define WATCH_STEP (STEP_COUNT - 1)
#define ART_WIDTH 552.0f
#define ART_HEIGHT 262.0f
#define LOOP_SECONDS 3.8
#define STEP_FADE 0.2

static UiWindow *window;
static int step;
static bool step_shown;
static bool watch_only;             // 只问观察文件夹的那一步
static double story_start[STEP_COUNT];
static Anim art_opacity[STEP_COUNT];
static Anim text_opacity, text_shift;
static Anim dot_width[STEP_COUNT], dot_opacity[STEP_COUNT];
static UiElement *title_label, *body_label, *skip_button, *back_button, *next_button, *close_button, *text_stack, *nav;
static UiElement *no_thanks_button, *add_folder_button;

bool onboarding_window_is_open(void)
{
    return window != NULL;
}

int onboarding_window_step(void)
{
    return window ? step : -1;
}

const wchar_t *onboarding_window_title_text(void)
{
    return window ? ui_label_text(title_label) : NULL;
}

const wchar_t *onboarding_window_body_text(void)
{
    return window ? ui_label_text(body_label) : NULL;
}

const wchar_t *onboarding_window_next_text(void)
{
    return window && !next_button->hidden ? ui_button_text(next_button) : NULL;
}

const wchar_t *onboarding_window_add_folder_text(void)
{
    return window && !add_folder_button->hidden ? ui_button_text(add_folder_button) : NULL;
}

bool onboarding_window_is_watch_only(void)
{
    return window && watch_only;
}

// ───────────── 关键帧 ─────────────

typedef enum FrameKind { FRAME_DISCRETE, FRAME_LINEAR, FRAME_EASE } FrameKind;

typedef struct KeyFrame
{
    double time;
    double value;
    FrameKind kind;
    Ease ease;
} KeyFrame;

#define DISCRETE(t, v) { (t), (v), FRAME_DISCRETE, { 0 } }
#define LINEAR(t, v) { (t), (v), FRAME_LINEAR, { 0 } }
#define EASED(t, v, e) { (t), (v), FRAME_EASE, e }

/// 和 WPF 的关键帧动画一样：离散的帧到点就跳，线性和缓动的帧从上一帧的值过渡过来；最后一帧之后停在最后的值。
static double keyframes(const KeyFrame *frames, int count, double time)
{
    double value = frames[0].value, previous_time = 0;
    for (int index = 0; index < count; index++)
    {
        const KeyFrame *frame = &frames[index];
        if (time < frame->time)
        {
            if (frame->kind == FRAME_DISCRETE) return value;
            double span = frame->time - previous_time;
            double t = span > 0 ? (time - previous_time) / span : 1;
            if (frame->kind == FRAME_EASE) t = ease_apply(frame->ease, t);
            return value + (frame->value - value) * t;
        }
        value = frame->value;
        previous_time = frame->time;
    }
    return value;
}

#define FRAMES(array, time) keyframes((array), (int)(sizeof(array) / sizeof((array)[0])), (time))

static Ease cubic_in_out(void)
{
    return EASE_OF(EASE_CUBIC_IN_OUT);
}

// ───────────── 画演示的小零件 ─────────────

static Color with_opacity(Color color, double opacity)
{
    color.a = (uint8_t)(color.a * (opacity < 0 ? 0 : opacity > 1 ? 1 : opacity));
    return color;
}

/// 小置物架：预制的阴影，圆角 14（纸牌圆角 8，描置物架的边框色）。
static void mini_shelf(GfxRect rect)
{
    bool cards = theme_is_cards();
    float radius = cards ? 8.0f : 14.0f;
    gfx_shadow(rect, radius, 24, 0, 6, COLOR_ARGB(46, 0, 0, 0));
    gfx_fill_rounded(rect, radius, theme_color(THEME_SHELF_SOLID));
    gfx_stroke_rounded(rect, radius, theme_color(cards ? THEME_SHELF_BORDER : THEME_CARD_BORDER), 1);
}

static void glyph(const wchar_t *text, GfxRect rect, float size, Color color)
{
    GfxTextStyle style = { GFX_FONT_ICON, size, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    gfx_text_draw(text, rect, &style, color);
}

static void text(const wchar_t *value, GfxRect rect, float size, int weight, Color color, GfxAlign align)
{
    GfxTextStyle style = { GFX_FONT_TEXT, size, weight, false, true, align, GFX_ALIGN_CENTER };
    gfx_text_draw(value, rect, &style, color);
}

/// 一张小卡片（58 × 58，纸牌是圆角 4 的小牌），中间一个图标；绕中心转 angle 度。
static void mini_card(float center_x, float center_y, float size, const wchar_t *icon, float icon_size, float angle)
{
    GfxRect rect = { center_x - size / 2, center_y - size / 2, center_x + size / 2, center_y + size / 2 };
    if (angle != 0) gfx_push_transform(gfx_matrix_rotate(angle, center_x, center_y));
    bool cards = theme_is_cards();
    gfx_fill_rounded(rect, cards ? 4.0f : 8.0f, theme_color(THEME_SHELF_TILE));
    gfx_stroke_rounded(rect, cards ? 4.0f : 8.0f, theme_color(THEME_SHELF_TILE_BORDER), cards ? 1.2f : 1.0f);
    glyph(icon, rect, icon_size, theme_color(THEME_ACCENT_TEXT));
    if (angle != 0) gfx_pop_transform();
}

/// 鼠标指针：白色填充、黑色描边。
static void cursor(float x, float y)
{
    static const float shape[] = { 0, 0, 0, 19, 4.6f, 14.4f, 7.8f, 21.4f, 11.2f, 19.9f, 8.1f, 13, 14, 13 };
    float points[14];
    for (int index = 0; index < 7; index++)
    {
        points[index * 2] = x + shape[index * 2];
        points[index * 2 + 1] = y + shape[index * 2 + 1];
    }
    gfx_polygon(points, 7, COLOR_WHITE, COLOR_BLACK, 1.2f);
}

/// 虚线框，圆角 10、4 3 的虚线。纸牌的放置区域（drop 为 true）是圆角 6、3 2 的短虚线，和置物架里拖入时的虚线牌一样。
static void dashed(GfxRect rect, float radius, ThemeColor stroke, ThemeColor fill, float width, double opacity, bool drop)
{
    bool short_dashes = drop && theme_is_cards();
    if (short_dashes) radius = 6;
    gfx_fill_rounded(rect, radius, with_opacity(theme_color(fill), opacity));
    gfx_stroke_rounded_dashed(rect, radius, with_opacity(theme_color(stroke), opacity), width, short_dashes ? 3.0f : 4.0f, short_dashes ? 2.0f : 3.0f);
}

// ───────────── 四步的演示 ─────────────

/// 第 1 步：欢迎。和真实的置物架一样：左上角钉子和移动/复制切换，右上角菜单和关闭，堆叠下方是名称和数量。
static void paint_step1(float x, float y, double time)
{
    // 上下浮动：1.6 秒到顶，再回来。
    double phase = fmod(time, 3.2);
    double t = phase < 1.6 ? phase / 1.6 : (3.2 - phase) / 1.6;
    float lift = (float)(-8 * ease_apply(EASE_OF(EASE_SINE_IN_OUT), t));
    GfxRect shelf = { x + 186, y + 32 + lift, x + 186 + 180, y + 32 + lift + 198 };
    mini_shelf(shelf);
    Color secondary = theme_color(THEME_TEXT_SECONDARY), accent = theme_color(THEME_ACCENT_TEXT);
    float header = shelf.top + 14;
    glyph(L"\xE840", (GfxRect){ shelf.left + 12, header - 8, shelf.left + 23, header + 8 }, 11, accent);
    GfxTextStyle pill_style = { GFX_FONT_TEXT, 9, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    float pill_width = gfx_text_measure(S(STR_TransferMove), &pill_style, 0).width + 12;
    GfxRect pill = { shelf.left + 31, header - 7, shelf.left + 31 + pill_width, header + 7 };
    gfx_fill_rounded(pill, 8, theme_color(THEME_CONTROL_ALT));
    gfx_text_draw(S(STR_TransferMove), pill, &pill_style, secondary);
    float right = shelf.right - 12;
    glyph(L"\xE711", (GfxRect){ right - 9, header - 8, right, header + 8 }, 9, secondary);
    glyph(L"\xE712", (GfxRect){ right - 9 - 12 - 11, header - 8, right - 9 - 12, header + 8 }, 11, secondary);
    glyph(L"\xE8FD", (GfxRect){ right - 9 - 12 - 11 - 12 - 11, header - 8, right - 9 - 12 - 11 - 12, header + 8 }, 11, secondary);
    // 中间的堆叠：上下一行名称和数量，底部一排快速操作。
    float footer_top = shelf.bottom - 38;
    GfxTextStyle count_style = { GFX_FONT_TEXT, 11, 600, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    wchar_t *count = str_item_count(3);
    float count_height = gfx_text_measure(count, &count_style, 0).height;
    float count_top = footer_top - 6 - count_height;
    float middle_x = (shelf.left + shelf.right) / 2, middle_y = (shelf.top + 28 + count_top - 2) / 2;
    mini_card(middle_x - 12, middle_y + 2, 58, L"\xE8B9", 26, -10);
    mini_card(middle_x + 11, middle_y + 1, 58, L"\xE8D2", 26, 7);
    mini_card(middle_x, middle_y, 58, L"\xE8A5", 26, 0);
    float badge_x = middle_x + 31, badge_y = middle_y - 30;
    gfx_fill_ellipse(badge_x, badge_y, 11, 11, theme_color(THEME_SHELF_TILE));
    gfx_fill_ellipse(badge_x, badge_y, 9, 9, theme_color(THEME_ACCENT));
    text(L"3", (GfxRect){ badge_x - 11, badge_y - 11, badge_x + 11, badge_y + 10 }, 11, 600, theme_color(THEME_TEXT_ON_ACCENT), GFX_ALIGN_CENTER);
    gfx_text_draw(count, (GfxRect){ shelf.left, count_top, shelf.right, count_top + count_height }, &count_style, theme_color(THEME_TEXT_PRIMARY));
    free(count);
    gfx_fill_rect((GfxRect){ shelf.left + 14, footer_top, shelf.right - 14, footer_top + 1 }, theme_color(THEME_DIVIDER));
    const wchar_t *actions[4] = { L"\xF012", L"\xE8C8", L"\xE8DE", L"\xE946" };
    float total = 4 * 34, left = middle_x - total / 2;
    for (int index = 0; index < 4; index++)
        glyph(actions[index], (GfxRect){ left + index * 34, footer_top, left + index * 34 + 34, shelf.bottom }, 14, theme_color(THEME_TEXT_PRIMARY));
}

/// 第 2 步：拖住并晃动，置物架弹出来。
static void paint_step2(float x, float y, double time)
{
    static const KeyFrame move[] = { LINEAR(0, 0), LINEAR(0.4, 0), LINEAR(0.58, -48), LINEAR(0.76, 46), LINEAR(0.94, -46), LINEAR(1.12, 44), LINEAR(1.32, 0) };
    static const KeyFrame opacity[] = { DISCRETE(0, 0), LINEAR(1.3, 0), LINEAR(1.55, 1), LINEAR(3.3, 1), LINEAR(3.6, 0) };
    KeyFrame scale[] = { DISCRETE(0, 0.9), LINEAR(1.3, 0.9), EASED(1.6, 1, EASE_BACK_OF(0.4)) };
    double shelf_opacity = FRAMES(opacity, time);
    if (shelf_opacity > 0.001)
    {
        float zoom = (float)FRAMES(scale, time);
        GfxRect shelf = { x + 186, y + 30, x + 186 + 180, y + 30 + 196 };
        gfx_push_transform(gfx_matrix_scale(zoom, zoom, (shelf.left + shelf.right) / 2, (shelf.top + shelf.bottom) / 2));
        gfx_push_opacity((float)shelf_opacity);
        mini_shelf(shelf);
        GfxRect zone = { shelf.left + 14, shelf.top + 14 + 22, shelf.right - 14, shelf.bottom - 14 };
        dashed(zone, 10, THEME_ACCENT, THEME_ACCENT_FAINT, 1.4f, 1, true);
        Color accent = theme_color(THEME_ACCENT_TEXT);
        GfxTextStyle hint = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_END };
        float hint_height = gfx_text_measure(S(STR_ReleaseToHold), &hint, 0).height;
        float bottom = shelf.bottom - 14 - 18;
        gfx_text_draw(S(STR_ReleaseToHold), (GfxRect){ shelf.left, bottom - hint_height, shelf.right, bottom }, &hint, accent);
        glyph(L"\xE896", (GfxRect){ shelf.left, bottom - hint_height - 4 - 24, shelf.right, bottom - hint_height - 4 }, 18, accent);
        gfx_pop_layer();
        gfx_pop_transform();
    }
    float offset = (float)FRAMES(move, time);
    mini_card(x + 246 + 30 + offset, y + 96 + 30, 60, L"\xE8A5", 26, 0);
    cursor(x + 246 + 40 + offset, y + 96 + 38);
}

/// 第 3 步：放进来，再拖出去。
static void paint_step3(float x, float y, double time)
{
    KeyFrame in_x[] = { DISCRETE(0, 250), EASED(1.0, 0, cubic_in_out()) };
    KeyFrame in_y[] = { DISCRETE(0, -60), EASED(1.0, 0, cubic_in_out()) };
    static const KeyFrame in_opacity[] = { DISCRETE(0, 1), DISCRETE(1.05, 0) };
    static const KeyFrame stack_opacity[] = { DISCRETE(0, 0), DISCRETE(1.05, 1), DISCRETE(1.7, 0) };
    static const KeyFrame out_opacity[] = { DISCRETE(0, 0), DISCRETE(1.7, 1), DISCRETE(2.75, 0) };
    KeyFrame out_x[] = { DISCRETE(0, 0), LINEAR(1.7, 0), EASED(2.7, 262, cubic_in_out()) };
    static const KeyFrame folder_scale[] = { DISCRETE(0, 1), LINEAR(2.7, 1), LINEAR(2.85, 1.12), LINEAR(3.1, 1) };

    GfxRect shelf = { x + 70, y + 34, x + 70 + 170, y + 34 + 194 };
    mini_shelf(shelf);
    dashed((GfxRect){ shelf.left + 14, shelf.top + 34, shelf.right - 14, shelf.bottom - 40 }, 10, THEME_CONTROL_STRONG_BORDER, THEME_TRANSPARENT, 1.2f, 0.4,
        false);
    if (FRAMES(stack_opacity, time) > 0.5)
        mini_card((shelf.left + shelf.right) / 2, (shelf.top + shelf.bottom) / 2 - 3, 58, L"\xE8B9", 26, 0);

    // 文件夹：卡片落进去时跳一下。
    float zoom = (float)FRAMES(folder_scale, time);
    GfxTextStyle name_style = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    GfxTextStyle folder_style = { GFX_FONT_ICON, 64, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    GfxSize folder_size = gfx_text_measure(L"\xE8B7", &folder_style, 0);
    GfxSize name_size = gfx_text_measure(S(STR_OnboardingSampleFolder), &name_style, 0);
    float width = folder_size.width > name_size.width ? folder_size.width : name_size.width;
    float left = x + 392, top = y + 86, height = folder_size.height + 4 + name_size.height;
    gfx_push_transform(gfx_matrix_scale(zoom, zoom, left + width / 2, top + height / 2));
    gfx_text_draw(L"\xE8B7", (GfxRect){ left, top, left + width, top + folder_size.height }, &folder_style, COLOR_RGB(0xE8, 0xA9, 0x3B));
    gfx_text_draw(S(STR_OnboardingSampleFolder), (GfxRect){ left, top + folder_size.height + 4, left + width, top + height }, &name_style,
        theme_color(THEME_TEXT_SECONDARY));
    gfx_pop_transform();

    if (FRAMES(in_opacity, time) > 0.5)
    {
        float dx = (float)FRAMES(in_x, time), dy = (float)FRAMES(in_y, time);
        mini_card(x + 126 + 29 + dx, y + 96 + 29 + dy, 58, L"\xE8B9", 26, 0);
        cursor(x + 126 + 38 + dx, y + 96 + 36 + dy);
    }
    if (FRAMES(out_opacity, time) > 0.5)
    {
        float dx = (float)FRAMES(out_x, time);
        mini_card(x + 126 + 29 + dx, y + 96 + 29, 58, L"\xE8B9", 26, 0);
        cursor(x + 126 + 38 + dx, y + 96 + 36);
    }
}

/// 第 4 步：拖到底部 → 按钮栏向上展开成一排操作并点亮“压缩” → 放下后收起，压缩包留在置物架上并提示。
static void paint_step4(float x, float y, double time)
{
    KeyFrame move_x[] = { DISCRETE(0, -150), EASED(1.0, 0, cubic_in_out()) };
    KeyFrame move_y[] = { DISCRETE(0, -110), EASED(1.0, 0, cubic_in_out()) };
    static const KeyFrame card_opacity[] = { DISCRETE(0, 1), DISCRETE(1.25, 0) };
    KeyFrame tiles_height[] = { DISCRETE(0, 30), EASED(0.7, 30, EASE_OF(EASE_CUBIC_OUT)), EASED(0.88, 66, EASE_OF(EASE_CUBIC_OUT)),
        EASED(1.3, 66, EASE_OF(EASE_CUBIC_OUT)), EASED(1.54, 30, EASE_OF(EASE_CUBIC_OUT)) };
    static const KeyFrame buttons_opacity[] = { DISCRETE(0, 1), LINEAR(0.7, 1), LINEAR(0.85, 0), LINEAR(1.35, 0), LINEAR(1.54, 1) };
    static const KeyFrame row_opacity[] = { DISCRETE(0, 0), LINEAR(0.7, 0), LINEAR(0.88, 1), LINEAR(1.3, 1), LINEAR(1.45, 0) };
    static const KeyFrame highlight[] = { DISCRETE(0, 0), LINEAR(0.88, 0), LINEAR(0.98, 1), LINEAR(1.3, 1), LINEAR(1.4, 0) };
    static const KeyFrame frame_opacity[] = { DISCRETE(0, 1), LINEAR(0.88, 1), LINEAR(0.98, 0.45) };
    static const KeyFrame zone_opacity[] = { DISCRETE(0, 1), LINEAR(1.3, 1), LINEAR(1.4, 0), LINEAR(3.5, 0), LINEAR(3.75, 1) };
    static const KeyFrame result_opacity[] = { DISCRETE(0, 0), LINEAR(1.4, 0), LINEAR(1.6, 1), LINEAR(3.3, 1), LINEAR(3.55, 0) };
    static const KeyFrame toast_opacity[] = { DISCRETE(0, 0), LINEAR(1.5, 0), LINEAR(1.7, 1), LINEAR(3.2, 1), LINEAR(3.45, 0) };

    GfxRect shelf = { x + 156, y + 24, x + 156 + 240, y + 24 + 214 };
    mini_shelf(shelf);
    GfxRect inner = { shelf.left + 10, shelf.top + 14, shelf.right - 10, shelf.bottom - 8 };
    float tiles = (float)FRAMES(tiles_height, time);
    GfxRect tiles_rect = { inner.left, inner.bottom - tiles, inner.right, inner.bottom };
    GfxRect upper = { inner.left, inner.top, inner.right, tiles_rect.top - 6 };
    Color accent = theme_color(THEME_ACCENT_TEXT), primary = theme_color(THEME_TEXT_PRIMARY);

    double zone = FRAMES(zone_opacity, time);
    if (zone > 0.001)
    {
        gfx_push_opacity((float)zone);
        dashed(upper, 10, THEME_ACCENT, THEME_ACCENT_FAINT, 1.4f, FRAMES(frame_opacity, time), true);
        float middle = (upper.top + upper.bottom) / 2;
        glyph(L"\xE896", (GfxRect){ upper.left, middle - 26, upper.right, middle - 2 }, 18, accent);
        text(S(STR_ReleaseToHold), (GfxRect){ upper.left, middle + 2, upper.right, middle + 18 }, 11, 400, accent, GFX_ALIGN_CENTER);
        gfx_pop_layer();
    }
    double result = FRAMES(result_opacity, time);
    if (result > 0.001)
    {
        gfx_push_opacity((float)result);
        float middle = (upper.top + upper.bottom) / 2;
        mini_card((upper.left + upper.right) / 2, middle - 12, 58, L"\xF012", 26, 0);
        text(S(STR_OnboardingSampleArchive), (GfxRect){ upper.left, middle + 17 + 8, upper.right, middle + 17 + 8 + 16 }, 11, 600, primary, GFX_ALIGN_CENTER);
        gfx_pop_layer();
    }

    // 按钮栏：平时一排图标，拖到上面时向上展开成一排操作。
    gfx_push_clip(tiles_rect);
    gfx_fill_rect((GfxRect){ tiles_rect.left + 4, tiles_rect.top, tiles_rect.right - 4, tiles_rect.top + 1 }, theme_color(THEME_DIVIDER));
    const wchar_t *icons[4] = { L"\xF012", L"\xE8C8", L"\xE8DE", L"\xE946" };
    const StringId labels[4] = { STR_ActionCompress, STR_ActionCopyPath, STR_ActionMoveTo, STR_ActionProperties };
    float cell = (inner.right - inner.left) / 4;
    double buttons = FRAMES(buttons_opacity, time);
    if (buttons > 0.001)
        for (int index = 0; index < 4; index++)
            glyph(icons[index], (GfxRect){ inner.left + cell * index, inner.bottom - 30, inner.left + cell * (index + 1), inner.bottom },
                14, with_opacity(primary, buttons));
    double row = FRAMES(row_opacity, time);
    if (row > 0.001)
    {
        gfx_push_opacity((float)row);
        for (int index = 0; index < 4; index++)
        {
            GfxRect tile = { inner.left + cell * index + 2, inner.bottom - 62, inner.left + cell * (index + 1) - 2, inner.bottom };
            bool cards = theme_is_cards();
            float radius = cards ? 6.0f : 8.0f;
            gfx_fill_rounded(tile, radius, theme_color(THEME_CARD));
            gfx_stroke_rounded(tile, radius, theme_color(cards ? THEME_SHELF_UNDER_CARD_BORDER : THEME_CARD_BORDER), 1);
            if (index == 0) gfx_fill_rounded(tile, radius, with_opacity(theme_color(THEME_ACCENT), FRAMES(highlight, time)));
            float middle = (tile.top + tile.bottom) / 2;
            glyph(icons[index], (GfxRect){ tile.left, middle - 20, tile.right, middle + 1 }, 16, primary);
            text(S(labels[index]), (GfxRect){ tile.left + 2, middle + 4, tile.right - 2, middle + 18 }, 10, 400, primary, GFX_ALIGN_CENTER);
        }
        gfx_pop_layer();
    }
    gfx_pop_clip();

    double toast = FRAMES(toast_opacity, time);
    if (toast > 0.001)
    {
        gfx_push_opacity((float)toast);
        wchar_t *message = str_created(S(STR_OnboardingSampleArchive));
        GfxTextStyle style = { GFX_FONT_TEXT, 11, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        float height = gfx_text_measure(message, &style, 0).height + 12;
        GfxRect box = { shelf.left + 10, shelf.bottom - 10 - height, shelf.right - 10, shelf.bottom - 10 };
        gfx_fill_rounded(box, 8, theme_color(THEME_FLYOUT));
        gfx_stroke_rounded(box, 8, theme_color(THEME_FLYOUT_BORDER), 1);
        glyph(L"\xE930", (GfxRect){ box.left + 8, box.top + 1, box.left + 8 + 14, box.bottom }, 13, theme_color(THEME_SUCCESS));
        gfx_text_draw(message, (GfxRect){ box.left + 8 + 14 + 6, box.top, box.right - 8, box.bottom }, &style, primary);
        free(message);
        gfx_pop_layer();
    }

    if (FRAMES(card_opacity, time) > 0.5)
    {
        float dx = (float)FRAMES(move_x, time), dy = (float)FRAMES(move_y, time);
        gfx_push_opacity(0.9f);
        mini_card(x + 155 + 24 + dx, y + 159 + 24 + dy, 48, L"\xE8A5", 22, 0);
        gfx_pop_layer();
        cursor(x + 155 + 38 + dx, y + 159 + 36 + dy);
    }
}

/// 第 5 步：新文件落进下载文件夹 → 右边缘探出置物架的一角（纸牌：斜着探出一张牌，牌角印着 1 和文档的花色）
/// → 光标移过去，角再伸出一点 → 点一下，置物架带着文件伸出来。
static void paint_step5(float x, float y, double time)
{
    KeyFrame drop_y[] = { DISCRETE(0, -70), EASED(0.55, 0, EASE_OF(EASE_CUBIC_IN)) };
    static const KeyFrame drop_opacity[] = { DISCRETE(0, 0), LINEAR(0.15, 1), LINEAR(0.45, 1), LINEAR(0.6, 0) };
    static const KeyFrame folder_scale[] = { DISCRETE(0, 1), LINEAR(0.55, 1), LINEAR(0.7, 1.12), LINEAR(0.95, 1) };
    KeyFrame peek[] = { DISCRETE(0, 0), LINEAR(0.85, 0), EASED(1.25, 1, EASE_BACK_OF(0.35)), LINEAR(2.0, 1), DISCRETE(2.05, 0) };
    KeyFrame hover[] = { DISCRETE(0, 0), LINEAR(1.6, 0), EASED(1.78, 1, EASE_OF(EASE_CUBIC_OUT)) };
    KeyFrame cursor_x[] = { DISCRETE(0, 300), LINEAR(1.15, 300), EASED(1.7, 528, cubic_in_out()) };
    KeyFrame cursor_y[] = { DISCRETE(0, 196), LINEAR(1.15, 196), EASED(1.7, 132, cubic_in_out()) };
    static const KeyFrame cursor_opacity[] = { DISCRETE(0, 0), LINEAR(1.0, 0), LINEAR(1.15, 1), LINEAR(2.2, 1), LINEAR(2.4, 0) };
    static const KeyFrame press[] = { DISCRETE(0, 0), LINEAR(1.9, 0), LINEAR(1.96, 1), LINEAR(2.05, 0) };
    KeyFrame shelf_x[] = { DISCRETE(0, 220), LINEAR(2.05, 220), EASED(2.5, 0, EASE_OF(EASE_QUINTIC_OUT)) };
    static const KeyFrame shelf_opacity[] = { DISCRETE(0, 0), DISCRETE(2.05, 1), LINEAR(3.35, 1), LINEAR(3.65, 0) };
    float right = x + ART_WIDTH;
    Color accent = theme_color(THEME_ACCENT_TEXT), primary = theme_color(THEME_TEXT_PRIMARY);

    // 下载文件夹：文件落进去时跳一下。
    float zoom = (float)FRAMES(folder_scale, time);
    GfxTextStyle name_style = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    GfxTextStyle folder_style = { GFX_FONT_ICON, 64, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    GfxSize folder_size = gfx_text_measure(L"\xE8B7", &folder_style, 0);
    GfxSize name_size = gfx_text_measure(S(STR_OnboardingSampleDownloads), &name_style, 0);
    float width = folder_size.width > name_size.width ? folder_size.width : name_size.width;
    float left = x + 96, top = y + 96, height = folder_size.height + 4 + name_size.height;
    double drop = FRAMES(drop_opacity, time);
    if (drop > 0.001)
    {
        gfx_push_opacity((float)drop);
        mini_card(left + width / 2, top + 14 + (float)FRAMES(drop_y, time), 44, L"\xE8A5", 20, 0);
        gfx_pop_layer();
    }
    gfx_push_transform(gfx_matrix_scale(zoom, zoom, left + width / 2, top + height / 2));
    gfx_text_draw(L"\xE8B7", (GfxRect){ left, top, left + width, top + folder_size.height }, &folder_style, COLOR_RGB(0xE8, 0xA9, 0x3B));
    glyph(L"\xE896", (GfxRect){ left, top + 18, left + width, top + folder_size.height - 2 }, 20, COLOR_RGB(0x7A, 0x4E, 0x00));
    gfx_text_draw(S(STR_OnboardingSampleDownloads), (GfxRect){ left, top + folder_size.height + 4, left + width, top + height }, &name_style,
        theme_color(THEME_TEXT_SECONDARY));
    gfx_pop_transform();

    // 探出的角：贴边的一侧伸到画面外面，像是从屏幕边缘后面探出来。纸牌：中心在画面外面，斜着露出靠里的一截。
    double out = FRAMES(peek, time);
    if (out > 0.001 && theme_is_cards())
    {
        float hovered = (float)FRAMES(hover, time);
        float visible = (float)out * 34 + hovered * 8;
        gfx_push_transform(gfx_matrix_multiply(gfx_matrix_rotate(-6 - 3 * hovered, 0, 0), gfx_matrix_translate(right - visible + 26, y + 132)));
        GfxRect card = { -26, -36, 26, 36 };
        gfx_shadow(card, 4, 12, 0, 2, COLOR_ARGB(60, 0, 0, 0));
        Color face = color_over(with_opacity(theme_color(THEME_SUBTLE_PRESSED), FRAMES(press, time)), theme_color(THEME_SHELF_TILE));
        gfx_fill_rounded(card, 4, face);
        gfx_stroke_rounded(card, 4, theme_color(THEME_SHELF_TILE_BORDER), 1.2f);
        glyph(L"\xE8A5", (GfxRect){ card.left + 6, card.top + 16, card.right - 6, card.bottom - 12 }, 20, accent);
        static const CardCorner corner = { 13, 13, 5, 2 };
        card_paint_corner(L"1", SUIT_DOCUMENT, &corner, card.left + 4, card.top + 3, primary, 1);
        gfx_pop_transform();
    }
    else if (out > 0.001)
    {
        float visible = (float)(out * 40 + FRAMES(hover, time) * 8);
        GfxRect tab = { right - visible, y + 100, right + 14, y + 100 + 64 };
        gfx_shadow(tab, 10, 12, 0, 2, COLOR_ARGB(60, 0, 0, 0));
        Color fill = color_over(with_opacity(theme_color(THEME_SUBTLE_PRESSED), FRAMES(press, time)), theme_color(THEME_SHELF_SOLID));
        gfx_fill_rounded(tab, 10, fill);
        gfx_stroke_rounded(tab, 10, theme_color(THEME_CARD_BORDER), 1);
        float inner = tab.left;
        glyph(L"\xE8A5", (GfxRect){ inner + 4, tab.top + 22, inner + 36, tab.bottom - 4 }, 18, accent);
        float badge_x = inner + 14, badge_y = tab.top + 12;
        gfx_fill_ellipse(badge_x, badge_y, 8, 8, theme_color(THEME_ACCENT));
        text(L"1", (GfxRect){ badge_x - 8, badge_y - 8, badge_x + 8, badge_y + 7 }, 10, 600, theme_color(THEME_TEXT_ON_ACCENT), GFX_ALIGN_CENTER);
    }

    // 点开后伸出来的置物架，里面就是刚才的文件。
    double shelf_shown = FRAMES(shelf_opacity, time);
    if (shelf_shown > 0.001)
    {
        gfx_push_opacity((float)shelf_shown);
        float shelf_left = right - 24 - 180 + (float)FRAMES(shelf_x, time);
        GfxRect shelf = { shelf_left, y + 32, shelf_left + 180, y + 32 + 196 };
        mini_shelf(shelf);
        float middle_x = (shelf.left + shelf.right) / 2, middle_y = shelf.top + 92;
        mini_card(middle_x, middle_y, 58, L"\xE8A5", 26, 0);
        text(S(STR_OnboardingSampleFile), (GfxRect){ shelf.left, middle_y + 42, shelf.right, middle_y + 58 }, 11, 600, primary, GFX_ALIGN_CENTER);
        gfx_pop_layer();
    }

    double pointer = FRAMES(cursor_opacity, time);
    if (pointer > 0.001)
    {
        gfx_push_opacity((float)pointer);
        cursor(x + (float)FRAMES(cursor_x, time), y + (float)FRAMES(cursor_y, time));
        gfx_pop_layer();
    }
}

/// 关闭了动画效果时各步演示停在这一刻：画面里该有的都在。第 5 步停在光标移到探出的角上。
static const double still_frames[STEP_COUNT] = { 0, 2, 1.3, 1.1, 1.8 };

static double story_time(int index, double time)
{
    if (!anim_enabled()) return still_frames[index];
    double elapsed = time - story_start[index];
    if (elapsed < 0) elapsed = 0;
    // 第 1 步一直上下浮动；其余每 3.8 秒循环一次。
    return index == 0 ? elapsed : fmod(elapsed, LOOP_SECONDS);
}

// ───────────── 窗口 ─────────────

static void update_step_text(void)
{
    switch (step)
    {
        case 0:
            ui_label_set_text(title_label, UI_STR(STR_OnboardingWelcomeTitle));
            ui_label_set_text(body_label, UI_STR(STR_OnboardingWelcomeBody));
            break;
        case 1:
            ui_label_set_text(title_label, UI_STR(STR_OnboardingShakeTitle));
            ui_label_set_text(body_label, UI_STR(STR_OnboardingShakeBody));
            break;
        case 2:
            ui_label_set_text(title_label, UI_STR(STR_OnboardingStackTitle));
            ui_label_set_text(body_label, UI_STR(theme_is_cards() ? STR_OnboardingStackBodyCards : STR_OnboardingStackBody));
            break;
        case WATCH_STEP:
            ui_label_set_text(title_label, UI_STR(STR_OnboardingWatchTitle));
            ui_label_set_text(body_label, UI_STR(STR_OnboardingWatchBody));
            break;
        default:
        {
            ui_label_set_text(title_label, UI_STR(STR_OnboardingActionsTitle));
            const AppSettings *settings = app_settings();
            bool has_hotkey = settings->hotkey && *settings->hotkey;
            wchar_t *body = str_onboarding_actions_body(has_hotkey ? settings->hotkey : NULL);
            ui_label_set_text(body_label, UI_LIT(body));
            free(body);
            break;
        }
    }
    ui_button_set_text(next_button, UI_STR(STR_Next));
}

/// 换到第 next 步：旧的演示淡出、新的淡入，说明文字从前进的方向滑入，指示点变宽变色。第一次显示和关闭了动画效果时直接到位。
static void show_step(int next)
{
    int previous = step;
    step = watch_only ? WATCH_STEP : next < 0 ? 0 : next >= STEP_COUNT ? STEP_COUNT - 1 : next;
    bool animate = step_shown && previous != step && anim_enabled();
    step_shown = true;
    double now = frames_time();
    for (int index = 0; index < STEP_COUNT; index++)
    {
        if (index == step)
        {
            // 刚换上来的从头播放；正在淡出又被切回来的接着播。
            if (art_opacity[index].value <= 0.001) story_start[index] = now;
            if (animate) anim_start(&art_opacity[index], 1, STEP_FADE, EASE_OF(EASE_CUBIC_OUT));
            else anim_set(&art_opacity[index], 1);
        }
        else if (animate) anim_start(&art_opacity[index], 0, STEP_FADE, EASE_OF(EASE_CUBIC_OUT));
        else anim_set(&art_opacity[index], 0);
        double width = index == step ? 18 : 6, opacity = index == step ? 1 : 0.4;
        if (animate)
        {
            anim_start(&dot_width[index], width, STEP_FADE, EASE_OF(EASE_CUBIC_OUT));
            anim_start(&dot_opacity[index], opacity, STEP_FADE, EASE_OF(EASE_CUBIC_OUT));
        }
        else
        {
            anim_set(&dot_width[index], width);
            anim_set(&dot_opacity[index], opacity);
        }
    }
    update_step_text();
    if (animate)
    {
        anim_set(&text_opacity, 0);
        anim_start(&text_opacity, 1, STEP_FADE, EASE_OF(EASE_CUBIC_OUT));
        anim_set(&text_shift, 14 * (step > previous ? 1 : -1));
        anim_start(&text_shift, 0, STEP_FADE, EASE_OF(EASE_CUBIC_OUT));
    }
    // 最后一步不再是“下一步”，而是问要不要观察文件夹。
    bool asking = step == WATCH_STEP;
    back_button->hidden = step == 0 || watch_only;
    skip_button->hidden = asking;
    next_button->hidden = asking;
    no_thanks_button->hidden = !asking;
    add_folder_button->hidden = !asking;
    ui_window_invalidate(window);
    ui_window_request_frame(window);
}

void onboarding_window_go(int index)
{
    if (window) show_step(index);
}

static void on_next(void *context)
{
    (void)context;
    if (step >= STEP_COUNT - 1) onboarding_window_close();
    else show_step(step + 1);
}

static void on_add_folder(void *context)
{
    (void)context;
    // 选好了就关掉引导；取消时留在这一步。
    if (app_choose_watch_folder(ui_window_handle(window))) onboarding_window_close();
}

static void on_no_thanks(void *context)
{
    (void)context;
    onboarding_window_close();
}

static void on_back(void *context)
{
    (void)context;
    show_step(step - 1);
}

static void on_skip(void *context)
{
    (void)context;
    onboarding_window_close();
}

static bool on_key(UiWindow *target, void *context, unsigned key)
{
    (void)target;
    (void)context;
    switch (key)
    {
        case VK_RIGHT:
            if (step < STEP_COUNT - 1) show_step(step + 1);
            return true;
        case VK_LEFT:
            if (step > 0 && !watch_only) show_step(step - 1);
            return true;
        case VK_ESCAPE:
            onboarding_window_close();
            return true;
        case VK_RETURN:
            if (step == WATCH_STEP) on_add_folder(NULL);
            else on_next(NULL);
            return true;
        default:
            return false;
    }
}

static void on_animate(UiWindow *target, void *context, double time)
{
    (void)context;
    bool running = anim_update(&text_opacity, time) | anim_update(&text_shift, time);
    for (int index = 0; index < STEP_COUNT; index++)
    {
        running |= anim_update(&art_opacity[index], time);
        running |= anim_update(&dot_width[index], time);
        running |= anim_update(&dot_opacity[index], time);
    }
    text_stack->opacity = (float)text_opacity.value;
    text_stack->transformed = text_shift.value != 0;
    text_stack->transform = gfx_matrix_translate((float)text_shift.value, 0);
    // 演示一直循环播放（关闭了动画效果时不播）。
    if (running || anim_enabled()) ui_window_request_frame(target);
}

static void root_paint(UiElement *element, double time)
{
    GfxRect bounds = element->bounds;
    GfxTextStyle caption = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    gfx_text_draw(watch_only ? L"DropShelf" : S(STR_OnboardingWelcomeTitle), (GfxRect){ 18, 0, bounds.right - 60, CAPTION_HEIGHT }, &caption,
        theme_color(THEME_TEXT_PRIMARY));
    GfxRect art = { 24, CAPTION_HEIGHT, bounds.right - 24, CAPTION_HEIGHT + ART_HEIGHT };
    bool cards = theme_is_cards();
    float art_radius = cards ? 8.0f : 12.0f;
    gfx_fill_rounded(art, art_radius, theme_color(cards ? THEME_CARD : THEME_LAYER));
    gfx_push_layer((GfxRect){ art.left + 1, art.top + 1, art.right - 1, art.bottom - 1 }, art_radius - 1, 1);
    // 画布 552 × 262 放在中间。
    float x = (art.left + art.right - ART_WIDTH) / 2, y = art.top;
    for (int index = 0; index < STEP_COUNT; index++)
    {
        float opacity = (float)art_opacity[index].value;
        if (opacity <= 0.001f) continue;
        gfx_push_opacity(opacity);
        double t = story_time(index, time);
        switch (index)
        {
            case 0: paint_step1(x, y, t); break;
            case 1: paint_step2(x, y, t); break;
            case 2: paint_step3(x, y, t); break;
            case 3: paint_step4(x, y, t); break;
            default: paint_step5(x, y, t); break;
        }
        gfx_pop_layer();
    }
    gfx_pop_layer();
    gfx_stroke_rounded(art, art_radius, theme_color(THEME_CARD_BORDER), 1);

    // 底部的指示点；只问观察文件夹时没有。
    float dot_x = 34, dot_y = nav->bounds.top + (nav->bounds.bottom - nav->bounds.top) / 2;
    for (int index = 0; index < STEP_COUNT && !watch_only; index++)
    {
        float width = (float)dot_width[index].value;
        Color color = theme_color(index == step ? THEME_ACCENT : THEME_CONTROL_STRONG_BORDER);
        gfx_fill_rounded((GfxRect){ dot_x, dot_y - 3, dot_x + width, dot_y + 3 }, 3, with_opacity(color, dot_opacity[index].value));
        dot_x += width + 8;
    }
}

static GfxSize root_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 0 };
}

static void root_arrange(UiElement *element)
{
    GfxRect bounds = element->bounds;
    ui_arrange(close_button, (GfxRect){ bounds.right - 46, 0, bounds.right, CAPTION_HEIGHT });
    ui_measure(text_stack, bounds.right - 68);
    float top = CAPTION_HEIGHT + ART_HEIGHT + 26;
    ui_arrange(text_stack, (GfxRect){ 34, top, bounds.right - 34, top + text_stack->desired.height });
    ui_measure(nav, bounds.right - 58);
    ui_arrange(nav, (GfxRect){ 34, bounds.bottom - 24 - nav->desired.height, bounds.right - 24, bounds.bottom - 24 });
}

static const UiElementType root_type = { root_measure, root_arrange, root_paint, NULL, NULL, false };

static void change_first_run(AppSettings *settings, void *context)
{
    (void)context;
    settings->has_completed_first_run = true;
}

static void on_closed(UiWindow *closed, void *context)
{
    (void)closed;
    (void)context;
    power_hold(&window, false);
    window = NULL;
    watch_only = false;
    title_label = body_label = skip_button = back_button = next_button = close_button = text_stack = nav = NULL;
    no_thanks_button = add_folder_button = NULL;
    if (!app_settings()->has_completed_first_run) app_update_settings(change_first_run, NULL);
    // 不管是选了、点了“不用了”还是跳过了引导，都算问过，以后不再单独问。
    app_mark_folder_watch_asked();
}

static void on_close(void *context)
{
    (void)context;
    onboarding_window_close();
}

static void open_window(bool only_watch)
{
    if (window)
    {
        ui_window_show(window);
        return;
    }
    watch_only = only_watch;
    UiWindowOptions options = { 0 };
    options.title = only_watch ? L"DropShelf" : S(STR_OnboardingWelcomeTitle);
    options.width = WINDOW_WIDTH;
    options.height = WINDOW_HEIGHT;
    options.backdrop = theme_is_cards() ? UI_BACKDROP_NONE : UI_BACKDROP_MICA;
    options.background = THEME_WINDOW_BACKGROUND;
    options.caption_height = CAPTION_HEIGHT;
    options.icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(theme_icon_resource()));
    options.key = on_key;
    options.animate = on_animate;
    options.closed = on_closed;
    window = ui_window_create(&options);
    if (!window) return;

    UiElement *root = ui_element_new(&root_type, sizeof(UiElement));
    close_button = ui_button(UI_BUTTON_CAPTION_CLOSE, UI_LIT(L"\xE711"), on_close, NULL);
    ui_set_tooltip(close_button, UI_STR(STR_CaptionClose));
    ui_add(root, close_button);
    text_stack = ui_stack(false, 0);
    title_label = ui_label(UI_STR(STR_OnboardingWelcomeTitle), UI_TEXT_SUBTITLE);
    ui_label_set_size(title_label, 22);
    ui_add(text_stack, title_label);
    body_label = ui_label(UI_STR(STR_OnboardingWelcomeBody), UI_TEXT_BODY);
    ui_label_style(body_label)->line_height = 22;
    ui_label_set_color(body_label, THEME_TEXT_SECONDARY);
    ui_set_margin(body_label, 0, 10, 0, 0);
    ui_add(text_stack, body_label);
    ui_add(root, text_stack);
    nav = ui_stack(true, 8);
    ui_add(nav, ui_spacer());
    skip_button = ui_button(UI_BUTTON_SUBTLE, UI_STR(STR_Skip), on_skip, NULL);
    skip_button->minimum_width = 72;
    back_button = ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Back), on_back, NULL);
    back_button->minimum_width = 88;
    next_button = ui_button(UI_BUTTON_ACCENT, UI_STR(STR_Next), on_next, NULL);
    next_button->minimum_width = 104;
    no_thanks_button = ui_button(UI_BUTTON_STANDARD, UI_STR(STR_NoThanks), on_no_thanks, NULL);
    no_thanks_button->minimum_width = 88;
    add_folder_button = ui_button(UI_BUTTON_ACCENT, UI_STR(STR_AddFolder), on_add_folder, NULL);
    add_folder_button->minimum_width = 104;
    ui_add(nav, skip_button);
    ui_add(nav, back_button);
    ui_add(nav, next_button);
    ui_add(nav, no_thanks_button);
    ui_add(nav, add_folder_button);
    ui_add(root, nav);
    ui_window_set_root(window, root);

    step = only_watch ? WATCH_STEP : 0;
    step_shown = false;
    anim_set(&text_opacity, 1);
    anim_set(&text_shift, 0);
    show_step(step);

    // 主窗口开着时放在它中间，否则放在屏幕中央。
    UiWindow *main = main_window_ui();
    RECT anchor;
    if (main && IsWindowVisible(ui_window_handle(main)) && GetWindowRect(ui_window_handle(main), &anchor))
    {
        float scale = ui_window_scale(main);
        int width = (int)(WINDOW_WIDTH * scale), height = (int)(WINDOW_HEIGHT * scale);
        ui_window_place(window, anchor.left + (anchor.right - anchor.left - width) / 2, anchor.top + (anchor.bottom - anchor.top - height) / 2,
            WINDOW_WIDTH, WINDOW_HEIGHT);
    }
    else ui_window_center(window);
    power_hold(&window, true);
    ui_window_show(window);
}

void onboarding_window_show(void)
{
    open_window(false);
}

void onboarding_window_show_watch(void)
{
    open_window(true);
}

void onboarding_window_close(void)
{
    if (window) ui_window_close(window);
}

void onboarding_window_apply_look(void)
{
    if (!window) return;
    ui_window_set_backdrop(window, theme_is_cards() ? UI_BACKDROP_NONE : UI_BACKDROP_MICA);
    ui_window_set_icon(window, LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(theme_icon_resource())));
    ui_window_invalidate(window);
}
