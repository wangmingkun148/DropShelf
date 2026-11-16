// 概览页的卡片翻开成设置页、返回时翻回去；设置页底下露出其余几张牌的边，点一条边从牌堆里抽出那一页。
// 和 C# 版的 MainWindow.Flip.cs、MainWindow.StackEdges.cs、MainWindow.StackSwitch.cs 对应，时间和曲线相同。
// C# 版把页面缓存成位图再变换；这里每帧直接按变换重画页面，Direct2D 画这些很快，也就没有“背面空白”的退路。
#include "main_window_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979

/// 转到侧面时卡片离屏幕最近，略微变高，背面带一点阴影，看起来像在三维空间里翻转。
#define FLIP_LIFT 1.06
#define FLIP_SHADE 0.22
// 打开：正面翻到侧面，再由背面展开；返回：页面收到侧面，再由正面落位（毫秒）。
#define OPEN_FRONT_MS 170.0
#define OPEN_BACK_MS 300.0
#define BACK_PAGE_MS 230.0
#define BACK_FRONT_MS 320.0
#define OPEN_EDGE (OPEN_FRONT_MS / (OPEN_FRONT_MS + OPEN_BACK_MS))
#define BACK_EDGE (BACK_PAGE_MS / (BACK_PAGE_MS + BACK_FRONT_MS))
// 打开时概览页在换面后下沉淡出，返回时一开始就从稍低处上移淡入（毫秒）。
#define OVERVIEW_LEAVE_MS 200.0
#define OVERVIEW_REVEAL_MS 300.0
// 拖动翻牌：拖过与卡片同宽的距离正好翻过半圈，距离限制在这个范围内。
#define DRAG_TURN_MIN 240.0f
#define DRAG_TURN_MAX 480.0f
#define DRAG_START 8.0f
#define VELOCITY_WINDOW_MS 80.0
#define FLING_MS 150.0
#define TAP_TURN 0.04

// 牌堆的边：每条高 6，比上面一条两侧各缩进 16；鼠标移上去时这一条再往下露出 20，显示是哪一页。
#define EDGE_COUNT (PAGE_COUNT - 1)
#define EDGE_BAND 6.0f
#define EDGE_INSET 16.0f
#define EDGE_REVEAL 20.0f
#define EDGE_RADIUS 6.0f
#define EDGE_STAGGER 30.0
#define EDGE_FACE_FADE 0.45
#define STACK_SWITCH_SECONDS 0.46
#define STACK_PULL 12.0f

// ───────────── 曲线 ─────────────

/// 起止都静止、在 edge 处正好过半的曲线：y1 = 0、y2 = 1 时参数 0.5 处 y = 0.5，x = 0.125 + 0.375 × (x1 + x2)。
static Ease halfway_at(double edge, double x1)
{
    return EASE_BEZIER_OF(x1, 0, (edge - 0.125) / 0.375 - x1, 1);
}

static double open_turn(double s) { return ease_apply(halfway_at(OPEN_EDGE, 0.3), s); }
static double back_turn(double s) { return ease_apply(halfway_at(BACK_EDGE, 0.42), s); }
static double open_travel(double s) { return ease_apply(EASE_BEZIER_OF(0.55, 0, 0.25, 1), s); }

/// 返回：卡片落位时略微越过原位（约为全程的 1.8%）再回来，停下时速度为零。
static double back_travel(double t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    double x = -cos(PI * t / 2);
    return 1 + 1.7 * x * x * x + 0.7 * x * x;
}

static double cubic_out(double t) { return ease_apply(EASE_OF(EASE_CUBIC_OUT), t); }
static double cubic_in(double t) { return ease_apply(EASE_OF(EASE_CUBIC_IN), t); }

static double clamp01(double value)
{
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

static double smooth(double x)
{
    x = clamp01(x);
    return x * x * (3 - 2 * x);
}

/// 从 0 到 1 的三次曲线，起止斜率为 m0、m1（匀速为 1）；斜率都在 0 到 3 之间时单调。
static double glide(double u, double m0, double m1)
{
    return (((m0 + m1 - 2) * u + 3 - 2 * m0 - m1) * u + m0) * u;
}

static GfxRect lerp_rect(GfxRect from, GfxRect to, double amount)
{
    float a = (float)amount;
    return (GfxRect){ from.left + (to.left - from.left) * a, from.top + (to.top - from.top) * a,
        from.right + (to.right - from.right) * a, from.bottom + (to.bottom - from.bottom) * a };
}

static float rect_width(GfxRect rect) { return rect.right - rect.left; }
static float rect_height(GfxRect rect) { return rect.bottom - rect.top; }

// ───────────── 状态 ─────────────

typedef struct Flip
{
    bool opening;
    int page;
    PageView *view;
    UiElement *tile;
    int others[EDGE_COUNT];         // 其余几张卡片，按牌堆里从近到远排列
    GfxRect other_homes[EDGE_COUNT];
    GfxRect home, area, edge;       // 卡片的原位、页面那张牌的位置、侧立换面时的位置
    bool dragged;                   // 拖动开始的翻牌，侧栏标题跟着朝向屏幕的一面切换
    int header;
    double total;
    double progress;
    int page_shown;                 // 朝向屏幕的是页面（1）还是卡片（0），-1 还没摆过
    bool dragging;
    double peak;
    double run_from, run_to, run_ms, start_slope, end_slope;
    double run_begun;
    double ran;                     // 已经播放的时间（毫秒），-1 表示还没有第一帧
    double lag;
    double last;
    bool running;
    // 背面当前的样子
    GfxMatrix back_matrix;
    GfxRect back_card, back_edge;
    float shade;
    GfxMatrix tile_matrix;
} Flip;

typedef struct StackEdge
{
    UiElement base;
    int index;
    int page;                       // 这条边是哪一页
    GfxRect rect;                   // 现在的位置（含露出和下移）
    GfxRect home;                   // 翻牌中卡面原来的位置；宽为 0 时没有卡面
    float top_radius, bottom_radius;
    float frame_opacity, face_opacity;
    bool label_visible;
    Anim reveal_height, reveal_shift, label_opacity;
    GfxRect band;                   // 静止时的位置
} StackEdge;

typedef struct StackSlide
{
    int page;
    int from_page;
    GfxRect from_area, to_area;
    GfxRect bands[EDGE_COUNT];
    int slots[EDGE_COUNT];
    int selected;
    Anim progress;
    GfxRect new_card, old_card;
    GfxMatrix new_matrix, old_matrix;
    float old_opacity;
} StackSlide;

static Flip flip;
static bool flipping;               // flip 有效
static bool morphing;
static StackSlide slide;
static bool sliding;
static StackEdge *edges[EDGE_COUNT];
static GfxRect edge_sheet;          // 上次摆放牌的边时页面那张牌的位置

// 拖动翻牌
static bool press_active;
static float press_x, press_y;
static UiElement *pressed_tile;
static float drag_origin, drag_sign, drag_span;
typedef struct DragSample
{
    double time;
    double turn;
} DragSample;
static DragSample samples[64];
static int sample_count;

bool flip_morphing(void)
{
    return morphing;
}

static double now_ms(void)
{
    return frames_time() * 1000;
}

static GfxRect bounds_of(UiElement *element)
{
    GfxRect rect = element->bounds;
    return rect;
}

static int others_of(int page, int *others)
{
    for (int offset = 1; offset < PAGE_COUNT; offset++) others[offset - 1] = (page + offset) % PAGE_COUNT;
    return EDGE_COUNT;
}

/// 第 k 条边（0 最近）静止时露在页面那张牌 sheet 底下的位置。
static GfxRect band_of(GfxRect sheet, int k)
{
    float inset = EDGE_INSET * (k + 1);
    float width = rect_width(sheet) - 2 * inset;
    return (GfxRect){ sheet.left + inset, sheet.bottom + EDGE_BAND * k, sheet.left + inset + (width > 0 ? width : 0), sheet.bottom + EDGE_BAND * k + EDGE_BAND };
}

// ───────────── 牌的边 ─────────────

static void edge_paint(UiElement *element, double time)
{
    (void)time;
    StackEdge *edge = (StackEdge *)element;
    GfxRect rect = edge->rect;
    if (rect_width(rect) <= 0.5f || rect_height(rect) <= 0.5f) return;
    gfx_push_clip(rect);
    // 卡面：卡片原来的样子拉伸到这条边现在的大小。
    if (edge->face_opacity > 0.001f && rect_width(edge->home) > 0 && rect_height(edge->home) > 0)
    {
        float sx = rect_width(rect) / rect_width(edge->home), sy = rect_height(rect) / rect_height(edge->home);
        GfxMatrix matrix = gfx_matrix_multiply(gfx_matrix_translate(-edge->home.left, -edge->home.top),
            gfx_matrix_multiply((GfxMatrix){ sx, 0, 0, sy, 0, 0 }, gfx_matrix_translate(rect.left, rect.top)));
        // 卡面画在它在概览页里的位置，再变到这条边上。
        GfxRect saved = M->tiles[edge->page]->bounds;
        M->tiles[edge->page]->bounds = edge->home;
        pages_paint_tile_face(M->tiles[edge->page], matrix, edge->face_opacity);
        M->tiles[edge->page]->bounds = saved;
    }
    if (edge->frame_opacity > 0.001f)
    {
        // 其余几张牌背面朝上，露出的边是牌背的斜格纹。
        gfx_push_opacity(edge->frame_opacity);
        ThemePattern back = theme_pattern(THEME_PATTERN_SHEET_BORDER);
        gfx_fill_rounded_corners_pattern(rect, edge->top_radius, edge->bottom_radius, &back);
        gfx_stroke_rounded_open_top(rect, edge->top_radius, edge->bottom_radius, theme_color(THEME_CARD_BACK_LINE), 1);
        gfx_pop_layer();
    }
    float label = (float)edge->label_opacity.value;
    if (edge->label_visible && label > 0.001f)
    {
        // 页名在这条边露出的部分里：图标和名称，靠下，用牌桌上的文字颜色。
        Color color = theme_color(THEME_TABLE_TEXT);
        color.a = (uint8_t)(color.a * label);
        GfxTextStyle glyph = { GFX_FONT_ICON, 12, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        GfxTextStyle text = { GFX_FONT_TEXT, 12, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        float top = rect.bottom - EDGE_REVEAL, left = rect.left + 14;
        float glyph_width = gfx_text_measure(main_page_glyph(edge->page), &glyph, 0).width;
        gfx_text_draw(main_page_glyph(edge->page), (GfxRect){ left, top + 1, left + glyph_width + 1, rect.bottom }, &glyph, color);
        gfx_text_draw(S(main_page_title(edge->page)), (GfxRect){ left + glyph_width + 8, top, rect.right - 8, rect.bottom }, &text, color);
    }
    gfx_pop_clip();
}

static void switch_stack_page(int index);
static void reveal_edge(int index);
static void pose_stack_switch(double progress);

static void edge_mouse(UiElement *element, const UiMouseEvent *event)
{
    StackEdge *edge = (StackEdge *)element;
    if (event->kind == UI_MOUSE_ENTER) reveal_edge(edge->index);
    else if (event->kind == UI_MOUSE_LEAVE) reveal_edge(-1);
    else if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) switch_stack_page(edge->index);
}

static const UiElementType edge_type = { NULL, NULL, edge_paint, edge_mouse, NULL, true };

/// 牌的边放在一层里，近的画在上面；这一层自己不接收鼠标。
static void stack_paint(UiElement *element, double time)
{
    (void)element;
    // 由 flip_paint_stage 按顺序画。
    (void)time;
}

static void stack_arrange(UiElement *element)
{
    (void)element;
}

static const UiElementType stack_type = { NULL, stack_arrange, stack_paint, NULL, NULL, false, NULL, NULL, NULL, false, NULL, NULL, false, true };

UiElement *flip_create_stack_layer(void)
{
    UiElement *layer = ui_element_new(&stack_type, sizeof(UiElement));
    layer->hidden = true;
    // 近的边后加，点击时先问它。
    for (int k = EDGE_COUNT - 1; k >= 0; k--)
    {
        StackEdge *edge = (StackEdge *)ui_element_new(&edge_type, sizeof(StackEdge));
        edge->index = k;
        edges[k] = edge;
        ui_add(layer, &edge->base);
    }
    return layer;
}

/// 边现在的位置：静止时在 band，鼠标移上去时这一条变高、后面的往下挪。
static void apply_edge_rect(StackEdge *edge, GfxRect rect)
{
    edge->rect = rect;
    edge->base.bounds = rect;
}

/// 在卡片原位 home 和露出的边 band 之间按 travel 摆放一条边：圆角由卡片的四个角变成只有下面两个，卡面淡出，外框淡入。
static void place_edge(StackEdge *edge, GfxRect home, GfxRect band, double travel)
{
    GfxRect rect = lerp_rect(home, band, travel);
    if (rect.right < rect.left) rect.right = rect.left;
    if (rect.bottom < rect.top) rect.bottom = rect.top;
    apply_edge_rect(edge, rect);
    edge->home = home;
    double t = clamp01(travel);
    edge->top_radius = (float)(TILE_RADIUS * (1 - t));
    edge->bottom_radius = (float)(TILE_RADIUS + (EDGE_RADIUS - TILE_RADIUS) * t);
    double frame = smooth(t / EDGE_FACE_FADE);
    edge->frame_opacity = (float)frame;
    edge->face_opacity = rect_width(home) > 0 ? (float)(1 - frame) : 0;
}

static void reset_edge_reveal(void)
{
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        anim_set(&edges[k]->reveal_height, EDGE_BAND);
        anim_set(&edges[k]->reveal_shift, 0);
        anim_set(&edges[k]->label_opacity, 0);
    }
}

/// 静止时按露出的高度和下移摆好。
static void pose_static_edges(void)
{
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        StackEdge *edge = edges[k];
        GfxRect band = edge->band;
        float shift = (float)edge->reveal_shift.value;
        GfxRect rect = { band.left, band.top + shift, band.right, band.top + shift + (float)edge->reveal_height.value };
        apply_edge_rect(edge, rect);
        edge->home = (GfxRect){ 0 };
        edge->top_radius = 0;
        edge->bottom_radius = EDGE_RADIUS;
        edge->frame_opacity = 1;
        edge->face_opacity = 0;
    }
}

void flip_layout_edges(void)
{
    if (morphing || M->stack_layer->hidden || M->page == PAGE_OVERVIEW) return;
    GfxRect sheet = main_sheet_area();
    if (memcmp(&sheet, &edge_sheet, sizeof(sheet)) == 0)
    {
        pose_static_edges();
        return;
    }
    edge_sheet = sheet;
    reset_edge_reveal();
    for (int k = 0; k < EDGE_COUNT; k++) edges[k]->band = band_of(sheet, k);
    pose_static_edges();
}

/// 鼠标移到第 index 条边上（-1 为移开）：这一条往下多露出一截，显示是哪一页；后面的边跟着下移，仍然露在它下面。
static void reveal_edge(int index)
{
    if (morphing || M->page == PAGE_OVERVIEW) return;
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        anim_start(&edges[k]->reveal_height, k == index ? EDGE_BAND + EDGE_REVEAL : EDGE_BAND, 0.18, MOTION_EASE);
        anim_start(&edges[k]->reveal_shift, index >= 0 && k > index ? EDGE_REVEAL : 0, 0.18, MOTION_EASE);
        anim_start(&edges[k]->label_opacity, k == index ? 1 : 0, 0.18, MOTION_EASE);
    }
    main_request_frame();
}

/// 设置页静止时露出其余几张牌的边，回到概览页时收起。
static void show_edges(void)
{
    if (M->page == PAGE_OVERVIEW)
    {
        M->stack_layer->hidden = true;
        return;
    }
    int others[EDGE_COUNT];
    others_of(M->page, others);
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        edges[k]->page = others[k];
        edges[k]->label_visible = true;
        edges[k]->base.hidden = false;
        ui_set_tooltip(&edges[k]->base, UI_STR(main_page_title(others[k])));
    }
    M->stack_layer->hidden = false;
    M->stack_layer->hit_through = false;
    memset(&edge_sheet, 0, sizeof(edge_sheet));
    flip_layout_edges();
}

// ───────────── 页面的显示 ─────────────

static void set_host_visible(PageView *view, bool visible)
{
    if (view->host->hidden == !visible) return;
    view->host->hidden = !visible;
    main_relayout();
}

/// 概览页列出各项设置，点进去显示对应页面。
void flip_show_page(int page)
{
    M->page = page;
    M->overview_scroll->hidden = page != PAGE_OVERVIEW;
    for (int index = 0; index < PAGE_COUNT; index++) set_host_visible(&M->pages[index], index == page);
    main_relayout();
    ui_window_layout(M->window);
    show_edges();
    ui_window_update_hover(M->window);
}

/// 概览页停在静止状态。
static void settle_overview(void)
{
    M->overview_page->opacity = 1;
    M->overview_page->transformed = false;
}

static void settle_tiles(void)
{
    for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->opacity = 1;
}

/// 页面回到静止状态：去掉变换和裁剪，边框交还给牌。
static void clear_flip_view(PageView *view)
{
    view->host->transformed = false;
    view->host->clip = false;
    view->sheet_border_hidden = false;
    ui_scroll_set_bar_hidden(view->scroll, false, false);
}

static void end_morph(void)
{
    morphing = false;
    flipping = false;
    main_repaint();
}

static void land(bool forward, bool jump);

void flip_cancel(void)
{
    if (!morphing) return;
    if (sliding)
    {
        sliding = false;
        for (int index = 0; index < PAGE_COUNT; index++) M->pages[index].sheet_border_hidden = false;
        M->stack_layer->hit_through = false;
        end_morph();
        flip_show_page(slide.page);
        return;
    }
    if (flipping)
    {
        // 拖动中被打断时退回起点，其余的直接摆到要去的终点。
        bool forward = !flip.dragging && flip.run_to >= 1;
        if (flip.dragging)
        {
            flip.dragging = false;
            press_active = false;
            if (GetCapture() == ui_window_handle(M->window)) ReleaseCapture();
        }
        land(forward, true);
    }
}

// ───────────── 翻牌的摆放 ─────────────

/// 卡片本身：从原位走向侧立处的比例为 travel，转过 angle（弧度，0 为正对屏幕，π/2 为侧立）。
static void pose_tile(double travel, double angle)
{
    GfxRect home = flip.home;
    GfxRect card = lerp_rect(home, flip.edge, travel);
    float sx = (float)(rect_width(card) * fabs(cos(angle)) / rect_width(home));
    float sy = (float)(rect_height(card) * (1 + (FLIP_LIFT - 1) * sin(angle)) / rect_height(home));
    float home_x = (home.left + home.right) / 2, home_y = (home.top + home.bottom) / 2;
    float card_x = (card.left + card.right) / 2, card_y = (card.top + card.bottom) / 2;
    // 绕卡片的中心缩放，再挪到 card 的中心。
    flip.tile_matrix = (GfxMatrix){ sx, 0, 0, sy, card_x - sx * home_x, card_y - sy * home_y };
}

/// 背面当前的矩形为 card，转过 angle（侧立为 π/2，正对屏幕为 π），绕中心横向缩放 kx、纵向缩放 ky；
/// 页面按 s = card 宽 / 页面宽 等比缩放后裁成 card 的高度。展开到 page 且正对屏幕时矩阵正好是单位矩阵。
static void pose_back(GfxRect card, GfxRect page, double angle)
{
    float kx = (float)fabs(cos(angle));
    float ky = (float)(1 + (FLIP_LIFT - 1) * sin(angle));
    float s = rect_width(card) / rect_width(page);
    float cx = (card.left + card.right) / 2, cy = (card.top + card.bottom) / 2;
    flip.back_matrix = (GfxMatrix){ kx * s, 0, 0, ky * s, cx + kx * (card.left - s * page.left - cx), cy + ky * (card.top - s * page.top - cy) };
    float height = rect_height(card) / s;
    flip.back_card = (GfxRect){ page.left, page.top, page.right, page.top + height };
    flip.back_edge = flip.back_card;
    flip.shade = (float)(FLIP_SHADE * sin(angle));
    PageView *view = flip.view;
    view->host->transformed = true;
    view->host->transform = flip.back_matrix;
    view->host->clip = true;
    view->host->clip_rect = flip.back_card;
    view->host->clip_radius = SHEET_RADIUS;
}

/// 概览页的透明度和下移距离。
static void pose_overview(double opacity, double shift)
{
    M->overview_page->opacity = (float)opacity;
    M->overview_page->transformed = true;
    M->overview_page->transform = gfx_matrix_translate(M->page_shift, (float)shift);
}

/// 打开时其余卡片在换面后依次离开原位，滑进页面那张牌底下排成牌的边，近的先到，最远的和页面同时落定；返回时反过来。
static void pose_edges(void)
{
    double start = flip.opening ? OPEN_FRONT_MS : 0;
    double span = flip.total - start - EDGE_STAGGER * (EDGE_COUNT - 1);
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        double u = clamp01((flip.progress * flip.total - start - EDGE_STAGGER * k) / span);
        place_edge(edges[k], flip.other_homes[k], band_of(flip.area, k), flip.opening ? open_travel(u) : 1 - back_travel(u));
    }
}

/// 侧立时换面：朝向屏幕的是页面时，页面显示在背面上，概览页和牌的边都挖出卡片的形状，卡片本身藏起；反过来页面藏起。
static void show_side(bool page_side)
{
    flip.page_shown = page_side;
    int header = page_side ? flip.page : PAGE_OVERVIEW;
    if (flip.dragged && flip.header != header)
    {
        flip.header = header;
        main_set_panel_header(header, true);
    }
}

static void pose(void)
{
    double s = flip.progress;
    bool page_side = flip.opening ? s >= OPEN_EDGE : s < BACK_EDGE;
    if (flip.page_shown != (int)page_side) show_side(page_side);
    if (flip.opening)
    {
        double travel = open_travel(s);
        double angle = PI * open_turn(s);
        double edge_travel = open_travel(OPEN_EDGE);
        if (page_side) pose_back(lerp_rect(flip.edge, flip.area, (travel - edge_travel) / (1 - edge_travel)), flip.area, angle);
        else pose_tile(travel / edge_travel, angle);
        double leave = cubic_in(clamp01((s - OPEN_EDGE) * flip.total / OVERVIEW_LEAVE_MS));
        pose_overview(1 - leave, 10 * leave);
    }
    else
    {
        double travel = back_travel(s);
        double angle = PI * (1 - back_turn(s));
        double edge_travel = back_travel(BACK_EDGE);
        if (page_side) pose_back(lerp_rect(flip.area, flip.edge, travel / edge_travel), flip.area, angle);
        else pose_tile(1 - (travel - edge_travel) / (1 - edge_travel), angle);
        double reveal = cubic_out(clamp01(s * flip.total / OVERVIEW_REVEAL_MS));
        pose_overview(reveal, 14 * (1 - reveal));
    }
    pose_edges();
    main_repaint();
}

// ───────────── 开始、播放和落定 ─────────────

static void attach_tiles(void)
{
    reset_edge_reveal();
    M->stack_layer->hidden = false;
    M->stack_layer->hit_through = true;
    flip.tile->opacity = 0;
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        edges[k]->page = flip.others[k];
        edges[k]->label_visible = false;
        edges[k]->base.hidden = false;
        M->tiles[flip.others[k]]->opacity = 0;
    }
}

static void begin_session(void)
{
    morphing = true;
    flipping = true;
    flip.dragging = flip.dragged;
    flip.page_shown = -1;
    flip.ran = -1;
    flip.running = false;
    flip.view->sheet_border_hidden = true;
    ui_scroll_set_bar_hidden(flip.view->scroll, true, false);
    attach_tiles();
    pose();
}

/// 点开的卡片翻过来：卡片本身转到侧面，换成目标页面作为背面，页面一边转回正面一边从卡片大小展开到整页。
static bool start_open(int page, bool drag)
{
    if (morphing || M->side_panel_moving || M->page != PAGE_OVERVIEW || page < 0 || page >= PAGE_COUNT) return false;
    PageView *view = &M->pages[page];
    // 页面先排好版（不显示），量得到那张牌的位置。
    ui_scroll_to_top(view->scroll);
    view->host->hidden = false;
    main_relayout();
    ui_window_layout(M->window);
    memset(&flip, 0, sizeof(flip));
    flip.opening = true;
    flip.page = page;
    flip.view = view;
    flip.tile = M->tiles[page];
    flip.home = bounds_of(flip.tile);
    flip.area = main_sheet_area();
    flip.edge = lerp_rect(flip.home, flip.area, open_travel(OPEN_EDGE));
    others_of(page, flip.others);
    for (int k = 0; k < EDGE_COUNT; k++) flip.other_homes[k] = bounds_of(M->tiles[flip.others[k]]);
    flip.dragged = drag;
    flip.header = drag ? PAGE_OVERVIEW : page;
    flip.total = OPEN_FRONT_MS + OPEN_BACK_MS;
    if (!drag) main_set_panel_header(page, true);
    begin_session();
    return true;
}

/// 返回时反过来：页面带着内容缩小并转到侧面，换成卡片本身转回正面落进原位，落位时略微回弹。
static bool start_back(bool drag)
{
    if (morphing || M->side_panel_moving || M->page == PAGE_OVERVIEW) return false;
    int page = M->page;
    PageView *view = &M->pages[page];
    if (!drag) main_set_panel_header(PAGE_OVERVIEW, true);
    // 概览页回到顶部、排好版；卡片的原位要在概览页静止时量。
    settle_overview();
    M->overview_scroll->hidden = false;
    ui_scroll_to_top(M->overview_scroll);
    main_relayout();
    ui_window_layout(M->window);
    memset(&flip, 0, sizeof(flip));
    flip.opening = false;
    flip.page = page;
    flip.view = view;
    flip.tile = M->tiles[page];
    flip.home = bounds_of(flip.tile);
    flip.area = main_sheet_area();
    flip.edge = lerp_rect(flip.area, flip.home, back_travel(BACK_EDGE));
    others_of(page, flip.others);
    for (int k = 0; k < EDGE_COUNT; k++) flip.other_homes[k] = bounds_of(M->tiles[flip.others[k]]);
    flip.dragged = drag;
    flip.header = drag ? page : PAGE_OVERVIEW;
    flip.total = BACK_PAGE_MS + BACK_FRONT_MS;
    begin_session();
    return true;
}

/// 从当前进度走到 target（1 为终点，0 为起点）。rate 是起步的速度（每毫秒的进度），小于 0 表示用匀速播放的速度。
static void run(double target, double rate)
{
    flip.dragging = false;
    if (!anim_enabled())
    {
        // 关闭了动画效果：直接摆到终点（或退回起点）。
        flip.progress = target;
        flip.run_to = target;
        pose();
        land(target >= 1, true);
        return;
    }
    double natural = 1 / flip.total;
    double distance = fabs(target - flip.progress);
    double sign = target > flip.progress ? 1 : target < flip.progress ? -1 : 0;
    double speed = (rate < 0 ? natural : rate) * sign;
    if (speed < 0) speed = 0;
    double milliseconds = distance / natural;
    if (speed > natural)
    {
        double fast = 2 * distance / (speed + natural);
        milliseconds = fast > milliseconds / 3 ? fast : milliseconds / 3;
    }
    flip.run_from = flip.progress;
    flip.run_to = target;
    flip.run_ms = milliseconds;
    flip.start_slope = distance > 0 ? fmin(3, speed * milliseconds / distance) : 1;
    flip.end_slope = distance > 0 ? natural * milliseconds / distance : 1;
    flip.run_begun = now_ms();
    flip.ran = -1;
    flip.lag = 0;
    flip.running = true;
    main_request_frame();
}

/// 翻牌走到头：页面或概览页回到静止状态。forward 表示走到了终点；jump 表示被打断，直接摆好。
static void land(bool forward, bool jump)
{
    flip.running = false;
    PageView *view = flip.view;
    bool page_shown = flip.opening == forward;
    int page = page_shown ? flip.page : PAGE_OVERVIEW;
    settle_tiles();
    settle_overview();
    clear_flip_view(view);
    end_morph();
    flip_show_page(page);
    if (flip.header != page)
    {
        flip.header = page;
        main_set_panel_header(page, !jump);
    }
    if (page_shown && !jump) ui_scroll_set_bar_hidden(view->scroll, false, true);
}

void flip_open(int page)
{
    if (start_open(page, false)) run(1, -1);
}

void flip_back(void)
{
    if (start_back(false)) run(1, -1);
}

void flip_animate(double time)
{
    (void)time;
    // 不在翻牌时，页面跟着状态栏让位（平移）。
    if (!morphing)
    {
        bool shifted = M->page_shift != 0;
        for (int index = 0; index < PAGE_COUNT; index++)
        {
            UiElement *host = M->pages[index].host;
            host->transformed = shifted;
            host->transform = gfx_matrix_translate(M->page_shift, 0);
        }
        M->stack_layer->transformed = shifted;
        M->stack_layer->transform = gfx_matrix_translate(M->page_shift, 0);
        M->overview_page->transformed = shifted;
        M->overview_page->transform = gfx_matrix_translate(M->page_shift, 0);
    }
    bool edges_running = false;
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        edges_running |= anim_update(&edges[k]->reveal_height, time);
        edges_running |= anim_update(&edges[k]->reveal_shift, time);
        edges_running |= anim_update(&edges[k]->label_opacity, time);
    }
    if (edges_running)
    {
        if (!morphing) pose_static_edges();
        main_request_frame();
    }
    if (sliding)
    {
        bool running = anim_update(&slide.progress, time);
        pose_stack_switch(slide.progress.value);
        if (running) main_request_frame();
        else
        {
            sliding = false;
            for (int index = 0; index < PAGE_COUNT; index++) M->pages[index].sheet_border_hidden = false;
            for (int index = 0; index < PAGE_COUNT; index++) clear_flip_view(&M->pages[index]);
            end_morph();
            flip_show_page(slide.page);
        }
        return;
    }
    if (!flipping || flip.dragging || !flip.running) return;
    // 按实际经过的时间推进；卡顿时一帧最多前进一小段，放慢而不跳过。
    double current = time * 1000;
    if (flip.ran < 0)
    {
        double elapsed = current - flip.run_begun;
        double first = FRAME_MAXIMUM_STEP * 1000 / 2;
        flip.ran = elapsed < 0 ? 0 : elapsed > first ? first : elapsed;
    }
    else
    {
        // 同一帧重复调用，或者时刻比上次还早（窗口消息里现画、用的是“现在”）：这次不走，下一帧接着走。
        if (current <= flip.last)
        {
            main_request_frame();
            return;
        }
        double lag_seconds = flip.lag / 1000;
        flip.ran += frame_clock_step((current - flip.last) / 1000, &lag_seconds) * 1000;
        flip.lag = lag_seconds * 1000;
    }
    flip.last = current;
    double u = flip.run_ms > 0 ? fmin(1, flip.ran / flip.run_ms) : 1;
    flip.progress = flip.run_from + (flip.run_to - flip.run_from) * glide(u, flip.start_slope, flip.end_slope);
    pose();
    if (u >= 1) land(flip.run_to >= 1, false);
    else main_request_frame();
}

// ───────────── 拖动翻牌 ─────────────

static void drag_flip(double turn)
{
    if (!flipping || !flip.dragging) return;
    turn = clamp01(turn);
    if (turn > flip.peak) flip.peak = turn;
    // 由转动曲线反求进度；位置、大小和概览页都按这个进度摆放，和点击时走的是同一条路线。
    Ease curve = flip.opening ? halfway_at(OPEN_EDGE, 0.3) : halfway_at(BACK_EDGE, 0.42);
    flip.progress = ease_bezier_inverse(curve, turn);
    pose();
}

static double turn_at(double s)
{
    return flip.opening ? open_turn(s) : back_turn(s);
}

/// 松手：按手上的速度往前推算一小段，过了侧立就翻过去，否则退回；在卡片上几乎没拖动就松手时按点击处理。
static void release_flip(double velocity, bool cancel)
{
    if (!flipping || !flip.dragging) return;
    bool tap = !cancel && flip.opening && flip.peak < TAP_TURN;
    bool forward = tap || (!cancel && turn_at(flip.progress) + velocity * FLING_MS >= 0.5);
    double low = fmax(0, flip.progress - 1e-3), high = fmin(1, flip.progress + 1e-3);
    double slope = (turn_at(high) - turn_at(low)) / (high - low);
    run(forward ? 1 : 0, tap ? -1 : slope > 0 ? velocity / slope : 0);
}

static double drag_velocity(void)
{
    if (sample_count == 0) return 0;
    double now = now_ms();
    double age0 = now - samples[0].time;
    double span = age0 < VELOCITY_WINDOW_MS ? age0 : VELOCITY_WINDOW_MS;
    if (span <= 1) return 0;
    double start = samples[sample_count - 1].turn;
    for (int index = 0; index < sample_count; index++)
    {
        double age = now - samples[index].time;
        if (age > span) continue;
        int previous = index > 0 ? index - 1 : 0;
        double before = now - samples[previous].time;
        double from = samples[previous].turn;
        start = before > age ? from + (samples[index].turn - from) * (before - span) / (before - age) : samples[index].turn;
        break;
    }
    return (samples[sample_count - 1].turn - start) / span;
}

/// 按在设置页的空白处、文字或设置卡片的底色上，而不是按钮、选项、开关、输入框、滚动条或可拖动的操作卡片上。
static bool is_blank(float x, float y)
{
    PageView *view = &M->pages[M->page];
    UiElement *hit = ui_hit_test(M->root, x, y);
    if (!hit) return ui_hit_test_any(M->root, x, y) && ui_is_within(ui_hit_test_any(M->root, x, y), view->host);
    if (!ui_is_within(hit, view->host)) return false;
    return hit == view->scroll && !ui_scroll_hits_bar(view->scroll, x, y);
}

static UiElement *tile_at(float x, float y)
{
    UiElement *hit = ui_hit_test(M->root, x, y);
    for (int index = 0; index < PAGE_COUNT; index++)
        if (hit == M->tiles[index]) return hit;
    return NULL;
}

bool flip_escape(void)
{
    if (!flipping || !flip.dragging) return false;
    press_active = false;
    release_flip(0, true);
    if (GetCapture() == ui_window_handle(M->window)) ReleaseCapture();
    return true;
}

bool flip_mouse(const UiMouseEvent *event)
{
    HWND handle = ui_window_handle(M->window);
    bool dragging = flipping && flip.dragging;
    switch (event->kind)
    {
        case UI_MOUSE_DOWN:
            press_active = false;
            // 翻牌期间页面跟着卡片变形，点击都先不处理。
            if (morphing) return true;
            if (event->right_button || dialogs_open()) return false;
            if (M->page == PAGE_OVERVIEW)
            {
                pressed_tile = tile_at(event->x, event->y);
                press_active = pressed_tile != NULL;
            }
            else
            {
                pressed_tile = NULL;
                press_active = is_blank(event->x, event->y);
            }
            press_x = event->x;
            press_y = event->y;
            return false;
        case UI_MOUSE_MOVE:
        {
            if (!press_active) return morphing && !dragging;
            if (GetKeyState(VK_LBUTTON) >= 0)
            {
                press_active = false;
                return false;
            }
            if (!dragging)
            {
                float dx = event->x - press_x, dy = event->y - press_y;
                if (fabsf(dx) < DRAG_START && fabsf(dy) < DRAG_START) return false;
                if (fabsf(dy) >= fabsf(dx))
                {
                    press_active = false;
                    return false;
                }
                // 窗口拿走鼠标：卡片不再处于按下状态，松手时也不会再触发点击。
                ui_window_cancel_press(M->window);
                int page = -1;
                for (int index = 0; index < PAGE_COUNT; index++)
                    if (M->tiles[index] == pressed_tile) page = index;
                bool ok = pressed_tile ? start_open(page, true) : start_back(true);
                if (!ok)
                {
                    press_active = false;
                    return false;
                }
                SetCapture(handle);
                drag_origin = event->x;
                drag_sign = dx > 0 ? 1.0f : -1.0f;
                float span = rect_width(flip.tile->bounds);
                drag_span = span < DRAG_TURN_MIN ? DRAG_TURN_MIN : span > DRAG_TURN_MAX ? DRAG_TURN_MAX : span;
                sample_count = 0;
            }
            double turn = clamp01((event->x - drag_origin) * drag_sign / drag_span);
            double now = now_ms();
            if (sample_count == 64)
            {
                memmove(samples, samples + 1, sizeof(DragSample) * 63);
                sample_count--;
            }
            samples[sample_count++] = (DragSample){ now, turn };
            while (sample_count > 2 && now - samples[1].time > VELOCITY_WINDOW_MS)
            {
                memmove(samples, samples + 1, sizeof(DragSample) * (size_t)(sample_count - 1));
                sample_count--;
            }
            drag_flip(turn);
            return true;
        }
        case UI_MOUSE_UP:
            press_active = false;
            if (dragging)
            {
                release_flip(drag_velocity(), false);
                if (GetCapture() == handle) ReleaseCapture();
                return true;
            }
            return morphing;
        case UI_MOUSE_WHEEL:
            return morphing;
        default:
            return false;
    }
}

void flip_close(void)
{
    memset(&flip, 0, sizeof(flip));
    memset(&slide, 0, sizeof(slide));
    flipping = morphing = sliding = false;
    press_active = false;
    pressed_tile = NULL;
    sample_count = 0;
    memset(edges, 0, sizeof(edges));
    memset(&edge_sheet, 0, sizeof(edge_sheet));
}

void flip_capture_lost(void)
{
    if (!flipping || !flip.dragging) return;
    press_active = false;
    release_flip(0, true);
}

// ───────────── 从牌边抽出另一页 ─────────────

/// 从露出的牌边抽出下一页：旧页面退到牌堆后面，新页面从牌堆前面抽出，先向下抽出一点，再向上展开到前台。
static void switch_stack_page(int index)
{
    if (morphing || M->side_panel_moving || M->page == PAGE_OVERVIEW || index < 0 || index >= EDGE_COUNT) return;
    int page = edges[index]->page;
    if (page == M->page) return;
    PageView *from = &M->pages[M->page], *to = &M->pages[page];
    to->host->hidden = false;
    main_relayout();
    ui_window_layout(M->window);
    memset(&slide, 0, sizeof(slide));
    slide.page = page;
    slide.from_page = M->page;
    slide.from_area = main_sheet_area();
    slide.to_area = slide.from_area;
    slide.selected = index;
    int others[EDGE_COUNT];
    others_of(page, others);
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        slide.bands[k] = edges[k]->rect;
        int wanted = k == index ? M->page : edges[k]->page;
        slide.slots[k] = 0;
        for (int slot = 0; slot < EDGE_COUNT; slot++)
            if (others[slot] == wanted) slide.slots[k] = slot;
        edges[k]->label_visible = false;
    }
    reset_edge_reveal();
    morphing = true;
    sliding = true;
    M->stack_layer->hit_through = true;
    // 新卡展开时下沿会变，单独画外框。
    to->sheet_border_hidden = true;
    (void)from;
    main_set_panel_header(page, true);
    anim_set(&slide.progress, 0);
    if (anim_enabled()) anim_start(&slide.progress, 1, STACK_SWITCH_SECONDS, EASE_OF(EASE_LINEAR));
    else anim_set(&slide.progress, 1);
    pose_stack_switch(0);
    main_request_frame();
}

/// 把 area 处的页面缩放、平移到 rect（等比，按宽度）。
static GfxMatrix sheet_pose(GfxRect area, GfxRect rect)
{
    float scale = rect_width(rect) / rect_width(area);
    return (GfxMatrix){ scale, 0, 0, scale, rect.left - scale * area.left, rect.top - scale * area.top };
}

static void pose_stack_switch(double progress)
{
    if (!sliding) return;
    double pull = STACK_PULL * smooth(progress / 0.18);
    double travel = cubic_out(clamp01((progress - 0.12) / 0.88));
    GfxRect start = slide.bands[slide.selected];
    start.top += (float)pull;
    start.bottom += (float)pull;
    GfxRect front = lerp_rect(start, slide.to_area, travel);
    GfxRect back = lerp_rect(slide.from_area, band_of(slide.to_area, slide.slots[slide.selected]), travel);
    slide.new_matrix = sheet_pose(slide.to_area, front);
    slide.old_matrix = sheet_pose(slide.from_area, back);
    slide.new_card = front;
    slide.old_card = back;
    double face_fade = smooth((travel - 0.55) / 0.4);
    slide.old_opacity = (float)(1 - face_fade);
    for (int k = 0; k < EDGE_COUNT; k++)
    {
        GfxRect rect = k == slide.selected ? back : lerp_rect(slide.bands[k], band_of(slide.to_area, slide.slots[k]), travel);
        place_edge(edges[k], (GfxRect){ 0 }, rect, 1);
        if (k == slide.selected)
        {
            edges[k]->frame_opacity = (float)face_fade;
            edges[k]->top_radius = (float)(SHEET_RADIUS * (1 - travel));
            edges[k]->bottom_radius = (float)(SHEET_RADIUS + (EDGE_RADIUS - SHEET_RADIUS) * travel);
        }
    }
    main_repaint();
}

// ───────────── 画内容区 ─────────────

/// 不在正常显示中的页面（翻牌、抽牌时的新旧两页）按给定的变换画一次。
static void paint_host(PageView *view, double time, GfxMatrix matrix, GfxRect clip, float opacity)
{
    UiElement *host = view->host;
    bool hidden = host->hidden, transformed = host->transformed, clipped = host->clip;
    GfxMatrix saved = host->transform;
    GfxRect saved_clip = host->clip_rect;
    float saved_radius = host->clip_radius, saved_opacity = host->opacity;
    host->hidden = false;
    host->transformed = true;
    host->transform = matrix;
    host->clip = true;
    host->clip_rect = clip;
    host->clip_radius = SHEET_RADIUS;
    host->opacity = opacity;
    ui_paint(host, time);
    host->hidden = hidden;
    host->transformed = transformed;
    host->transform = saved;
    host->clip = clipped;
    host->clip_rect = saved_clip;
    host->clip_radius = saved_radius;
    host->opacity = saved_opacity;
}

/// 画牌的边：远的先画，近的压在上面。
static void paint_edges(double time)
{
    if (M->stack_layer->hidden) return;
    UiElement *layer = M->stack_layer;
    if (layer->transformed) gfx_push_transform(layer->transform);
    int order[EDGE_COUNT];
    for (int k = 0; k < EDGE_COUNT; k++) order[k] = k;
    // 抽牌时按新的位置排上下。
    if (sliding)
        for (int a = 0; a < EDGE_COUNT; a++)
            for (int b = a + 1; b < EDGE_COUNT; b++)
                if (slide.slots[order[b]] > slide.slots[order[a]])
                {
                    int swap = order[a];
                    order[a] = order[b];
                    order[b] = swap;
                }
    if (!sliding)
        for (int a = 0; a < EDGE_COUNT / 2; a++)
        {
            int swap = order[a];
            order[a] = order[EDGE_COUNT - 1 - a];
            order[EDGE_COUNT - 1 - a] = swap;
        }
    for (int index = 0; index < EDGE_COUNT; index++)
        if (!edges[order[index]]->base.hidden) ui_paint(&edges[order[index]]->base, time);
    if (layer->transformed) gfx_pop_transform();
}

void flip_paint_stage(double time)
{
    GfxRect stage = M->stage;
    GfxRect outer = { stage.left - 2, stage.top - 2, stage.right + 2, stage.bottom + 2 };
    bool hole = flipping && flip.page_shown == 1;

    // 翻牌和抽牌时，卡片范围里的旧内容（概览页、旧页面、牌的边）要挖掉，不能透过半透明的牌面。
    // 内容区的底色是纯色：先照常画，再用底色把卡片的形状直接盖回去。效果和开一层挖空一样，
    // 却不用每帧一张内容区大小的中间位图（显卡驱动用过以后不还给系统，内存只增不减）。
    // 抽牌只在设置页之间，那时概览页是隐藏的，不会被一起盖掉。
    if (hole || sliding) gfx_push_clip(outer);
    if (!M->overview_scroll->hidden) ui_paint(M->overview_scroll, time);

    if (sliding)
    {
        // 旧页面退到牌堆后面：只露出新卡片以外的部分。
        GfxRect clip = { slide.from_area.left, slide.from_area.top, slide.from_area.right, slide.from_area.bottom };
        paint_host(&M->pages[slide.from_page], time, slide.old_matrix, clip, slide.old_opacity);
        paint_edges(time);
        gfx_fill_rounded_replace(slide.new_card, SHEET_RADIUS, gfx_matrix_identity(), main_stage_background());
        gfx_pop_clip();
        // 新页面从牌堆前面抽出，外框单独画，下沿跟着卡片走。
        PageView *to = &M->pages[slide.page];
        GfxRect to_clip = slide.to_area;
        to_clip.bottom = to_clip.top + rect_height(slide.new_card) / (rect_width(slide.new_card) / rect_width(slide.to_area));
        paint_host(to, time, slide.new_matrix, to_clip, 1);
        // 新牌按宽度缩放着展开，外面那圈牌背跟着一起缩放。
        ThemePattern back = theme_pattern(THEME_PATTERN_SHEET_BORDER);
        float band = SHEET_BAND * rect_width(slide.new_card) / rect_width(slide.to_area);
        gfx_stroke_rounded_pattern(slide.new_card, SHEET_RADIUS, &back, band);
        return;
    }

    paint_edges(time);
    if (hole)
    {
        gfx_fill_rounded_replace(flip.back_card, SHEET_RADIUS, flip.back_matrix, main_stage_background());
        gfx_pop_clip();
    }

    if (flipping && flip.page_shown == 0)
    {
        // 卡片本身转动时压在其余卡片上面。
        pages_paint_tile_face(flip.tile, flip.tile_matrix, 1);
    }
    for (int index = 0; index < PAGE_COUNT; index++)
    {
        PageView *view = &M->pages[index];
        if (flipping && view == flip.view)
        {
            if (flip.page_shown != 1) continue;
            paint_host(view, time, flip.back_matrix, flip.back_card, 1);
            // 背面的边框和翻转阴影：和页面共用同一个变换，不会错开。
            gfx_push_transform(flip.back_matrix);
            GfxRect card = flip.back_card;
            if (flip.shade > 0.001f) gfx_fill_rounded(card, SHEET_RADIUS, COLOR_ARGB((uint8_t)(255 * flip.shade), 0, 0, 0));
            ThemePattern back = theme_pattern(THEME_PATTERN_SHEET_BORDER);
            gfx_stroke_rounded_pattern(card, SHEET_RADIUS, &back, SHEET_BAND);
            gfx_pop_transform();
            continue;
        }
        if (!view->host->hidden) ui_paint(view->host, time);
    }
}
