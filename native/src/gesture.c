#include "gesture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int shake_sensitivity_clamp(int value)
{
    return value < SHAKE_STEADY ? SHAKE_STEADY : value > SHAKE_RESPONSIVE ? SHAKE_RESPONSIVE : value;
}

// ───────────── 按时间排列的记录 ─────────────

static void queue_push(TimeQueue *queue, long long time, double distance)
{
    if (queue->count == queue->capacity)
    {
        int capacity = queue->capacity ? queue->capacity * 2 : 64;
        TimeEntry *items = xalloc((size_t)capacity * sizeof(TimeEntry));
        for (int index = 0; index < queue->count; index++)
            items[index] = queue->items[(queue->head + index) % queue->capacity];
        free(queue->items);
        queue->items = items;
        queue->head = 0;
        queue->capacity = capacity;
    }
    queue->items[(queue->head + queue->count) % queue->capacity] = (TimeEntry){ time, distance };
    queue->count++;
}

static const TimeEntry *queue_peek(const TimeQueue *queue)
{
    return queue->count > 0 ? &queue->items[queue->head] : NULL;
}

static TimeEntry queue_pop(TimeQueue *queue)
{
    TimeEntry entry = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    return entry;
}

static void queue_clear(TimeQueue *queue)
{
    queue->head = 0;
    queue->count = 0;
}

// ───────────── 灵敏度 ─────────────

static const GestureProfile steady_profile = { 88, 700, 6, 380, 1400 };
static const GestureProfile balanced_profile = { 34, 153, 3, 180, 800 };
static const GestureProfile responsive_profile = { 15, 70, 2, 100, 850 };

/// 正好晃 required_reversals + 1 段时，每段平均要多远。
static double profile_amplitude(const GestureProfile *profile)
{
    return profile->minimum_total_travel / (profile->required_reversals + 1);
}

/// 正好晃这么多段时，每段最多可以用多久。
static double profile_segment_time(const GestureProfile *profile)
{
    return profile->maximum_duration_milliseconds / (profile->required_reversals + 1.0);
}

static double mix(double from, double to, double t)
{
    return from + (to - from) * t;
}

static void profile_range(int sensitivity, const GestureProfile **from, const GestureProfile **to, double *t)
{
    double position = shake_sensitivity_clamp(sensitivity) / 100.0;
    if (position <= 0.5)
    {
        *from = &steady_profile;
        *to = &balanced_profile;
        *t = position * 2;
    }
    else
    {
        *from = &balanced_profile;
        *to = &responsive_profile;
        *t = position * 2 - 1;
    }
}

/// 0–50 在稳健与均衡之间、50–100 在均衡与灵敏之间按比例取值，掉头次数和每段幅度都连续变化。
/// 掉头次数不是整数时，取整后少掉的部分换成总距离：例如 3.5 次，要么掉头 4 次，要么掉头 3 次但每段晃得更远。
/// 取整和 C# 的 Math.Round 一样，正好一半时取偶数（rint）。
GestureProfile gesture_profile_for(int sensitivity)
{
    const GestureProfile *from, *to;
    double t;
    profile_range(sensitivity, &from, &to, &t);
    double reversals = mix(from->required_reversals, to->required_reversals, t);
    GestureProfile profile;
    profile.minimum_segment_distance = mix(from->minimum_segment_distance, to->minimum_segment_distance, t);
    profile.minimum_total_travel = (reversals + 1) * mix(profile_amplitude(from), profile_amplitude(to), t);
    profile.required_reversals = (int)floor(reversals + 1e-6);
    profile.minimum_duration_milliseconds = (int)rint(mix(from->minimum_duration_milliseconds, to->minimum_duration_milliseconds, t));
    profile.maximum_duration_milliseconds = (int)rint((reversals + 1) * mix(profile_segment_time(from), profile_segment_time(to), t));
    return profile;
}

void shake_describe(int sensitivity, int *reversals, int *distance)
{
    const GestureProfile *from, *to;
    double t;
    profile_range(sensitivity, &from, &to, &t);
    *reversals = (int)ceil(mix(from->required_reversals, to->required_reversals, t) - 1e-6);
    *distance = (int)rint(mix(profile_amplitude(from), profile_amplitude(to), t));
}

// ───────────── 晃动 ─────────────

void shake_detector_init(ShakeDetector *detector, int sensitivity)
{
    memset(detector, 0, sizeof(*detector));
    detector->profile = gesture_profile_for(sensitivity);
}

void shake_detector_free(ShakeDetector *detector)
{
    free(detector->reversals.items);
    free(detector->travel.items);
    memset(detector, 0, sizeof(*detector));
}

static void clear_window(ShakeDetector *detector)
{
    queue_clear(&detector->reversals);
    queue_clear(&detector->travel);
    detector->total_travel = 0;
}

void shake_detector_reset(ShakeDetector *detector)
{
    detector->has_last_point = false;
    detector->last_x = 0;
    detector->last_y = 0;
    detector->last_time = 0;
    detector->direction_x = 0;
    detector->direction_y = 0;
    detector->segment_distance = 0;
    detector->consecutive_edge_samples = 0;
    detector->tolerated_side_step = false;
    detector->has_raw_point = false;
    clear_window(detector);
}

void shake_detector_set_sensitivity(ShakeDetector *detector, int sensitivity)
{
    detector->profile = gesture_profile_for(sensitivity);
    shake_detector_reset(detector);
}

static void set_last_point(ShakeDetector *detector, int x, int y, long long time)
{
    detector->last_x = x;
    detector->last_y = y;
    detector->last_time = time;
    detector->has_last_point = true;
}

static void reset_motion_keeping_last_point(ShakeDetector *detector)
{
    detector->tolerated_side_step = false;
    detector->direction_x = 0;
    detector->direction_y = 0;
    detector->segment_distance = 0;
    clear_window(detector);
}

static void trim(ShakeDetector *detector, long long now)
{
    long long cutoff = now - detector->profile.maximum_duration_milliseconds;
    const TimeEntry *entry;
    while ((entry = queue_peek(&detector->reversals)) && entry->time < cutoff) queue_pop(&detector->reversals);
    while ((entry = queue_peek(&detector->travel)) && entry->time < cutoff) detector->total_travel -= queue_pop(&detector->travel).distance;
    // 队列空了就从零算起，加减留下的舍入误差不会一直带下去。
    if (detector->travel.count == 0) detector->total_travel = 0;
}

static void add_travel(ShakeDetector *detector, long long time, double distance)
{
    queue_push(&detector->travel, time, distance);
    detector->total_travel += distance;
    trim(detector, time);
}

bool shake_detector_add_raw_sample(ShakeDetector *detector, int x, int y, long long milliseconds, bool near_outer_screen_edge)
{
    if (detector->has_raw_point)
    {
        int delta_x = x - detector->raw_x;
        int delta_y = y - detector->raw_y;
        if (delta_x * delta_x + delta_y * delta_y < 36 && milliseconds - detector->raw_time < 30) return false;
    }

    detector->raw_x = x;
    detector->raw_y = y;
    detector->raw_time = milliseconds;
    detector->has_raw_point = true;
    return shake_detector_add_sample(detector, x, y, milliseconds, near_outer_screen_edge);
}

bool shake_detector_add_sample(ShakeDetector *detector, int x, int y, long long milliseconds, bool near_outer_screen_edge)
{
    if (!detector->has_last_point)
    {
        set_last_point(detector, x, y, milliseconds);
        return false;
    }

    long long elapsed = milliseconds - detector->last_time;
    int delta_x = x - detector->last_x;
    int delta_y = y - detector->last_y;
    set_last_point(detector, x, y, milliseconds);

    if (near_outer_screen_edge)
    {
        detector->consecutive_edge_samples++;
        if (detector->consecutive_edge_samples >= 2) reset_motion_keeping_last_point(detector);
        return false;
    }
    detector->consecutive_edge_samples = 0;

    // 高回报率鼠标的两次采样可能落在同一毫秒，时间差为 0 属于正常情况。
    if (elapsed < 0 || elapsed > 500)
    {
        reset_motion_keeping_last_point(detector);
        return false;
    }

    double distance = sqrt((double)(delta_x * delta_x + delta_y * delta_y));
    if (distance < 2) return false;

    double new_direction_x = delta_x / distance;
    double new_direction_y = delta_y / distance;
    if (detector->direction_x == 0 && detector->direction_y == 0)
    {
        add_travel(detector, milliseconds, distance);
        detector->direction_x = new_direction_x;
        detector->direction_y = new_direction_y;
        detector->segment_distance = distance;
        return false;
    }

    double alignment = new_direction_x * detector->direction_x + new_direction_y * detector->direction_y;
    if (alignment > -0.18 && alignment < 0.18 && !detector->tolerated_side_step &&
        distance < detector->profile.minimum_segment_distance * 0.6)
    {
        // 掉头瞬间常有一次细小的侧向位移，容忍一次；连续的侧向移动（如绕圈）仍按普通移动处理。
        detector->tolerated_side_step = true;
        return false;
    }
    detector->tolerated_side_step = false;

    if (alignment <= -0.18)
    {
        add_travel(detector, milliseconds, distance);
        if (detector->segment_distance >= detector->profile.minimum_segment_distance)
            queue_push(&detector->reversals, milliseconds, 0);

        detector->direction_x = new_direction_x;
        detector->direction_y = new_direction_y;
        detector->segment_distance = distance;
    }
    else if (alignment >= 0.18)
    {
        add_travel(detector, milliseconds, distance);
        detector->segment_distance += distance;
    }
    else
    {
        // 大角度转弯视为普通移动，不计入来回晃动。
        reset_motion_keeping_last_point(detector);
        add_travel(detector, milliseconds, distance);
        detector->direction_x = new_direction_x;
        detector->direction_y = new_direction_y;
        detector->segment_distance = distance;
        return false;
    }

    // 两个分支都刚经过 add_travel，超出时间窗口的记录已经去掉。
    const TimeEntry *first = queue_peek(&detector->travel);
    long long gesture_duration = first ? milliseconds - first->time : 0;
    if (detector->reversals.count < detector->profile.required_reversals ||
        detector->total_travel < detector->profile.minimum_total_travel ||
        gesture_duration < detector->profile.minimum_duration_milliseconds)
        return false;

    shake_detector_reset(detector);
    return true;
}

// ───────────── 屏幕边缘停留 ─────────────

void edge_dwell_init(EdgeDwellDetector *detector, int dwell_milliseconds)
{
    memset(detector, 0, sizeof(*detector));
    detector->dwell_milliseconds = dwell_milliseconds;
}

void edge_dwell_reset(EdgeDwellDetector *detector)
{
    detector->edge = EDGE_NONE;
    detector->since = 0;
    detector->anchor = 0;
}

bool edge_dwell_update(EdgeDwellDetector *detector, ScreenEdge edge, int x, int y, long long milliseconds)
{
    if (edge == EDGE_NONE)
    {
        edge_dwell_reset(detector);
        return false;
    }

    int along = edge == EDGE_TOP ? x : y;
    if (edge != detector->edge || abs(along - detector->anchor) > EDGE_MAXIMUM_DRIFT)
    {
        detector->edge = edge;
        detector->since = milliseconds;
        detector->anchor = along;
        return false;
    }

    if (milliseconds - detector->since < detector->dwell_milliseconds) return false;
    edge_dwell_reset(detector);
    return true;
}

// ───────────── 按示范定灵敏度 ─────────────
// 把每次示范原样交给识别器，看它最稳健能在哪一档触发，取几次里最吃力的那次再往灵敏方向留一点余量。
// 用的就是平时识别晃动的那套判断，结果和实际使用一致。

bool shake_calibration_triggers(const ShakeSample *stroke, int count, int sensitivity)
{
    ShakeDetector detector;
    shake_detector_init(&detector, sensitivity);
    bool triggered = false;
    for (int index = 0; index < count && !triggered; index++)
        triggered = shake_detector_add_raw_sample(&detector, stroke[index].x, stroke[index].y, stroke[index].time, false);
    shake_detector_free(&detector);
    return triggered;
}

/// 从最灵敏往稳健方向找，到第一个触发不了的为止。
int shake_calibration_steadiest_level(const ShakeSample *stroke, int count)
{
    int steadiest = -1;
    for (int level = SHAKE_RESPONSIVE; level >= SHAKE_STEADY; level--)
    {
        if (!shake_calibration_triggers(stroke, count, level)) break;
        steadiest = level;
    }
    return steadiest;
}

int shake_calibration_recommend(const int *steadiest_levels, int count)
{
    int maximum = steadiest_levels[0];
    for (int index = 1; index < count; index++)
        if (steadiest_levels[index] > maximum) maximum = steadiest_levels[index];
    return shake_sensitivity_clamp(maximum + CALIBRATION_MARGIN);
}
