// 手势识别：按住鼠标时的晃动和拖到屏幕边缘后的停留。这里只有判断逻辑，不碰系统，便于测试；
// 监听鼠标的后台线程在 gesture_service.c。与 C# 版的 MouseGestureDetector、EdgeDwellDetector、ShakeCalibration 逐行对应。
#pragma once

#include "base.h"
#include "settings.h"

typedef enum ScreenEdge { EDGE_NONE, EDGE_LEFT, EDGE_TOP, EDGE_RIGHT } ScreenEdge;

/// 呼出方式和停留时间的换算（auto_open_uses_shake 等）在 settings.h。
int shake_sensitivity_clamp(int value);

// ───────────── 晃动 ─────────────

typedef struct GestureProfile
{
    double minimum_segment_distance;
    double minimum_total_travel;
    int required_reversals;
    int minimum_duration_milliseconds;
    int maximum_duration_milliseconds;
} GestureProfile;

/// 按时间先后排列的记录，从头部取出过期的，从尾部加入新的。
typedef struct TimeEntry
{
    long long time;
    double distance;
} TimeEntry;

typedef struct TimeQueue
{
    TimeEntry *items;
    int head;
    int count;
    int capacity;
} TimeQueue;

typedef struct ShakeDetector
{
    TimeQueue reversals;
    TimeQueue travel;
    double total_travel;            // travel 里各段距离之和，随入队、出队增减
    GestureProfile profile;
    bool has_last_point;
    int last_x;
    int last_y;
    long long last_time;
    double direction_x;
    double direction_y;
    double segment_distance;
    int consecutive_edge_samples;
    bool tolerated_side_step;
    bool has_raw_point;
    int raw_x;
    int raw_y;
    long long raw_time;
} ShakeDetector;

/// sensitivity 是 0–100，见 SHAKE_STEADY 等。
void shake_detector_init(ShakeDetector *detector, int sensitivity);
void shake_detector_free(ShakeDetector *detector);
void shake_detector_set_sensitivity(ShakeDetector *detector, int sensitivity);
void shake_detector_reset(ShakeDetector *detector);

/// 接收原始鼠标事件（可能高达 1000 次/秒），按位移或时间重新取样后再判断，
/// 避免高回报率鼠标的细碎位移让方向判断失真。识别到晃动时返回 true。
bool shake_detector_add_raw_sample(ShakeDetector *detector, int x, int y, long long milliseconds, bool near_outer_screen_edge);
bool shake_detector_add_sample(ShakeDetector *detector, int x, int y, long long milliseconds, bool near_outer_screen_edge);

GestureProfile gesture_profile_for(int sensitivity);

/// 用于设置页的说明：每段晃这么远（像素）、掉头这么多次一定能触发。
void shake_describe(int sensitivity, int *reversals, int *distance);

// ───────────── 屏幕边缘停留 ─────────────

/// 沿边缘移动超过这个距离（屏幕像素）就重新计时。
#define EDGE_MAXIMUM_DRIFT 80

typedef struct EdgeDwellDetector
{
    int dwell_milliseconds;
    ScreenEdge edge;
    long long since;
    int anchor;
} EdgeDwellDetector;

void edge_dwell_init(EdgeDwellDetector *detector, int dwell_milliseconds);

/// 报告光标的最新位置；在同一边缘停留满设定时间时返回 true（之后需要重新进入边缘才会再次触发）。
/// 是否位于边缘由调用方判断，这里只负责计时。
bool edge_dwell_update(EdgeDwellDetector *detector, ScreenEdge edge, int x, int y, long long milliseconds);
void edge_dwell_reset(EdgeDwellDetector *detector);

// ───────────── 按示范定灵敏度 ─────────────

/// 示范时记下的一个鼠标位置（屏幕像素）和时刻（毫秒）。
typedef struct ShakeSample
{
    int x;
    int y;
    long long time;
} ShakeSample;

#define CALIBRATION_DEMONSTRATIONS 3
/// 建议值比“刚好能触发”再灵敏这么多：平时拖着文件晃，不会每次都和示范时一样用力。
#define CALIBRATION_MARGIN 10

/// 这次示范在这个灵敏度下能不能触发。
bool shake_calibration_triggers(const ShakeSample *stroke, int count, int sensitivity);

/// 这次示范最稳健能在哪个灵敏度触发；晃得太轻、最灵敏也触发不了时返回 -1。
int shake_calibration_steadiest_level(const ShakeSample *stroke, int count);

/// 几次示范都能触发的最稳健的灵敏度，再加上余量。
int shake_calibration_recommend(const int *steadiest_levels, int count);
