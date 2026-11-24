// 探针窗口的 Wayland 版：识别到手势后在光标处放一个看不见的透明窗口，系统拖放经过它时
// GTK 会送来 drag-enter，由此确认用户真的在拖着东西，再弹出置物架。
// Wayland 上的窗口没有“穿透”，所以只存在 0.9 秒：确认到了或者超时就立刻关掉。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probe.h"
#include "log.h"
#include "ui.h"
#include "ui_linux.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define PROBE_SIZE 320.0
#define PROBE_TIMEOUT_MILLISECONDS 900

static UiWindow *probe_window;
static void (*detected_callback)(void);
static guint timeout_source;

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

static gboolean probe_timeout(gpointer data)
{
    (void)data;
    timeout_source = 0;
    log_trace(L"探针窗口超时：没有收到拖放");
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
    log_trace(local ? L"探针收到本程序自己的拖动，不算" : L"探针确认正在拖放");
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
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(monitor, &info)) return;
    int size = (int)lround(PROBE_SIZE);
    POINT position = probe_place(center, info.rcWork, size, size);
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
    ui_window_place(probe_window, position.x, position.y, (float)size, (float)size);
    ui_window_show_with(probe_window, false);
    timeout_source = g_timeout_add(PROBE_TIMEOUT_MILLISECONDS, probe_timeout, NULL);
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