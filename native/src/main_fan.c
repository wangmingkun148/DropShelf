// 概览页底部的最近关闭的置物架：排成一手牌，鼠标横向移动时选出离光标最近的一张，抽出、回正并放大，相邻的牌向两侧让开。
// 点一张恢复成置物架；拖出窗口在松手处恢复；拖回牌堆里换位置。
// 和 C# 版的 FanPanel、MainWindow.HistoryDrag.cs、CardGhostWindow 对应。
#include "main_window_internal.h"
#include "shelf_history.h"
#include "shelf_window.h"
#include "task.h"
#include "thumbnail.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CARD_WIDTH 150.0f
#define CARD_HEIGHT 200.0f
#define MAX_SPACING 98.0f
#define TILT 2.2f
#define EDGE_DROP 34.0f
#define LIFT 92.0f
#define HOT_SCALE 1.14f
#define PUSH 42.0f
#define MOVE_SECONDS 0.3

// 拖动
#define DRAG_THRESHOLD 4.0f
#define HISTORY_RETURN_REACH 60.0f
#define DETACHED_SCALE 1.06f
#define TEAR_DISTANCE 48.0f
#define HOLD_FOLLOW 0.3f
#define RETURN_PULL 0.25f
// 跟随光标的卡牌
#define FOLLOW_RATE 60.0
#define TURN_RATE 9.0
#define SCALE_RATE 14.0
#define SWAY_PER_SPEED 0.007
#define MAX_SWAY 8.0
#define FADE_SECONDS 0.18
#define SETTLE_TIMEOUT 0.8

/// 卡片上的一张缩略图；读取不到时显示文件或文件夹图标。在后台查明，卡片销毁了也能安全收尾。
typedef struct Thumb
{
    LONG references;
    wchar_t *path;
    GfxImage *image;
    bool folder;
    bool known;
} Thumb;

typedef struct Card
{
    ShelfHistoryEntry *entry;
    wchar_t *title;
    wchar_t *detail;
    Thumb *stack[3];        // 最后放入的在最上面
    GfxRect base;           // 排版的位置（相对面板左上角）
    Anim scale, angle, x, y;
    double goal_scale, goal_angle, goal_x, goal_y;
    bool has_goals;
    Anim hint;              // “点击恢复”
    int z;
    bool hidden;            // 拖着的牌（由跟随光标的卡牌代替）
} Card;

typedef struct Fan
{
    UiElement base;
} Fan;

static Card *cards;
static int card_count;
static int hot = -1, last_hot = -1, pressed = -1;
static bool remove_pressed;
static int dragged = -1, gap = -1;
static float center_x, spacing, panel_width;
static UiElement *fan;

// 拖出
static bool dragging;
static bool torn;
static int drag_index;
static float press_x, press_y;     // 面板坐标
static POINT drag_start_cursor;
static float grab_x, grab_y;        // 光标相对卡牌中心（屏幕像素）

// 跟随光标的卡牌
typedef struct GhostPose
{
    double x, y;            // 中心，屏幕像素
    double scale, angle;
} GhostPose;

typedef enum GhostMode { GHOST_IDLE, GHOST_FOLLOW, GHOST_SETTLE, GHOST_VANISH } GhostMode;

static struct
{
    GhostMode mode;
    Card card;              // 画的那张牌的副本（缩略图有引用）
    GhostPose pose;
    GhostPose vanish_target;
    double sway;
    double last;
    double settle_start;
    double opacity;
    int settle_index;       // 飞向哪张牌（拖着的那张）
    int original_index;     // 它原来在第几个
    int move_to;            // 放回到第几个
    bool in_window;
    UiWindow *window;       // 拖出主窗口时的透明小窗口
    float window_size;
    bool has_last;
} ghost;

static void ghost_stop(void);

// ───────────── 缩略图 ─────────────

static Thumb *thumb_retain(Thumb *thumb)
{
    if (thumb) InterlockedIncrement(&thumb->references);
    return thumb;
}

static void thumb_release(Thumb *thumb)
{
    if (!thumb || InterlockedDecrement(&thumb->references) > 0) return;
    gfx_image_release(thumb->image);
    free(thumb->path);
    free(thumb);
}

static void on_thumbnail(GfxImage *image, void *context)
{
    Thumb *thumb = context;
    gfx_image_release(thumb->image);
    thumb->image = image;
    if (M->window) main_repaint();
    thumb_release(thumb);
}

static void probe_folder(void *data)
{
    Thumb *thumb = data;
    thumb->folder = path_is_directory(thumb->path);
}

static void probed_folder(void *data)
{
    Thumb *thumb = data;
    thumb->known = true;
    if (M->window) main_repaint();
    // 再取缩略图（和资源管理器里看到的一样）。
    thumbnail_load(thumb->path, 96, on_thumbnail, thumb);
}

static Thumb *thumb_create(const wchar_t *path)
{
    Thumb *thumb = xcalloc(1, sizeof(Thumb));
    thumb->references = 1;
    thumb->path = wcs_dup(path);
    // 是文件夹还是文件在后台查明后才显示图标，打开主窗口时不在界面线程上逐个访问磁盘。
    task_run(probe_folder, probed_folder, thumb_retain(thumb));
    return thumb;
}

// ───────────── 牌的位置 ─────────────

static void card_free(Card *card)
{
    shelf_history_entry_release(card->entry);
    free(card->title);
    free(card->detail);
    for (int index = 0; index < 3; index++) thumb_release(card->stack[index]);
}

static int slots_count(void)
{
    return dragged >= 0 && gap < 0 ? card_count - 1 : card_count;
}

static float slot_spacing(int slots)
{
    if (slots <= 1) return 0;
    float value = (panel_width - CARD_WIDTH) / (slots - 1);
    return value < 0 ? 0 : value > MAX_SPACING ? MAX_SPACING : value;
}

static float slot_center(int slot, int slots)
{
    return center_x + (slot - (slots - 1) / 2.0f) * slot_spacing(slots);
}

static float center_of(int index)
{
    return center_x + (index - (card_count - 1) / 2.0f) * spacing;
}

static float rest_y(int index, int count)
{
    float mid = (count - 1) / 2.0f;
    if (mid <= 0) return 0;
    float ratio = fabsf(index - mid) / mid;
    return ratio * ratio * EDGE_DROP;
}

/// 第 index 张牌现在排在第几个位置：拖动时其余的牌依次补上拖走的位置，再在空位处让开。
static int slot_of(int index)
{
    if (dragged < 0) return index;
    if (index == dragged) return gap >= 0 ? gap : index;
    int slot = index > dragged ? index - 1 : index;
    return gap >= 0 && slot >= gap ? slot + 1 : slot;
}

static int index_of_slot(int slot)
{
    for (int index = 0; index < card_count; index++)
        if (slot_of(index) == slot && (index != dragged || gap >= 0)) return index;
    return -1;
}

static void card_set(Anim *anim, double *goal, double value, Ease ease, bool animate)
{
    animate &= anim_enabled();
    if (animate && *goal == value && (anim->running || anim->value == value)) return;
    *goal = value;
    if (animate) anim_start(anim, value, MOVE_SECONDS, ease);
    else anim_set(anim, value);
}

static void apply_state(bool animate)
{
    int slots = slots_count();
    float mid = (slots - 1) / 2.0f;
    int hot_slot = hot >= 0 && hot < card_count ? slot_of(hot) : -1;
    for (int index = 0; index < card_count; index++)
    {
        Card *card = &cards[index];
        int slot = slot_of(index);
        bool is_hot = index == hot;
        float rest = (slot - mid) * TILT;
        float push = 0;
        if (hot_slot >= 0 && !is_hot)
        {
            int distance = abs(slot - hot_slot);
            push = (slot > hot_slot ? 1 : -1) * (distance < 3 ? (3 - distance) / 3.0f : 0) * PUSH;
        }
        float offset = slot_center(slot, slots) - center_of(index) + push;
        Ease ease = is_hot ? EASE_BACK_OF(0.3) : EASE_OF(EASE_CUBIC_OUT);
        // 拖着的牌是隐藏的，直接放到空位上，放回时卡牌飞向它的最终位置。
        bool smooth = animate && index != dragged;
        card_set(&card->scale, &card->goal_scale, is_hot ? HOT_SCALE : 1, ease, smooth);
        card_set(&card->angle, &card->goal_angle, is_hot ? 0 : rest, EASE_OF(EASE_CUBIC_OUT), smooth);
        card_set(&card->x, &card->goal_x, offset, EASE_OF(EASE_CUBIC_OUT), smooth);
        card_set(&card->y, &card->goal_y, is_hot ? -LIFT - rest_y(index, card_count) : rest_y(slot, slots) - rest_y(index, card_count), ease, smooth);
        // 中间的牌在上面；刚放回去的牌在落下过程中仍压在邻居上面。
        card->z = is_hot ? 1000 : index == last_hot ? 999 : slots - (int)ceilf(fabsf(slot - mid));
        double hint = is_hot ? 1 : 0;
        if (card->hint.to != hint) anim_start(&card->hint, hint, is_hot ? 0.2 : 0.15, EASE_OF(EASE_LINEAR));
    }
    // 选出的牌超出面板上沿，这块区域也要接收鼠标。
    float extend = hot >= 0 ? LIFT + CARD_HEIGHT * (HOT_SCALE - 1) + 8 : 0;
    fan->hit_extend_top = extend;
    if (M->history_section) M->history_section->hit_extend_top = extend;
    main_request_frame();
}

static void set_hot(int index)
{
    if (index == hot) return;
    if (hot >= 0) last_hot = hot;
    hot = index;
    apply_state(true);
}

/// 牌换过、或窗口变了：按面板现在的宽度算出每张牌排版的位置。
static void layout_cards(bool keep_poses)
{
    GfxRect bounds = fan->bounds;
    float width = bounds.right - bounds.left;
    if (width <= 0) return;
    panel_width = width;
    center_x = width / 2;
    spacing = card_count > 1 ? (width - CARD_WIDTH) / (card_count - 1) : 0;
    if (spacing < 0) spacing = 0;
    if (spacing > MAX_SPACING) spacing = MAX_SPACING;
    for (int index = 0; index < card_count; index++)
    {
        float left = center_of(index) - CARD_WIDTH / 2, top = rest_y(index, card_count);
        GfxRect next = { left, top, left + CARD_WIDTH, top + CARD_HEIGHT };
        if (keep_poses && cards[index].has_goals)
        {
            // 排版位置挪了：位移减去这段，画面停在原处，再从这里滑向新的终点。
            Card *card = &cards[index];
            anim_set(&card->x, card->x.value - (next.left - card->base.left));
            anim_set(&card->y, card->y.value - (next.top - card->base.top));
            card->goal_x = card->goal_y = NAN;
        }
        cards[index].base = next;
        cards[index].has_goals = true;
    }
}

// ───────────── 画牌 ─────────────

/// 一张牌在面板里的变换：以底边中点为原点缩放、旋转，再平移。
static GfxMatrix card_matrix(const Card *card, float scale, float angle, float x, float y)
{
    float origin_x = (card->base.left + card->base.right) / 2, origin_y = card->base.bottom;
    GfxMatrix matrix = gfx_matrix_multiply(gfx_matrix_scale(scale, scale, origin_x, origin_y), gfx_matrix_rotate(angle, origin_x, origin_y));
    return gfx_matrix_multiply(matrix, gfx_matrix_translate(x, y));
}

static void paint_mini_card(const Thumb *thumb, float left, float top, float angle, float x, float y, float scale)
{
    if (!thumb) return;
    // 60 × 60 的小卡片，绕中心转。
    GfxRect rect = { left, top, left + 60, top + 60 };
    float cx = left + 30, cy = top + 30;
    GfxMatrix matrix = gfx_matrix_multiply(gfx_matrix_rotate(angle, cx, cy), gfx_matrix_translate(x, y));
    gfx_push_transform(matrix);
    gfx_fill_rounded(rect, 8, theme_color(THEME_SHELF_TILE));
    gfx_stroke_rounded(rect, 8, theme_color(THEME_SHELF_TILE_BORDER), 1);
    GfxRect inner = { rect.left + 6, rect.top + 6, rect.right - 6, rect.bottom - 6 };
    if (thumb->image)
    {
        float width = (float)gfx_image_width(thumb->image), height = (float)gfx_image_height(thumb->image);
        float fit = fminf((inner.right - inner.left) / width, (inner.bottom - inner.top) / height);
        float w = width * fit, h = height * fit;
        float ix = (inner.left + inner.right - w) / 2, iy = (inner.top + inner.bottom - h) / 2;
        gfx_draw_image(thumb->image, (GfxRect){ ix, iy, ix + w, iy + h }, 1);
    }
    else if (thumb->known)
    {
        GfxTextStyle glyph = { GFX_FONT_ICON, 24, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        gfx_text_draw(thumb->folder ? L"\xE8B7" : L"\xE8A5", inner, &glyph, theme_color(THEME_TEXT_SECONDARY));
    }
    (void)scale;
    gfx_pop_transform();
}

/// 画一张牌，左上角在 (0, 0)：卡片底色、缩略图堆叠、标题、详情；选出时边框、“点击恢复”和删除按钮。
static void paint_card_content(const Card *card, float left, float top, bool is_hot, float hint, bool remove_hovered)
{
    GfxRect rect = { left, top, left + CARD_WIDTH, top + CARD_HEIGHT };
    gfx_fill_rounded(rect, 12, theme_color(THEME_FLYOUT));
    gfx_stroke_rounded(rect, 12, theme_color(is_hot ? THEME_ACCENT : THEME_FLYOUT_BORDER), 1);
    // 缩略图堆叠：120 × 84 的画布缩进 60 高（宽约 86），居中。
    float canvas_scale = 60.0f / 84.0f;
    float canvas_width = 120 * canvas_scale;
    float canvas_left = left + 12 + (CARD_WIDTH - 24 - canvas_width) / 2, canvas_top = top + 16;
    gfx_push_transform((GfxMatrix){ canvas_scale, 0, 0, canvas_scale, canvas_left, canvas_top });
    paint_mini_card(card->stack[2], 30, 14, -10, -10, 2, 1);
    paint_mini_card(card->stack[1], 30, 13, 7, 9, 1, 1);
    paint_mini_card(card->stack[0], 30, 12, 0, 0, 0, 1);
    gfx_pop_transform();
    float y = top + 16 + 60 + 10;
    GfxTextStyle title = { GFX_FONT_TEXT, 13, 400, true, true, GFX_ALIGN_CENTER, GFX_ALIGN_START, 18 };
    GfxSize title_size = gfx_text_measure(card->title, &title, CARD_WIDTH - 24);
    float title_height = title_size.height > 36 ? 36 : title_size.height;
    gfx_text_draw(card->title, (GfxRect){ left + 12, y, left + CARD_WIDTH - 12, y + title_height }, &title, theme_color(THEME_TEXT_PRIMARY));
    y += title_height + 4;
    GfxTextStyle detail = { GFX_FONT_TEXT, 11, 400, false, true, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    float detail_height = gfx_text_measure(L"Ag", &detail, 0).height;
    gfx_text_draw(card->detail, (GfxRect){ left + 12, y, left + CARD_WIDTH - 12, y + detail_height }, &detail, theme_color(THEME_TEXT_SECONDARY));
    y += detail_height + 10;
    if (hint > 0.001f)
    {
        GfxTextStyle text = { GFX_FONT_TEXT, 11, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        float width = gfx_text_measure(S(STR_ClickToRestore), &text, 0).width + 20;
        float height = gfx_text_measure(S(STR_ClickToRestore), &text, 0).height + 5;
        float pill_left = left + (CARD_WIDTH - width) / 2;
        gfx_push_opacity(hint);
        gfx_fill_rounded((GfxRect){ pill_left, y, pill_left + width, y + height }, 10, theme_color(THEME_ACCENT));
        gfx_text_draw(S(STR_ClickToRestore), (GfxRect){ pill_left, y, pill_left + width, y + height - 1 }, &text, theme_color(THEME_TEXT_ON_ACCENT));
        gfx_pop_layer();
    }
    if (is_hot)
    {
        GfxRect close = { left + CARD_WIDTH - 4 - 24, top + 4, left + CARD_WIDTH - 4, top + 28 };
        if (remove_hovered) gfx_fill_rounded(close, 4, theme_color(remove_pressed ? THEME_SUBTLE_PRESSED : THEME_SUBTLE_HOVER));
        GfxTextStyle glyph = { GFX_FONT_ICON, 9, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        gfx_text_draw(GLYPH_CLOSE, close, &glyph, theme_color(THEME_TEXT_PRIMARY));
    }
}

static bool remove_hovered_now;

static void fan_paint(UiElement *element, double time)
{
    (void)time;
    if (card_count == 0) return;
    GfxRect bounds = element->bounds;
    // 按上下次序画：z 小的先画。
    int order[SHELF_HISTORY_CAPACITY];
    int count = card_count < SHELF_HISTORY_CAPACITY ? card_count : SHELF_HISTORY_CAPACITY;
    for (int index = 0; index < count; index++) order[index] = index;
    for (int a = 0; a < count; a++)
        for (int b = a + 1; b < count; b++)
            if (cards[order[b]].z < cards[order[a]].z)
            {
                int swap = order[a];
                order[a] = order[b];
                order[b] = swap;
            }
    gfx_push_transform(gfx_matrix_translate(bounds.left, bounds.top));
    for (int position = 0; position < count; position++)
    {
        Card *card = &cards[order[position]];
        if (card->hidden) continue;
        GfxMatrix matrix = card_matrix(card, (float)card->scale.value, (float)card->angle.value, (float)card->x.value, (float)card->y.value);
        gfx_push_transform(matrix);
        bool pressed_now = order[position] == pressed && order[position] == hot && !remove_pressed && element->pressed;
        if (pressed_now) gfx_push_opacity(0.85f);
        paint_card_content(card, card->base.left, card->base.top, order[position] == hot, (float)card->hint.value,
            order[position] == hot && remove_hovered_now);
        if (pressed_now) gfx_pop_layer();
        gfx_pop_transform();
    }
    gfx_pop_transform();
}

static void fan_arrange(UiElement *element)
{
    (void)element;
    if (fabsf((element->bounds.right - element->bounds.left) - panel_width) >= 0.5f)
    {
        layout_cards(false);
        apply_state(false);
    }
}

// ───────────── 光标在哪张牌上 ─────────────

/// 光标在选出的牌抽出后的位置上时保持选中，否则移到牌靠边处会离相邻的牌更近、牌被换掉，点不到右上角的按钮。
static bool hot_contains(float x, float y)
{
    float bottom = CARD_HEIGHT - LIFT;
    return fabsf(x - slot_center(slot_of(hot), slots_count())) <= CARD_WIDTH * HOT_SCALE / 2 && y >= bottom - CARD_HEIGHT * HOT_SCALE && y <= bottom;
}

static int index_at(float x, float y)
{
    int slots = slots_count();
    if (slots <= 0) return -1;
    if (hot >= 0 && hot < card_count && hot_contains(x, y)) return hot;
    float half = CARD_WIDTH / 2;
    if (y < 0 || x < slot_center(0, slots) - half || x > slot_center(slots - 1, slots) + half) return -1;
    float step = slot_spacing(slots);
    int slot = step > 0 ? (int)lroundf((x - center_x) / step + (slots - 1) / 2.0f) : 0;
    slot = slot < 0 ? 0 : slot > slots - 1 ? slots - 1 : slot;
    return index_of_slot(slot);
}

/// 选出的牌右上角的删除按钮。坐标是面板里的。
static bool on_remove_button(float x, float y)
{
    if (hot < 0 || hot >= card_count) return false;
    Card *card = &cards[hot];
    GfxMatrix matrix = card_matrix(card, (float)card->goal_scale, (float)card->goal_angle, (float)card->goal_x, (float)card->goal_y), inverse;
    if (!gfx_matrix_invert(matrix, &inverse)) return false;
    gfx_matrix_apply(inverse, &x, &y);
    float left = card->base.left + CARD_WIDTH - 4 - 24, top = card->base.top + 4;
    return x >= left && x < left + 24 && y >= top && y < top + 24;
}

static int slot_at(float x)
{
    if (card_count <= 1 || spacing <= 0) return 0;
    int slot = (int)lroundf((x - center_x) / spacing + (card_count - 1) / 2.0f);
    return slot < 0 ? 0 : slot > card_count - 1 ? card_count - 1 : slot;
}

static void set_gap(int dragged_index, int gap_index)
{
    if (dragged_index < 0) gap_index = -1;
    if (dragged_index == dragged && gap_index == gap) return;
    dragged = dragged_index;
    gap = gap_index;
    apply_state(true);
}

// ───────────── 牌的屏幕位置 ─────────────

static float window_scale(void)
{
    return ui_window_scale(M->window);
}

/// 第 index 张牌按动画终点在屏幕上的中心、缩放和角度。
static bool pose_of(int index, GhostPose *pose)
{
    if (index < 0 || index >= card_count || !M->window || M->overview_scroll->hidden) return false;
    Card *card = &cards[index];
    GfxMatrix matrix = card_matrix(card, (float)card->goal_scale, (float)card->goal_angle, (float)card->goal_x, (float)card->goal_y);
    float cx = (card->base.left + card->base.right) / 2, cy = (card->base.top + card->base.bottom) / 2;
    gfx_matrix_apply(matrix, &cx, &cy);
    cx += fan->bounds.left;
    cy += fan->bounds.top;
    ui_element_to_window(fan, &cx, &cy);
    POINT screen = ui_window_to_screen(M->window, cx, cy);
    pose->x = screen.x;
    pose->y = screen.y;
    pose->scale = card->goal_scale;
    pose->angle = card->goal_angle;
    return true;
}

static POINT cursor_point(void)
{
    POINT point = {0};
    GetCursorPos(&point);
    return point;
}

/// 光标离牌堆多近：0 表示不在放回范围内，从范围上沿到牌堆上沿平滑增加到 1。
static double return_weight(POINT screen)
{
    if (!M->window) return 0;
    float x, y;
    ui_window_from_screen(M->window, screen, &x, &y);
    GfxSize size = ui_window_size(M->window);
    if (x < 0 || y < 0 || x > size.width || y > size.height) return 0;
    float lx = x, ly = y;
    ui_element_to_local(fan, &lx, &ly);
    lx -= fan->bounds.left;
    ly -= fan->bounds.top;
    float height = fan->bounds.bottom - fan->bounds.top;
    if (lx < 0 || lx > panel_width || ly < -HISTORY_RETURN_REACH || ly > height) return 0;
    double t = (ly + HISTORY_RETURN_REACH) / HISTORY_RETURN_REACH;
    t = t < 0.01 ? 0.01 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

// ───────────── 跟随光标的卡牌 ─────────────

static void ghost_paint_card(double opacity)
{
    if (opacity <= 0.001) return;
    gfx_push_opacity((float)opacity);
    gfx_shadow((GfxRect){ 0, 0, CARD_WIDTH, CARD_HEIGHT }, 12, 24, 0, 6, COLOR_ARGB(64, 0, 0, 0));
    paint_card_content(&ghost.card, 0, 0, false, 0, false);
    gfx_pop_layer();
}

/// 卡牌的变换：中心在 (x, y)，缩放、旋转（含晃动）。
static GfxMatrix ghost_matrix(float x, float y, float scale_factor)
{
    float scale = (float)ghost.pose.scale * scale_factor;
    float angle = (float)(ghost.pose.angle + ghost.sway);
    GfxMatrix matrix = gfx_matrix_multiply(gfx_matrix_translate(-CARD_WIDTH / 2, -CARD_HEIGHT / 2), gfx_matrix_scale(scale, scale, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(angle, 0, 0));
    return gfx_matrix_multiply(matrix, gfx_matrix_translate(x, y));
}

void fan_paint_ghost(double time)
{
    (void)time;
    if (ghost.mode == GHOST_IDLE || !ghost.in_window || !M->window) return;
    float x, y;
    POINT center = { (LONG)lround(ghost.pose.x), (LONG)lround(ghost.pose.y) };
    ui_window_from_screen(M->window, center, &x, &y);
    // 牌堆附近时画在主窗口里，超出窗口底边的部分随牌堆一起被裁掉。
    gfx_push_clip(M->stage);
    gfx_push_transform(ghost_matrix(x, y, 1));
    ghost_paint_card(ghost.opacity);
    gfx_pop_transform();
    gfx_pop_clip();
}

static void ghost_window_paint(UiWindow *window, void *context, double time)
{
    (void)context;
    (void)time;
    if (ghost.mode == GHOST_IDLE || ghost.in_window) return;
    GfxSize size = ui_window_size(window);
    gfx_push_transform(ghost_matrix(size.width / 2, size.height / 2, 1));
    ghost_paint_card(ghost.opacity);
    gfx_pop_transform();
}

static bool ghost_window_message(UiWindow *window, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    (void)window;
    (void)context;
    (void)wparam;
    (void)lparam;
    // 收尾动画期间点到卡牌上的鼠标交给下面的窗口。
    if (message != WM_NCHITTEST) return false;
    *result = HTTRANSPARENT;
    return true;
}

static void ghost_window_closed(UiWindow *window, void *context)
{
    (void)context;
    if (ghost.window == window) ghost.window = NULL;
}

static void ensure_ghost_window(void)
{
    if (ghost.window) return;
    // 留出旋转、放大和阴影的空间。
    ghost.window_size = ceilf(sqrtf(CARD_WIDTH * CARD_WIDTH + CARD_HEIGHT * CARD_HEIGHT) * 1.2f + 48);
    UiWindowOptions options = { 0 };
    options.title = L"DropShelf Card";
    options.popup = true;
    options.no_activate = true;
    options.width = options.height = ghost.window_size;
    options.paint = ghost_window_paint;
    options.message = ghost_window_message;
    options.closed = ghost_window_closed;
    ghost.window = ui_window_create(&options);
    if (ghost.window) SetWindowLongPtrW(ui_window_handle(ghost.window), GWL_EXSTYLE,
        GetWindowLongPtrW(ui_window_handle(ghost.window), GWL_EXSTYLE) | WS_EX_TRANSPARENT);
}

static void ghost_apply(void)
{
    // 拖离牌堆后画在独立的小窗口里，可以拖出主窗口。
    bool in_window = !torn || ghost.mode == GHOST_SETTLE || return_weight(cursor_point()) > 0;
    if (ghost.mode == GHOST_VANISH) in_window = false;
    ghost.in_window = in_window;
    if (!in_window)
    {
        ensure_ghost_window();
        if (ghost.window)
        {
            HWND handle = ui_window_handle(ghost.window);
            float scale = ui_window_scale(ghost.window);
            int size = (int)(ghost.window_size * scale);
            SetWindowPos(handle, HWND_TOPMOST, (int)lround(ghost.pose.x) - size / 2, (int)lround(ghost.pose.y) - size / 2, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ui_window_repaint(ghost.window);
            ui_window_request_frame(ghost.window);
        }
    }
    else if (ghost.window && IsWindowVisible(ui_window_handle(ghost.window)))
    {
        ShowWindow(ui_window_handle(ghost.window), SW_HIDE);
    }
    main_repaint();
}

static void ghost_begin(const Card *card, GhostPose start)
{
    card_free(&ghost.card);
    memset(&ghost.card, 0, sizeof(ghost.card));
    ghost.card.entry = shelf_history_entry_retain(card->entry);
    ghost.card.title = wcs_dup(card->title);
    ghost.card.detail = wcs_dup(card->detail);
    for (int index = 0; index < 3; index++) ghost.card.stack[index] = thumb_retain(card->stack[index]);
    ghost.pose = start;
    ghost.sway = 0;
    ghost.opacity = 1;
    ghost.has_last = false;
    ghost.mode = GHOST_FOLLOW;
    ghost_apply();
    main_request_frame();
}

static void ghost_stop(void)
{
    ghost.mode = GHOST_IDLE;
    if (ghost.window) ShowWindow(ui_window_handle(ghost.window), SW_HIDE);
    card_free(&ghost.card);
    memset(&ghost.card, 0, sizeof(ghost.card));
    if (M->window) main_repaint();
}

/// 拖动分三段：先被牌堆吸住、只跟一小段；拉过一定距离后脱离并略微放大，顿一下跟上光标；
/// 拖回牌堆上方时按卡牌位置让出空位，越靠近牌堆，卡牌被空位吸得越紧、越接近原来的大小。
static bool drag_goal(GhostPose start, GhostPose *goal)
{
    POINT cursor = cursor_point();
    double pulled_x = cursor.x - drag_start_cursor.x, pulled_y = cursor.y - drag_start_cursor.y;
    if (!torn && sqrt(pulled_x * pulled_x + pulled_y * pulled_y) / window_scale() < TEAR_DISTANCE)
    {
        *goal = (GhostPose){ start.x + pulled_x * HOLD_FOLLOW, start.y + pulled_y * HOLD_FOLLOW, start.scale, start.angle };
        return true;
    }
    torn = true;
    double free_x = cursor.x - grab_x, free_y = cursor.y - grab_y;
    double weight = return_weight(cursor);
    if (weight > 0)
    {
        POINT free_point = { (LONG)lround(free_x), (LONG)lround(free_y) };
        float x, y;
        ui_window_from_screen(M->window, free_point, &x, &y);
        ui_element_to_local(fan, &x, &y);
        set_gap(drag_index, slot_at(x - fan->bounds.left));
    }
    else set_gap(drag_index, -1);
    GhostPose slot;
    if (weight <= 0 || !pose_of(drag_index, &slot))
    {
        *goal = (GhostPose){ free_x, free_y, DETACHED_SCALE, 0 };
        return true;
    }
    double pull = RETURN_PULL * weight;
    *goal = (GhostPose){ free_x + (slot.x - free_x) * pull, free_y + (slot.y - free_y) * pull, DETACHED_SCALE + (slot.scale - DETACHED_SCALE) * weight,
        slot.angle * pull };
    return true;
}

static GhostPose drag_start_pose;

static void end_drag(bool cancel);

/// 位置、大小和角度每帧按指数衰减向目标靠拢，跟手但略有滞后；横向移动越快卡牌越向移动方向倾斜，停下后慢慢回正。
static void ghost_frame(double time)
{
    if (ghost.mode == GHOST_IDLE) return;
    if (!ghost.has_last)
    {
        ghost.has_last = true;
        ghost.last = time;
        main_request_frame();
        return;
    }
    double dt = time - ghost.last;
    if (dt <= 0)
    {
        main_request_frame();
        return;
    }
    if (dt > 0.05) dt = 0.05;
    ghost.last = time;

    GhostPose goal = ghost.pose;
    bool has_goal = true;
    switch (ghost.mode)
    {
        case GHOST_FOLLOW:
            // 松手的消息没送到时（如捕获被打断）卡牌会一直粘在光标上：按键实际已经松开就照常放下。
            if (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) >= 0 && dragging)
            {
                end_drag(false);
                return;
            }
            has_goal = drag_goal(drag_start_pose, &goal);
            break;
        case GHOST_SETTLE:
            has_goal = pose_of(ghost.settle_index, &goal);
            break;
        case GHOST_VANISH:
            goal = ghost.vanish_target;
            break;
        default:
            break;
    }
    if (!has_goal && ghost.mode == GHOST_SETTLE)
    {
        // 要放回的牌已不在（如列表刷新）：照常收尾，然后原地淡出。
        ghost.mode = GHOST_VANISH;
        ghost.vanish_target = ghost.pose;
        goal = ghost.pose;
    }
    double previous_x = ghost.pose.x;
    double follow = 1 - exp(-dt * FOLLOW_RATE);
    ghost.pose.x += (goal.x - ghost.pose.x) * follow;
    ghost.pose.y += (goal.y - ghost.pose.y) * follow;
    double speed = (ghost.pose.x - previous_x) / dt / window_scale();
    double sway_goal = speed * SWAY_PER_SPEED;
    sway_goal = sway_goal < -MAX_SWAY ? -MAX_SWAY : sway_goal > MAX_SWAY ? MAX_SWAY : sway_goal;
    ghost.sway += (sway_goal - ghost.sway) * (1 - exp(-dt * TURN_RATE));
    ghost.pose.angle += (goal.angle - ghost.pose.angle) * (1 - exp(-dt * TURN_RATE));
    ghost.pose.scale += (goal.scale - ghost.pose.scale) * (1 - exp(-dt * SCALE_RATE));

    if (ghost.mode == GHOST_VANISH)
    {
        ghost.opacity -= dt / FADE_SECONDS;
        if (ghost.opacity <= 0)
        {
            ghost_stop();
            return;
        }
    }
    else if (ghost.mode == GHOST_SETTLE)
    {
        double dx = goal.x - ghost.pose.x, dy = goal.y - ghost.pose.y;
        bool arrived = sqrt(dx * dx + dy * dy) < 1 && fabs(goal.scale - ghost.pose.scale) < 0.01 && fabs(goal.angle - ghost.pose.angle - ghost.sway) < 0.5;
        if (arrived || time - ghost.settle_start >= SETTLE_TIMEOUT)
        {
            int index = ghost.settle_index, move_to = ghost.move_to;
            ShelfHistoryEntry *entry = index >= 0 && index < card_count ? shelf_history_entry_retain(cards[index].entry) : NULL;
            if (index >= 0 && index < card_count) cards[index].hidden = false;
            ghost_stop();
            if (move_to == ghost.original_index) set_gap(-1, -1);
            // 牌堆保持放回后的排法；卡片沿用原来的，只换位置，记录在后台保存。
            else if (entry) shelf_history_move(entry, move_to);
            shelf_history_entry_release(entry);
            return;
        }
    }
    ghost_apply();
    main_request_frame();
}

// ───────────── 拖出和放下 ─────────────

static void begin_drag(int index)
{
    GhostPose start;
    if (!pose_of(index, &start)) return;
    // 截图前先收起选中状态，拖着的是一张干净的卡牌；起点是卡牌现在的样子。
    Card *card = &cards[index];
    GfxMatrix matrix = card_matrix(card, (float)card->scale.value, (float)card->angle.value, (float)card->x.value, (float)card->y.value);
    float cx = (card->base.left + card->base.right) / 2, cy = (card->base.top + card->base.bottom) / 2;
    gfx_matrix_apply(matrix, &cx, &cy);
    cx += fan->bounds.left;
    cy += fan->bounds.top;
    ui_element_to_window(fan, &cx, &cy);
    POINT screen = ui_window_to_screen(M->window, cx, cy);
    start = (GhostPose){ screen.x, screen.y, card->scale.value, card->angle.value };
    dragging = true;
    torn = false;
    drag_index = index;
    drag_start_cursor = cursor_point();
    grab_x = (float)(drag_start_cursor.x - start.x);
    grab_y = (float)(drag_start_cursor.y - start.y);
    drag_start_pose = start;
    last_hot = -1;
    hot = -1;
    card->hidden = true;
    anim_set(&card->hint, 0);
    dragged = index;
    gap = index;
    apply_state(true);
    ghost.original_index = index;
    ghost_begin(card, start);
}

/// 在牌堆上松手时卡牌飞进让出的空位并移到这个位置，取消时飞回原位；其他位置松手时在光标处打开置物架，卡牌缩小淡出。
static void end_drag(bool cancel)
{
    if (!dragging) return;
    dragging = false;
    POINT cursor = cursor_point();
    if (cancel || !torn || return_weight(cursor) > 0)
    {
        // 拖着的牌排到要放回的位置上，卡牌飞向它；落定后再调整列表顺序。
        int index = !cancel && torn && gap >= 0 ? gap : drag_index;
        set_gap(drag_index, index);
        ghost.settle_index = drag_index;
        ghost.original_index = drag_index;
        ghost.move_to = index;
        ghost.mode = GHOST_SETTLE;
        ghost.settle_start = frames_time();
        if (!anim_enabled()) ghost.settle_start = -1000;
        main_request_frame();
        return;
    }
    ShelfHistoryEntry *entry = shelf_history_entry_retain(cards[drag_index].entry);
    ghost.vanish_target = (GhostPose){ cursor.x, cursor.y, 0.7, 0 };
    ghost.mode = GHOST_VANISH;
    if (!anim_enabled()) ghost_stop();
    cards[drag_index].hidden = false;
    set_gap(-1, -1);
    shelves_restore(entry, shelves_open_at(cursor, false, true, EDGE_NONE));
    shelf_history_entry_release(entry);
}

bool fan_dragging(void)
{
    return dragging;
}

bool fan_escape(void)
{
    if (!dragging) return false;
    end_drag(true);
    return true;
}

// ───────────── 鼠标 ─────────────

static void restore_entry(int index)
{
    if (index < 0 || index >= card_count) return;
    ShelfHistoryEntry *entry = shelf_history_entry_retain(cards[index].entry);
    shelves_restore(entry, main_open_shelf());
    shelf_history_entry_release(entry);
}

static void remove_entry(int index)
{
    if (index < 0 || index >= card_count) return;
    ShelfHistoryEntry *entry = shelf_history_entry_retain(cards[index].entry);
    shelf_history_remove(entry);
    shelf_history_entry_release(entry);
}

static void fan_mouse(UiElement *element, const UiMouseEvent *event)
{
    float x = event->x - element->bounds.left, y = event->y - element->bounds.top;
    switch (event->kind)
    {
        case UI_MOUSE_ENTER:
        case UI_MOUSE_MOVE:
            if (dragging || ghost.mode == GHOST_SETTLE) return;
            if (pressed >= 0 && element->pressed && !remove_pressed &&
                (fabsf(x - press_x) >= DRAG_THRESHOLD || fabsf(y - press_y) >= DRAG_THRESHOLD))
            {
                int index = pressed;
                pressed = -1;
                begin_drag(index);
                return;
            }
            if (!element->pressed) set_hot(index_at(x, y));
            {
                bool over = on_remove_button(x, y);
                if (over != remove_hovered_now)
                {
                    remove_hovered_now = over;
                    main_repaint();
                }
                // 牌上显示“恢复置物架”的提示，删除按钮上显示它自己的。
                ui_set_tooltip(element, over ? UI_STR(STR_RemoveHistoryTooltip) : hot >= 0 ? UI_STR(STR_RestoreShelf) : UI_NO_TEXT);
                ui_window_set_tooltip(element->window, ui_element_tooltip(element));
            }
            break;
        case UI_MOUSE_LEAVE:
            if (!element->pressed && !dragging) set_hot(-1);
            remove_hovered_now = false;
            break;
        case UI_MOUSE_DOWN:
            if (event->right_button || ghost.mode != GHOST_IDLE) return;
            pressed = hot;
            remove_pressed = on_remove_button(x, y);
            press_x = x;
            press_y = y;
            main_repaint();
            break;
        case UI_MOUSE_UP:
        {
            if (dragging)
            {
                // 光标被别的窗口拿走（-1, -1）时退回原位。
                end_drag(event->x < 0 && event->y < 0 && !event->inside);
                pressed = -1;
                return;
            }
            int index = pressed;
            bool remove = remove_pressed;
            pressed = -1;
            remove_pressed = false;
            if (!event->inside || index < 0) return;
            int now = index_at(x, y);
            if (now != index) return;
            if (remove && on_remove_button(x, y)) remove_entry(index);
            else if (!remove) restore_entry(index);
            break;
        }
        default:
            break;
    }
}

static const UiElementType fan_type = { NULL, fan_arrange, fan_paint, fan_mouse, NULL, true, NULL, NULL, NULL, false, NULL, NULL };

UiElement *fan_create(void)
{
    fan = ui_element_new(&fan_type, sizeof(Fan));
    return fan;
}

// ───────────── 刷新 ─────────────

static void build_card(Card *card, ShelfHistoryEntry *entry)
{
    memset(card, 0, sizeof(*card));
    card->entry = shelf_history_entry_retain(entry);
    card->title = shelf_history_entry_title(entry);
    wchar_t *time = shelf_history_entry_time(entry);
    card->detail = str_history_card_detail(entry->count, time);
    free(time);
    // 最后放入的项目在最上面。
    for (int index = 0; index < 3 && index < entry->count; index++) card->stack[index] = thumb_create(entry->items[entry->count - 1 - index].path);
    anim_set(&card->scale, 1);
}

void fan_refresh(bool reuse)
{
    if (!fan) return;
    int count = 0;
    ShelfHistoryEntry *const *entries = shelf_history_entries(&count);
    if (count > SHELF_HISTORY_CAPACITY) count = SHELF_HISTORY_CAPACITY;
    // 调整顺序或删掉一张后，其余的牌沿用原来的，缩略图也不必重新读取；换语言时整组换新。
    Card *next = xcalloc((size_t)(count > 0 ? count : 1), sizeof(Card));
    bool *used = xcalloc((size_t)(card_count > 0 ? card_count : 1), sizeof(bool));
    for (int index = 0; index < count; index++)
    {
        int found = -1;
        for (int old = 0; reuse && old < card_count && found < 0; old++)
            if (!used[old] && cards[old].entry == entries[index]) found = old;
        if (found >= 0)
        {
            next[index] = cards[found];
            used[found] = true;
        }
        else build_card(&next[index], entries[index]);
        next[index].hidden = false;
    }
    for (int old = 0; old < card_count; old++)
        if (!used[old]) card_free(&cards[old]);
    free(used);
    free(cards);
    cards = next;
    card_count = count;
    if (dragging)
    {
        dragging = false;
        ghost_stop();
    }
    if (ghost.mode == GHOST_SETTLE) ghost_stop();
    dragged = gap = -1;
    hot = last_hot = pressed = -1;
    if (M->history_section) M->history_section->hidden = count == 0;
    layout_cards(reuse);
    // 光标下的牌直接保持选中，不会先落下再抽出。
    apply_state(reuse);
    ui_window_update_hover(M->window);
    main_relayout();
}

void fan_animate(double time)
{
    bool running = false;
    for (int index = 0; index < card_count; index++)
    {
        Card *card = &cards[index];
        running |= anim_update(&card->scale, time);
        running |= anim_update(&card->angle, time);
        running |= anim_update(&card->x, time);
        running |= anim_update(&card->y, time);
        running |= anim_update(&card->hint, time);
    }
    ghost_frame(time);
    if (running) main_request_frame();
}

void fan_close(void)
{
    dragging = false;
    ghost_stop();
    if (ghost.window) ui_window_close(ghost.window);
    ghost.window = NULL;
    for (int index = 0; index < card_count; index++) card_free(&cards[index]);
    free(cards);
    cards = NULL;
    card_count = 0;
    hot = last_hot = pressed = dragged = gap = -1;
    fan = NULL;
    panel_width = 0;
}

// ───────────── 测试用 ─────────────

int fan_card_count(void)
{
    return card_count;
}

const ShelfHistoryEntry *fan_card_entry(int index)
{
    return index >= 0 && index < card_count ? cards[index].entry : NULL;
}

int fan_card_thumbs(int index)
{
    int thumbs = 0;
    for (int slot = 0; index >= 0 && index < card_count && slot < 3; slot++) thumbs += cards[index].stack[slot] != NULL;
    return thumbs;
}

int fan_card_z(int index)
{
    return index >= 0 && index < card_count ? cards[index].z : 0;
}

bool fan_card_hot(int index)
{
    return index >= 0 && index == hot;
}

const void *fan_card_identity(int index)
{
    // 沿用的牌原样搬过去，标题的字符串还是原来那一份；重新生成的牌是新的字符串。
    return index >= 0 && index < card_count ? cards[index].title : NULL;
}
