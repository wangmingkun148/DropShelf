// 置物架的动画，和 C# 版的 ShelfWindow.Motion.cs、ShelfWindow.Entrance.cs、ShelfMotion.cs 对应。
// 拖进来的文件从松手处飞进堆叠或列表；堆叠和列表之间切换时，每张牌飞到列表里自己那一行（或飞回来）。
// 窗口大小平滑过渡，但不和牌同时动：展开时窗口先变大再飞牌，收起时牌飞回去之后窗口再变小。动画途中再有变化时直接跳到终点。
#include "shelf_window_internal.h"
#include "log.h"
#include "task.h"

#include <math.h>
#include <shellscalingapi.h>
#include <stdlib.h>
#include <string.h>

#define LAND_DURATION 0.380
#define LAND_STAGGER 0.080
#define PUSH_DELAY 0.120
#define PUSH_DURATION 0.300
#define FLY_DURATION 0.280
#define FLY_STAGGER 0.024
#define ROW_FADE_DELAY 0.070
#define ROW_FADE_DURATION 0.200
#define ROW_STAGGER_LIMIT 5
#define FADE_OUT_DURATION 0.120
#define RESIZE_DURATION 0.160
#define REMOVE_DURATION 0.160
#define CLOSE_UP_DURATION 0.220
#define CLOSE_DURATION 0.170
#define CLOSE_SCALE 0.94f
#define BADGE_POP_DURATION 0.240

// ───────────── 时钟 ─────────────

bool clock_tick(MotionClock *clock, double time)
{
    if (clock->has_last && time == clock->last) return false;
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

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
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
    shelf->mark_override = false;
    shelf->mark_opacity = 1;
}

static void reset_item_motion(ShelfWindow *shelf)
{
    for (int index = 0; index < shelf->model->count; index++) shelf_item_motion_reset(shelf->model->items[index]);
}

static void close_up_rows(void *data);
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
        // 列表移除之后补位：等数量变化引起的刷新按新的数量排好列表，再从移除前的位置滑过去。
        shelf->hold_size = remains && expanded;
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
        if (remains && expanded) task_post(close_up_rows, shelf_window_retain(shelf));
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

void shelf_card_landed_mark(ShelfWindow *shelf, int count)
{
    shelf->mark_override = true;
    shelf->mark_count = count;
    if (count < 2) return;
    // 每落下一张，角标的数字加一并弹一下。
    shelf->badge_popping = true;
    clock_reset(&shelf->badge_clock);
    shelf->badge_scale = 1.28f;
    request_frame(shelf);
}

static void card_landed(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)t;
    shelf_card_landed_mark(shelf, tween->number);
}

/// 松手后，拖进来的文件从松手处飞进堆叠：从拖动图像大小放大，划一道弧落到自己的位置，原来的牌往下退一层。
/// 一次拖入多个时最多 3 张依次落下，最上面那张最后落下，数量随每张落下增加。
bool shelf_play_landing(ShelfWindow *shelf, float x, float y, int added)
{
    shelf_finish_motion(shelf);
    int shown = shelf->model->count < 3 ? shelf->model->count : 3;
    int incoming = added < shown ? added : shown;
    if (incoming <= 0) return false;
    shelf_layout(shelf);
    float start_x, start_y;
    shelf_to_canvas(shelf, x, y, &start_x, &start_y);
    int count = shelf->model->count;
    shelf->mark_override = true;
    shelf->mark_count = count - incoming;
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
            tween.number = count - card;
            tween.completed = card_landed;
            start_tween(shelf, tween);
            end = max_of(end, delay + LAND_DURATION);
        }
        else
        {
            Tween tween = make_tween(PUSH_DELAY, PUSH_DURATION, apply_card_lerp);
            tween.card = card;
            tween.from = shelf_card_home(card - incoming);
            tween.to = home;
            start_tween(shelf, tween);
            end = max_of(end, PUSH_DELAY + PUSH_DURATION);
        }
    }
    shelf->cleanup |= CLEANUP_REST_CARDS | CLEANUP_STACK_MARK;
    shelf_finish_after(shelf, end);
    return true;
}

// ───────────── 列表里的行 ─────────────

static void apply_thumb_arc(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    CardPose pose = arc_pose(tween->from, tween->to, tween->control_x, tween->control_y, t);
    ShelfItemMotion *motion = &tween->item->motion;
    motion->thumb_dx = pose.x;
    motion->thumb_dy = pose.y;
    motion->thumb_angle = pose.angle;
    motion->thumb_scale = pose.scale;
    motion->thumb_opacity = pose.opacity;
}

/// 文字从左边滑出：a 是起点的偏移。
static void apply_text_in(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    tween->item->motion.text_opacity = t;
    tween->item->motion.text_dx = tween->a * (1 - t);
}

/// 整行从左边滑入（a 是起点的偏移）或滑出（flag）。
static void apply_row_slide(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    ShelfItemMotion *motion = &tween->item->motion;
    if (tween->flag)
    {
        motion->row_opacity = tween->b * (1 - t);
        motion->row_dx = tween->a * t;
    }
    else
    {
        motion->row_opacity = t;
        motion->row_dx = tween->a * (1 - t);
    }
}

/// 补位：整行从 a 的偏移滑到原位，flag 时同时淡入。
static void apply_row_close_up(ShelfWindow *shelf, Tween *tween, float t)
{
    (void)shelf;
    tween->item->motion.row_dy = tween->a * (1 - t);
    if (tween->flag) tween->item->motion.row_opacity = t;
}

/// 列表状态下拖入：新文件的图标从松手处划一道弧飞到自己那一行的行首，快落下时这一行的文字从左边滑出。
/// 一次拖入多个时最多 3 个依次落下；新项目在列表末尾，看不到时先滚过去。lead 是窗口先长高的时间。
bool shelf_play_list_landing(ShelfWindow *shelf, float x, float y, int added, double lead)
{
    if (lead == 0) shelf_finish_motion(shelf);
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
        GfxRect thumb = shelf_row_thumb_rect(shelf, index);
        float center_x = (thumb.left + thumb.right) / 2, center_y = (thumb.top + thumb.bottom) / 2;
        double delay = lead + LAND_STAGGER * (incoming - 1 - order);
        Tween fly = make_tween(delay, LAND_DURATION, apply_thumb_arc);
        fly.item = item;
        fly.from = (CardPose){ x - center_x, y - center_y, -8, 1.5f, 0 };
        fly.to = (CardPose){ 0, 0, 0, 1, 1 };
        fly.control_x = fly.from.x / 2;
        fly.control_y = fminf(fly.from.y, 0) - 20;
        start_tween(shelf, fly);
        double text_delay = delay + LAND_DURATION / 2;
        Tween text = make_tween(text_delay, ROW_FADE_DURATION, apply_text_in);
        text.item = item;
        text.a = -10;
        start_tween(shelf, text);
        end = max_of(end, text_delay + ROW_FADE_DURATION);
    }
    shelf->cleanup |= CLEANUP_ITEM_MOTION;
    shelf_finish_after(shelf, max_of(end, lead));
    return true;
}

/// 撤销移除之后放回：列表里放回的行从左边滑回来；堆叠里放回的牌从“撤销”按钮处飞回去，只有放回的正好是最上面几张时才播放。
bool shelf_play_return(ShelfWindow *shelf, float x, float y, ShelfItem **items, int count, double lead)
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
    if (lead == 0) shelf_finish_motion(shelf);
    if (count == 0) return false;
    shelf_scroll_into_view(shelf, shelf_model_index_of(shelf->model, items[0]));
    bool any = false;
    for (int index = 0; index < count; index++)
    {
        if (!shelf_model_contains(shelf->model, items[index])) continue;
        Tween tween = make_tween(lead, ROW_FADE_DURATION, apply_row_slide);
        tween.item = items[index];
        tween.a = -12;
        start_tween(shelf, tween);
        any = true;
    }
    if (!any) return false;
    shelf->cleanup |= CLEANUP_ITEM_MOTION;
    shelf_finish_after(shelf, lead + ROW_FADE_DURATION);
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
    // 正在滑入或退出时只改大小，不和滑入动画抢位置。
    if (shelf->sliding || (x == last[0] && y == last[1]))
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

static float client_height(ShelfWindow *shelf)
{
    return ui_window_size(shelf->ui).height;
}

/// 列表里多了几行、窗口要变高：列表先固定为变高后的大小，窗口平滑长高把新的行露出来，之后再播落下的动画。
double shelf_grow_for_rows(ShelfWindow *shelf)
{
    shelf_layout(shelf);
    float width, height;
    shelf_target_size(shelf, shelf->model->expanded, &width, &height);
    float actual = client_height(shelf);
    if (height <= actual + 0.5f || !shelf->handle) return 0;
    GfxRect list = shelf->layout.list;
    shelf->list_pinned = true;
    shelf->list_pin = (GfxSize){ list.right - list.left, list.bottom - list.top + height - actual };
    shelf_layout(shelf);
    tween_window_size(shelf, 0);
    shelf->cleanup |= CLEANUP_UNPIN | CLEANUP_WINDOW_SIZE;
    return RESIZE_DURATION;
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

static void remove_rows(ShelfWindow *shelf, ShelfItem **leaving, int count, RemoveKind kind, bool remains)
{
    shelf_layout(shelf);
    // 剩下的、看得见的行移除前的位置，移除后按它补位。
    free(shelf->rows_before);
    free(shelf->rows_before_y);
    shelf->rows_before = xalloc((size_t)shelf->model->count * sizeof(ShelfItem *));
    shelf->rows_before_y = xalloc((size_t)shelf->model->count * sizeof(float));
    shelf->rows_before_count = 0;
    GfxRect list = shelf->layout.list;
    for (int index = 0; index < shelf->model->count; index++)
    {
        ShelfItem *item = shelf->model->items[index];
        GfxRect row = shelf_row_rect(shelf, index);
        if (listed(leaving, count, item) || row.bottom < list.top || row.top > list.bottom) continue;
        shelf->rows_before[shelf->rows_before_count] = item;
        shelf->rows_before_y[shelf->rows_before_count++] = row.top - list.top;
    }
    remember_removal(shelf, leaving, count, kind, remains);
    shelf->cleanup |= CLEANUP_ITEM_MOTION;
    for (int index = 0; index < count; index++)
    {
        Tween tween = make_tween(0, REMOVE_DURATION, apply_row_slide);
        tween.item = leaving[index];
        tween.flag = true;
        tween.a = -12;
        tween.b = take_dimmed(shelf, leaving[index]);
        start_tween(shelf, tween);
    }
    shelf_finish_after(shelf, REMOVE_DURATION);
}

/// 列表移除之后补位：每一行从移除前的位置滑到现在的位置，之前看不见的行从下面一点淡入；补完窗口再平滑变小。
static void close_up_rows(void *data)
{
    ShelfWindow *shelf = data;
    shelf->hold_size = false;
    if (shelf->closing || shelf->closed || !shelf_window_is_visible(shelf) || !shelf->model->expanded)
    {
        if (!shelf->closing && !shelf->closed) shelf_update_window_size(shelf);
        shelf_window_release(shelf);
        return;
    }
    shelf_finish_motion(shelf);
    shelf_layout(shelf);
    GfxRect list = shelf->layout.list;
    bool moved = false;
    for (int index = 0; index < shelf->model->count; index++)
    {
        ShelfItem *item = shelf->model->items[index];
        GfxRect row = shelf_row_rect(shelf, index);
        if (row.bottom < list.top || row.top > list.bottom) continue;
        int before = -1;
        for (int at = 0; at < shelf->rows_before_count && before < 0; at++)
            if (shelf->rows_before[at] == item) before = at;
        bool appears = before < 0;
        float from = appears ? 8 : shelf->rows_before_y[before] - (row.top - list.top);
        if (fabsf(from) < 0.5f) continue;
        Tween tween = make_tween(0, CLOSE_UP_DURATION, apply_row_close_up);
        tween.item = item;
        tween.a = from;
        tween.flag = appears;
        start_tween(shelf, tween);
        moved = true;
    }
    shelf->rows_before_count = 0;
    double lead = moved ? CLOSE_UP_DURATION : 0;
    tween_window_size(shelf, lead);
    shelf->cleanup |= CLEANUP_ITEM_MOTION | CLEANUP_WINDOW_SIZE;
    shelf_finish_after(shelf, lead + RESIZE_DURATION);
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

/// 移除项目：列表里要移除的行向左滑出并淡出，堆叠里要移除的牌缩小、下沉、转开并淡出，然后才真正移除，剩下的补位。
/// 动画途中被打断时立即移除。关闭了动画效果、看不见或 animate 为 false 时直接移除。
void shelf_remove_with_motion(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool animate)
{
    shelf_finish_motion(shelf);
    bool expanded_before = shelf->model->expanded;
    ShelfItem **leaving = xalloc((size_t)(count ? count : 1) * sizeof(ShelfItem *));
    int leaving_count = 0;
    for (int index = 0; index < count; index++)
        if (shelf_model_contains(shelf->model, items[index]) && !listed(leaving, leaving_count, items[index])) leaving[leaving_count++] = items[index];
    if (!animate || !anim_enabled() || !shelf_window_is_visible(shelf) || leaving_count == 0)
        shelf_perform_remove(shelf, leaving, leaving_count, kind, expanded_before);
    else
    {
        bool remains = leaving_count < shelf->model->count;
        shelf->remove_expanded_before = expanded_before;
        if (shelf->model->expanded) remove_rows(shelf, leaving, leaving_count, kind, remains);
        else remove_from_stack(shelf, leaving, leaving_count, kind, remains, expanded_before);
    }
    free(leaving);
}

// ───────────── 堆叠和列表之间切换 ─────────────

static void apply_switch_card(ShelfWindow *shelf, Tween *tween, float t)
{
    CardPose pose = lerp_pose(tween->from, tween->to, t);
    // 飞到行首时和行里的图标交叉淡入淡出，两者大小不完全一样也看不出跳动。
    if (tween->flag && tween->item)
    {
        pose.opacity = clampf((1 - t) / 0.25f, 0, 1);
        tween->item->motion.thumb_opacity = 1 - pose.opacity;
    }
    shelf->card_posed[tween->card] = true;
    shelf->card_pose[tween->card] = pose;
}

static void apply_mark_fade(ShelfWindow *shelf, Tween *tween, float t)
{
    shelf->mark_opacity = tween->flag ? 1 - t : t;
}

/// 堆叠和列表之间切换：堆叠保持收起时的大小留在左上角，牌飞到列表里自己那一行并缩成行首图标，各行随后从左边滑出；
/// 收起时各行淡出，图标飞回去叠成一摞。展开时窗口先平滑变大再开始，收起时等这些都结束了窗口再平滑变小。
void shelf_animate_view_switch(ShelfWindow *shelf)
{
    shelf_finish_motion(shelf);
    shelf_layout(shelf);
    ShelfModel *model = shelf->model;
    bool to_list = model->expanded;
    float width, height;
    shelf_target_size(shelf, to_list, &width, &height);
    GfxSize actual = ui_window_size(shelf->ui);
    GfxRect body = shelf->layout.body;
    GfxSize body_size = { body.right - body.left + width - actual.width, body.bottom - body.top + height - actual.height };
    GfxSize stack_size = to_list ? (GfxSize){ body.right - body.left, body.bottom - body.top } : body_size;
    GfxSize list_size = to_list ? body_size : (GfxSize){ body.right - body.left, body.bottom - body.top };

    // 堆叠和列表都固定为各自显示时的大小，窗口比它们大或小时只是露出或裁掉一部分。
    shelf->switching = true;
    shelf->stack_pinned = shelf->list_pinned = true;
    shelf->stack_pin = stack_size;
    shelf->list_pin = list_size;
    shelf_layout(shelf);
    if (to_list && model->count > 0) shelf_scroll_into_view(shelf, model->count - 1);
    shelf_layout(shelf);
    GfxRect list = shelf->layout.list;
    float list_height = list_size.height;

    double lead = to_list ? RESIZE_DURATION : 0;
    double end = 0;
    for (int card = 0; card < 3; card++)
    {
        ShelfItem *item = shelf_model_from_end(model, card + 1);
        if (!item) continue;
        int index = shelf_model_index_of(model, item);
        GfxRect thumb = shelf_row_thumb_rect(shelf, index);
        bool lands = thumb.top - list.top >= 0 && thumb.bottom - list.top <= list_height;
        CardPose target;
        if (lands)
        {
            float cx, cy;
            shelf_to_canvas(shelf, (thumb.left + thumb.right) / 2, (thumb.top + thumb.bottom) / 2, &cx, &cy);
            target = (CardPose){ cx, cy, 0, (32 / shelf->layout.canvas_scale) / SHELF_CARD_SIZE, 1 };
            item->motion.thumb_opacity = 0;
        }
        else
        {
            // 这一行在列表可见范围之外：牌朝列表底边飞去并淡出，或从那里淡入。
            float ex, ey;
            shelf_to_canvas(shelf, list.left + 22, list.top + list_height, &ex, &ey);
            target = (CardPose){ ex, ey, 0, 0.3f, 0 };
        }
        double delay = lead + FLY_STAGGER * (to_list ? card : 2 - card);
        Tween tween = make_tween(delay, FLY_DURATION, apply_switch_card);
        tween.card = card;
        tween.from = to_list ? shelf_card_home(card) : target;
        tween.to = to_list ? target : shelf_card_home(card);
        tween.flag = to_list && lands;
        if (tween.flag) tween.item = item;
        start_tween(shelf, tween);
        end = max_of(end, delay + FLY_DURATION);
    }

    // 只动看得见的行，依次滑出的先后也按看得见的顺序排。
    int order = 0;
    for (int index = 0; index < model->count; index++)
    {
        GfxRect row = shelf_row_rect(shelf, index);
        float top = row.top - 1 - list.top;
        if (top + SHELF_ROW_PITCH <= 0 || top >= list_height) continue;
        int stagger = order < ROW_STAGGER_LIMIT ? order : ROW_STAGGER_LIMIT;
        order++;
        Tween tween;
        if (to_list)
        {
            double delay = lead + ROW_FADE_DELAY + FLY_STAGGER * stagger;
            tween = make_tween(delay, ROW_FADE_DURATION, apply_row_slide);
            tween.a = -10;
            end = max_of(end, delay + ROW_FADE_DURATION);
        }
        else
        {
            tween = make_tween(0, FADE_OUT_DURATION, apply_row_slide);
            tween.flag = true;
            tween.a = -10;
            tween.b = 1;
        }
        tween.item = model->items[index];
        start_tween(shelf, tween);
    }

    double fade_delay = to_list ? 0 : FLY_DURATION - FADE_OUT_DURATION;
    Tween mark = make_tween(fade_delay, to_list ? FADE_OUT_DURATION : ROW_FADE_DURATION, apply_mark_fade);
    mark.flag = to_list;
    start_tween(shelf, mark);
    end = max_of(end, fade_delay + ROW_FADE_DURATION);

    // 展开时上面的动画都推迟到窗口变大之后；收起时等牌都回到堆叠里再把窗口变小。
    tween_window_size(shelf, to_list ? 0 : end);
    if (!to_list) end += RESIZE_DURATION;
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

static int pixels(double dip, double scale)
{
    return (int)lround(dip * scale);
}

/// 朝某条屏幕边缘的偏移；不是边缘时往下。
static void toward(ScreenEdge edge, double distance, double scale, int *x, int *y)
{
    *x = *y = 0;
    switch (edge)
    {
        case EDGE_LEFT: *x = -pixels(distance, scale); break;
        case EDGE_RIGHT: *x = pixels(distance, scale); break;
        case EDGE_TOP: *y = -pixels(distance, scale); break;
        default: *y = pixels(distance / 2, scale); break;
    }
}

/// 窗口滑入前的起始偏移：边缘呼出时从屏幕外侧滑入，其余情况从下方升起。
static void open_offset(const ShelfWindow *shelf, double scale, int *x, int *y)
{
    if (shelf->entrance == ENTRANCE_EDGE) toward(shelf->edge, 40, scale, x, y);
    else
    {
        *x = 0;
        *y = pixels(shelf->entrance == ENTRANCE_SHAKE ? 16 : 12, scale);
    }
}

/// 关闭时窗口退回的方向：边缘呼出的退回边缘，其余情况轻微下沉。
static void close_offset(const ShelfWindow *shelf, double scale, int *x, int *y)
{
    if (shelf->entrance == ENTRANCE_EDGE) toward(shelf->edge, 24, scale, x, y);
    else
    {
        *x = 0;
        *y = pixels(10, scale);
    }
}

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

/// 逐帧移动整个窗口。只改变位置不改变大小，系统材质不需要重新计算，移动很流畅。
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

/// 弹出动画：窗口从来源方向滑入，内容同时淡入并放大到原尺寸。晃动呼出时内容从光标处弹出，带一点回弹；从屏幕边缘呼出时从该边缘滑入。
/// 低性能模式下窗口直接出现在原处，内容在窗口里走同样一段路，系统不用每帧重新计算亚克力背景。
void shelf_animate_in(ShelfWindow *shelf, POINT cursor)
{
    shelf_stop_slide(shelf);
    shelf_stop_pop(shelf);
    shelf->leaving = false;
    shelf->surface_dx = shelf->surface_dy = 0;
    shelf->surface_scale = 1;
    if (!anim_enabled())
    {
        shelf->surface_opacity = 1;
        shelf_repaint(shelf);
        return;
    }
    switch (shelf->entrance)
    {
        case ENTRANCE_SHAKE:
            shelf->pop_duration = 0.420;
            shelf->pop_fade = 0.200;
            shelf->pop_start_scale = 0.82;
            shelf->pop_ease = EASE_BACK_OF(0.28);
            break;
        case ENTRANCE_EDGE:
            shelf->pop_duration = 0.380;
            shelf->pop_fade = 0.220;
            shelf->pop_start_scale = 0.96;
            shelf->pop_ease = EASE_OF(EASE_QUINTIC_OUT);
            break;
        default:
            shelf->pop_duration = 0.300;
            shelf->pop_fade = 0.180;
            shelf->pop_start_scale = 0.94;
            shelf->pop_ease = EASE_OF(EASE_QUINTIC_OUT);
            break;
    }
    RECT rect;
    bool low = low_performance_enabled();
    shelf->origin_x = 0.5f;
    shelf->origin_y = 0.4f;
    shelf->pop_shift_x = shelf->pop_shift_y = 0;
    if (shelf->handle && GetWindowRect(shelf->handle, &rect))
    {
        int width = rect.right - rect.left, height = rect.bottom - rect.top;
        // 内容从光标所在的位置展开。
        if (shelf->entrance == ENTRANCE_SHAKE && width > 0 && height > 0)
        {
            shelf->origin_x = clampf((float)(cursor.x - rect.left) / width, 0.15f, 0.85f);
            shelf->origin_y = clampf((float)(cursor.y - rect.top) / height, 0.15f, 0.85f);
        }
        if (!low)
        {
            int dx, dy;
            open_offset(shelf, shelf_scale(shelf), &dx, &dy);
            slide_window(shelf, rect.left + dx, rect.top + dy, rect.left, rect.top, shelf->pop_duration, EASE_OF(EASE_QUINTIC_OUT), true);
        }
    }
    if (low)
    {
        int dx, dy;
        open_offset(shelf, 1, &dx, &dy);
        shelf->pop_shift_x = (float)dx;
        shelf->pop_shift_y = (float)dy;
    }
    // 淡入和放大与滑入用同一种时钟逐帧推进。
    shelf->popping = true;
    clock_reset(&shelf->pop_clock);
    shelf->surface_opacity = 0;
    shelf->surface_scale = (float)shelf->pop_start_scale;
    shelf->surface_dx = shelf->pop_shift_x;
    shelf->surface_dy = shelf->pop_shift_y;
    request_frame(shelf);
}

static void update_pop(ShelfWindow *shelf, double time)
{
    if (!shelf->popping || !clock_tick(&shelf->pop_clock, time)) return;
    double elapsed = shelf->pop_clock.elapsed;
    shelf->surface_opacity = (float)ease_apply(EASE_OF(EASE_CUBIC_OUT), fmin(1, elapsed / shelf->pop_fade));
    double progress = fmin(1, elapsed / shelf->pop_duration);
    shelf->surface_scale = (float)(shelf->pop_start_scale + (1 - shelf->pop_start_scale) * ease_apply(shelf->pop_ease, progress));
    double rest = 1 - ease_apply(EASE_OF(EASE_QUINTIC_OUT), progress);
    shelf->surface_dx = (float)(shelf->pop_shift_x * rest);
    shelf->surface_dy = (float)(shelf->pop_shift_y * rest);
    if (progress < 1) return;
    shelf->popping = false;
    shelf->surface_opacity = 1;
    shelf->surface_scale = 1;
    shelf->surface_dx = shelf->surface_dy = 0;
}

void shelf_start_leave(ShelfWindow *shelf)
{
    // 出场还没播完就关闭：从当前的大小和透明度接着退出。
    shelf_stop_pop(shelf);
    shelf->leaving = true;
    clock_reset(&shelf->leave_clock);
    shelf->leave_from_opacity = shelf->surface_opacity;
    shelf->leave_from_scale = shelf->surface_scale;
    shelf->leave_from_dx = shelf->surface_dx;
    shelf->leave_from_dy = shelf->surface_dy;
    shelf->leave_shift_x = shelf->leave_shift_y = 0;
    // 窗口本身也朝来时的方向退回一小段；低性能模式下窗口不动，内容在窗口里退回。
    if (low_performance_enabled())
    {
        int dx, dy;
        close_offset(shelf, 1, &dx, &dy);
        shelf->leave_shift_x = (float)dx;
        shelf->leave_shift_y = (float)dy;
    }
    else
    {
        shelf->leave_shift_x = shelf->leave_from_dx;
        shelf->leave_shift_y = shelf->leave_from_dy;
        RECT rect;
        if (GetWindowRect(shelf->handle, &rect))
        {
            int dx, dy;
            close_offset(shelf, shelf_scale(shelf), &dx, &dy);
            slide_window(shelf, rect.left, rect.top, rect.left + dx, rect.top + dy, CLOSE_DURATION, EASE_OF(EASE_CUBIC_IN), false);
        }
    }
    request_frame(shelf);
}

static void close_after_leave(void *data)
{
    ShelfWindow *shelf = data;
    shelf_window_close_now(shelf);
    shelf_window_release(shelf);
}

static void update_leave(ShelfWindow *shelf, double time)
{
    if (!shelf->leaving || !clock_tick(&shelf->leave_clock, time)) return;
    double progress = fmin(1, shelf->leave_clock.elapsed / CLOSE_DURATION);
    float eased = (float)ease_apply(EASE_OF(EASE_CUBIC_IN), progress);
    shelf->surface_opacity = shelf->leave_from_opacity * (1 - eased);
    shelf->surface_scale = shelf->leave_from_scale + (CLOSE_SCALE - shelf->leave_from_scale) * eased;
    shelf->surface_dx = shelf->leave_from_dx + (shelf->leave_shift_x - shelf->leave_from_dx) * eased;
    shelf->surface_dy = shelf->leave_from_dy + (shelf->leave_shift_y - shelf->leave_from_dy) * eased;
    if (progress < 1) return;
    shelf->leaving = false;
    // 不在画的过程中销毁窗口，放到消息循环里去关。
    task_post(close_after_leave, shelf_window_retain(shelf));
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
    update_leave(shelf, time);
    update_motion(shelf, time);
    bool running = shelf->sliding || shelf->popping || shelf->leaving || shelf->motion_running;

    if (shelf->badge_popping && clock_tick(&shelf->badge_clock, time))
    {
        double progress = fmin(1, shelf->badge_clock.elapsed / BADGE_POP_DURATION);
        shelf->badge_scale = (float)(1.28 + (1 - 1.28) * ease_apply(EASE_BACK_OF(0.5), progress));
        if (progress >= 1) shelf->badge_popping = false;
    }
    running |= shelf->badge_popping;

    running |= update_anim(&shelf->overlay_opacity, time);
    running |= update_anim(&shelf->body_opacity, time);
    running |= update_anim(&shelf->busy_opacity, time);
    running |= update_anim(&shelf->toast_opacity, time);
    running |= update_anim(&shelf->toast_offset, time);
    // 放置区域淡出后才真正收起。
    if (!shelf->overlay_opacity.running && shelf->overlay_opacity.value <= 0 && shelf->overlay_visible && !shelf->drag_inside)
        shelf->overlay_visible = false;
    for (int index = 0; index < shelf->model->count; index++) running |= update_anim(&shelf->model->items[index]->dim, time);
    if (running) request_frame(shelf);
}
