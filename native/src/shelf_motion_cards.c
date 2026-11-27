// 置物架的动画，和 C# style 版的 ShelfWindow.Motion.cs、ShelfWindow.Entrance.cs、ShelfMotion.cs 对应。
// 拖进来的文件从松手处飞进堆叠或平铺的格子；堆叠和平铺之间切换时，牌从堆叠里一张张发到各自的格子（或收回来）。
// 窗口大小平滑过渡，但不和牌同时动：展开时窗口先变大再发牌，收起时牌收回去之后窗口再变小。动画途中再有变化时直接跳到终点。
#include "look_variant.h"
#include "shelf_window_internal.h"
#include "shelf_card.h"
#include "log.h"
#include "task.h"

#include <math.h>
#ifdef _WIN32
#include <shellscalingapi.h>
#endif
#include <stdlib.h>
#include <string.h>

#define LAND_DURATION 0.380
#define LAND_STAGGER 0.080
#define PUSH_DELAY 0.120
#define PUSH_DURATION 0.300
/// 松手时摊开的牌，在新牌开始落下这么久之后合拢。
#define FAN_CLOSE_DELAY 0.200
#define FAN_CLOSE_DURATION 0.260
#define FAN_DURATION 0.200
#define FLY_DURATION 0.260
#define DEAL_STAGGER 0.034
/// 整批发牌最多错开这么多个 DEAL_STAGGER，正好是小尺寸平铺 8 张牌的间隔数。
#define MAX_DEAL_GAPS 7.0
#define NAME_FADE_DURATION 0.190
#define FADE_OUT_DURATION 0.100
#define RESIZE_DURATION 0.170
#define REMOVE_DURATION 0.160
#define CLOSE_UP_DURATION 0.220
#define RANK_ROLL_DURATION 0.220

// ───────────── 时钟 ─────────────

bool clock_tick(MotionClock *clock, double time)
{
    // 同一帧不重复走；比上次还早的时刻（窗口消息里现画、用的是“现在”）也不往回走。
    if (clock->has_last && time <= clock->last) return false;
    if (clock->has_last) clock->elapsed += frame_clock_step(time - clock->last, &clock->lag);
    clock->last = time;
    clock->has_last = true;
    return true;
}

static void clock_reset(MotionClock *clock)
{
    *clock = (MotionClock){0};
}

static double max_of(double a, double b)
{
    return a > b ? a : b;
}

static CardPose lerp_pose(CardPose from, CardPose to, float t)
{
    return (CardPose){ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.angle + (to.angle - from.angle) * t,
        from.scale + (to.scale - from.scale) * t, from.opacity + (to.opacity - from.opacity) * t };
}

/// 落下的路径：位置沿二次贝塞尔曲线划一道弧，缩放途中略微超过原大小，开头很快变得不透明。
static CardPose arc_pose(CardPose from, CardPose to, float control_x, float control_y, float t)
{
    float u = 1 - t;
    return (CardPose){ u * u * from.x + 2 * u * t * control_x + t * t * to.x, u * u * from.y + 2 * u * t * control_y + t * t * to.y,
        from.angle + (to.angle - from.angle) * t, from.scale + (to.scale - from.scale) * t + 0.06f * sinf(3.14159265f * t),
        fminf(1, t / 0.15f) };
}

// ───────────── 逐帧动画 ─────────────

static void request_frame(ShelfWindow *shelf)
{
    if (shelf->ui) ui_window_request_frame(shelf->ui);
}

/// 加一段动画：先停在起点（apply(0)），之后逐帧推进。没有动画在播时时钟从头开始。
static void start_tween(ShelfWindow *shelf, Tween tween)
{
    if (!shelf->motion_running)
    {
        clock_reset(&shelf->motion_clock);
        shelf->motion_running = true;
    }
    tween.start = shelf->motion_clock.elapsed;
    if (tween.item) shelf_item_retain(tween.item);
    if (tween.apply) tween.apply(shelf, &tween, 0);
    if (shelf->tween_count == shelf->tween_capacity)
    {
        shelf->tween_capacity = shelf->tween_capacity ? shelf->tween_capacity * 2 : 16;
        shelf->tweens = xrealloc(shelf->tweens, (size_t)shelf->tween_capacity * sizeof(Tween));
    }
    shelf->tweens[shelf->tween_count++] = tween;
    request_frame(shelf);
}

static Tween make_tween(double delay, double duration, TweenApply apply)
{
    Tween tween = {0};
    tween.delay = delay;
    tween.duration = duration;
    tween.apply = apply;
    tween.card = -1;
    return tween;
}

static void finish_completed(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)tween, (void)t;
    shelf_finish_motion(shelf);
}

void shelf_finish_after(ShelfWindow *shelf, double end)
{
    Tween tween = make_tween(0, end, NULL);
    tween.completed = finish_completed;
    start_tween(shelf, tween);
}

void shelf_stop_motion(ShelfWindow *shelf)
{
    // 先摘下列表再放引用：放引用不会再回到这里，但收尾可能又加新的动画。
    int count = shelf->tween_count;
    Tween *tweens = shelf->tweens;
    shelf->tweens = NULL;
    shelf->tween_count = shelf->tween_capacity = 0;
    for (int index = 0; index < count; index++) shelf_item_release(tweens[index].item);
    free(tweens);
    shelf->motion_running = false;
}

static void update_motion(ShelfWindow *shelf, double time)
{
    if (!shelf->motion_running || !clock_tick(&shelf->motion_clock, time)) return;
    double now = shelf->motion_clock.elapsed;
    for (int index = 0; index < shelf->tween_count; index++)
    {
        Tween *tween = &shelf->tweens[index];
        double elapsed = now - tween->start - tween->delay;
        if (elapsed < 0) continue;
        double progress = tween->duration <= 0 ? 1 : fmin(elapsed / tween->duration, 1);
        float eased = (float)ease_apply(EASE_OF(EASE_CUBIC_OUT), progress);
        if (tween->apply) tween->apply(shelf, tween, eased);
        if (progress < 1) continue;
        Tween done = *tween;
        memmove(&shelf->tweens[index], &shelf->tweens[index + 1], (size_t)(shelf->tween_count - index - 1) * sizeof(Tween));
        shelf->tween_count--;
        index--;
        // 收尾可能直接结束全部动画并清空列表，循环随之结束。
        if (done.completed) done.completed(shelf, &done, 1);
        shelf_item_release(done.item);
    }
    if (shelf->tween_count == 0) shelf_stop_motion(shelf);
}

// ───────────── 收尾 ─────────────

static void rest_cards(ShelfWindow *shelf)
{
    for (int card = 0; card < 3; card++) shelf->card_posed[card] = false;
}

static void restore_stack_mark(ShelfWindow *shelf)
{
    shelf->rank_hidden = false;
    shelf->mark_opacity = 1;
}

static void reset_item_motion(ShelfWindow *shelf)
{
    for (int index = 0; index < shelf->model->count; index++) shelf_item_motion_reset(shelf->model->items[index]);
}

static void close_up_tiles(void *data);
static void rise_stack(void *data);

typedef struct RisePlan
{
    ShelfWindow *shelf;
    ShelfItem *shown[3];
} RisePlan;

void shelf_finish_motion(ShelfWindow *shelf)
{
    shelf_stop_motion(shelf);
    unsigned cleanup = shelf->cleanup;
    shelf->cleanup = 0;
    if (!cleanup) return;
    if (cleanup & CLEANUP_ITEM_MOTION) reset_item_motion(shelf);
    if (cleanup & CLEANUP_REST_CARDS) rest_cards(shelf);
    if (cleanup & CLEANUP_FAN) shelf->fan_held = false;
    if (cleanup & CLEANUP_STACK_MARK) restore_stack_mark(shelf);
    if (cleanup & CLEANUP_UNPIN) shelf->stack_pinned = shelf->list_pinned = false;
    if (cleanup & CLEANUP_UPDATE_VIEWS) shelf->switching = false;
    if (cleanup & CLEANUP_REMOVE)
    {
        ShelfItem **items = shelf->removing;
        int count = shelf->removing_count;
        shelf->removing = NULL;
        shelf->removing_count = 0;
        bool remains = shelf->remove_remains;
        bool expanded = shelf->remove_expanded;
        RisePlan *rise = NULL;
        if (remains && !expanded)
        {
            rise = xcalloc(1, sizeof(RisePlan));
            rise->shelf = shelf_window_retain(shelf);
            for (int card = 0; card < 3; card++) rise->shown[card] = shelf_item_retain(shelf->shown_before[card]);
        }
        for (int card = 0; card < 3; card++)
        {
            shelf_item_release(shelf->shown_before[card]);
            shelf->shown_before[card] = NULL;
        }
        shelf_perform_remove(shelf, items, count, shelf->remove_kind, shelf->remove_expanded_before);
        for (int index = 0; index < count; index++) shelf_item_release(items[index]);
        free(items);
        // 平铺移除之后补位：等数量变化引起的刷新按新的数量排好格子，再从移除前的位置滑过去。
        if (remains && expanded) task_post(close_up_tiles, shelf_window_retain(shelf));
        if (rise) task_post(rise_stack, rise);
    }
    if (cleanup & CLEANUP_WINDOW_SIZE) shelf_update_window_size(shelf);
    shelf_repaint(shelf);
}

// ───────────── 堆叠里的牌 ─────────────

static void apply_card_lerp(ShelfWindow *shelf, Tween *tween, float t)
{
    shelf->card_posed[tween->card] = true;
    shelf->card_pose[tween->card] = lerp_pose(tween->from, tween->to, t);
}

static void apply_card_arc(ShelfWindow *shelf, Tween *tween, float t)
{
    shelf->card_posed[tween->card] = true;
    shelf->card_pose[tween->card] = arc_pose(tween->from, tween->to, tween->control_x, tween->control_y, t);
}

void shelf_roll_rank(ShelfWindow *shelf)
{
    if (!anim_enabled() || !shelf_window_is_visible(shelf)) return;
    shelf->rank_rolling = true;
    clock_reset(&shelf->rank_clock);
    shelf->rank_shift = 8;
    shelf->rank_opacity = 0;
    request_frame(shelf);
}

/// 最上面那张落下后牌角出现，数字从下面滚上来。
static void card_landed(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)t;
    if (tween->card != 0) return;
    shelf->rank_hidden = false;
    shelf_roll_rank(shelf);
}

// ───────────── 拖入时摊开的牌 ─────────────

CardPose shelf_fan_pose(int index, float progress)
{
    // 放置区域里 176 × 108 的画布上，两张牌叠在 (56, 10)；摊开时左边那张（最上面的）转开 13° 往左移，右边那张接着转到 12° 往右移。
    float center_x = 56 + SHELF_CARD_WIDTH / 2, center_y = 10 + SHELF_CARD_HEIGHT / 2;
    if (index == 0) return (CardPose){ center_x - 34 * progress, center_y + 4 * progress, -13 * progress, 1, 1 };
    return (CardPose){ center_x + 11 + 23 * progress, center_y + 2 + 2 * progress, 7 + 5 * progress, 1, 1 };
}

void shelf_fan_out(ShelfWindow *shelf)
{
    anim_set(&shelf->fan, 0);
    anim_start(&shelf->fan, 1, FAN_DURATION, EASE_OF(EASE_CUBIC_OUT));
    request_frame(shelf);
}

/// 摊开的第 index 张牌换到堆叠的画布上：松手后堆叠里的牌从这里合拢，看起来是同一摞牌。没有那张牌时返回 false。
static bool fan_pose_in_stack(ShelfWindow *shelf, int index, CardPose *pose)
{
    if (!shelf->overlay_visible || !shelf_model_from_end(shelf->model, index + 1)) return false;
    const ShelfLayout *layout = &shelf->layout;
    CardPose fan = shelf_fan_pose(index, (float)shelf->fan.value);
    float x = layout->drop_canvas_x + fan.x * layout->drop_canvas_scale, y = layout->drop_canvas_y + fan.y * layout->drop_canvas_scale;
    float cx, cy;
    shelf_to_canvas(shelf, x, y, &cx, &cy);
    *pose = (CardPose){ cx, cy, fan.angle, layout->drop_canvas_scale / (layout->canvas_scale > 0 ? layout->canvas_scale : 1), 1 };
    return true;
}

/// 摊开时第 card 张牌的姿态；没有摊开的那张停在原位但看不见。
static CardPose fan_start(ShelfWindow *shelf, int card)
{
    if (card < 2 && shelf->fan_valid[card]) return shelf->fan_poses[card];
    CardPose home = shelf_card_home(card);
    home.opacity = 0;
    return home;
}

bool shelf_take_fan(ShelfWindow *shelf)
{
    shelf_layout(shelf);
    for (int index = 0; index < 2; index++) shelf->fan_valid[index] = fan_pose_in_stack(shelf, index, &shelf->fan_poses[index]);
    return shelf->fan_valid[0];
}

void shelf_hold_fan(ShelfWindow *shelf)
{
    if (!anim_enabled() || !shelf->fan_valid[0]) return;
    shelf_finish_motion(shelf);
    // 最上面两张分在左右，第三张先藏起来，牌角也藏起来，中间空着等新牌。
    for (int card = 0; card < 3; card++)
    {
        shelf->card_posed[card] = true;
        shelf->card_pose[card] = fan_start(shelf, card);
    }
    shelf->rank_hidden = true;
    shelf->fan_held = true;
    shelf->cleanup |= CLEANUP_FAN | CLEANUP_REST_CARDS | CLEANUP_STACK_MARK;
    shelf_repaint(shelf);
}

void shelf_close_fan(ShelfWindow *shelf)
{
    if (!shelf->fan_held) return;
    shelf_stop_motion(shelf);
    shelf->fan_held = false;
    for (int card = 0; card < 3; card++)
    {
        Tween tween = make_tween(0, FAN_CLOSE_DURATION, apply_card_lerp);
        tween.card = card;
        tween.from = fan_start(shelf, card);
        tween.to = shelf_card_home(card);
        start_tween(shelf, tween);
    }
    shelf->cleanup |= CLEANUP_REST_CARDS | CLEANUP_STACK_MARK;
    shelf_finish_after(shelf, FAN_CLOSE_DURATION);
}

/// 松手后，拖进来的文件从松手处飞进堆叠：从拖动图像大小放大，划一道弧落到自己的位置，原来的牌往下退一层。
/// 一次拖入多个时最多 3 张依次落下，最上面那张最后落下，落下后牌角的数字滚上来。
/// 拖入时摊开过（fan_held）的，新牌落进中间的空位，快落定时原来的牌从左右合拢到下面。
bool shelf_play_landing(ShelfWindow *shelf, float x, float y, int added)
{
    bool fan = shelf->fan_held;
    shelf->fan_held = false;
    shelf_finish_motion(shelf);
    int shown = shelf->model->count < 3 ? shelf->model->count : 3;
    int incoming = added < shown ? added : shown;
    if (incoming <= 0) return false;
    shelf_layout(shelf);
    float start_x, start_y;
    shelf_to_canvas(shelf, x, y, &start_x, &start_y);
    // 落下动画期间最上面那张还没落下，先藏起牌角。
    shelf->rank_hidden = true;
    double end = 0;
    for (int card = 0; card < shown; card++)
    {
        CardPose home = shelf_card_home(card);
        if (card < incoming)
        {
            double delay = LAND_STAGGER * (incoming - 1 - card);
            Tween tween = make_tween(delay, LAND_DURATION, apply_card_arc);
            tween.card = card;
            tween.from = (CardPose){ start_x, start_y, -8, 0.5f, 0 };
            tween.to = home;
            tween.control_x = (tween.from.x + home.x) / 2;
            tween.control_y = fminf(tween.from.y, home.y) - 20;
            tween.completed = card_landed;
            start_tween(shelf, tween);
            end = max_of(end, delay + LAND_DURATION);
        }
        else
        {
            // 退一层的牌原来在上面第 card - incoming 张的位置，摊开过就从摊开的样子合拢。
            int above = card - incoming;
            double delay = fan ? LAND_STAGGER * (incoming - 1) + FAN_CLOSE_DELAY : PUSH_DELAY;
            double duration = fan ? FAN_CLOSE_DURATION : PUSH_DURATION;
            Tween tween = make_tween(delay, duration, apply_card_lerp);
            tween.card = card;
            tween.from = fan ? fan_start(shelf, above) : shelf_card_home(above);
            tween.to = home;
            start_tween(shelf, tween);
            end = max_of(end, delay + duration);
        }
    }
    shelf->cleanup |= CLEANUP_REST_CARDS | CLEANUP_STACK_MARK;
    shelf_finish_after(shelf, end);
    return true;
}

// ───────────── 平铺里的牌 ─────────────

static void set_tile_pose(ShelfItem *item, CardPose pose)
{
    ShelfItemMotion *motion = &item->motion;
    motion->dx = pose.x;
    motion->dy = pose.y;
    motion->angle = pose.angle;
    motion->scale = pose.scale;
    motion->opacity = pose.opacity;
}

/// 牌从 from 划一道弧（经过 control）落到原位。
static void apply_tile_arc(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    set_tile_pose(tween->item, arc_pose(tween->from, tween->to, tween->control_x, tween->control_y, t));
}

/// 牌在 from 和 to 之间直线过渡；flag 时途中略微抬起再放平（发牌）。
static void apply_tile_lerp(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    CardPose pose = lerp_pose(tween->from, tween->to, t);
    if (tween->flag) pose.scale += 0.04f * sinf(3.14159265f * t);
    set_tile_pose(tween->item, pose);
}

/// 要移除的牌缩小、往下沉一点、略微转开并淡出；flag 反过来是撤销后放回。a 是转开的方向，b 是起始的透明度。
static void apply_tile_leave(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    float k = tween->flag ? 1 - t : t;
    float opacity = tween->flag ? t : tween->b * (1 - t);
    set_tile_pose(tween->item, (CardPose){ 0, 10 * k, tween->a * 6 * k, 1 - 0.3f * k, opacity });
}

/// 名字淡入；flag 时淡出。
static void apply_tile_name(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    tween->item->motion.name_opacity = tween->flag ? 1 - t : t;
}

/// 平铺状态下拖入：新文件的牌从松手处划一道弧落到自己的格子，快落下时名字淡入。
/// 一次拖入多个时最多 3 张依次落下，最后一张最后落下；新项目在末尾，看不到时先滚过去。
bool shelf_play_grid_landing(ShelfWindow *shelf, float x, float y, int added)
{
    shelf_finish_motion(shelf);
    int count = shelf->model->count;
    int incoming = added < count ? added : count;
    if (incoming > 3) incoming = 3;
    if (incoming <= 0) return false;
    shelf_layout(shelf);
    shelf_scroll_into_view(shelf, count - 1);
    double end = 0;
    for (int order = 0; order < incoming; order++)
    {
        int index = count - 1 - order;
        ShelfItem *item = shelf->model->items[index];
        GfxRect card = shelf_tile_card_rect(shelf, index);
        float center_x = (card.left + card.right) / 2, center_y = (card.top + card.bottom) / 2;
        double delay = LAND_STAGGER * (incoming - 1 - order);
        Tween fly = make_tween(delay, LAND_DURATION, apply_tile_arc);
        fly.item = item;
        fly.from = (CardPose){ x - center_x, y - center_y, -8, 0.5f, 0 };
        fly.to = (CardPose){ 0, 0, 0, 1, 1 };
        fly.control_x = fly.from.x / 2;
        fly.control_y = fminf(fly.from.y, 0) - 20;
        start_tween(shelf, fly);
        double name_delay = delay + LAND_DURATION / 2;
        Tween name = make_tween(name_delay, NAME_FADE_DURATION, apply_tile_name);
        name.item = item;
        start_tween(shelf, name);
        end = max_of(end, name_delay + NAME_FADE_DURATION);
    }
    shelf->cleanup |= CLEANUP_ITEM_MOTION;
    shelf_finish_after(shelf, end);
    return true;
}

/// 平铺里这张牌现在看不看得见（格子和平铺的范围有重叠）。height 是平铺的可见高度。
static bool tile_visible(ShelfWindow *shelf, int index, float height)
{
    GfxRect slot = shelf_tile_rect(shelf, index);
    float top = slot.top - shelf->layout.list.top - 1;
    return top + SHELF_SLOT_HEIGHT > 0 && top < height;
}

/// 撤销移除之后放回：平铺里放回的牌从下面一点升回原来的格子、转正并淡入（和移除时相反），看不到时先滚到第一张；
/// 堆叠里放回的牌从“撤销”按钮处飞回去，只有放回的正好是最上面几张时才播放。
bool shelf_play_return(ShelfWindow *shelf, float x, float y, ShelfItem **items, int count)
{
    if (!shelf->model->expanded)
    {
        int top = 0;
        while (top < 3 && top < shelf->model->count)
        {
            ShelfItem *item = shelf_model_from_end(shelf->model, top + 1);
            bool back = false;
            for (int index = 0; index < count && !back; index++) back = items[index] == item;
            if (!back) break;
            top++;
        }
        return top > 0 && shelf_play_landing(shelf, x, y, top);
    }
    shelf_finish_motion(shelf);
    if (count == 0) return false;
    shelf_scroll_into_view(shelf, shelf_model_index_of(shelf->model, items[0]));
    shelf_layout(shelf);
    float height = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    int order = 0;
    for (int index = 0; index < count; index++)
    {
        int at = shelf_model_index_of(shelf->model, items[index]);
        if (at < 0 || !tile_visible(shelf, at, height)) continue;
        Tween tween = make_tween(0, CLOSE_UP_DURATION, apply_tile_leave);
        tween.item = items[index];
        tween.flag = true;
        tween.a = order % 2 == 0 ? -1.0f : 1.0f;
        start_tween(shelf, tween);
        order++;
    }
    if (order == 0) return false;
    shelf->cleanup |= CLEANUP_ITEM_MOTION;
    shelf_finish_after(shelf, CLOSE_UP_DURATION);
    return true;
}

// ───────────── 窗口大小 ─────────────

static void apply_window_size(ShelfWindow *shelf, Tween *tween, float t)
{
    if (!shelf->handle) return;
    const int *from = tween->window_from, *to = tween->window_to;
    int x = (int)lroundf(from[0] + (to[0] - from[0]) * t);
    int y = (int)lroundf(from[1] + (to[1] - from[1]) * t);
    int width = (int)lroundf(from[2] + (to[2] - from[2]) * t);
    int height = (int)lroundf(from[3] + (to[3] - from[3]) * t);
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int *last = tween->window_last;
    // 正在滑入、或有牌在替它出场离场时只改大小，不和它们抢位置。
    if (shelf->sliding || shelf->card || (x == last[0] && y == last[1]))
    {
        flags |= SWP_NOMOVE;
        x = last[0];
        y = last[1];
    }
    if (x == last[0] && y == last[1] && width == last[2] && height == last[3]) return;
    last[0] = x;
    last[1] = y;
    last[2] = width;
    last[3] = height;
    SetWindowPos(shelf->handle, NULL, x, y, width, height, flags);
}

static void window_size_done(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)tween, (void)t;
    shelf_update_window_size(shelf);
}

/// 窗口大小逐帧过渡到目标大小。贴着工作区右边的一直贴着，其余情况左上角不动；终点放不下时位置随大小一起朝工作区里移。
static void tween_window_size(ShelfWindow *shelf, double delay)
{
    RECT rect;
    if (!shelf->handle || !GetWindowRect(shelf->handle, &rect))
    {
        shelf_update_window_size(shelf);
        return;
    }
    float width, height;
    shelf_target_size(shelf, shelf->model->expanded, &width, &height);
    float scale = shelf_scale(shelf);
    int to_width = (int)lroundf(width * scale), to_height = (int)lroundf(height * scale);
    int to_left, to_top;
    shelf_fit_resized(rect, to_width, to_height, &to_left, &to_top);
    Tween tween = make_tween(delay, RESIZE_DURATION, apply_window_size);
    int from[4] = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
    int to[4] = { to_left, to_top, to_width, to_height };
    memcpy(tween.window_from, from, sizeof(from));
    memcpy(tween.window_to, to, sizeof(to));
    memcpy(tween.window_last, from, sizeof(from));
    tween.completed = window_size_done;
    start_tween(shelf, tween);
}

// ───────────── 移除 ─────────────

static bool listed(ShelfItem **items, int count, const ShelfItem *item)
{
    for (int index = 0; index < count; index++)
        if (items[index] == item) return true;
    return false;
}

/// 拖出时变淡的项目由移除动画接着淡出：返回现在的透明度，变淡本身停在原样。
static float take_dimmed(ShelfWindow *shelf, ShelfItem *item)
{
    float now = (float)item->dim.value;
    for (int index = 0; index < shelf->dimmed_count; index++)
    {
        if (shelf->dimmed[index] != item) continue;
        shelf_item_release(item);
        shelf->dimmed[index] = shelf->dimmed[--shelf->dimmed_count];
        break;
    }
    anim_set(&item->dim, 1);
    return now;
}

static void remember_removal(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool remains)
{
    shelf->removing = xalloc((size_t)count * sizeof(ShelfItem *));
    for (int index = 0; index < count; index++) shelf->removing[index] = shelf_item_retain(items[index]);
    shelf->removing_count = count;
    shelf->remove_kind = kind;
    shelf->remove_remains = remains;
    shelf->remove_expanded = shelf->model->expanded;
    shelf->cleanup |= CLEANUP_REMOVE;
}

/// 平铺里要移除的牌缩小、往下沉一点、略微转开并淡出；剩下的、生成出来的牌记下移除前的格子，移除后按它补位。
static void remove_tiles(ShelfWindow *shelf, ShelfItem **leaving, int count, RemoveKind kind, bool remains)
{
    shelf_layout(shelf);
    int first, last;
    shelf_visible_tiles(shelf, &first, &last);
    size_t capacity = last >= first ? (size_t)(last - first + 1) : 1;
    free(shelf->rows_before);
    free(shelf->rows_before_x);
    free(shelf->rows_before_y);
    shelf->rows_before = xalloc(capacity * sizeof(ShelfItem *));
    shelf->rows_before_x = xalloc(capacity * sizeof(float));
    shelf->rows_before_y = xalloc(capacity * sizeof(float));
    shelf->rows_before_count = 0;
    GfxRect list = shelf->layout.list;
    // 要移除的先做上记号，不用每一张都去 leaving 里找（全选几千项再移除时是几百万次）。
    for (int index = 0; index < count; index++) leaving[index]->marked = true;
    for (int index = first; index <= last; index++)
    {
        ShelfItem *item = shelf->model->items[index];
        if (item->marked) continue;
        GfxRect slot = shelf_tile_rect(shelf, index);
        shelf->rows_before[shelf->rows_before_count] = item;
        shelf->rows_before_x[shelf->rows_before_count] = slot.left - list.left;
        shelf->rows_before_y[shelf->rows_before_count++] = slot.top - list.top;
    }
    for (int index = 0; index < count; index++) leaving[index]->marked = false;
    remember_removal(shelf, leaving, count, kind, remains);
    shelf->cleanup |= CLEANUP_ITEM_MOTION;
    // 只有生成出来的牌播放离开的动画：这回给它们做记号，按传入的顺序交替往两边转，不用逐个查在第几张。
    for (int index = first; index <= last; index++) shelf->model->items[index]->marked = true;
    int order = 0;
    for (int index = 0; index < count; index++)
    {
        if (!leaving[index]->marked) continue;
        Tween tween = make_tween(0, REMOVE_DURATION, apply_tile_leave);
        tween.item = leaving[index];
        tween.a = order % 2 == 0 ? -1.0f : 1.0f;
        tween.b = take_dimmed(shelf, leaving[index]);
        start_tween(shelf, tween);
        order++;
    }
    for (int index = first; index <= last; index++) shelf->model->items[index]->marked = false;
    shelf_finish_after(shelf, REMOVE_DURATION);
}

/// 平铺移除之后补位：每张牌从移除前的格子滑到现在的格子，之前没有生成的牌从下面一点淡入。
static void close_up_tiles(void *data)
{
    ShelfWindow *shelf = data;
    if (shelf->closing || shelf->closed || !shelf_window_is_visible(shelf) || !shelf->model->expanded)
    {
        shelf->rows_before_count = 0;
        shelf_window_release(shelf);
        return;
    }
    shelf_finish_motion(shelf);
    shelf_layout(shelf);
    GfxRect list = shelf->layout.list;
    int first, last;
    shelf_visible_tiles(shelf, &first, &last);
    bool moved = false;
    for (int index = first; index <= last; index++)
    {
        ShelfItem *item = shelf->model->items[index];
        GfxRect slot = shelf_tile_rect(shelf, index);
        int before = -1;
        for (int at = 0; at < shelf->rows_before_count && before < 0; at++)
            if (shelf->rows_before[at] == item) before = at;
        CardPose from;
        if (before >= 0)
        {
            float dx = shelf->rows_before_x[before] - (slot.left - list.left), dy = shelf->rows_before_y[before] - (slot.top - list.top);
            if (hypotf(dx, dy) < 0.5f) continue;
            from = (CardPose){ dx, dy, 0, 1, 1 };
        }
        else from = (CardPose){ 0, 8, 0, 0.92f, 0 };
        Tween tween = make_tween(0, CLOSE_UP_DURATION, apply_tile_lerp);
        tween.item = item;
        tween.from = from;
        tween.to = (CardPose){ 0, 0, 0, 1, 1 };
        start_tween(shelf, tween);
        moved = true;
    }
    shelf->rows_before_count = 0;
    if (moved)
    {
        shelf->cleanup |= CLEANUP_ITEM_MOTION;
        shelf_finish_after(shelf, CLOSE_UP_DURATION);
    }
    shelf_window_release(shelf);
}

/// 堆叠里要移除的牌缩小、往下沉一点、略微转开并淡出：b 是起始的透明度，flag 决定往哪边转。
static void apply_card_leave(ShelfWindow *shelf, Tween *tween, float t)
{
    CardPose home = tween->from;
    float side = tween->flag ? 1.0f : -1.0f;
    shelf->card_posed[tween->card] = true;
    shelf->card_pose[tween->card] = (CardPose){ home.x, home.y + 10 * t, home.angle + side * 6 * t, 1 - 0.3f * t, tween->b * (1 - t) };
}

static void remove_from_stack(ShelfWindow *shelf, ShelfItem **leaving, int count, RemoveKind kind, bool remains, bool expanded_before)
{
    int going = 0;
    for (int card = 0; card < 3; card++)
    {
        ShelfItem *item = shelf_model_from_end(shelf->model, card + 1);
        shelf->shown_before[card] = shelf_item_retain(item);
        if (item && listed(leaving, count, item)) going++;
    }
    if (going == 0)
    {
        // 移除的都不在最上面三张里，堆叠看起来不变。
        for (int card = 0; card < 3; card++)
        {
            shelf_item_release(shelf->shown_before[card]);
            shelf->shown_before[card] = NULL;
        }
        shelf_perform_remove(shelf, leaving, count, kind, expanded_before);
        return;
    }
    remember_removal(shelf, leaving, count, kind, remains);
    shelf->cleanup |= CLEANUP_REST_CARDS;
    int order = 0;
    for (int card = 0; card < 3; card++)
    {
        ShelfItem *item = shelf_model_from_end(shelf->model, card + 1);
        if (!item || !listed(leaving, count, item)) continue;
        Tween tween = make_tween(0, REMOVE_DURATION, apply_card_leave);
        tween.card = card;
        tween.from = shelf_card_home(card);
        tween.b = take_dimmed(shelf, item);
        tween.flag = order % 2 != 0;
        start_tween(shelf, tween);
        order++;
    }
    shelf_finish_after(shelf, REMOVE_DURATION);
}

/// 堆叠移除之后补位：每张牌从它原来所在的那一层升到现在这一层，新露出来的从最下面一层淡入。
static void rise_stack(void *data)
{
    RisePlan *plan = data;
    ShelfWindow *shelf = plan->shelf;
    if (!shelf->closing && !shelf->closed && shelf_window_is_visible(shelf) && !shelf->model->expanded)
    {
        shelf_finish_motion(shelf);
        bool moved = false;
        for (int card = 0; card < 3; card++)
        {
            ShelfItem *item = shelf_model_from_end(shelf->model, card + 1);
            if (!item) continue;
            int then = -1;
            for (int at = 0; at < 3 && then < 0; at++)
                if (plan->shown[at] == item) then = at;
            if (then == card) continue;
            CardPose from = then > card ? shelf_card_home(then) : shelf_card_home(2);
            if (then <= card)
            {
                from.y += 6;
                from.scale = 0.94f;
                from.opacity = 0;
            }
            Tween tween = make_tween(0, CLOSE_UP_DURATION, apply_card_lerp);
            tween.card = card;
            tween.from = from;
            tween.to = shelf_card_home(card);
            start_tween(shelf, tween);
            moved = true;
        }
        if (moved)
        {
            shelf->cleanup |= CLEANUP_REST_CARDS;
            shelf_finish_after(shelf, CLOSE_UP_DURATION);
        }
    }
    for (int card = 0; card < 3; card++) shelf_item_release(plan->shown[card]);
    shelf_window_release(shelf);
    free(plan);
}

/// 移除牌：要移除的牌缩小、往下沉一点、略微转开并淡出，然后才真正移除。平铺时剩下的牌从原来的格子滑到新的格子补上空位，新露出来的牌淡入；
/// 堆叠时下面的牌升上来补位，新露出来的从最下面淡入。拖出后移除时从拖动期间变淡的样子接着淡出。
/// 动画途中被打断时立即移除，补位直接到位。关闭了动画效果、看不见或 animate 为 false 时直接移除。
void shelf_remove_with_motion(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool animate)
{
    shelf_finish_motion(shelf);
    bool expanded_before = shelf->model->expanded;
    ShelfItem **leaving = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
    int leaving_count = 0;
    // 还在置物架上的才移除，重复传入的只算一次：置物架上的先做上记号，挑走一个去掉一个记号，
    // 不用每一项都把置物架和已经挑出的找一遍（全选几千项再移除时是几百万次）。
    ShelfModel *model = shelf->model;
    for (int index = 0; index < model->count; index++) model->items[index]->marked = true;
    for (int index = 0; index < count; index++)
    {
        if (!items[index]->marked) continue;
        items[index]->marked = false;
        leaving[leaving_count++] = items[index];
    }
    for (int index = 0; index < model->count; index++) model->items[index]->marked = false;
    if (!animate || !anim_enabled() || !shelf_window_is_visible(shelf) || leaving_count == 0)
        shelf_perform_remove(shelf, leaving, leaving_count, kind, expanded_before);
    else
    {
        bool remains = leaving_count < shelf->model->count;
        shelf->remove_expanded_before = expanded_before;
        if (shelf->model->expanded) remove_tiles(shelf, leaving, leaving_count, kind, remains);
        else remove_from_stack(shelf, leaving, leaving_count, kind, remains, expanded_before);
    }
    free(leaving);
}

// ───────────── 堆叠和平铺之间切换 ─────────────

static void apply_mark_fade(ShelfWindow *shelf, Tween *tween, float t)
{
    shelf->mark_opacity = tween->flag ? 1 - t : t;
}

/// 堆叠和平铺之间切换。展开时窗口先平滑变大，堆叠里的牌藏起来，平铺里看得见的牌从堆叠的位置出发，一张张发到各自的格子，
/// 落下时略微抬起再放平，名字随后淡入；最后几张占住堆叠顶上几张的位置，其余的从最下面那张出发、压在它们下面，起手时那一摞和堆叠一模一样。
/// 收起时名字先淡出，牌倒着一张张收回堆叠，之后窗口再平滑变小。只有看得见的牌参与，其余的直接出现或消失。
void shelf_animate_view_switch(ShelfWindow *shelf)
{
    shelf_finish_motion(shelf);
    shelf_layout(shelf);
    ShelfModel *model = shelf->model;
    bool to_grid = model->expanded;
    float width, height;
    shelf_target_size(shelf, to_grid, &width, &height);
    GfxSize actual = ui_window_size(shelf->ui);
    GfxRect body = shelf->layout.body;
    GfxSize now = { body.right - body.left, body.bottom - body.top };
    GfxSize body_size = { now.width + width - actual.width, now.height + height - actual.height };

    // 堆叠和平铺都固定为各自显示时的大小，窗口比它们大或小时只是露出或裁掉一部分；平铺压在堆叠上面，牌飞过堆叠时不会被挡住。
    shelf->switching = true;
    shelf->stack_pinned = shelf->list_pinned = true;
    shelf->stack_pin = to_grid ? now : body_size;
    shelf->list_pin = to_grid ? body_size : now;
    shelf_layout(shelf);
    // 展开时平铺滚到最后：堆叠最上面几张是最新放进来的，在平铺末尾，这样发出去的牌里一定有它们，项目再多也一样。
    if (to_grid && model->count > 0) shelf_scroll_into_view(shelf, model->count - 1);
    shelf_layout(shelf);
    const ShelfLayout *layout = &shelf->layout;

    // 堆叠里各张牌在窗口里的姿态；切换期间堆叠里的牌藏起来，由平铺里的牌替它们飞。
    int shown = model->count < 3 ? model->count : 3;
    int start_count = shown > 0 ? shown : 1;
    CardPose starts[3];
    for (int card = 0; card < 3; card++)
    {
        CardPose home = shelf_card_home(card);
        if (card < start_count)
            starts[card] = (CardPose){ layout->canvas_x + home.x * layout->canvas_scale, layout->canvas_y + home.y * layout->canvas_scale, home.angle,
                layout->canvas_scale, 1 };
        home.opacity = 0;
        shelf->card_posed[card] = true;
        shelf->card_pose[card] = home;
    }

    // 只动看得见的牌。
    int first, last;
    shelf_visible_tiles(shelf, &first, &last);
    int *tiles = xalloc((size_t)(last >= first ? last - first + 1 : 1) * sizeof(int));
    int tile_count = 0;
    float grid_height = layout->list.bottom - layout->list.top;
    for (int index = first; index <= last; index++)
    {
        GfxRect slot = shelf_tile_rect(shelf, index);
        if (slot.bottom - layout->list.top > 0 && slot.top - layout->list.top < grid_height) tiles[tile_count++] = index;
    }

    double lead = to_grid ? RESIZE_DURATION : FADE_OUT_DURATION / 2;
    // 牌多时间隔缩短：大尺寸的平铺不比小尺寸的发得久。
    double stagger = DEAL_STAGGER * fmin(1, MAX_DEAL_GAPS / fmax(1, tile_count - 1));
    double end = 0;
    for (int order = 0; order < tile_count; order++)
    {
        int index = tiles[order];
        GfxRect card = shelf_tile_card_rect(shelf, index);
        float center_x = (card.left + card.right) / 2, center_y = (card.top + card.bottom) / 2;
        // 从后往前数：最后一张对着堆叠最上面那张，依次往下，其余的都从最下面那张出发。
        int from_card = tile_count - 1 - order < start_count - 1 ? tile_count - 1 - order : start_count - 1;
        CardPose stack = starts[from_card];
        CardPose away = { stack.x - center_x, stack.y - center_y, stack.angle, stack.scale, 1 };
        CardPose home = { 0, 0, 0, 1, 1 };
        double delay = lead + stagger * (to_grid ? order : tile_count - 1 - order);
        Tween fly = make_tween(delay, FLY_DURATION, apply_tile_lerp);
        fly.item = model->items[index];
        fly.from = to_grid ? away : home;
        fly.to = to_grid ? home : away;
        fly.flag = true;
        start_tween(shelf, fly);
        end = max_of(end, delay + FLY_DURATION);
        double name_delay = to_grid ? delay + FLY_DURATION * 0.6 : 0;
        Tween name = make_tween(name_delay, to_grid ? NAME_FADE_DURATION : FADE_OUT_DURATION, apply_tile_name);
        name.item = model->items[index];
        name.flag = !to_grid;
        start_tween(shelf, name);
        if (to_grid) end = max_of(end, name_delay + NAME_FADE_DURATION);
    }
    free(tiles);

    // 堆叠下方的说明和牌角：展开时随牌发出淡出，收起时在最后一张收回前淡入。
    double caption_delay = to_grid ? lead : max_of(0, end - NAME_FADE_DURATION);
    double caption_duration = to_grid ? FADE_OUT_DURATION : NAME_FADE_DURATION;
    Tween mark = make_tween(caption_delay, caption_duration, apply_mark_fade);
    mark.flag = to_grid;
    start_tween(shelf, mark);
    end = max_of(end, caption_delay + caption_duration);

    // 展开时上面的动画都推迟到窗口变大之后；收起时牌是从各个格子飞回来的，等它们都回到堆叠里再把窗口变小。
    tween_window_size(shelf, to_grid ? 0 : end);
    if (!to_grid) end += RESIZE_DURATION;
    shelf->cleanup |= CLEANUP_REST_CARDS | CLEANUP_ITEM_MOTION | CLEANUP_STACK_MARK | CLEANUP_UNPIN | CLEANUP_UPDATE_VIEWS | CLEANUP_WINDOW_SIZE;
    shelf_finish_after(shelf, end);
}

// ───────────── 拖出时变淡 ─────────────

void shelf_dim_dragged(ShelfWindow *shelf, ShelfItem **items, int count)
{
    if (!anim_enabled()) return;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        // 堆叠里只有看得见的三张会变淡。
        if (!shelf->model->expanded)
        {
            bool shown = false;
            for (int card = 1; card <= 3 && !shown; card++) shown = shelf_model_from_end(shelf->model, card) == item;
            if (!shown) continue;
        }
        anim_start(&item->dim, 0.4, 0.150, EASE_OF(EASE_CUBIC_OUT));
        shelf->dimmed = xrealloc(shelf->dimmed, (size_t)(shelf->dimmed_count + 1) * sizeof(ShelfItem *));
        shelf->dimmed[shelf->dimmed_count++] = shelf_item_retain(item);
    }
    request_frame(shelf);
}

void shelf_restore_dimmed(ShelfWindow *shelf)
{
    for (int index = 0; index < shelf->dimmed_count; index++)
    {
        anim_start(&shelf->dimmed[index]->dim, 1, 0.150, EASE_OF(EASE_CUBIC_OUT));
        shelf_item_release(shelf->dimmed[index]);
    }
    shelf->dimmed_count = 0;
    request_frame(shelf);
}

// ───────────── 出场和离场 ─────────────
// 快捷键、托盘菜单等主动打开：窗口从下方升起一点，内容淡入并放大到原尺寸。晃动和边缘呼出由一张牌替置物架出场，关闭时也由一张牌离场，见 shelf_card.h。

#define RISE_DURATION 0.300
#define RISE_FADE_DURATION 0.180
#define RISE_START_SCALE 0.94f
#define RISE_DISTANCE 12.0

static void move_window(ShelfWindow *shelf, int x, int y)
{
    SetWindowPos(shelf->handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void shelf_stop_slide(ShelfWindow *shelf)
{
    shelf->sliding = false;
}

void shelf_stop_pop(ShelfWindow *shelf)
{
    shelf->popping = false;
}

/// 逐帧移动整个窗口。只改变位置不改变大小，移动很流畅。
static void slide_window(ShelfWindow *shelf, int from_x, int from_y, int to_x, int to_y, double duration, Ease ease, bool keep_in_work_area)
{
    shelf_stop_slide(shelf);
    if (!shelf->handle || (from_x == to_x && from_y == to_y))
    {
        if (keep_in_work_area) shelf_keep_in_work_area(shelf);
        return;
    }
    move_window(shelf, from_x, from_y);
    shelf->sliding = true;
    clock_reset(&shelf->slide_clock);
    shelf->slide_from_x = shelf->slide_x = from_x;
    shelf->slide_from_y = shelf->slide_y = from_y;
    shelf->slide_to_x = to_x;
    shelf->slide_to_y = to_y;
    shelf->slide_duration = duration;
    shelf->slide_ease = ease;
    shelf->slide_keep_in_work_area = keep_in_work_area;
    request_frame(shelf);
}

static void update_slide(ShelfWindow *shelf, double time)
{
    if (!shelf->sliding || !clock_tick(&shelf->slide_clock, time)) return;
    double progress = fmin(1, shelf->slide_clock.elapsed / shelf->slide_duration);
    double eased = ease_apply(shelf->slide_ease, progress);
    int x = (int)lround(shelf->slide_from_x + (shelf->slide_to_x - shelf->slide_from_x) * eased);
    int y = (int)lround(shelf->slide_from_y + (shelf->slide_to_y - shelf->slide_from_y) * eased);
    // 取整后位置没变时不必移动窗口。
    if (x != shelf->slide_x || y != shelf->slide_y) move_window(shelf, x, y);
    shelf->slide_x = x;
    shelf->slide_y = y;
    if (progress < 1) return;
    shelf->sliding = false;
    if (shelf->slide_keep_in_work_area) shelf_keep_in_work_area(shelf);
}

/// 主动打开时的弹出动画：窗口从下方升起一点，内容同时淡入并放大到原尺寸。
/// 低性能模式下窗口直接出现在原处，内容在窗口里升起同样一段：不用逐帧移动窗口。
void shelf_animate_in(ShelfWindow *shelf)
{
    shelf_stop_slide(shelf);
    shelf_stop_pop(shelf);
    shelf->surface_dx = shelf->surface_dy = 0;
    shelf->surface_scale = 1;
    if (!anim_enabled())
    {
        shelf->surface_opacity = 1;
        shelf_repaint(shelf);
        return;
    }
    bool low = low_performance_enabled();
    RECT rect;
    if (!low && shelf->handle && GetWindowRect(shelf->handle, &rect))
        slide_window(shelf, rect.left, rect.top + (int)lround(RISE_DISTANCE * shelf_scale(shelf)), rect.left, rect.top, RISE_DURATION,
            EASE_OF(EASE_QUINTIC_OUT), true);
    // 低性能模式下内容在窗口里升起同样一段（DIP），缓动和升起窗口一样。
    shelf->pop_shift_y = low ? (float)RISE_DISTANCE : 0;
    // 淡入、放大和升起用同一种时钟逐帧推进，第一帧耽搁时一起从头开始。
    shelf->popping = true;
    clock_reset(&shelf->pop_clock);
    shelf->surface_opacity = 0;
    shelf->surface_scale = RISE_START_SCALE;
    shelf->surface_dy = shelf->pop_shift_y;
    request_frame(shelf);
}

static void update_pop(ShelfWindow *shelf, double time)
{
    if (!shelf->popping || !clock_tick(&shelf->pop_clock, time)) return;
    double elapsed = shelf->pop_clock.elapsed;
    double eased = ease_apply(EASE_OF(EASE_QUINTIC_OUT), fmin(1, elapsed / RISE_DURATION));
    shelf->surface_opacity = (float)ease_apply(EASE_OF(EASE_CUBIC_OUT), fmin(1, elapsed / RISE_FADE_DURATION));
    shelf->surface_scale = (float)(RISE_START_SCALE + (1 - RISE_START_SCALE) * eased);
    shelf->surface_dy = (float)(shelf->pop_shift_y * (1 - eased));
    if (elapsed < RISE_DURATION) return;
    shelf->popping = false;
    shelf->surface_opacity = 1;
    shelf->surface_scale = 1;
    shelf->surface_dx = shelf->surface_dy = 0;
}

static RECT work_area_of(RECT rect)
{
    POINT center = { rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcWork;
}

/// 离场的牌现身的同一刻置物架隐身，中间不会空一帧，两层阴影也不会叠在一起。
static void exit_shown(void *context)
{
    ShelfWindow *shelf = context;
    window_set_cloaked(shelf->handle, true);
}

static void close_after_exit(void *data)
{
    ShelfWindow *shelf = data;
    shelf_window_close_now(shelf);
    shelf_window_release(shelf);
}

static void exit_finished(void *context)
{
    ShelfWindow *shelf = context;
    shelf->card = NULL;
    // 不在画的过程中销毁窗口，放到消息循环里去关。
    task_post(close_after_exit, shelf_window_retain(shelf));
}

bool shelf_play_exit(ShelfWindow *shelf)
{
    RECT rect = {0};
    GfxImage *face = shelf->handle && GetWindowRect(shelf->handle, &rect) ? ui_window_capture(shelf->ui) : NULL;
    ShelfCard *card = face ? shelf_card_rent() : NULL;
    if (!card)
    {
        gfx_image_release(face);
        return false;
    }
    double scale = shelf_scale(shelf);
    RECT work = work_area_of(rect);
    CardMotion motion = card_motion_exit(shelf->entrance == ENTRANCE_EDGE ? shelf->edge : EDGE_NONE, shelf->flipped_on_horizontal_axis, rect, work, scale);
    shelf->card = card;
    shelf_card_play(card, rect, work, scale, face, &motion, exit_shown, exit_finished, shelf);
    gfx_image_release(face);
    return true;
}

// ───────────── 每一帧 ─────────────

static bool update_anim(Anim *anim, double time)
{
    return anim_update(anim, time);
}

void shelf_animate(UiWindow *window, void *context, double time)
{
    (void)window;
    ShelfWindow *shelf = context;
    if (shelf->closed) return;
    update_slide(shelf, time);
    update_pop(shelf, time);
    update_motion(shelf, time);
    bool running = shelf->sliding || shelf->popping || shelf->motion_running;

    // 牌角的数字从下面滚上来。
    if (shelf->rank_rolling && clock_tick(&shelf->rank_clock, time))
    {
        double progress = fmin(1, shelf->rank_clock.elapsed / RANK_ROLL_DURATION);
        float eased = (float)ease_apply(EASE_OF(EASE_CUBIC_OUT), progress);
        shelf->rank_shift = 8 * (1 - eased);
        shelf->rank_opacity = eased;
        if (progress >= 1) shelf->rank_rolling = false;
    }
    running |= shelf->rank_rolling;
    running |= update_anim(&shelf->fan, time);

    running |= update_anim(&shelf->overlay_opacity, time);
    running |= update_anim(&shelf->body_opacity, time);
    running |= update_anim(&shelf->busy_opacity, time);
    running |= update_anim(&shelf->toast_opacity, time);
    running |= update_anim(&shelf->toast_offset, time);
    running |= update_anim(&shelf->tiles_height, time);
    running |= update_anim(&shelf->tiles_opacity, time);
    running |= update_anim(&shelf->bar_opacity, time);
    running |= update_anim(&shelf->frame_opacity, time);
    for (int index = 0; index < SHELF_MAX_ACTIONS; index++) running |= update_anim(&shelf->tile_highlight[index], time);
    running |= update_anim(&shelf->merge_dim, time);
    running |= update_anim(&shelf->merge_charge_opacity, time);
    running |= update_anim(&shelf->merge_charge_scale, time);
    running |= update_anim(&shelf->drop_label_opacity, time);
    running |= update_anim(&shelf->drop_zone_scale, time);
    shelf_update_merge_pulse(shelf);
    running |= shelf->drop_zone_scale.running;
    // 放置区域淡出后才真正收起，合并的蓄力底色也在这时去掉。
    if (!shelf->overlay_opacity.running && shelf->overlay_opacity.value <= 0 && shelf->overlay_visible && !shelf->drag_inside &&
        shelf->merge_hint == MERGE_HINT_NONE)
    {
        shelf->overlay_visible = false;
        shelf_reset_merge_charge(shelf);
    }
    for (int index = 0; index < shelf->model->count; index++)
    {
        ShelfItem *item = shelf->model->items[index];
        running |= update_anim(&item->dim, time);
        running |= update_anim(&item->lift, time);
    }
    if (running) request_frame(shelf);
}
