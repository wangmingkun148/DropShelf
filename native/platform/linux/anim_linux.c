// anim.c 的逐帧回调在 GLib 主循环里的实现。
// Windows 版用 DwmFlush 等下一次刷新；这里用 16ms 的定时器，等 window 层接上 GdkFrameClock 后再改成和刷新同步。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "anim.h"

#include <glib.h>
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