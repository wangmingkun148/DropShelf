#include "gesture_service.h"
#include "log.h"
#include "power.h"

#include <stdlib.h>

/// 光标停着不动时没有鼠标输入，按住期间用计时器定期检查是否停在边缘。
#define EDGE_POLL_MILLISECONDS 40
#define EDGE_TOLERANCE 1
/// 没按住鼠标时，处理完手上的输入后休息这么久再处理下一批。
#define IDLE_BATCH_MILLISECONDS 8

_Static_assert(sizeof(LPARAM) == 8, "光标位置按两个 32 位数放进 lparam");

static HWND notify_window;
static UINT notify_message;
static HANDLE thread;
static DWORD thread_id;
static HANDLE started;
static HWND input_window;
static volatile bool listening;
static volatile bool shake_enabled = true;
static volatile bool edge_enabled;
static volatile bool paused;
static volatile LONG requested_sensitivity;
static volatile LONG requested_dwell;

// 以下只在监听线程里使用。
static int applied_sensitivity;
static ShakeDetector detector;
static EdgeDwellDetector edge_detector;
static bool left_down;
static bool right_down;
static bool triggered;
static long long last_button_check;
static int moves_this_press;
static UINT_PTR edge_timer;
static bool has_monitor;
static RECT monitor_bounds;
static RECT monitor_work;
/// 所有显示器合起来的范围，每次按下后第一次用到时读取。
static bool has_virtual_screen;
static RECT virtual_screen;

GestureEvent gesture_message_event(WPARAM wparam)
{
    return (GestureEvent)(wparam & 0xFF);
}

ScreenEdge gesture_message_edge(WPARAM wparam)
{
    return (ScreenEdge)((wparam >> 8) & 0xFF);
}

POINT gesture_message_point(LPARAM lparam)
{
    unsigned long long value = (unsigned long long)lparam;
    return (POINT){ (LONG)(int)(unsigned)(value & 0xFFFFFFFFu), (LONG)(int)(unsigned)(value >> 32) };
}

static void notify(GestureEvent event, ScreenEdge edge, POINT point)
{
    unsigned long long packed = ((unsigned long long)(unsigned)point.y << 32) | (unsigned)point.x;
    PostMessageW(notify_window, notify_message, (WPARAM)event | ((WPARAM)edge << 8), (LPARAM)packed);
}

/// 高精度毫秒时间。GetTickCount64 的精度只有约 15.6 毫秒，
/// 会让高回报率鼠标的多次采样拿到相同时间，导致晃动无法识别。
static long long now_milliseconds(void)
{
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart * 1000 / frequency.QuadPart;
}

static bool is_button_physically_down(void)
{
    return GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0;
}

static bool contains(const RECT *rect, POINT point)
{
    return point.x >= rect->left && point.x < rect->right && point.y >= rect->top && point.y < rect->bottom;
}

// ───────────── 屏幕边缘 ─────────────

static void start_edge_timer(void)
{
    if (!edge_timer) edge_timer = SetTimer(NULL, 0, EDGE_POLL_MILLISECONDS, NULL);
}

static void stop_edge_timer(void)
{
    if (!edge_timer) return;
    KillTimer(NULL, edge_timer);
    edge_timer = 0;
}

static bool refresh_monitor(POINT point)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    MONITORINFO info = { .cbSize = sizeof(info) };
    has_monitor = monitor && GetMonitorInfoW(monitor, &info);
    if (!has_monitor) return false;
    monitor_bounds = info.rcMonitor;
    monitor_work = info.rcWork;
    return true;
}

static bool is_outside_all_monitors(POINT point)
{
    return MonitorFromPoint(point, MONITOR_DEFAULTTONULL) == NULL;
}

/// 光标是否贴在屏幕的外侧边缘：左、右、上三边，且边缘外没有其他显示器。
/// 任务栏所在位置（工作区以外）和底边不算，避免与拖到任务栏切换窗口冲突。
static ScreenEdge hit_test_edge(POINT point)
{
    if (!has_monitor || !contains(&monitor_bounds, point))
    {
        if (!refresh_monitor(point)) return EDGE_NONE;
    }

    RECT bounds = monitor_bounds;
    bool near_left = point.x <= bounds.left + EDGE_TOLERANCE;
    bool near_right = point.x >= bounds.right - 1 - EDGE_TOLERANCE;
    bool near_top = point.y <= bounds.top + EDGE_TOLERANCE;
    if (!near_left && !near_right && !near_top) return EDGE_NONE;
    if (!contains(&monitor_work, point)) return EDGE_NONE;

    if (near_left && is_outside_all_monitors((POINT){ bounds.left - 1, point.y })) return EDGE_LEFT;
    if (near_right && is_outside_all_monitors((POINT){ bounds.right, point.y })) return EDGE_RIGHT;
    if (near_top && is_outside_all_monitors((POINT){ point.x, bounds.top - 1 })) return EDGE_TOP;
    return EDGE_NONE;
}

/// 检查边缘停留；触发时返回 true。
static bool check_edge(POINT point, long long now)
{
    edge_detector.dwell_milliseconds = requested_dwell;
    ScreenEdge edge = hit_test_edge(point);
    if (!edge_dwell_update(&edge_detector, edge, point.x, point.y, now)) return false;

    triggered = true;
    stop_edge_timer();
    notify(GESTURE_EDGE_DWELL, edge, point);
    return true;
}

/// 按住期间每条输入都要判断，屏幕范围一次按下只向系统查询一次。
static bool is_near_outer_screen_edge(POINT point)
{
    const int margin = 5;
    if (!has_virtual_screen)
    {
        int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtual_screen = (RECT){ left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        has_virtual_screen = true;
    }
    return point.x <= virtual_screen.left + margin || point.y <= virtual_screen.top + margin ||
           point.x >= virtual_screen.right - margin || point.y >= virtual_screen.bottom - margin;
}

// ───────────── 按下和松开 ─────────────

static void begin_press(void)
{
    power_follow_process();
    power_hold(&detector, true);
    moves_this_press = 0;
    triggered = false;
    last_button_check = now_milliseconds();
    shake_detector_reset(&detector);
    edge_dwell_reset(&edge_detector);
    // 每次按下时重新读取显示器信息，显示器布局变化后也能正确判断边缘。
    has_monitor = false;
    has_virtual_screen = false;
    if (edge_enabled && !paused) start_edge_timer();
}

static void end_press(void)
{
    power_hold(&detector, false);
    power_background_thread();
    if (log_trace_enabled() && moves_this_press > 20)
        log_trace(L"按住期间收到 %d 次移动，%ls", moves_this_press, triggered ? L"已识别手势" : L"未识别手势");
    bool released = triggered;
    triggered = false;
    shake_detector_reset(&detector);
    edge_dwell_reset(&edge_detector);
    stop_edge_timer();
    if (released) notify(GESTURE_RELEASED, EDGE_NONE, (POINT){0});
}

static void press(bool left)
{
    if (!left_down && !right_down) begin_press();
    if (left) left_down = true;
    else right_down = true;
}

static void release(bool left)
{
    // 计时器发现按键已松开时会提前结束本次按住，之后到达的松开输入直接忽略。
    if (!left_down && !right_down) return;
    if (left) left_down = false;
    else right_down = false;
    if (!left_down && !right_down) end_press();
}

static void apply_sensitivity_if_changed(void)
{
    int requested = requested_sensitivity;
    if (requested == applied_sensitivity) return;
    applied_sensitivity = requested;
    shake_detector_set_sensitivity(&detector, requested);
}

static void on_move(void)
{
    if (!left_down && !right_down) return;
    moves_this_press++;
    long long now = now_milliseconds();
    if (now - last_button_check > 250)
    {
        // 遇到安全桌面等情况可能收不到松开的输入，定期用实际按键状态校正。
        last_button_check = now;
        if (!is_button_physically_down())
        {
            left_down = right_down = false;
            end_press();
            return;
        }
    }

    POINT point;
    if (triggered || paused || !GetCursorPos(&point)) return;
    if (edge_enabled && check_edge(point, now)) return;
    if (!shake_enabled) return;
    apply_sensitivity_if_changed();
    if (!shake_detector_add_raw_sample(&detector, point.x, point.y, now, is_near_outer_screen_edge(point))) return;

    triggered = true;
    stop_edge_timer();
    notify(GESTURE_SHAKE, EDGE_NONE, point);
}

static void on_edge_timer(void)
{
    if (!edge_enabled || triggered || paused || (!left_down && !right_down))
    {
        stop_edge_timer();
        return;
    }
    if (!is_button_physically_down())
    {
        left_down = right_down = false;
        end_press();
        return;
    }
    POINT point;
    if (GetCursorPos(&point)) check_edge(point, now_milliseconds());
}

/// 处理一条鼠标原始输入。原始输入只有相对位移，光标位置另行读取。
static void on_input(HRAWINPUT handle)
{
    RAWINPUT input;
    UINT size = sizeof(input);
    if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 ||
        input.header.dwType != RIM_TYPEMOUSE) return;

    // 同一条输入可能同时包含按下、移动和松开，按发生顺序处理。
    USHORT buttons = input.data.mouse.usButtonFlags;
    if (buttons & RI_MOUSE_LEFT_BUTTON_DOWN) press(true);
    if (buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) press(false);
    if (input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0 || (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        on_move();
    if (buttons & RI_MOUSE_LEFT_BUTTON_UP) release(true);
    if (buttons & RI_MOUSE_RIGHT_BUTTON_UP) release(false);
}

/// 没按住鼠标时只需要知道有没有按下。高回报率的鼠标每秒有上千条输入，逐条唤醒线程不划算：
/// 队列里暂时没有新的输入时休息一小会儿，这期间到达的输入攒在队列里，醒来后按顺序一口气处理完。
/// 按下最多晚一次休息的时间（系统计时精度下约 16 毫秒）才被注意到，晃动至少要一百多毫秒，不受影响；按住期间不休息。
static void rest_while_idle(void)
{
    if (((GetQueueStatus(QS_RAWINPUT) >> 16) & QS_RAWINPUT) == 0) Sleep(IDLE_BATCH_MILLISECONDS);
}

// ───────────── 监听线程 ─────────────

static bool register_mouse(DWORD flags, HWND target)
{
    RAWINPUTDEVICE device = { 0x01 /* HID_USAGE_PAGE_GENERIC */, 0x02 /* HID_USAGE_GENERIC_MOUSE */, flags, target };
    return RegisterRawInputDevices(&device, 1, sizeof(device)) != 0;
}

/// 创建只接收消息的隐藏窗口，并登记为程序在后台时也接收鼠标原始输入。
static bool start_listening(void)
{
    input_window = CreateWindowExW(0, L"STATIC", L"DropShelf Input", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!input_window)
    {
        log_info(L"鼠标输入窗口创建失败，错误码 %lu", GetLastError());
        return false;
    }
    if (register_mouse(RIDEV_INPUTSINK, input_window)) return true;

    log_info(L"鼠标原始输入注册失败，错误码 %lu", GetLastError());
    DestroyWindow(input_window);
    input_window = NULL;
    return false;
}

static void stop_listening(void)
{
    if (listening) register_mouse(RIDEV_REMOVE, NULL);
    listening = false;
    if (input_window) DestroyWindow(input_window);
    input_window = NULL;
}

static DWORD WINAPI run(void *parameter)
{
    (void)parameter;
    // 没按住时只等按下，节能运行即可；按住后跟随进程，进程也切到正常性能。
    power_background_thread();
    listening = start_listening();
    SetEvent(started);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        if (message.message == WM_TIMER && message.hwnd == NULL && message.wParam == edge_timer)
        {
            on_edge_timer();
            continue;
        }
        // 原始输入处理完仍要交给默认窗口过程，系统才会释放这条输入。
        if (message.message == WM_INPUT) on_input((HRAWINPUT)message.lParam);
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_INPUT && !left_down && !right_down) rest_while_idle();
    }

    stop_edge_timer();
    stop_listening();
    power_hold(&detector, false);
    return 0;
}

bool gesture_service_start(HWND window, UINT message, int sensitivity)
{
    if (thread) return listening;
    notify_window = window;
    notify_message = message;
    requested_sensitivity = sensitivity;
    applied_sensitivity = sensitivity;
    shake_detector_init(&detector, sensitivity);
    requested_dwell = edge_dwell_milliseconds(DWELL_MEDIUM);
    edge_dwell_init(&edge_detector, requested_dwell);
    left_down = right_down = triggered = false;

    started = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, run, NULL, 0, &thread_id);
    if (!thread)
    {
        log_win32_error(L"手势识别线程启动失败", GetLastError());
        CloseHandle(started);
        started = NULL;
        return false;
    }
    SetThreadDescription(thread, L"DropShelf 手势识别");
    WaitForSingleObject(started, 2000);
    return listening;
}

void gesture_service_stop(void)
{
    if (!thread) return;
    PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(thread, 1000);
    CloseHandle(thread);
    thread = NULL;
    thread_id = 0;
    if (started) CloseHandle(started);
    started = NULL;
    // 线程在一秒内没有结束时不释放它还可能用到的识别器。
    if (!listening) shake_detector_free(&detector);
}

bool gesture_service_is_running(void)
{
    return listening;
}

void gesture_service_set_sensitivity(int sensitivity)
{
    requested_sensitivity = sensitivity;
}

void gesture_service_set_trigger(int trigger)
{
    shake_enabled = auto_open_uses_shake(trigger);
    edge_enabled = auto_open_uses_edge(trigger);
}

void gesture_service_set_edge_dwell(int dwell)
{
    requested_dwell = edge_dwell_milliseconds(dwell);
}

void gesture_service_set_paused(bool value)
{
    paused = value;
}
