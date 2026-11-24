// 显示器和工作区的 Win32 形状垫片。GTK 只知道每台显示器的位置和缩放；
// 工作区里被面板/停靠栏占掉的部分（Windows 的 rcWork）在 Wayland 上要问合成器，M2 接 Hyprland IPC 后再扣掉。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "base.h"
#include "hyprland_linux.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>

static GdkMonitor *monitor_at_index(int index)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return NULL;
    GListModel *monitors = gdk_display_get_monitors(display);
    if (index < 0 || index >= (int)g_list_model_get_n_items(monitors)) return NULL;
    return g_list_model_get_item(monitors, index);
}

static void screen_rect(GdkMonitor *monitor, RECT *rect)
{
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    *rect = (RECT){ geometry.x, geometry.y, geometry.x + geometry.width, geometry.y + geometry.height };
}

HMONITOR MonitorFromPoint(POINT point, DWORD flags)
{
    (void)flags;
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return NULL;
    GListModel *monitors = gdk_display_get_monitors(display);
    guint count = g_list_model_get_n_items(monitors);
    int nearest = 0;
    long long best = -1;
    for (guint index = 0; index < count; index++)
    {
        GdkMonitor *monitor = g_list_model_get_item(monitors, index);
        if (!monitor) continue;
        RECT rect;
        screen_rect(monitor, &rect);
        g_object_unref(monitor);
        if (PtInRect(&rect, point)) return (HMONITOR)(intptr_t)(index + 1);
        long long dx = point.x < rect.left ? rect.left - point.x : point.x >= rect.right ? point.x - rect.right + 1 : 0;
        long long dy = point.y < rect.top ? rect.top - point.y : point.y >= rect.bottom ? point.y - rect.bottom + 1 : 0;
        long long distance = dx * dx + dy * dy;
        if (best < 0 || distance < best)
        {
            best = distance;
            nearest = (int)index;
        }
    }
    return count > 0 ? (HMONITOR)(intptr_t)(nearest + 1) : NULL;
}

BOOL GetMonitorInfoW(HMONITOR handle, MONITORINFO *info)
{
    if (!info) return FALSE;
    int index = handle ? (int)(intptr_t)handle - 1 : 0;
    GdkMonitor *monitor = monitor_at_index(index);
    if (!monitor && index != 0) monitor = monitor_at_index(0);
    if (!monitor) return FALSE;
    RECT rect;
    screen_rect(monitor, &rect);
    const char *model = gdk_monitor_get_model(monitor);
    wchar_t *name = model ? wide_from_utf8(model, strlen(model)) : NULL;
    g_object_unref(monitor);
    info->rcMonitor = rect;
    info->rcWork = rect;
    info->dwFlags = 0;
    // 扣掉面板、停靠栏占掉的边（Hyprland 的 reserved）。别的合成器上没有 IPC，工作区就等于整个屏幕。
    HyprMonitor monitors[8];
    int count = hyprland_monitors(monitors, 8);
    for (int at = 0; at < count; at++)
    {
        HyprMonitor *found = &monitors[at];
        bool matches = found->rect.left == rect.left && found->rect.top == rect.top && found->rect.right == rect.right &&
                       found->rect.bottom == rect.bottom;
        if (!matches && name && found->name) matches = wcs_equal_ignore_case(name, found->name);
        if (!matches) continue;
        RECT work = { rect.left + found->reserved_left, rect.top + found->reserved_top, rect.right - found->reserved_right,
            rect.bottom - found->reserved_bottom };
        if (work.right > work.left && work.bottom > work.top) info->rcWork = work;
        break;
    }
    hyprland_monitors_free(monitors, count);
    free(name);
    return TRUE;
}

HRESULT GetDpiForMonitor(HMONITOR handle, int dpi_type, UINT *dpi_x, UINT *dpi_y)
{
    (void)dpi_type;
    int index = handle ? (int)(intptr_t)handle - 1 : 0;
    GdkMonitor *monitor = monitor_at_index(index);
    if (!monitor) return E_INVALIDARG;
    int scale = gdk_monitor_get_scale_factor(monitor);
    g_object_unref(monitor);
    if (scale < 1) scale = 1;
    if (dpi_x) *dpi_x = (UINT)(96 * scale);
    if (dpi_y) *dpi_y = (UINT)(96 * scale);
    return S_OK;
}

static POINT last_pointer;
static bool pointer_known;

void ui_pointer_seen_global(int x, int y)
{
    last_pointer = (POINT){ x, y };
    pointer_known = true;
}

/// 开发时用 DROPSHELF_POINTER=x,y 强制指定光标位置：可以把测试用的窗口放到指定的显示器上，
/// 不用真的把鼠标移过去。设了它就不再问 Hyprland。
static bool pointer_from_environment(POINT *point)
{
    static bool checked;
    static POINT from_environment;
    static bool found;
    if (!checked)
    {
        checked = true;
        const char *text = g_getenv("DROPSHELF_POINTER");
        int x = 0, y = 0;
        if (text && sscanf(text, "%d,%d", &x, &y) == 2)
        {
            from_environment = (POINT){ x, y };
            found = true;
        }
    }
    if (found) *point = from_environment;
    return found;
}

BOOL GetCursorPos(POINT *point)
{
    if (!point) return FALSE;
    if (pointer_from_environment(point)) return TRUE;
    // Hyprland 知道全局光标在哪；拿不到时用我们自己窗口里最后看到的指针位置，再不行就取第一台显示器的中间。
    if (hyprland_cursor_position(&point->x, &point->y)) return TRUE;
    if (pointer_known)
    {
        *point = last_pointer;
        return TRUE;
    }
    GdkMonitor *monitor = monitor_at_index(0);
    if (!monitor) return FALSE;
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    g_object_unref(monitor);
    *point = (POINT){ geometry.x + geometry.width / 2, geometry.y + geometry.height / 2 };
    return TRUE;
}