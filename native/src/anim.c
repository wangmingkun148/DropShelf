#include "anim.h"
#include "settings.h"

#ifdef _WIN32
#include <dwmapi.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ───────────── 缓动 ─────────────

static double cubic(double p1, double p2, double s)
{
    return 3 * p1 * s * (1 - s) * (1 - s) + 3 * p2 * s * s * (1 - s) + s * s * s;
}

/// 两个控制点的这一坐标在 0 到 1 之间时，坐标随参数单调增加：二分求出坐标等于 value 的参数。
static double solve(double p1, double p2, double value)
{
    // 两端直接给出，二分只会逼近，动画的起点和终点要正好落在关键帧上。
    if (value <= 0) return 0;
    if (value >= 1) return 1;
    double low = 0, high = 1;
    for (int index = 0; index < 30; index++)
    {
        double middle = (low + high) / 2;
        if (cubic(p1, p2, middle) < value) low = middle;
        else high = middle;
    }
    return (low + high) / 2;
}

double ease_apply(Ease ease, double t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (ease.kind)
    {
        // 与 WPF 的 SineEase 相同。
        case EASE_SINE_OUT: return sin(t * 1.5707963267948966);
        case EASE_SINE_IN_OUT: return (1 - cos(t * 3.141592653589793)) / 2;
        // 与 WPF 的 CubicEase（EaseInOut）相同。
        case EASE_CUBIC_IN_OUT: return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
        case EASE_CUBIC_IN: return t * t * t;
        case EASE_CUBIC_OUT:
        {
            double s = 1 - t;
            return 1 - s * s * s;
        }
        case EASE_QUINTIC_OUT:
        {
            double s = 1 - t;
            return 1 - s * s * s * s * s;
        }
        case EASE_BACK_OUT:
        {
            // 与 WPF 的 BackEase（EaseOut）相同。
            double s = 1 - t;
            return 1 - (s * s * s - s * ease.amplitude * sin(3.14159265358979323846 * s));
        }
        case EASE_BEZIER: return cubic(ease.y1, ease.y2, solve(ease.x1, ease.x2, t));
        default: return t;
    }
}

double ease_bezier_inverse(Ease ease, double y)
{
    return cubic(ease.x1, ease.x2, solve(ease.y1, ease.y2, y));
}

// ───────────── 动画时钟 ─────────────

double frame_clock_step(double passed, double *lag)
{
    if (passed <= FRAME_MAXIMUM_STEP) return passed;
    double held = passed - FRAME_MAXIMUM_STEP;
    if (held > FRAME_MAXIMUM_LAG - *lag) held = FRAME_MAXIMUM_LAG - *lag;
    *lag += held;
    return passed - held;
}

bool anim_enabled(void)
{
#ifdef _WIN32
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return true;
    return enabled != FALSE;
#else
    // M3 接上 GTK 的 gtk-enable-animations 和系统的“减少动态效果”；在此之前照常播放动画。
    return true;
#endif
}

void anim_set(Anim *anim, double value)
{
    anim->value = anim->from = anim->to = value;
    anim->running = false;
}

void anim_start_delayed(Anim *anim, double to, double duration, double delay, Ease ease)
{
    if (duration <= 0 || !anim_enabled())
    {
        anim_set(anim, to);
        return;
    }
    anim->from = anim->value;
    anim->to = to;
    anim->duration = duration;
    anim->delay = delay;
    anim->ease = ease;
    anim->elapsed = 0;
    anim->lag = 0;
    anim->has_last_time = false;
    anim->running = true;
}

void anim_start(Anim *anim, double to, double duration, Ease ease)
{
    anim_start_delayed(anim, to, duration, 0, ease);
}

bool anim_update(Anim *anim, double time)
{
    if (!anim->running) return false;
    if (anim->has_last_time)
    {
        // 同一帧重复调用不多走；时间比上次还早时（逐帧回调给的是下一次刷新的时刻，窗口消息里现画的一帧用的是现在）也不往回走。
        if (time <= anim->last_time) return true;
        anim->elapsed += frame_clock_step(time - anim->last_time, &anim->lag);
    }
    anim->last_time = time;
    anim->has_last_time = true;

    double progress = (anim->elapsed - anim->delay) / anim->duration;
    if (progress >= 1)
    {
        anim->value = anim->to;
        anim->running = false;
        return false;
    }
    anim->value = anim->from + (anim->to - anim->from) * ease_apply(anim->ease, progress < 0 ? 0 : progress);
    return true;
}

// ───────────── 逐帧回调 ─────────────
// Windows：一个后台线程用 DwmFlush 等屏幕的下一次刷新，然后给界面线程发一条消息；界面线程在这条消息里调用各个订阅者。
// Linux：实现在 native/platform/linux/anim_linux.c，用 GLib 的定时器。

#ifdef _WIN32
// 一个后台线程用 DwmFlush 等屏幕的下一次刷新，然后给界面线程发一条消息；界面线程在这条消息里调用各个订阅者。
// 交给订阅者的时刻是这一帧将要显示的时刻（DWM 报告的上一次刷新加一个刷新周期），不是消息处理时的“现在”：
// 消息有时排在鼠标输入后面晚到几毫秒，按“现在”算的话相邻两帧走的距离忽大忽小，看起来发抖；按显示的时刻算，每帧正好走一个周期。
// 上一条消息处理完之前不再发：发来的消息排在鼠标和键盘之前处理，画一帧比刷新间隔慢时如果马上又来一条，
// 界面线程会一直在画、顾不上鼠标（光标移动很久才有反应）；现在这时跳过一次刷新，中间先处理输入。

#define FRAME_WINDOW_CLASS L"DropShelfFrameWindow"
#define MESSAGE_FRAME (WM_APP + 20)
/// 一条帧消息超过这么久还没处理完（回调里进了模态循环，如弹出菜单），照常再发，模态循环里的动画不停。
#define FRAME_STUCK_MILLISECONDS 100

typedef struct Subscriber
{
    FrameHandler handler;
    void *context;
} Subscriber;

static Subscriber *subscribers;
static int subscriber_count;
static HWND frame_window;
static HANDLE frame_thread;
static HANDLE frame_wake;           // 有订阅时置位，线程才开始等刷新
static volatile LONG frame_pending; // 已经发出、还没处理完的帧消息
static volatile LONG64 frame_posted_at;
static volatile LONG frame_stopping;
static volatile LONG64 frame_target;    // 下一帧将要显示的时刻（性能计数器的读数），读不到时为 0
static volatile LONG64 frame_period;    // 刷新周期（性能计数器的计数），读不到时为 0

static LARGE_INTEGER clock_frequency, clock_origin;

static void clock_start(void)
{
    if (clock_frequency.QuadPart != 0) return;
    QueryPerformanceFrequency(&clock_frequency);
    QueryPerformanceCounter(&clock_origin);
}

/// 性能计数器的读数换成 frames_time 的秒数。
static double clock_seconds(LONG64 counter)
{
    clock_start();
    return (double)(counter - clock_origin.QuadPart) / (double)clock_frequency.QuadPart;
}

double frames_time(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return clock_seconds(now.QuadPart);
}

double frames_refresh_period(void)
{
    LONG64 period = InterlockedCompareExchange64(&frame_period, 0, 0);
    clock_start();
    if (period <= 0) return 1.0 / 60;
    return (double)period / (double)clock_frequency.QuadPart;
}

/// 这一帧用的时刻：DWM 给的显示时刻，在合理的范围内（不早于现在、不晚于现在加两个周期）才用，否则用现在。
static double frame_time_now(void)
{
    double now = frames_time();
    LONG64 target = InterlockedCompareExchange64(&frame_target, 0, 0);
    if (target <= 0) return now;
    double predicted = clock_seconds(target);
    double period = frames_refresh_period();
    return predicted >= now && predicted - now <= 2 * period ? predicted : now;
}

static DWORD WINAPI frame_thread_procedure(LPVOID parameter)
{
    (void)parameter;
    while (!frame_stopping)
    {
        WaitForSingleObject(frame_wake, INFINITE);
        if (frame_stopping) break;
        // 桌面合成不可用时（极少见）DwmFlush 立即返回失败，改为按 60Hz 等待。
        if (FAILED(DwmFlush()))
        {
            Sleep(16);
            InterlockedExchange64(&frame_target, 0);
        }
        else
        {
            // 刚合成完一帧：现在画的内容在下一次刷新时显示。
            DWM_TIMING_INFO timing = { .cbSize = (UINT32)sizeof(timing) };
            if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &timing)) && timing.qpcRefreshPeriod > 0 && timing.qpcVBlank > 0)
            {
                InterlockedExchange64(&frame_period, (LONG64)timing.qpcRefreshPeriod);
                InterlockedExchange64(&frame_target, (LONG64)(timing.qpcVBlank + timing.qpcRefreshPeriod));
            }
            else InterlockedExchange64(&frame_target, 0);
        }
        LONG64 now = (LONG64)GetTickCount64();
        bool stuck = frame_pending && now - frame_posted_at > FRAME_STUCK_MILLISECONDS;
        if (InterlockedCompareExchange(&frame_pending, 1, 0) != 0 && !stuck) continue;
        InterlockedExchange64(&frame_posted_at, now);
        if (!PostMessageW(frame_window, MESSAGE_FRAME, 0, 0)) InterlockedExchange(&frame_pending, 0);
    }
    return 0;
}

static LRESULT CALLBACK frame_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message != MESSAGE_FRAME) return DefWindowProcW(window, message, wparam, lparam);
    int count = subscriber_count;
    if (count > 0)
    {
        // 回调里可能增减订阅，先留一份副本；调用前再确认它还在订阅。
        double time = frame_time_now();
        Subscriber *copy = xalloc((size_t)count * sizeof(Subscriber));
        memcpy(copy, subscribers, (size_t)count * sizeof(Subscriber));
        for (int index = 0; index < count; index++)
        {
            bool still_subscribed = false;
            for (int current = 0; current < subscriber_count && !still_subscribed; current++)
                still_subscribed = subscribers[current].handler == copy[index].handler && subscribers[current].context == copy[index].context;
            if (still_subscribed) copy[index].handler(copy[index].context, time);
        }
        free(copy);
    }
    // 都画完了才允许发下一条。
    InterlockedExchange(&frame_pending, 0);
    return 0;
}

static bool frames_start(void)
{
    if (frame_window) return true;
    // 时钟的起点在界面线程上定好，之后后台线程读到的显示时刻和界面线程用的是同一个起点。
    clock_start();
    WNDCLASSEXW class = { .cbSize = sizeof(class) };
    class.lpfnWndProc = frame_window_procedure;
    class.hInstance = GetModuleHandleW(NULL);
    class.lpszClassName = FRAME_WINDOW_CLASS;
    RegisterClassExW(&class);
    frame_window = CreateWindowExW(0, FRAME_WINDOW_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, class.hInstance, NULL);
    if (!frame_window) return false;
    frame_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    frame_stopping = 0;
    frame_thread = CreateThread(NULL, 0, frame_thread_procedure, NULL, 0, NULL);
    return frame_thread != NULL;
}

void frames_subscribe(FrameHandler handler, void *context)
{
    for (int index = 0; index < subscriber_count; index++)
        if (subscribers[index].handler == handler && subscribers[index].context == context) return;
    if (!frames_start()) return;
    subscribers = xrealloc(subscribers, (size_t)(subscriber_count + 1) * sizeof(Subscriber));
    subscribers[subscriber_count++] = (Subscriber){ handler, context };
    SetEvent(frame_wake);
}

void frames_unsubscribe(FrameHandler handler, void *context)
{
    for (int index = 0; index < subscriber_count; index++)
    {
        if (subscribers[index].handler != handler || subscribers[index].context != context) continue;
        memmove(&subscribers[index], &subscribers[index + 1], (size_t)(subscriber_count - index - 1) * sizeof(Subscriber));
        subscriber_count--;
        break;
    }
    if (subscriber_count == 0 && frame_wake) ResetEvent(frame_wake);
}

void frames_shutdown(void)
{
    if (frame_thread)
    {
        InterlockedExchange(&frame_stopping, 1);
        SetEvent(frame_wake);
        WaitForSingleObject(frame_thread, 500);
        CloseHandle(frame_thread);
        frame_thread = NULL;
    }
    if (frame_wake) CloseHandle(frame_wake);
    frame_wake = NULL;
    if (frame_window) DestroyWindow(frame_window);
    frame_window = NULL;
    free(subscribers);
    subscribers = NULL;
    subscriber_count = 0;
}

#endif // _WIN32

// ───────────── 低性能模式 ─────────────

/// 刷新率不低于这个值时动画隔一帧更新，例如 165Hz 降到 82 帧；更低的刷新率照常每帧更新。
#define PACED_REFRESH 100
#define POWER_CHECK_INTERVAL 2000

static int performance_mode = PERFORMANCE_AUTO;

void low_performance_set_mode(int mode)
{
    performance_mode = mode;
}

int low_performance_mode(void)
{
    return performance_mode;
}

/// 使用电池或系统开启了节能模式。逐帧回调里也会问到，电源状态几秒内不会来回变，两秒内不重复查。
static bool saving_power(void)
{
#ifdef _WIN32
    static ULONGLONG checked;
    static bool has_checked, saving;
    ULONGLONG now = GetTickCount64();
    if (has_checked && now - checked < POWER_CHECK_INTERVAL) return saving;
    checked = now;
    has_checked = true;
    SYSTEM_POWER_STATUS status;
    saving = GetSystemPowerStatus(&status) && (status.ACLineStatus == 0 || status.SystemStatusFlag == 1);
    return saving;
#else
    return anim_linux_saving_power();
#endif
}

bool low_performance_enabled(void)
{
    if (performance_mode == PERFORMANCE_LOW) return true;
    if (performance_mode == PERFORMANCE_NORMAL) return false;
    return saving_power();
}

int display_refresh_rate(void)
{
#ifdef _WIN32
    HDC screen = GetDC(NULL);
    if (!screen) return 0;
    int refresh = GetDeviceCaps(screen, VREFRESH);
    ReleaseDC(NULL, screen);
    return refresh;
#else
    // GLib 的逐帧回调按显示器自己的节奏来，这里只用于低性能模式隔帧的判断；先按 60Hz。
    return 60;
#endif
}

bool low_performance_skip(double time)
{
    static double frame = -1, shown, interval;
    static bool skip;
    if (!low_performance_enabled()) return false;
    if (time == frame) return skip;
    // 隔了一阵才来新的一帧，是新一段动画开始：重新读一次刷新率（换了显示器也跟着变），这一帧照常更新。
    if (time < frame || time - frame > 0.1)
    {
        int refresh = display_refresh_rate();
        interval = refresh >= PACED_REFRESH ? 1.5 / refresh : 0;
        shown = time;
        skip = false;
    }
    else
    {
        // 距上次更新不到 1.5 帧就跳过，于是正好隔一帧更新；偶尔掉一帧时下一帧马上补上。
        skip = time - shown < interval;
        if (!skip) shown = time;
    }
    frame = time;
    return skip;
}
