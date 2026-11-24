// Linux 的手势识别：按键和移动从 evdev 读，光标位置问 Hyprland 的 IPC。
// Wayland 客户端看不到全局鼠标，所以按住期间按固定间隔取一次光标位置喂给共享的 shake.c / edge 逻辑；
// 认出晃动或边缘停留后，一样通过 PostMessage 交给界面线程，参数打包方式和 Windows 版相同。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gesture_linux.h"
#include "gesture_service.h"
#include "hyprland_linux.h"
#include "log.h"
#include "power.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/// 按住期间多久取一次光标位置（毫秒）。晃动识别按时间重新取样，60 次/秒足够。
#define PRESSED_TICK_MILLISECONDS 16
/// 光标停着不动时也要按这个间隔检查边缘停留。
#define EDGE_POLL_MILLISECONDS 40
/// 没按住鼠标时多久回来看一眼有没有新插上的鼠标。
#define IDLE_RESCAN_MILLISECONDS 2000
#define EDGE_TOLERANCE 1
#define MAXIMUM_DEVICES 16

_Static_assert(sizeof(LPARAM) >= 8, "光标位置按两个 32 位数放进 lparam");

typedef struct MouseDevice
{
    int fd;
    bool left_down;
    bool right_down;
    char path[64];
} MouseDevice;

static HWND notify_window;
static UINT notify_message;
static pthread_t thread;
static bool thread_started;
static int wake_pipe[2] = { -1, -1 };
static volatile bool running;
static volatile bool shake_enabled = true;
static volatile bool edge_enabled;
static volatile bool paused;
static volatile int requested_sensitivity;
static volatile int requested_dwell;

// 以下只在监听线程里使用。
static MouseDevice devices[MAXIMUM_DEVICES];
static int device_count;
static int applied_sensitivity;
static ShakeDetector detector;
static EdgeDwellDetector edge_detector;
static bool any_pressed;
static bool triggered;
static int moves_this_press;
static int last_x, last_y;
static bool has_last_point;
static long long last_edge_check;
/// 每次按下时读一次显示器布局，按住期间一直用它。
static HyprMonitor screen[8];
static int screen_count;

static long long now_milliseconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void notify(GestureEvent event, ScreenEdge edge, POINT point)
{
    unsigned long long packed = ((unsigned long long)(unsigned)point.y << 32) | (unsigned)point.x;
    PostMessageW(notify_window, notify_message, (WPARAM)event | ((WPARAM)edge << 8), (LPARAM)packed);
}

// ───────────── 屏幕边缘（纯判断，测试也用） ─────────────

static bool rect_contains(RECT rect, int x, int y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

static bool on_any_monitor(const HyprMonitor *monitors, int count, int x, int y)
{
    for (int index = 0; index < count; index++)
        if (rect_contains(monitors[index].rect, x, y)) return true;
    return false;
}

ScreenEdge gesture_edge_at(const HyprMonitor *monitors, int count, int x, int y)
{
    const HyprMonitor *monitor = NULL;
    for (int index = 0; index < count; index++)
        if (rect_contains(monitors[index].rect, x, y))
        {
            monitor = &monitors[index];
            break;
        }
    if (!monitor) return EDGE_NONE;
    RECT bounds = monitor->rect;
    RECT work = { bounds.left + monitor->reserved_left, bounds.top + monitor->reserved_top, bounds.right - monitor->reserved_right,
        bounds.bottom - monitor->reserved_bottom };
    bool near_left = x <= bounds.left + EDGE_TOLERANCE;
    bool near_right = x >= bounds.right - 1 - EDGE_TOLERANCE;
    bool near_top = y <= bounds.top + EDGE_TOLERANCE;
    if (!near_left && !near_right && !near_top) return EDGE_NONE;
    // 面板、停靠栏占掉的地方不算边缘：那里松开鼠标是面板的操做。
    if (!rect_contains(work, x, y)) return EDGE_NONE;
    if (near_left && !on_any_monitor(monitors, count, bounds.left - 1, y)) return EDGE_LEFT;
    if (near_right && !on_any_monitor(monitors, count, bounds.right, y)) return EDGE_RIGHT;
    if (near_top && !on_any_monitor(monitors, count, x, bounds.top - 1)) return EDGE_TOP;
    return EDGE_NONE;
}

/// 所有显示器合起来的外框向外扩一点，晃动检测用它判断光标是不是贴着屏幕外沿。
/// 两台显示器中间的分界不算，否则在副屏之间来回拖时晃动会被压掉。
static bool near_outer_edge(const HyprMonitor *monitors, int count, int x, int y)
{
    if (count == 0) return false;
    const int margin = 5;
    RECT bounds = monitors[0].rect;
    for (int index = 1; index < count; index++)
    {
        if (monitors[index].rect.left < bounds.left) bounds.left = monitors[index].rect.left;
        if (monitors[index].rect.top < bounds.top) bounds.top = monitors[index].rect.top;
        if (monitors[index].rect.right > bounds.right) bounds.right = monitors[index].rect.right;
        if (monitors[index].rect.bottom > bounds.bottom) bounds.bottom = monitors[index].rect.bottom;
    }
    return x <= bounds.left + margin || y <= bounds.top + margin || x >= bounds.right - margin || y >= bounds.bottom - margin;
}

// ───────────── 设备 ─────────────

static bool test_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (int)(8 * sizeof(unsigned long))] >> (bit % (int)(8 * sizeof(unsigned long)))) & 1ul;
}

/// 是不是能用的鼠标：有按键、有相对移动。触摸板也算（同样有 BTN_LEFT 和 REL_X/REL_Y）。
static bool usable_mouse(int fd)
{
    unsigned long types[(EV_MAX / (int)(8 * sizeof(unsigned long))) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof types), types) < 0) return false;
    if (!test_bit(types, EV_KEY) || !test_bit(types, EV_REL) || !test_bit(types, EV_SYN)) return false;
    unsigned long keys[(KEY_MAX / (int)(8 * sizeof(unsigned long))) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || !test_bit(keys, BTN_LEFT)) return false;
    unsigned long relative[(REL_MAX / (int)(8 * sizeof(unsigned long))) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof relative), relative) < 0) return false;
    return test_bit(relative, REL_X) && test_bit(relative, REL_Y);
}

static void close_device(MouseDevice *device)
{
    if (device->fd >= 0) close(device->fd);
    device->fd = -1;
}

/// 扫描 /dev/input，把还没打开的鼠标设备打开。已经打开的不动。
static void scan_devices(void)
{
    DIR *directory = opendir("/dev/input");
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%.32s", entry->d_name);
        bool already_open = false;
        int slot = -1;
        for (int index = 0; index < device_count; index++)
        {
            if (strcmp(devices[index].path, path) != 0) continue;
            if (devices[index].fd >= 0) already_open = true;
            else if (slot < 0) slot = index;
        }
        if (already_open) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!usable_mouse(fd))
        {
            close(fd);
            continue;
        }
        // 确认是鼠标以后才占名额：别的设备节点（键盘、电源键……）不能先把槽位用完。
        if (slot < 0)
        {
            if (device_count >= MAXIMUM_DEVICES)
            {
                close(fd);
                continue;
            }
            slot = device_count++;
        }
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        wchar_t *wide_path = wide_from_utf8(path, strlen(path));
        wchar_t *wide_name = wide_from_utf8(name, strlen(name));
        log_info(L"监听鼠标设备 %ls（%ls）", wide_path ? wide_path : L"", wide_name ? wide_name : L"");
        free(wide_path);
        free(wide_name);
        devices[slot] = (MouseDevice){ .fd = fd };
        strncpy(devices[slot].path, path, sizeof devices[0].path - 1);
    }
    closedir(directory);
}

static void close_all_devices(void)
{
    for (int index = 0; index < device_count; index++) close_device(&devices[index]);
    device_count = 0;
}

// ───────────── 按下和松开 ─────────────

static bool any_button_down(void)
{
    for (int index = 0; index < device_count; index++)
        if (devices[index].fd >= 0 && (devices[index].left_down || devices[index].right_down)) return true;
    return false;
}

/// 左键的全局状态，界面线程也会读：1 按住、0 松开、-1 不知道（没有设备）。
static volatile int left_button_state = -1;

int gesture_left_button_state(void)
{
    return left_button_state;
}

static void update_left_button_state(void)
{
    bool down = false;
    for (int index = 0; index < device_count; index++)
        if (devices[index].fd >= 0 && devices[index].left_down) down = true;
    left_button_state = device_count > 0 ? (down ? 1 : 0) : -1;
}

static void apply_sensitivity_if_changed(void)
{
    int requested = requested_sensitivity;
    if (requested == applied_sensitivity) return;
    applied_sensitivity = requested;
    shake_detector_set_sensitivity(&detector, requested);
}

static void load_screen(void)
{
    hyprland_monitors_free(screen, screen_count);
    screen_count = hyprland_monitors(screen, 8);
}

static bool check_edge(int x, int y, long long now)
{
    edge_detector.dwell_milliseconds = requested_dwell;
    ScreenEdge edge = gesture_edge_at(screen, screen_count, x, y);
    if (!edge_dwell_update(&edge_detector, edge, x, y, now)) return false;
    triggered = true;
    notify(GESTURE_EDGE_DWELL, edge, (POINT){ x, y });
    return true;
}

static void begin_press(void)
{
    power_follow_process();
    power_hold(&detector, true);
    moves_this_press = 0;
    triggered = false;
    has_last_point = false;
    last_edge_check = 0;
    // 每次按下时重新读显示器信息，显示器布局变化后也能正确判断边缘。
    load_screen();
    shake_detector_reset(&detector);
    edge_dwell_reset(&edge_detector);
}

static void end_press(void)
{
    power_hold(&detector, false);
    power_background_thread();
    // 按住期间光标位置是靠 Hyprland IPC 取的，这里记一笔能让“晃动没反应”有据可查。
    if (moves_this_press >= 8)
        log_info(L"按住期间取到 %d 次光标位置，%ls", moves_this_press, triggered ? L"已识别手势" : L"未识别手势");
    bool released = triggered;
    triggered = false;
    shake_detector_reset(&detector);
    edge_dwell_reset(&edge_detector);
    has_last_point = false;
    hyprland_monitors_free(screen, screen_count);
    screen_count = 0;
    if (released) notify(GESTURE_RELEASED, EDGE_NONE, (POINT){ 0, 0 });
}

/// 按住的每轮：有位置变化就交给晃动和边缘检测；停着不动时按间隔检查边缘停留。
static void on_tick(void)
{
    if (triggered || paused) return;
    int x = 0, y = 0;
    if (!hyprland_cursor_position(&x, &y)) return;
    long long now = now_milliseconds();
    if (!has_last_point || x != last_x || y != last_y)
    {
        has_last_point = true;
        last_x = x;
        last_y = y;
        moves_this_press++;
        if (edge_enabled && check_edge(x, y, now)) return;
        if (!shake_enabled) return;
        apply_sensitivity_if_changed();
        if (!shake_detector_add_raw_sample(&detector, x, y, now, near_outer_edge(screen, screen_count, x, y))) return;
        triggered = true;
        notify(GESTURE_SHAKE, EDGE_NONE, (POINT){ x, y });
    }
    else if (edge_enabled && now - last_edge_check >= EDGE_POLL_MILLISECONDS)
    {
        last_edge_check = now;
        check_edge(x, y, now);
    }
}

static void reset_button_state(void)
{
    for (int index = 0; index < device_count; index++)
    {
        devices[index].left_down = false;
        devices[index].right_down = false;
    }
}

// ───────────── 监听线程 ─────────────

static void read_device(MouseDevice *device)
{
    struct input_event events[64];
    for (;;)
    {
        ssize_t got = read(device->fd, events, sizeof events);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (got <= 0)
        {
            // 设备拔掉了：下一轮空闲时会重新扫描。
            wchar_t *wide_path = wide_from_utf8(device->path, strlen(device->path));
            log_info(L"鼠标设备 %ls 已断开", wide_path ? wide_path : L"");
            free(wide_path);
            close_device(device);
            return;
        }
        size_t count = (size_t)got / sizeof events[0];
        for (size_t index = 0; index < count; index++)
        {
            const struct input_event *event = &events[index];
            if (event->type != EV_KEY) continue;
            if (event->code != BTN_LEFT && event->code != BTN_RIGHT) continue;
            bool down = event->value != 0;
            if (event->code == BTN_LEFT) device->left_down = down;
            else device->right_down = down;
        }
    }
}

static void *run(void *parameter)
{
    (void)parameter;
    power_background_thread();
    running = true;
    for (;;)
    {
        struct pollfd waited[MAXIMUM_DEVICES + 1];
        MouseDevice *owners[MAXIMUM_DEVICES + 1];
        int count = 0;
        for (int index = 0; index < device_count; index++)
        {
            if (devices[index].fd < 0) continue;
            waited[count] = (struct pollfd){ .fd = devices[index].fd, .events = POLLIN };
            owners[count++] = &devices[index];
        }
        waited[count] = (struct pollfd){ .fd = wake_pipe[0], .events = POLLIN };
        owners[count++] = NULL;
        int timeout = any_pressed ? PRESSED_TICK_MILLISECONDS : IDLE_RESCAN_MILLISECONDS;
        int ready = poll(waited, (nfds_t)count, timeout);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (waited[count - 1].revents & POLLIN) break;          // 要求停止
        for (int index = 0; index < count - 1; index++)
        {
            if (!(waited[index].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            read_device(owners[index]);
        }
        update_left_button_state();

        // 按键状态变了：开始或结束一次按住。
        bool down = any_button_down();
        if (down && !any_pressed)
        {
            any_pressed = true;
            begin_press();
        }
        else if (!down && any_pressed)
        {
            any_pressed = false;
            end_press();
        }
        if (any_pressed) on_tick();
        else if (ready == 0) scan_devices();                     // 空闲超时：看看有没有新设备
    }
    reset_button_state();
    close_all_devices();
    left_button_state = -1;
    running = false;
    return NULL;
}

// ───────────── 对外的接口 ─────────────

bool gesture_service_start(HWND window, UINT message, int sensitivity)
{
    if (thread_started) return running;
    if (!hyprland_available())
    {
        log_info(L"没有 Hyprland IPC，晃动和边缘停留用不了，只能靠快捷键呼出");
        return false;
    }
    notify_window = window;
    notify_message = message;
    requested_sensitivity = sensitivity;
    applied_sensitivity = sensitivity;
    requested_dwell = edge_dwell_milliseconds(DWELL_MEDIUM);
    shake_detector_init(&detector, sensitivity);
    edge_dwell_init(&edge_detector, requested_dwell);
    triggered = false;
    has_last_point = false;
    any_pressed = false;
    for (int index = 0; index < MAXIMUM_DEVICES; index++) devices[index].fd = -1;

    scan_devices();
    if (device_count == 0)
    {
        log_info(L"没有可读的鼠标设备（需要 input 组的权限），只能靠快捷键呼出");
        return false;
    }
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        close_all_devices();
        return false;
    }
    if (pthread_create(&thread, NULL, run, NULL) != 0)
    {
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        wake_pipe[0] = wake_pipe[1] = -1;
        close_all_devices();
        return false;
    }
    // 线程名方便在调试器里认出来。
    pthread_setname_np(thread, "dropshelf-gest");
    thread_started = true;
    return true;
}

void gesture_service_stop(void)
{
    if (!thread_started) return;
    char wake = 1;
    ssize_t written = write(wake_pipe[1], &wake, 1);
    (void)written;
    pthread_join(thread, NULL);
    thread_started = false;
    running = false;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    wake_pipe[0] = wake_pipe[1] = -1;
    shake_detector_free(&detector);
}

bool gesture_service_is_running(void)
{
    return running;
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

// ───────────── 消息参数的拆解（和 Windows 版相同） ─────────────

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
    return (POINT){ (LONG)(int)(unsigned)(value & 0xFFFFFFFFull), (LONG)(int)(unsigned)(value >> 32) };
}