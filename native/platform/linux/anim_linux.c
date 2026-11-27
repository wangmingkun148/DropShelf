// anim.c 的逐帧回调在 GLib 主循环里的实现。
// Windows 版用 DwmFlush 等下一次刷新；这里用 16ms 的定时器，等 window 层接上 GdkFrameClock 后再改成和刷新同步。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "anim.h"

#include <dirent.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Subscriber
{
    FrameHandler handler;
    void *context;
} Subscriber;

static Subscriber *subscribers;
static int subscriber_count;
static guint timer;
static gint64 origin;

static double now_seconds(void)
{
    if (origin == 0) origin = g_get_monotonic_time();
    return (double)(g_get_monotonic_time() - origin) / 1000000.0;
}

static gboolean tick(gpointer data)
{
    (void)data;
    double time = now_seconds();
    int count = subscriber_count;
    Subscriber *copy = count ? xalloc((size_t)count * sizeof(Subscriber)) : NULL;
    if (count) memcpy(copy, subscribers, (size_t)count * sizeof(Subscriber));
    for (int index = 0; index < count; index++)
    {
        bool still_subscribed = false;
        for (int current = 0; current < subscriber_count && !still_subscribed; current++)
            still_subscribed = subscribers[current].handler == copy[index].handler && subscribers[current].context == copy[index].context;
        if (still_subscribed) copy[index].handler(copy[index].context, time);
    }
    free(copy);
    if (subscriber_count == 0 && timer)
    {
        g_source_remove(timer);
        timer = 0;
    }
    return timer != 0;
}

double frames_time(void)
{
    return now_seconds();
}

double frames_refresh_period(void)
{
    return 1.0 / 60.0;
}

void frames_subscribe(FrameHandler handler, void *context)
{
    for (int index = 0; index < subscriber_count; index++)
        if (subscribers[index].handler == handler && subscribers[index].context == context) return;
    subscribers = xrealloc(subscribers, (size_t)(subscriber_count + 1) * sizeof(Subscriber));
    subscribers[subscriber_count++] = (Subscriber){ handler, context };
    if (!timer) timer = g_timeout_add(16, tick, NULL);
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
    if (subscriber_count == 0 && timer)
    {
        g_source_remove(timer);
        timer = 0;
    }
}

void frames_shutdown(void)
{
    if (timer)
    {
        g_source_remove(timer);
        timer = 0;
    }
    free(subscribers);
    subscribers = NULL;
    subscriber_count = 0;
}

/// 读 /sys/class/power_supply/名称/字段 的第一行（去掉换行）；读不到时为空。
static void read_power_supply(const char *name, const char *field, char *value, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", name, field);
    value[0] = 0;
    FILE *file = fopen(path, "r");
    if (!file) return;
    if (!fgets(value, (int)size, file)) value[0] = 0;
    fclose(file);
    value[strcspn(value, "\n")] = 0;
}

/// 本机的电池在放电（鼠标、手柄这类外设的电池 scope 是 Device，不算）。台式机没有电池，总是 false。
static bool on_battery(void)
{
    DIR *directory = opendir("/sys/class/power_supply");
    if (!directory) return false;
    bool discharging = false;
    struct dirent *entry;
    while (!discharging && (entry = readdir(directory)))
    {
        if (entry->d_name[0] == '.') continue;
        char type[32], scope[32], status[32];
        read_power_supply(entry->d_name, "type", type, sizeof(type));
        if (strcmp(type, "Battery") != 0) continue;
        read_power_supply(entry->d_name, "scope", scope, sizeof(scope));
        if (strcmp(scope, "Device") == 0) continue;
        read_power_supply(entry->d_name, "status", status, sizeof(status));
        discharging = strcmp(status, "Discharging") == 0;
    }
    closedir(directory);
    return discharging;
}

bool anim_linux_saving_power(void)
{
    static gint64 checked;
    static bool has_checked, saving;
    static GPowerProfileMonitor *profiles;
    gint64 now = g_get_monotonic_time();
    if (has_checked && now - checked < 2 * G_USEC_PER_SEC) return saving;
    checked = now;
    has_checked = true;
    // 没有 power-profiles-daemon 时 GLib 给的是一直报“没开节能”的实现。
    if (!profiles) profiles = g_power_profile_monitor_dup_default();
    saving = on_battery() || (profiles && g_power_profile_monitor_get_power_saver_enabled(profiles));
    return saving;
}
