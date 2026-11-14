// 动画：逐帧回调、动画时钟、缓动，以及低性能模式。
#pragma once

#include "base.h"

// ───────────── 缓动 ─────────────
// 都是把进度 t（0–1）换成完成的比例；回弹的缓动中途会超过 1。

typedef enum EaseKind
{
    EASE_LINEAR,
    EASE_CUBIC_OUT,
    EASE_QUINTIC_OUT,
    EASE_BACK_OUT,      // 略带回弹，amplitude 越大弹得越远
    EASE_BEZIER         // 与 CSS 的 cubic-bezier 相同
} EaseKind;

typedef struct Ease
{
    EaseKind kind;
    double x1, y1, x2, y2;      // EASE_BEZIER 的两个控制点
    double amplitude;           // EASE_BACK_OUT
} Ease;

#define EASE_OF(kind) ((Ease){ (kind), 0, 0, 0, 0, 0 })
#define EASE_BEZIER_OF(x1, y1, x2, y2) ((Ease){ EASE_BEZIER, (x1), (y1), (x2), (y2), 0 })
#define EASE_BACK_OF(amplitude) ((Ease){ EASE_BACK_OUT, 0, 0, 0, 0, (amplitude) })

/// 界面上通用的缓动：开头快、收尾慢，cubic-bezier(0.2, 0, 0, 1)。
#define EASE_STANDARD EASE_BEZIER_OF(0.2, 0, 0, 1)

double ease_apply(Ease ease, double t);

/// 贝塞尔缓动反过来由完成的比例求进度。
double ease_bezier_inverse(Ease ease, double y);

// ───────────── 动画时钟 ─────────────
// 从第一帧画出来开始计时，一帧最多前进 FRAME_MAXIMUM_STEP。
// 按真实时间推进时，某一帧耽搁了（刚显示窗口、处理器被压到低频），下一帧画面就直接跳过一截；
// 这样改为动画略微拖长、轨迹保持连续。帧率不低于每秒 30 帧时和真实时间一致。
// 整段动画最多因此拖长 FRAME_MAXIMUM_LAG：偶尔的一两帧耽搁能接住，一直跑不满帧时不会越拖越长变成慢动作。

#define FRAME_MAXIMUM_STEP 0.034    // 60Hz 下两帧的时间（秒）
#define FRAME_MAXIMUM_LAG 0.100     // 一段动画累计最多比真实时间慢这么多（秒）

/// 过了 passed 秒的一帧动画前进多少；lag 是这段动画已经拖长的时间，随之累加。
double frame_clock_step(double passed, double *lag);

/// 一个随时间从 from 变到 to 的数值。用法：anim_start 之后每帧调用 anim_update，读 value；running 为 false 时已经到位。
typedef struct Anim
{
    double value;
    double from, to;
    double duration;        // 秒
    double delay;           // 秒
    Ease ease;
    bool running;
    double elapsed;
    double lag;
    double last_time;
    bool has_last_time;
} Anim;

/// 立即到位，不播放动画。
void anim_set(Anim *anim, double value);

/// 从当前的值变到 to。用户在 Windows 中关闭了动画效果时直接到位。
void anim_start(Anim *anim, double to, double duration, Ease ease);
void anim_start_delayed(Anim *anim, double to, double duration, double delay, Ease ease);

/// 按这一帧的时刻（frames_time 给的）推进；同一帧重复调用不会多走。返回是否还在进行。
bool anim_update(Anim *anim, double time);

/// 用户在 Windows 中关闭了动画效果时返回 false，动画都直接到位。
bool anim_enabled(void);

// ───────────── 逐帧回调 ─────────────
// 有订阅时每次屏幕刷新在界面线程调用一次，没有订阅时不占用处理器。

typedef void (*FrameHandler)(void *context, double time);

void frames_subscribe(FrameHandler handler, void *context);
void frames_unsubscribe(FrameHandler handler, void *context);

/// 当前的时刻（秒），与逐帧回调收到的 time 同一个起点。
double frames_time(void);

/// 程序退出前调用：停掉等待屏幕刷新的线程。
void frames_shutdown(void);

// ───────────── 低性能模式 ─────────────
// 在动画效果上做几处不显眼的让步，减轻显卡负担：高刷新率屏幕上动画隔一帧更新；窗口不用半透明的系统材质。

void low_performance_set_mode(int mode);    // PerformanceMode
int low_performance_mode(void);

/// 现在是否启用。自动时在使用电池或系统开启节能模式期间启用，每次用到时现查（两秒内不重复查）。
bool low_performance_enabled(void);

/// 逐帧回调里调用：这一帧要不要跳过不更新。各窗口在同一帧拿到同样的时刻，都跳过同一帧。跟着光标的动画不用它。
bool low_performance_skip(double time);

/// 主显示器的刷新率；读不到时为 0。
int display_refresh_rate(void);
