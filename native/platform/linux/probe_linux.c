// 探针窗口的 Wayland 版：识别到手势后在光标处放一个看不见的透明窗口，系统拖放经过它时
// GTK 会送来 drag-enter，由此确认用户真的在拖着东西，再弹出置物架。
// Wayland 上的窗口没有“穿透”，所以只存在 0.9 秒：确认到了或者超时就立刻关掉。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gesture_linux.h"
#include "log.h"
#include "probe.h"
#include "ui.h"
#include "ui_linux.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define PROBE_SIZE 320.0
/// 看不到全局按键时，探针只活这么久。
#define PROBE_TIMEOUT_MILLISECONDS 900
/// 能看到全局左键时，按住就一直等着：拖动中的指针多半是停着的，合成器要等下一次移动
/// 才会把拖放焦点换到探针上，0.9 秒经常不够。
#define PROBE_HOLD_POLL_MILLISECONDS 150
#define PROBE_MAXIMUM_MILLISECONDS 6000

static UiWindow *probe_window;
static void (*detected_callback)(void);
static guint timeout_source;
static gint64 probe_shown_at;

static void close_probe(void)
{
    if (timeout_source)
    {
        g_source_remove(timeout_source);
        timeout_source = 0;
    }
    if (probe_window)
    {
        ui_window_close(probe_window);
        probe_window = NULL;
    }
}

static POINT probe_center;          // 探针现在摆在哪

/// 把探针摆到光标正中；取不到那个点所在的显示器就返回 false。
static bool place_probe(POINT center)
{
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(monitor, &info))
    {
        log_info(L"探针窗口：取不到 (%ld, %ld) 所在显示器的工作区，不显示", center.x, center.y);
        return false;
    }
    int size = (int)lround(PROBE_SIZE);
    POINT position = probe_place(center, info.rcWork, size, size);
    ui_window_place(probe_window, position.x, position.y, (float)size, (float)size);
    probe_center = position;
    return true;
}

/// 探针活着的时候定期看一眼：左键还按着就继续等，松开了或者等太久了就收起来。
/// 同时让探针一直待在光标下面：拖动中合成器只在光标移动时重新找拖放目标，
/// 探针被落在原地的话，用户晃完不动了就一直收不到拖放。
static gboolean probe_check(gpointer data)
{
    (void)data;
    int buttons = gesture_left_button_state();
    gint64 elapsed = (g_get_monotonic_time() - probe_shown_at) / 1000;
    if (buttons == 0) log_info(L"探针窗口关闭：左键已经松开，没有收到拖放");
    else if (elapsed >= (buttons < 0 ? PROBE_TIMEOUT_MILLISECONDS : PROBE_MAXIMUM_MILLISECONDS))
        log_info(L"探针窗口关闭：等了 %lld 毫秒没有收到拖放", (long long)elapsed);
    else
    {
        POINT cursor;
        if (probe_window && GetCursorPos(&cursor))
        {
            int size = (int)lround(PROBE_SIZE);
            int moved_x = (int)(cursor.x - (probe_center.x + size / 2));
            int moved_y = (int)(cursor.y - (probe_center.y + size / 2));
            if (moved_x * moved_x + moved_y * moved_y > 16) place_probe(cursor);
        }
        return G_SOURCE_CONTINUE;
    }
    timeout_source = 0;
    close_probe();
    return G_SOURCE_REMOVE;
}

static gboolean probe_confirmed(gpointer data)
{
    (void)data;
    close_probe();
    if (detected_callback) detected_callback();
    return G_SOURCE_REMOVE;
}

static bool probe_drop(UiWindow *window, void *context, UiDropPhase phase, GdkDrop *drop, const GValue *value, double x, double y,
    GdkDragAction *action)
{
    (void)window;
    (void)context;
    (void)value;
    (void)x;
    (void)y;
    if (phase != UI_DROP_ENTER) return false;
    GdkDrag *drag = drop ? gdk_drop_get_drag(drop) : NULL;
    bool local = drag ? ui_drag_is_local(drag) : false;
    if (local) log_trace(L"探针收到本程序自己的拖动，不算");
    else log_info(L"探针确认正在拖放，弹出置物架");
    if (local) return false;
    if (action) *action = GDK_ACTION_COPY;
    // 不在 GTK 的拖放回调里创建窗口，下一轮主循环再弹置物架。
    g_idle_add(probe_confirmed, NULL);
    return true;
}

bool probe_create(HINSTANCE instance, void (*detected)(void))
{
    (void)instance;
    detected_callback = detected;
    return true;
}

void probe_show_at(POINT center)
{
    if (!detected_callback) return;
    close_probe();
    int size = (int)lround(PROBE_SIZE);
    UiWindowOptions options = {0};
    options.title = L"DropShelf Probe";
    options.width = (float)size;
    options.height = (float)size;
    options.popup = true;
    options.no_activate = true;
    probe_window = ui_window_create(&options);
    if (!probe_window)
    {
        log_info(L"探针窗口创建失败");
        return;
    }
    ui_window_set_drop_handler(probe_window, probe_drop, NULL);
    if (!place_probe(center))
    {
        close_probe();
        return;
    }
    ui_window_show_with(probe_window, false);
    log_info(L"探针窗口：在 (%d,%d) %dx%d 等着拖放，左键%ls", probe_center.x, probe_center.y, size, size,
        gesture_left_button_state() > 0 ? L"按着" : gesture_left_button_state() == 0 ? L"没按" : L"状态未知");
    probe_shown_at = g_get_monotonic_time();
    timeout_source = g_timeout_add(PROBE_HOLD_POLL_MILLISECONDS, probe_check, NULL);
}

void probe_hide(void)
{
    close_probe();
}

void probe_destroy(void)
{
    close_probe();
    detected_callback = NULL;
}