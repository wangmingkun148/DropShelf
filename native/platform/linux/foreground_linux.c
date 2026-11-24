// 前台程序监视：在 Wayland 上看不到别的程序的进程名，只能拿窗口的 class（app_id）。
// 屏蔽列表里写的就是这个标识（比如 firefox、steam_app_123）。
// 前台变了的事从 Hyprland 的事件流里收，不用轮询；判断本身每次都重新问一次当前的前台窗口。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "foreground.h"
#include "foreground_linux.h"
#include "hyprland_linux.h"
#include "json.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/// 本程序自己的窗口不算“前台程序”。
static const wchar_t *self_classes[] = { L"dropshelf", L"io.github.wangmingkun148.DropShelf" };

static bool is_self(const wchar_t *name)
{
    for (size_t index = 0; index < sizeof self_classes / sizeof self_classes[0]; index++)
        if (wcs_equal_ignore_case(name, self_classes[index])) return true;
    return false;
}

// ───────────── 解析（纯函数，测试用） ─────────────

static const wchar_t *client_class(const JsonValue *client)
{
    const JsonValue *name = json_get(client, L"class");
    if (!name || name->type != JSON_STRING || wcs_is_blank(name->string)) return NULL;
    return name->string;
}

/// 有映射、没被隐藏的窗口才算“有窗口”。
static bool client_visible(const JsonValue *client)
{
    const JsonValue *mapped = json_get(client, L"mapped");
    if (mapped && mapped->type == JSON_BOOL && !mapped->boolean) return false;
    const JsonValue *hidden = json_get(client, L"hidden");
    if (hidden && hidden->type == JSON_BOOL && hidden->boolean) return false;
    return true;
}

static bool client_at(const JsonValue *client, int x, int y)
{
    const JsonValue *at = json_get(client, L"at");
    const JsonValue *size = json_get(client, L"size");
    if (!at || at->type != JSON_ARRAY || at->count < 2) return false;
    if (!size || size->type != JSON_ARRAY || size->count < 2) return false;
    if (at->items[0]->type != JSON_NUMBER || at->items[1]->type != JSON_NUMBER) return false;
    if (size->items[0]->type != JSON_NUMBER || size->items[1]->type != JSON_NUMBER) return false;
    int left = (int)at->items[0]->number, top = (int)at->items[1]->number;
    int width = (int)size->items[0]->number, height = (int)size->items[1]->number;
    return x >= left && x < left + width && y >= top && y < top + height;
}

wchar_t *foreground_active_class(const wchar_t *text)
{
    JsonValue *value = json_parse(text, 0);
    if (!value) return NULL;
    wchar_t *result = NULL;
    if (value->type == JSON_OBJECT)
    {
        const wchar_t *name = client_class(value);
        if (name) result = wcs_dup(name);
    }
    json_free(value);
    return result;
}

bool foreground_active_fullscreen(const wchar_t *text)
{
    JsonValue *value = json_parse(text, 0);
    if (!value) return false;
    bool fullscreen = false;
    if (value->type == JSON_OBJECT)
    {
        const JsonValue *state = json_get(value, L"fullscreen");
        if (state && state->type == JSON_NUMBER && state->number != 0) fullscreen = true;
    }
    json_free(value);
    return fullscreen;
}

bool foreground_is_fullscreen(void)
{
    char *reply = hyprland_request("j/activewindow");
    if (!reply) return false;
    wchar_t *text = wide_from_utf8(reply, strlen(reply));
    free(reply);
    if (!text) return false;
    bool fullscreen = foreground_active_fullscreen(text);
    free(text);
    return fullscreen;
}

static int compare_names(const void *first, const void *second)
{
    const wchar_t *a = *(const wchar_t *const *)first, *b = *(const wchar_t *const *)second;
    return wcscmp(a, b);
}

void foreground_clients_apps(const wchar_t *text, StrList *names)
{
    *names = (StrList){0};
    JsonValue *value = json_parse(text, 0);
    if (!value) return;
    if (value->type == JSON_ARRAY)
    {
        for (int index = 0; index < value->count; index++)
        {
            const JsonValue *client = value->items[index];
            if (client->type != JSON_OBJECT || !client_visible(client)) continue;
            const wchar_t *name = client_class(client);
            if (!name || is_self(name)) continue;
            if (strlist_index_of(names, name, true) >= 0) continue;
            strlist_add(names, name);
        }
    }
    json_free(value);
    if (names->count > 1) qsort(names->items, (size_t)names->count, sizeof(wchar_t *), compare_names);
}

void foreground_clients_at(const wchar_t *text, int x, int y, StrList *classes)
{
    *classes = (StrList){0};
    JsonValue *value = json_parse(text, 0);
    if (!value) return;
    if (value->type == JSON_ARRAY)
    {
        for (int index = 0; index < value->count; index++)
        {
            const JsonValue *client = value->items[index];
            if (client->type != JSON_OBJECT || !client_visible(client) || !client_at(client, x, y)) continue;
            const wchar_t *name = client_class(client);
            if (!name || strlist_index_of(classes, name, true) >= 0) continue;
            strlist_add(classes, name);
        }
    }
    json_free(value);
}

// ───────────── 监视 ─────────────

static StrList blocked;
static bool is_blocked;
static void (*listener)(bool blocked_changed);
static pthread_t thread;
static bool thread_started;
static int wake_pipe[2] = { -1, -1 };
static gint update_pending;

static char *request_clients(void)
{
    return hyprland_request("j/clients");
}

/// 问一次前台窗口，更新屏蔽状态；变了就告诉监听者。
static void update(void)
{
    if (blocked.count == 0)
    {
        if (!is_blocked) return;
    }
    char *reply = hyprland_request("j/activewindow");
    wchar_t *active = NULL;
    if (reply)
    {
        wchar_t *text = wide_from_utf8(reply, strlen(reply));
        free(reply);
        if (text)
        {
            active = foreground_active_class(text);
            free(text);
        }
    }
    bool now_blocked = active && strlist_index_of(&blocked, active, true) >= 0;
    free(active);
    if (now_blocked == is_blocked) return;
    is_blocked = now_blocked;
    log_trace(now_blocked ? L"屏蔽的应用切到前台，暂停弹出置物架" : L"屏蔽的应用离开前台，恢复弹出置物架");
    if (listener) listener(now_blocked);
}

static gboolean update_idle(gpointer data)
{
    (void)data;
    g_atomic_int_set(&update_pending, 0);
    update();
    return G_SOURCE_REMOVE;
}

static void schedule_update(void)
{
    if (g_atomic_int_compare_and_exchange(&update_pending, 0, 1)) g_idle_add(update_idle, NULL);
}

/// 收 Hyprland 的事件流；只关心前台窗口变了，收到就重新问一次。
static void *watch_events(void *parameter)
{
    (void)parameter;
    char path[512];
    if (!hyprland_event_path(path, sizeof path)) return NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof address.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0)
    {
        close(fd);
        return NULL;
    }
    char buffer[4096];
    size_t used = 0;
    for (;;)
    {
        struct pollfd waited[2] = { { .fd = fd, .events = POLLIN }, { .fd = wake_pipe[0], .events = POLLIN } };
        int ready = poll(waited, 2, -1);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (waited[1].revents & POLLIN) break;
        if (!(waited[0].revents & (POLLIN | POLLERR | POLLHUP))) continue;
        ssize_t got = read(fd, buffer + used, sizeof buffer - used);
        if (got <= 0) break;
        used += (size_t)got;
        // 按行处理，最后一行不完整就留到下次。
        size_t start = 0;
        for (size_t index = 0; index < used; index++)
        {
            if (buffer[index] != '\n') continue;
            if ((size_t)(index - start) >= 14 && strncmp(buffer + start, "activewindow>>", 14) == 0) schedule_update();
            start = index + 1;
        }
        if (start > 0)
        {
            memmove(buffer, buffer + start, used - start);
            used -= start;
        }
        if (used == sizeof buffer) used = 0;   // 异常长的行，丢掉重来
    }
    close(fd);
    return NULL;
}

static void stop_watching(void)
{
    if (!thread_started) return;
    char wake = 1;
    ssize_t written = write(wake_pipe[1], &wake, 1);
    (void)written;
    pthread_join(thread, NULL);
    thread_started = false;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    wake_pipe[0] = wake_pipe[1] = -1;
}

static void start_watching(void)
{
    if (thread_started) return;
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
    if (pthread_create(&thread, NULL, watch_events, NULL) != 0)
    {
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        wake_pipe[0] = wake_pipe[1] = -1;
        return;
    }
    pthread_setname_np(thread, "dropshelf-fg");
    thread_started = true;
}

void foreground_watcher_set_listener(void (*changed)(bool blocked_changed))
{
    listener = changed;
}

void foreground_watcher_set_blocked_apps(const StrList *names)
{
    strlist_clear(&blocked);
    for (int index = 0; index < names->count; index++)
        if (strlist_index_of(&blocked, names->items[index], true) < 0) strlist_add(&blocked, names->items[index]);
    if (blocked.count > 0) start_watching();
    else stop_watching();
    update();
}

bool foreground_watcher_is_blocked(void)
{
    return is_blocked;
}

bool foreground_watcher_check(void)
{
    update();
    return is_blocked;
}

bool foreground_watcher_is_blocked_at(POINT point)
{
    if (blocked.count == 0) return false;
    if (foreground_watcher_check()) return true;
    char *reply = request_clients();
    if (!reply) return false;
    wchar_t *text = wide_from_utf8(reply, strlen(reply));
    free(reply);
    if (!text) return false;
    StrList classes = {0};
    foreground_clients_at(text, point.x, point.y, &classes);
    free(text);
    bool result = false;
    for (int index = 0; index < classes.count && !result; index++)
        if (strlist_index_of(&blocked, classes.items[index], true) >= 0) result = true;
    strlist_clear(&classes);
    return result;
}

void foreground_watcher_stop(void)
{
    stop_watching();
    strlist_clear(&blocked);
    is_blocked = false;
}

void foreground_windowed_apps(StrList *names)
{
    *names = (StrList){0};
    char *reply = request_clients();
    if (!reply) return;
    wchar_t *text = wide_from_utf8(reply, strlen(reply));
    free(reply);
    if (!text) return;
    foreground_clients_apps(text, names);
    free(text);
}

wchar_t *process_file_name(DWORD process_id)
{
    // Linux 上没有“程序文件名”这个概念，屏蔽列表用的是窗口的 class。
    (void)process_id;
    return NULL;
}