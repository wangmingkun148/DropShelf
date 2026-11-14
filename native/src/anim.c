#include "anim.h"
#include "settings.h"

#include <dwmapi.h>
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
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return true;
    return enabled != FALSE;
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
        if (time == anim->last_time) return true;
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
// 一个后台线程用 DwmFlush 等屏幕的下一次刷新，然后给界面线程发一条消息；界面线程在这条消息里调用各个订阅者。
// 上一条消息还没处理完时不再发，界面线程忙的时候不会积压。

#define FRAME_WINDOW_CLASS L"DropShelfFrameWindow"
#define MESSAGE_FRAME (WM_APP + 20)

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
static volatile LONG frame_pending; // 已经发出、还没处理的帧消息
static volatile LONG frame_stopping;

double frames_time(void)
{
    static LARGE_INTEGER frequency, origin;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        origin = now;
    }
    return (double)(now.QuadPart - origin.QuadPart) / (double)frequency.QuadPart;
}

static DWORD WINAPI frame_thread_procedure(LPVOID parameter)
{
    (void)parameter;
    while (!frame_stopping)
    {
        WaitForSingleObject(frame_wake, INFINITE);
        if (frame_stopping) break;
        // 桌面合成不可用时（极少见）DwmFlush 立即返回失败，改为按 60Hz 等待。
        if (FAILED(DwmFlush())) Sleep(16);
        if (InterlockedCompareExchange(&frame_pending, 1, 0) == 0 && !PostMessageW(frame_window, MESSAGE_FRAME, 0, 0))
            InterlockedExchange(&frame_pending, 0);
    }
    return 0;
}

static LRESULT CALLBACK frame_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message != MESSAGE_FRAME) return DefWindowProcW(window, message, wparam, lparam);
    InterlockedExchange(&frame_pending, 0);
    int count = subscriber_count;
    if (count == 0) return 0;
    // 回调里可能增减订阅，先留一份副本；调用前再确认它还在订阅。
    double time = frames_time();
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
    return 0;
}

static bool frames_start(void)
{
    if (frame_window) return true;
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
    static ULONGLONG checked;
    static bool has_checked, saving;
    ULONGLONG now = GetTickCount64();
    if (has_checked && now - checked < POWER_CHECK_INTERVAL) return saving;
    checked = now;
    has_checked = true;
    SYSTEM_POWER_STATUS status;
    saving = GetSystemPowerStatus(&status) && (status.ACLineStatus == 0 || status.SystemStatusFlag == 1);
    return saving;
}

bool low_performance_enabled(void)
{
    if (performance_mode == PERFORMANCE_LOW) return true;
    if (performance_mode == PERFORMANCE_NORMAL) return false;
    return saving_power();
}

int display_refresh_rate(void)
{
    HDC screen = GetDC(NULL);
    if (!screen) return 0;
    int refresh = GetDeviceCaps(screen, VREFRESH);
    ReleaseDC(NULL, screen);
    return refresh;
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
