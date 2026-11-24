#include "shelf_card.h"
#include "theme.h"
#include "ui.h"

#include <dwmapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
/// 翻到侧面时沿翻转轴的方向略微放大，看起来像离开了桌面。
#define CARD_LIFT 1.06
/// 翻到侧面时最暗压上这么多黑色。
#define MAX_SHADE 0.3
/// 窗口在牌经过的范围外再留出阴影的位置（DIP）。
#define SHADOW_ROOM 44.0
#define EASE_IN_OUT EASE_BEZIER_OF(0.42, 0, 0.58, 1)

// ───────────── 关键帧 ─────────────

static CardKey key(double ms, double x, double y, double scale, double angle, double turn)
{
    CardKey result = { ms, x, y, scale, angle, turn, 1, false, EASE_OF(EASE_LINEAR) };
    return result;
}

static CardKey eased(CardKey key, Ease ease)
{
    key.has_ease = true;
    key.ease = ease;
    return key;
}

static CardMotion motion_of(const CardKey *keys, int count, bool horizontal_axis, double fade_in_ms)
{
    CardMotion motion = { .count = count, .horizontal_axis = horizontal_axis, .fade_in_ms = fade_in_ms };
    memcpy(motion.keys, keys, (size_t)count * sizeof(CardKey));
    return motion;
}

static double lerp(double from, double to, double t)
{
    return from + (to - from) * t;
}

double card_motion_duration(const CardMotion *motion)
{
    return motion->count > 0 ? motion->keys[motion->count - 1].ms : 0;
}

static CardKey fade_in(const CardMotion *motion, CardKey pose, double ms)
{
    if (ms < motion->fade_in_ms) pose.opacity *= fmax(0, ms) / motion->fade_in_ms;
    return pose;
}

CardKey card_motion_at(const CardMotion *motion, double ms)
{
    for (int index = 1; index < motion->count; index++)
    {
        const CardKey *from = &motion->keys[index - 1], *to = &motion->keys[index];
        if (ms >= to->ms) continue;
        double t = (ms - from->ms) / fmax(1, to->ms - from->ms);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        if (from->has_ease) t = ease_apply(from->ease, t);
        CardKey pose = key(ms, lerp(from->x, to->x, t), lerp(from->y, to->y, t), lerp(from->scale, to->scale, t), lerp(from->angle, to->angle, t),
            lerp(from->turn, to->turn, t));
        pose.opacity = lerp(from->opacity, to->opacity, t);
        return fade_in(motion, pose, ms);
    }
    return fade_in(motion, motion->keys[motion->count - 1], ms);
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

/// 从 count 个版本里随机挑一个，不和上一次相同。
static int pick(int count, int *last)
{
    int next = (int)(next_random() % (unsigned)(*last < 0 ? count : count - 1));
    if (*last >= 0 && next >= *last) next++;
    *last = next;
    return next;
}

// ───────────── 晃动呼出 ─────────────
// x、y 是起手时牌中心相对落点的位置（DIP）。全程约 350 毫秒，开头 40 毫秒淡入。

/// 绕竖轴左右翻开。
static CardMotion flip_sideways(double x, double y)
{
    CardKey keys[] = {
        eased(key(0, x, y, 0.5, -8, 180), EASE_BEZIER_OF(0.25, 0.6, 0.4, 1)),
        key(135, x * 0.5, y * 0.5, 0.74, -5, 128),
        eased(key(230, x * 0.1, y * 0.1, 0.97, -1, 30), EASE_BEZIER_OF(0.3, 0.7, 0.4, 1)),
        eased(key(300, 0, 0, 1.025, 0.4, -4), EASE_IN_OUT),
        key(355, 0, 0, 1, 0, 0),
    };
    return motion_of(keys, ARRAYSIZE(keys), false, 40);
}

/// 绕横轴从下往上翻起，落定前略微抬高。
static CardMotion flip_upward(double x, double y)
{
    CardKey keys[] = {
        eased(key(0, x, y, 0.5, 5, 180), EASE_BEZIER_OF(0.25, 0.6, 0.4, 1)),
        key(130, x * 0.55, y * 0.55 - 6, 0.76, 3, 125),
        eased(key(225, x * 0.12, y * 0.12 - 2, 0.98, 0.5, 28), EASE_BEZIER_OF(0.3, 0.7, 0.4, 1)),
        eased(key(290, 0, -2, 1.02, 0, -5), EASE_IN_OUT),
        key(345, 0, 0, 1, 0, 0),
    };
    return motion_of(keys, ARRAYSIZE(keys), true, 40);
}

/// 转着翻开：起手倾角大，边翻边转正。
static CardMotion spin_open(double x, double y)
{
    CardKey keys[] = {
        eased(key(0, x, y, 0.45, -24, 180), EASE_BEZIER_OF(0.2, 0.65, 0.35, 1)),
        key(145, x * 0.45, y * 0.45, 0.78, -10, 120),
        eased(key(240, x * 0.08, y * 0.08, 0.99, 2, 24), EASE_BEZIER_OF(0.3, 0.7, 0.4, 1)),
        eased(key(305, 0, 0, 1.02, -0.8, -3), EASE_IN_OUT),
        key(360, 0, 0, 1, 0, 0),
    };
    return motion_of(keys, ARRAYSIZE(keys), false, 40);
}

/// 从光标上方落下来，划一道弧翻开，倾角朝另一边。
static CardMotion drop_open(double x, double y)
{
    CardKey keys[] = {
        eased(key(0, x, y - 36, 0.5, 9, 180), EASE_BEZIER_OF(0.25, 0.6, 0.4, 1)),
        key(135, x * 0.5, y * 0.5 - 22, 0.75, 5, 126),
        eased(key(230, x * 0.1, y * 0.1 - 5, 0.97, 1, 28), EASE_BEZIER_OF(0.3, 0.7, 0.4, 1)),
        eased(key(300, 0, 1.5, 1.02, -0.4, -4), EASE_IN_OUT),
        key(355, 0, 0, 1, 0, 0),
    };
    return motion_of(keys, ARRAYSIZE(keys), false, 40);
}

CardMotion card_motion_shake_in(double cursor_x, double cursor_y)
{
    static int last = -1;
    double x = cursor_x * 0.9, y = cursor_y * 0.9;
    switch (pick(4, &last))
    {
        case 0: return flip_sideways(x, y);
        case 1: return flip_upward(x, y);
        case 2: return spin_open(x, y);
        default: return drop_open(x, y);
    }
}

// ───────────── 边缘呼出和甩回 ─────────────

/// 边缘方向上的一帧：out 是沿离开那条边的方向走了多远，across 是沿着这条边偏了多少（DIP）。
typedef struct EdgeStep
{
    double ms, out, across, scale, angle;
    bool has_ease;
    Ease ease;
} EdgeStep;

static EdgeStep step(double ms, double out, double across, double scale, double angle)
{
    EdgeStep result = { ms, out, across, scale, angle, false, EASE_OF(EASE_LINEAR) };
    return result;
}

static EdgeStep step_eased(EdgeStep step, Ease ease)
{
    step.has_ease = true;
    step.ease = ease;
    return step;
}

/// 一种走法：d 是起手（甩回时是最后）牌中心离落点有多远（DIP）；写进 steps，返回有几帧。
typedef int (*EdgeSteps)(double d, EdgeStep *steps);

static int copy_steps(const EdgeStep *list, int count, EdgeStep *steps)
{
    memcpy(steps, list, (size_t)count * sizeof(EdgeStep));
    return count;
}

/// 甩出：一路回正，冲过落点几像素，反向偏一点再停稳。
static int fling(double d, EdgeStep *steps)
{
    EdgeStep list[] = {
        step_eased(step(0, d, 14, 1, 16), EASE_BEZIER_OF(0.2, 0.5, 0.45, 1)),
        step_eased(step(112, d * 0.44, 6, 1, 8), EASE_BEZIER_OF(0.25, 0.6, 0.4, 1)),
        step_eased(step(252, -9, -1, 1.02, -2), EASE_IN_OUT),
        step(378, 0, 0, 1, 0),
    };
    return copy_steps(list, ARRAYSIZE(list), steps);
}

/// 旋入：转得更多，从略偏上的地方划进来。
static int spin_in(double d, EdgeStep *steps)
{
    EdgeStep list[] = {
        step_eased(step(0, d, -40, 1, 28), EASE_BEZIER_OF(0.2, 0.55, 0.4, 1)),
        step_eased(step(130, d * 0.4, -14, 1, 12), EASE_BEZIER_OF(0.25, 0.6, 0.4, 1)),
        step_eased(step(260, -6, 2, 1.015, -3), EASE_IN_OUT),
        step_eased(step(330, 2, 0, 1, 1), EASE_IN_OUT),
        step(400, 0, 0, 1, 0),
    };
    return copy_steps(list, ARRAYSIZE(list), steps);
}

/// 滑停：几乎不转，沿直线滑进来，像被桌面摩擦着来回晃两下停住。
static int slide_stop(double d, EdgeStep *steps)
{
    EdgeStep list[] = {
        step_eased(step(0, d, 0, 1, 6), EASE_BEZIER_OF(0.15, 0.55, 0.3, 1)),
        step_eased(step(200, -12, 0, 1, -2.5), EASE_IN_OUT),
        step_eased(step(290, 4, 0, 1, 1.2), EASE_IN_OUT),
        step_eased(step(360, -1.5, 0, 1, -0.4), EASE_IN_OUT),
        step(420, 0, 0, 1, 0),
    };
    return copy_steps(list, ARRAYSIZE(list), steps);
}

/// 抛起落下：飞行中略微放大像离开了桌面，落下时轻轻一压。
static int toss_down(double d, EdgeStep *steps)
{
    EdgeStep list[] = {
        step_eased(step(0, d, 20, 1.04, 18), EASE_BEZIER_OF(0.2, 0.5, 0.45, 1)),
        step_eased(step(140, d * 0.38, 8, 1.06, 7), EASE_BEZIER_OF(0.3, 0.6, 0.4, 1)),
        step_eased(step(270, -5, -1, 0.985, -1.5), EASE_IN_OUT),
        step(380, 0, 0, 1, 0),
    };
    return copy_steps(list, ARRAYSIZE(list), steps);
}

/// 甩回：先往里收一下，再甩回那条边。
static int fling_back(double d, EdgeStep *steps)
{
    EdgeStep list[] = {
        step_eased(step(0, 0, 0, 1, 0), EASE_BEZIER_OF(0.3, 0, 0.5, 1)),
        step_eased(step(70, -5, 0, 1.01, -1), EASE_BEZIER_OF(0.55, 0, 0.85, 0.35)),
        step(270, d, 12, 1, 16),
    };
    return copy_steps(list, ARRAYSIZE(list), steps);
}

/// 左右两边的倾角互为镜像，顶边的倾角减半。
static double tilt_of(ScreenEdge edge)
{
    return edge == EDGE_LEFT ? -1 : edge == EDGE_TOP ? 0.5 : 1;
}

static CardKey edge_key(ScreenEdge edge, double tilt, EdgeStep step)
{
    double out_x = edge == EDGE_LEFT ? -1 : edge == EDGE_TOP ? 0 : 1, out_y = edge == EDGE_TOP ? -1 : 0;
    // 左右两边沿竖直方向偏，顶边沿水平方向偏。
    double across_x = fabs(out_y), across_y = fabs(out_x);
    CardKey result = key(step.ms, out_x * step.out + across_x * step.across, out_y * step.out + across_y * step.across, step.scale,
        step.angle * tilt, 0);
    result.has_ease = step.has_ease;
    result.ease = step.ease;
    return result;
}

/// 牌中心要沿离开那条边的方向走多远（DIP），转过 angle、放大 grow 倍的牌连同阴影才完全落到工作区外。
static double outside(ScreenEdge edge, RECT target, RECT work, double dpi_scale, double angle, double grow)
{
    double radians = angle * PI / 180;
    double half_width = (target.right - target.left) / 2.0 / dpi_scale, half_height = (target.bottom - target.top) / 2.0 / dpi_scale;
    double center_x = target.left + (target.right - target.left) / 2.0, center_y = target.top + (target.bottom - target.top) / 2.0;
    double across = cos(radians) * half_width + sin(radians) * half_height;
    double down = sin(radians) * half_width + cos(radians) * half_height;
    double gap, extent;
    if (edge == EDGE_LEFT) gap = (center_x - work.left) / dpi_scale, extent = across;
    else if (edge == EDGE_TOP) gap = (center_y - work.top) / dpi_scale, extent = down;
    else gap = (work.right - center_x) / dpi_scale, extent = across;
    return gap + extent * grow + 24;
}

static CardMotion edge_motion(ScreenEdge edge, double tilt, const EdgeStep *steps, int count)
{
    CardMotion motion = { .count = count };
    for (int index = 0; index < count; index++) motion.keys[index] = edge_key(edge, tilt, steps[index]);
    return motion;
}

CardMotion card_motion_edge_in(ScreenEdge edge, RECT target, RECT work, double dpi_scale)
{
    static int last = -1;
    static const EdgeSteps variants[] = { fling, spin_in, slide_stop, toss_down };
    EdgeSteps steps = variants[pick(ARRAYSIZE(variants), &last)];
    double tilt = tilt_of(edge);
    EdgeStep list[CARD_MOTION_MAX_KEYS];
    // 起手那一帧的转角和缩放决定牌要退到多远才完全在工作区外，它们和距离无关，先按距离为零取出来。
    steps(0, list);
    double d = outside(edge, target, work, dpi_scale, list[0].angle * fabs(tilt), list[0].scale);
    return edge_motion(edge, tilt, list, steps(d, list));
}

/// 翻回背面，缩小淡出。
static CardMotion flip_away(bool horizontal_axis)
{
    CardKey last = key(280, 0, 16, 0.72, 4, 180);
    last.opacity = 0;
    CardKey keys[] = {
        eased(key(0, 0, 0, 1, 0, 0), EASE_BEZIER_OF(0.45, 0, 0.75, 0.45)),
        eased(key(140, 0, 4, 0.93, 1.5, 90), EASE_BEZIER_OF(0.25, 0.5, 0.5, 1)),
        last,
    };
    return motion_of(keys, ARRAYSIZE(keys), horizontal_axis, 0);
}

CardMotion card_motion_exit(ScreenEdge edge, bool horizontal_axis, RECT target, RECT work, double dpi_scale)
{
    if (edge == EDGE_NONE) return flip_away(horizontal_axis);
    double tilt = tilt_of(edge);
    EdgeStep list[CARD_MOTION_MAX_KEYS];
    // 最后一帧的牌连同阴影要完全落到工作区外。
    int count = fling_back(0, list);
    double d = outside(edge, target, work, dpi_scale, list[count - 1].angle * fabs(tilt), list[count - 1].scale);
    return edge_motion(edge, tilt, list, fling_back(d, list));
}

// ───────────── 牌的窗口 ─────────────
// 透明、置顶、不抢焦点的窗口里画一张牌：正面是置物架的截图，背面是牌背，按关键帧移动、转动和翻面。
// 窗口只盖住牌经过的范围并限制在工作区内，牌从屏幕外进来时，伸出工作区的部分自然被窗口裁掉。
// 窗口不接收拖放：动画期间在牌上松手什么也不会发生，不会落进下面的程序。
// 翻面不做真正的透视：沿翻转方向按余弦压扁，另一方向略微放大，和设置页的翻牌一样。播完隐藏后放回去，下次直接复用。

typedef enum CardStage
{
    STAGE_IDLE,
    /// 刚显示，先隐着身等第一帧画好，再以画好的样子现身：上次隐藏前留下的旧画面、改了大小后还没画的部分都不会闪出来。
    /// 离场时置物架在牌现身的同一刻隐身，中间不会空一帧，两层阴影也不会叠在一起。
    STAGE_SHOWING,
    STAGE_PLAYING,
    /// 播完后再停留两帧，盖住置物架现身前的那一帧。
    STAGE_LINGERING
} CardStage;

struct ShelfCard
{
    UiWindow *window;
    HWND handle;
    ShelfCard *next;            // 空闲的牌连成一串
    CardStage stage;
    int frames;
    CardMotion motion;
    CardKey pose;
    RECT target, bounds;
    float width, height, radius;
    GfxImage *face;
    // 换风格的翻牌（shelf_card_flip）：牌从 from 的位置和大小翻到 target，背面是新风格的样子（还没截好时是牌背）。
    bool morph;
    RECT from;
    GfxImage *back;
    ShelfCardHandler shown, finished;
    void *context;
    double elapsed, lag, last_time;
};

static ShelfCard *idle_cards;

void window_set_cloaked(HWND window, bool cloaked)
{
    if (!window) return;
    BOOL value = cloaked;
    DwmSetWindowAttribute(window, DWMWA_CLOAK, &value, sizeof(value));
}

static GfxRect inset(GfxRect rect, float by)
{
    return (GfxRect){ rect.left + by, rect.top + by, rect.right - by, rect.bottom - by };
}

/// 按姿态摆放：先绕中心压扁或放大、再转动，最后移到落点加偏移处；翻过侧面后换成另一面。
static void card_paint(UiWindow *window, void *context, double time)
{
    (void)time;
    ShelfCard *card = context;
    if (card->stage == STAGE_IDLE || !card->face) return;
    CardKey pose = card->pose;
    float opacity = (float)(pose.opacity < 0 ? 0 : pose.opacity > 1 ? 1 : pose.opacity);
    if (opacity <= 0.001f) return;
    float scale = ui_window_scale(window);
    RECT place = card->target;
    if (card->morph)
    {
        // 翻过去的同时从原来的大小变成新的大小（两种风格的置物架大小不同）。
        double t = fmin(fmax(pose.turn / 180, 0), 1);
        place.left = (LONG)lround(card->from.left + (card->target.left - card->from.left) * t);
        place.top = (LONG)lround(card->from.top + (card->target.top - card->from.top) * t);
        place.right = (LONG)lround(card->from.right + (card->target.right - card->from.right) * t);
        place.bottom = (LONG)lround(card->from.bottom + (card->target.bottom - card->from.bottom) * t);
        card->width = (float)((place.right - place.left) / scale);
        card->height = (float)((place.bottom - place.top) / scale);
    }
    float x = (float)((place.left - card->bounds.left + (place.right - place.left) / 2.0) / scale + pose.x);
    float y = (float)((place.top - card->bounds.top + (place.bottom - place.top) / 2.0) / scale + pose.y);
    double turn = pose.turn * PI / 180;
    float squash = (float)(fmax(fabs(cos(turn)), 0.01) * pose.scale);
    float lift = (float)((1 + (CARD_LIFT - 1) * fabs(sin(turn))) * pose.scale);
    GfxMatrix matrix = gfx_matrix_translate(-card->width / 2, -card->height / 2);
    matrix = gfx_matrix_multiply(matrix, card->motion.horizontal_axis ? gfx_matrix_scale(lift, squash, 0, 0) : gfx_matrix_scale(squash, lift, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate((float)pose.angle, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(x, y));
    gfx_push_transform(matrix);
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
    GfxRect rect = { 0, 0, card->width, card->height };
    float radius = card->radius;
    gfx_shadow(rect, radius, 30, 0, 8, COLOR_ARGB(77, 0, 0, 0));
    if (cos(turn) >= 0)
    {
        gfx_push_layer(rect, radius, 1);
        gfx_draw_image(card->face, rect, 1);
        gfx_pop_layer();
        gfx_stroke_rounded(inset(rect, 0.5f), fmaxf(0, radius - 0.5f), theme_color(THEME_SHELF_BORDER), 1);
    }
    else if (card->morph && card->back)
    {
        gfx_push_layer(rect, radius, 1);
        gfx_draw_image(card->back, rect, 1);
        gfx_pop_layer();
        gfx_stroke_rounded(inset(rect, 0.5f), fmaxf(0, radius - 0.5f), theme_color(THEME_SHELF_BORDER), 1);
    }
    else
    {
        ThemePattern back = theme_pattern(THEME_PATTERN_SHEET_BORDER);
        gfx_fill_rounded_pattern(rect, radius, &back);
        gfx_stroke_rounded(inset(rect, 0.6f), fmaxf(0, radius - 0.6f), theme_color(THEME_CARD_BACK_LINE), 1.2f);
        // 牌背里面一圈细线：离边 1.2 + 9。
        gfx_stroke_rounded(inset(rect, 10.7f), fmaxf(2, radius - 4) - 0.5f, COLOR_ARGB(0x99, 0xFB, 0xFA, 0xF6), 1);
    }
    float shade = (float)(MAX_SHADE * fabs(sin(turn)));
    if (shade > 0.002f) gfx_fill_rounded(rect, radius, COLOR_ARGB((uint8_t)lroundf(shade * 255), 0, 0, 0));
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

static void call_once(ShelfCardHandler *handler, void *context)
{
    ShelfCardHandler taken = *handler;
    *handler = NULL;
    if (taken) taken(context);
}

static void release_card(ShelfCard *card)
{
    card->stage = STAGE_IDLE;
    if (card->handle) ShowWindow(card->handle, SW_HIDE);
    // 不再引用置物架的截图。
    gfx_image_release(card->face);
    card->face = NULL;
    gfx_image_release(card->back);
    card->back = NULL;
    card->morph = false;
    card->context = NULL;
    if (!card->window) return;
    card->next = idle_cards;
    idle_cards = card;
}

static void card_animate(UiWindow *window, void *context, double time)
{
    ShelfCard *card = context;
    switch (card->stage)
    {
        case STAGE_IDLE:
            return;
        case STAGE_SHOWING:
            ui_window_request_frame(window);
            if (++card->frames < 2) return;
            card->stage = STAGE_PLAYING;
            // 从这一帧起计时，卡顿时放慢而不跳过，见 frame_clock_step。
            card->elapsed = card->lag = 0;
            card->last_time = time;
            window_set_cloaked(card->handle, false);
            call_once(&card->shown, card->context);
            return;
        case STAGE_PLAYING:
        {
            ui_window_request_frame(window);
            if (time > card->last_time) card->elapsed += frame_clock_step(time - card->last_time, &card->lag);
            card->last_time = time;
            double ms = card->elapsed * 1000;
            card->pose = card_motion_at(&card->motion, ms);
            if (ms < card_motion_duration(&card->motion)) return;
            card->stage = STAGE_LINGERING;
            card->frames = 0;
            call_once(&card->finished, card->context);
            return;
        }
        case STAGE_LINGERING:
            if (++card->frames >= 2) release_card(card);
            else ui_window_request_frame(window);
            return;
    }
}

static void card_closed(UiWindow *window, void *context)
{
    (void)window;
    ShelfCard *card = context;
    // 退出时所有窗口一起关掉：空闲的牌直接释放；正在用的由置物架撤下（只剩一个没有窗口的空壳）。
    bool idle = false;
    for (ShelfCard **link = &idle_cards; *link; link = &(*link)->next)
    {
        if (*link != card) continue;
        *link = card->next;
        idle = true;
        break;
    }
    card->window = NULL;
    card->handle = NULL;
    card->stage = STAGE_IDLE;
    card->shown = card->finished = NULL;
    gfx_image_release(card->face);
    card->face = NULL;
    gfx_image_release(card->back);
    card->back = NULL;
    if (idle) free(card);
}

static ShelfCard *card_create(void)
{
    ShelfCard *card = xcalloc(1, sizeof(ShelfCard));
    UiWindowOptions options = { 0 };
    options.title = L"DropShelf Card";
    options.popup = true;
    options.no_activate = true;
    options.width = options.height = 64;
    options.context = card;
    options.paint = card_paint;
    options.animate = card_animate;
    options.closed = card_closed;
    card->window = ui_window_create(&options);
    if (!card->window)
    {
        free(card);
        return NULL;
    }
    card->handle = ui_window_handle(card->window);
    return card;
}

ShelfCard *shelf_card_rent(void)
{
    if (!idle_cards) return card_create();
    ShelfCard *card = idle_cards;
    idle_cards = card->next;
    card->next = NULL;
    return card;
}

void shelf_card_prewarm(void)
{
    if (idle_cards) return;
    ShelfCard *card = card_create();
    if (!card) return;
    card->next = idle_cards;
    idle_cards = card;
}

/// 窗口的范围：盖住每一帧牌的外接矩形（按转角和翻面时最大的放大量估算），再留出阴影，限制在 area 内。
static RECT card_bounds(RECT target, RECT area, double dpi_scale, const CardMotion *motion)
{
    double center_x = target.left + (target.right - target.left) / 2.0, center_y = target.top + (target.bottom - target.top) / 2.0;
    double half_width = (target.right - target.left) / 2.0, half_height = (target.bottom - target.top) / 2.0;
    double left = target.left, top = target.top, right = target.right, bottom = target.bottom;
    for (int index = 0; index < motion->count; index++)
    {
        const CardKey *key = &motion->keys[index];
        double angle = fabs(key->angle) * PI / 180;
        double grow = key->scale * CARD_LIFT;
        double extent_x = (cos(angle) * half_width + sin(angle) * half_height) * grow;
        double extent_y = (sin(angle) * half_width + cos(angle) * half_height) * grow;
        double x = center_x + key->x * dpi_scale, y = center_y + key->y * dpi_scale;
        left = fmin(left, x - extent_x);
        right = fmax(right, x + extent_x);
        top = fmin(top, y - extent_y);
        bottom = fmax(bottom, y + extent_y);
    }
    double room = SHADOW_ROOM * dpi_scale;
    RECT bounds = { (LONG)fmax(area.left, floor(left - room)), (LONG)fmax(area.top, floor(top - room)), (LONG)fmin(area.right, ceil(right + room)),
        (LONG)fmin(area.bottom, ceil(bottom + room)) };
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) bounds = target;
    return bounds;
}

static void place(ShelfCard *card)
{
    RECT bounds = card->bounds;
    SetWindowPos(card->handle, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
}

void shelf_card_play(ShelfCard *card, RECT target, RECT area, double dpi_scale, GfxImage *face, const CardMotion *motion,
    ShelfCardHandler shown, ShelfCardHandler finished, void *context)
{
    if (!card->window) return;
    card->motion = *motion;
    card->target = target;
    card->shown = shown;
    card->finished = finished;
    card->context = context;
    card->width = (float)((target.right - target.left) / dpi_scale);
    card->height = (float)((target.bottom - target.top) / dpi_scale);
    // Windows 11 上置物架窗口是圆角的，牌也画成同样的圆角，现身时轮廓不变。
    card->radius = ui_is_windows_11() ? 8.0f : 0.0f;
    gfx_image_release(card->face);
    card->face = gfx_image_retain(face);
    card->bounds = card_bounds(target, area, dpi_scale, motion);
    place(card);
    window_set_cloaked(card->handle, true);
    if (!IsWindowVisible(card->handle)) ShowWindow(card->handle, SW_SHOWNOACTIVATE);
    // 跨越缩放不同的显示器时大小可能被系统改掉，再放一次。
    place(card);
    card->pose = card_motion_at(motion, 0);
    card->stage = STAGE_SHOWING;
    card->frames = 0;
    ui_window_request_frame(card->window);
}

CardMotion card_motion_look_flip(double delay_ms, double duration_ms)
{
    // 先停着等前一张翻到一半（多米诺），再绕竖轴翻过来；翻到侧面时略微抬起。
    CardKey keys[] = {
        key(0, 0, 0, 1, 0, 0),
        eased(key(delay_ms, 0, 0, 1, 0, 0), EASE_IN_OUT),
        eased(key(delay_ms + duration_ms * 0.5, 0, 0, 1.04, 0, 90), EASE_IN_OUT),
        key(delay_ms + duration_ms, 0, 0, 1, 0, 180),
    };
    return motion_of(keys, ARRAYSIZE(keys), false, 0);
}

void shelf_card_flip(ShelfCard *card, RECT from, double dpi_scale, GfxImage *front, double delay_ms, double duration_ms, ShelfCardHandler shown,
    ShelfCardHandler finished, void *context)
{
    if (!card->window) return;
    card->motion = card_motion_look_flip(delay_ms, duration_ms);
    card->morph = true;
    card->from = card->target = from;
    card->shown = shown;
    card->finished = finished;
    card->context = context;
    card->width = (float)((from.right - from.left) / dpi_scale);
    card->height = (float)((from.bottom - from.top) / dpi_scale);
    card->radius = ui_is_windows_11() ? 8.0f : 0.0f;
    gfx_image_release(card->face);
    card->face = gfx_image_retain(front);
    gfx_image_release(card->back);
    card->back = NULL;
    // 窗口先按原来的范围放，背面截好以后再按新旧两个范围放大（shelf_card_set_back）。
    double room = SHADOW_ROOM * dpi_scale + fmax(from.right - from.left, from.bottom - from.top) * (CARD_LIFT - 1);
    card->bounds = (RECT){ (LONG)(from.left - room), (LONG)(from.top - room), (LONG)(from.right + room), (LONG)(from.bottom + room) };
    place(card);
    window_set_cloaked(card->handle, true);
    if (!IsWindowVisible(card->handle)) ShowWindow(card->handle, SW_SHOWNOACTIVATE);
    place(card);
    card->pose = card_motion_at(&card->motion, 0);
    card->stage = STAGE_SHOWING;
    card->frames = 0;
    ui_window_request_frame(card->window);
}

void shelf_card_set_back(ShelfCard *card, GfxImage *back, RECT to, double dpi_scale)
{
    if (!card || card->stage == STAGE_IDLE || !card->morph) return;
    gfx_image_release(card->back);
    card->back = gfx_image_retain(back);
    card->target = to;
    double room = SHADOW_ROOM * dpi_scale + fmax(fmax(to.right - to.left, to.bottom - to.top), fmax(card->from.right - card->from.left, card->from.bottom - card->from.top)) *
                                              (CARD_LIFT - 1);
    card->bounds = (RECT){ (LONG)(fmin(card->from.left, to.left) - room), (LONG)(fmin(card->from.top, to.top) - room),
        (LONG)(fmax(card->from.right, to.right) + room), (LONG)(fmax(card->from.bottom, to.bottom) + room) };
    place(card);
    ui_window_request_frame(card->window);
}

void shelf_card_cancel(ShelfCard *card)
{
    if (!card) return;
    card->shown = card->finished = NULL;
    if (card->stage != STAGE_IDLE) release_card(card);
}
