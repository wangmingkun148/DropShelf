// 和 Hyprland 的 IPC 说话：只用得到两件东西——光标在哪、显示器的工作区。
// 每次请求单独连一次 socket（和 hyprctl 一样），回复读完就关；调用方都是界面线程，量也不大。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hyprland_linux.h"
#include "json.h"
#include "log.h"

#include <glib.h>

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/// 一条命令最多等这么久（毫秒）。Hyprland 正常时几微秒就回了；等不到就当 IPC 不可用。
static const int reply_timeout = 120;

static unsigned long long now_milliseconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (unsigned long long)time.tv_sec * 1000 + (unsigned long long)time.tv_nsec / 1000000;
}

static bool socket_path(char *out, size_t size)
{
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!signature || !*signature) return false;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
    {
        int length = snprintf(out, size, "%s/hypr/%s/.socket.sock", runtime, signature);
        if (length > 0 && (size_t)length < size) return true;
    }
    // 旧版本放在 /tmp/hypr 下。
    int length = snprintf(out, size, "/tmp/hypr/%s/.socket.sock", signature);
    return length > 0 && (size_t)length < size;
}

bool hyprland_event_path(char *out, size_t size)
{
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!signature || !*signature) return false;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
    {
        int length = snprintf(out, size, "%s/hypr/%s/.socket2.sock", runtime, signature);
        if (length > 0 && (size_t)length < size) return true;
    }
    int length = snprintf(out, size, "/tmp/hypr/%s/.socket2.sock", signature);
    return length > 0 && (size_t)length < size;
}

bool hyprland_available(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned long long checked_at;
    static bool available;
    unsigned long long now = now_milliseconds();
    pthread_mutex_lock(&lock);
    // 每秒重新看一眼：Hyprland 可能在这之后才启动或重启。
    if (checked_at == 0 || now - checked_at >= 1000)
    {
        char path[512];
        available = socket_path(path, sizeof path) && access(path, F_OK) == 0;
        checked_at = now;
    }
    bool result = available;
    pthread_mutex_unlock(&lock);
    return result;
}

/// 发一条命令，读回全部回复；失败时返回 NULL。回复按字节读。
char *hyprland_request(const char *command)
{
    char path[512];
    if (!socket_path(path, sizeof path)) return NULL;
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return NULL;
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof address.sun_path - 1);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof address) != 0)
    {
        close(socket_fd);
        return NULL;
    }
    size_t length = strlen(command);
    if (write(socket_fd, command, length) != (ssize_t)length)
    {
        close(socket_fd);
        return NULL;
    }
    char *data = NULL;
    size_t used = 0, capacity = 0;
    for (;;)
    {
        struct pollfd waited = { .fd = socket_fd, .events = POLLIN };
        int ready = poll(&waited, 1, reply_timeout);
        if (ready == 0) break;                      // 超时：按已经读到的算
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        char buffer[4096];
        ssize_t got = read(socket_fd, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (used + (size_t)got + 1 > capacity)
        {
            capacity = (used + (size_t)got + 1) * 2;
            data = xrealloc(data, capacity);
        }
        memcpy(data + used, buffer, (size_t)got);
        used += (size_t)got;
    }
    close(socket_fd);
    if (!data || used == 0)
    {
        free(data);
        return NULL;
    }
    data[used] = 0;
    return data;
}

bool hyprland_cursor_position(int *x, int *y)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static bool cached;
    static int cached_x, cached_y;
    static unsigned long long cached_at;
    unsigned long long now = now_milliseconds();
    pthread_mutex_lock(&lock);
    bool result = false;
    // 4 毫秒内的重复询问直接用上次的结果：打开置物架、开始拖动时可能连着问好几次。
    if (cached && now - cached_at < 4)
    {
        if (x) *x = cached_x;
        if (y) *y = cached_y;
        result = true;
    }
    else
    {
        cached = false;
        char *reply = hyprland_available() ? hyprland_request("cursorpos") : NULL;
        int point_x = 0, point_y = 0;
        if (reply && sscanf(reply, "%d,%d", &point_x, &point_y) == 2)
        {
            cached = true;
            cached_x = point_x;
            cached_y = point_y;
            cached_at = now;
            if (x) *x = point_x;
            if (y) *y = point_y;
            result = true;
        }
        free(reply);
    }
    pthread_mutex_unlock(&lock);
    return result;
}

/// hyprctl -j monitors 的一条：逻辑大小要按缩放折算，旋转 90° 或 270° 时宽高对调。
static bool parse_monitor(const JsonValue *value, HyprMonitor *monitor)
{
    const JsonValue *name = json_get(value, L"name");
    const JsonValue *x = json_get(value, L"x");
    const JsonValue *y = json_get(value, L"y");
    const JsonValue *width = json_get(value, L"width");
    const JsonValue *height = json_get(value, L"height");
    if (!x || x->type != JSON_NUMBER || !y || y->type != JSON_NUMBER) return false;
    if (!width || width->type != JSON_NUMBER || height->type != JSON_NUMBER) return false;
    const JsonValue *scale = json_get(value, L"scale");
    const JsonValue *transform = json_get(value, L"transform");
    int transform_value = transform && transform->type == JSON_NUMBER ? (int)transform->number : 0;
    double scale_value = scale && scale->type == JSON_NUMBER && scale->number > 0 ? scale->number : 1;
    bool turned = transform_value % 2 != 0;
    double logical_width = (turned ? height->number : width->number) / scale_value;
    double logical_height = (turned ? width->number : height->number) / scale_value;
    if (logical_width < 1 || logical_height < 1) return false;
    memset(monitor, 0, sizeof *monitor);
    if (name && name->type == JSON_STRING) monitor->name = wcs_dup(name->string);
    monitor->rect = (RECT){ (LONG)x->number, (LONG)y->number, (LONG)(x->number + lround(logical_width)),
        (LONG)(y->number + lround(logical_height)) };
    monitor->scale = (int)lround(scale_value);
    const JsonValue *reserved = json_get(value, L"reserved");
    if (reserved && reserved->type == JSON_ARRAY && reserved->count == 4)
    {
        monitor->reserved_left = (int)reserved->items[0]->number;
        monitor->reserved_top = (int)reserved->items[1]->number;
        monitor->reserved_right = (int)reserved->items[2]->number;
        monitor->reserved_bottom = (int)reserved->items[3]->number;
    }
    return true;
}

int hyprland_parse_monitors(const wchar_t *text, HyprMonitor *out, int max)
{
    if (!text || !out || max <= 0) return 0;
    JsonValue *value = json_parse(text, 0);
    if (!value) return 0;
    int count = 0;
    if (value->type == JSON_ARRAY)
    {
        for (int index = 0; index < value->count && count < max; index++)
        {
            if (value->items[index]->type != JSON_OBJECT) continue;
            HyprMonitor monitor;
            if (!parse_monitor(value->items[index], &monitor)) continue;
            out[count++] = monitor;
        }
    }
    json_free(value);
    return count;
}

void hyprland_monitors_free(HyprMonitor *monitors, int count)
{
    if (!monitors) return;
    for (int index = 0; index < count; index++) free(monitors[index].name);
    memset(monitors, 0, (size_t)count * sizeof(HyprMonitor));
}

/// 从 JSON 数组里取第 index 个数字。
static bool json_number_at(const JsonValue *array, int index, double *out)
{
    if (!array || array->type != JSON_ARRAY || index < 0 || index >= array->count) return false;
    const JsonValue *value = array->items[index];
    if (!value || value->type != JSON_NUMBER) return false;
    *out = value->number;
    return true;
}

/// hyprctl -j clients 的一条：窗口在逻辑坐标里的位置。缩放和旋转都已经算进去了。
static bool parse_client_rect(const JsonValue *value, RECT *rect)
{
    const JsonValue *at = json_get(value, L"at");
    const JsonValue *size = json_get(value, L"size");
    double x = 0, y = 0, width = 0, height = 0;
    if (!json_number_at(at, 0, &x) || !json_number_at(at, 1, &y)) return false;
    if (!json_number_at(size, 0, &width) || !json_number_at(size, 1, &height)) return false;
    if (width < 1 || height < 1) return false;
    *rect = (RECT){ (LONG)lround(x), (LONG)lround(y), (LONG)lround(x + width), (LONG)lround(y + height) };
    return true;
}

bool hyprland_client_at(int x, int y, HyprClientAt *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!hyprland_available()) return false;
    char *clients_text = hyprland_request("j/clients");
    if (!clients_text) return false;
    char *workspace_text = hyprland_request("j/activeworkspace");
    size_t clients_length = strlen(clients_text);
    size_t workspace_length = workspace_text ? strlen(workspace_text) : 0;
    wchar_t *clients_wide = wide_from_utf8(clients_text, clients_length);
    wchar_t *workspace_wide = workspace_text ? wide_from_utf8(workspace_text, workspace_length) : NULL;
    free(clients_text);
    free(workspace_text);
    JsonValue *clients = clients_wide ? json_parse(clients_wide, 0) : NULL;
    JsonValue *workspace = workspace_wide ? json_parse(workspace_wide, 0) : NULL;
    free(clients_wide);
    free(workspace_wide);
    bool result = false;
    if (clients && clients->type == JSON_ARRAY)
    {
        // 别的虚拟桌面上的窗口在 X 那边照样存在，得按当前工作区筛一遍。
        int active = -1;
        const JsonValue *id = workspace && workspace->type == JSON_OBJECT ? json_get(workspace, L"id") : NULL;
        if (id && id->type == JSON_NUMBER) active = (int)lround(id->number);
        int best_focus = 0;
        for (int index = 0; index < clients->count; index++)
        {
            const JsonValue *client = clients->items[index];
            if (!client || client->type != JSON_OBJECT) continue;
            const JsonValue *mapped = json_get(client, L"mapped");
            const JsonValue *hidden = json_get(client, L"hidden");
            if (mapped && mapped->type == JSON_BOOL && !mapped->boolean) continue;
            if (hidden && hidden->type == JSON_BOOL && hidden->boolean) continue;
            const JsonValue *space = json_get(client, L"workspace");
            const JsonValue *space_id = space && space->type == JSON_OBJECT ? json_get(space, L"id") : NULL;
            if (active >= 0 && (!space_id || space_id->type != JSON_NUMBER || (int)lround(space_id->number) != active)) continue;
            RECT rect;
            if (!parse_client_rect(client, &rect)) continue;
            if (x < rect.left || y < rect.top || x >= rect.right || y >= rect.bottom) continue;
            // 窗口叠在一起时，最后动过的那个在最好的一面。
            const JsonValue *focus = json_get(client, L"focusHistoryID");
            int focus_value = focus && focus->type == JSON_NUMBER ? (int)lround(focus->number) : 999;
            if (result && focus_value >= best_focus) continue;
            const JsonValue *xwayland = json_get(client, L"xwayland");
            const JsonValue *pid = json_get(client, L"pid");
            out->found = true;
            out->xwayland = xwayland && xwayland->type == JSON_BOOL && xwayland->boolean;
            out->pid = pid && pid->type == JSON_NUMBER ? (int)lround(pid->number) : 0;
            out->rect = rect;
            best_focus = focus_value;
            result = true;
        }
    }
    if (clients) json_free(clients);
    if (workspace) json_free(workspace);
    return result;
}

// ───────────── 快捷键 ─────────────

/// 我们加的绑定用这个描述，运行时再看到它就知道是自己上次留下的。
static const wchar_t *bind_description = L"DropShelf";

HyprBindState hyprland_bind_state(unsigned modmask, const char *key, const char *command)
{
    if (!key || !*key) return HYPR_BIND_FREE;
    char *reply = hyprland_request("j/binds");
    if (!reply) return HYPR_BIND_FREE;
    wchar_t *text = wide_from_utf8(reply, strlen(reply));
    free(reply);
    if (!text) return HYPR_BIND_FREE;
    wchar_t *wide_key = wide_from_utf8(key, strlen(key));
    JsonValue *value = json_parse(text, 0);
    free(text);
    if (!value || !wide_key)
    {
        free(wide_key);
        json_free(value);
        return HYPR_BIND_FREE;
    }
    HyprBindState state = HYPR_BIND_FREE;
    if (value->type == JSON_ARRAY)
    {
        for (int index = 0; index < value->count && state == HYPR_BIND_FREE; index++)
        {
            const JsonValue *entry = value->items[index];
            const JsonValue *mask = json_get(entry, L"modmask");
            const JsonValue *name = json_get(entry, L"key");
            if (!mask || mask->type != JSON_NUMBER) continue;
            if (!name || name->type != JSON_STRING) continue;
            // 只比修饰键和主键；键盘锁定之类的不影响是不是同一个组合键。
            unsigned theirs = (unsigned)mask->number;
            if ((theirs & (1u | 4u | 8u | 64u)) != (modmask & (1u | 4u | 8u | 64u))) continue;
            if (!wcs_equal_ignore_case(name->string, wide_key)) continue;
            // 描述能对上就是上次我们自己加的；命令行比对不上也不用管（Lua 配置里参数是函数编号）。
            const JsonValue *description = json_get(entry, L"description");
            bool ours = description && description->type == JSON_STRING &&
                wcscmp(description->string, bind_description) == 0;
            if (!ours && command)
            {
                const JsonValue *dispatcher = json_get(entry, L"dispatcher");
                const JsonValue *argument = json_get(entry, L"arg");
                ours = dispatcher && dispatcher->type == JSON_STRING && argument && argument->type == JSON_STRING &&
                    wcs_equal_ignore_case(dispatcher->string, L"exec");
                if (ours)
                {
                    wchar_t *wide_command = wide_from_utf8(command, strlen(command));
                    ours = wide_command && wcscmp(argument->string, wide_command) == 0;
                    free(wide_command);
                }
            }
            state = ours ? HYPR_BIND_OURS : HYPR_BIND_OTHER;
        }
    }
    free(wide_key);
    json_free(value);
    return state;
}

/// "CTRL SHIFT" 和 "SPACE" 拼成 Lua 配置里的写法 "CTRL + SHIFT + SPACE"。调用方 free。
static char *lua_key_text(const char *modifiers, const char *key)
{
    char buffer[256];
    size_t used = 0;
    buffer[0] = 0;
    const char *start = modifiers;
    while (*start)
    {
        const char *end = strchr(start, ' ');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length > 0 && used + length + 4 < sizeof buffer)
        {
            if (used > 0)
            {
                memcpy(buffer + used, " + ", 3);
                used += 3;
            }
            memcpy(buffer + used, start, length);
            used += length;
            buffer[used] = 0;
        }
        start = end ? end + 1 : start + length;
        while (*start == ' ') start++;
    }
    if (used > 0 && used + 4 < sizeof buffer)
    {
        memcpy(buffer + used, " + ", 3);
        used += 3;
        buffer[used] = 0;
    }
    snprintf(buffer + used, sizeof buffer - used, "%s", key);
    return strdup(buffer);
}

/// 放进 Lua 双引号字符串里的转义。
static char *lua_escape(const char *text)
{
    GString *escaped = g_string_new(NULL);
    for (const char *cursor = text; *cursor; cursor++)
    {
        if (*cursor == '"' || *cursor == '\\') g_string_append_c(escaped, '\\');
        g_string_append_c(escaped, *cursor);
    }
    return g_string_free(escaped, FALSE);
}

static bool keyword_ok(const char *line)
{
    char *reply = hyprland_request(line);
    bool ok = reply && strncmp(reply, "ok", 2) == 0;
    log_trace(L"Hyprland 命令 %hs → %hs", line, reply ? reply : "（没有回复）");
    free(reply);
    return ok;
}

bool hyprland_bind_hotkey(unsigned modmask, const char *modifiers, const char *key, const char *command)
{
    (void)modmask;
    if (!modifiers || !key || !command) return false;
    if (!hyprland_available()) return false;
    char line[2048];
    int length = snprintf(line, sizeof line, "keyword bindd %s, %s, DropShelf, exec, %s", modifiers, key, command);
    if (length > 0 && (size_t)length < sizeof line && keyword_ok(line)) return true;
    // Lua 配置的 Hyprland 不认 keyword，改用 eval 调配置 API。
    char *lua_key = lua_key_text(modifiers, key);
    char *escaped = lua_escape(command);
    length = snprintf(line, sizeof line, "eval hl.bind(\"%s\", hl.dsp.exec_cmd(\"%s\"), {description=\"DropShelf\"})",
        lua_key, escaped);
    free(lua_key);
    free(escaped);
    if (length <= 0 || (size_t)length >= sizeof line) return false;
    return keyword_ok(line);
}

/// modmask 转回配置文件里的修饰键写法。
static char *modifiers_from_mask(unsigned mask)
{
    char buffer[64];
    buffer[0] = 0;
    if (mask & 4u) strncat(buffer, "CTRL ", sizeof buffer - strlen(buffer) - 1);
    if (mask & 8u) strncat(buffer, "ALT ", sizeof buffer - strlen(buffer) - 1);
    if (mask & 1u) strncat(buffer, "SHIFT ", sizeof buffer - strlen(buffer) - 1);
    if (mask & 64u) strncat(buffer, "SUPER ", sizeof buffer - strlen(buffer) - 1);
    size_t length = strlen(buffer);
    if (length > 0 && buffer[length - 1] == ' ') buffer[length - 1] = 0;
    return strdup(buffer);
}

void hyprland_clear_own_binds(void)
{
    char *reply = hyprland_request("j/binds");
    if (!reply) return;
    wchar_t *text = wide_from_utf8(reply, strlen(reply));
    free(reply);
    if (!text) return;
    JsonValue *value = json_parse(text, 0);
    free(text);
    if (!value) return;
    if (value->type == JSON_ARRAY)
    {
        for (int index = 0; index < value->count; index++)
        {
            const JsonValue *entry = value->items[index];
            const JsonValue *description = json_get(entry, L"description");
            if (!description || description->type != JSON_STRING || wcscmp(description->string, bind_description) != 0) continue;
            const JsonValue *mask = json_get(entry, L"modmask");
            const JsonValue *name = json_get(entry, L"key");
            if (!name || name->type != JSON_STRING) continue;
            char *key = utf8_from_wide(name->string, NULL);
            if (!key) continue;
            char *modifiers = modifiers_from_mask(mask && mask->type == JSON_NUMBER ? (unsigned)mask->number : 0);
            hyprland_unbind_hotkey(modifiers, key);
            free(modifiers);
            free(key);
        }
    }
    json_free(value);
}

bool hyprland_unbind_hotkey(const char *modifiers, const char *key)
{
    if (!modifiers || !key || !hyprland_available()) return false;
    char line[512];
    int length = snprintf(line, sizeof line, "keyword unbind %s, %s", modifiers, key);
    if (length > 0 && (size_t)length < sizeof line && keyword_ok(line)) return true;
    char *lua_key = lua_key_text(modifiers, key);
    length = snprintf(line, sizeof line, "eval hl.unbind(\"%s\")", lua_key);
    free(lua_key);
    if (length <= 0 || (size_t)length >= sizeof line) return false;
    return keyword_ok(line);
}

bool hyprland_float_windows(const char *app_id)
{
    if (!app_id || !*app_id || !hyprland_available()) return false;
    char line[1024];
    // Lua 配置只能走 eval（和快捷键的兜底一样）；旧的经典配置没有 hl，会报错，再退回 keyword。
    char *escaped = lua_escape(app_id);
    if (escaped)
    {
        int length = snprintf(line, sizeof line,
            "eval hl.window_rule({ match = { class = \"^%s$\" }, float = true })", escaped);
        free(escaped);
        if (length > 0 && (size_t)length < sizeof line && keyword_ok(line)) return true;
    }
    // 0.5x 起 windowrule 统一成一条；0.4x 只认 windowrulev2。
    int length = snprintf(line, sizeof line, "keyword windowrule float, class:^(%s)$", app_id);
    if (length > 0 && (size_t)length < sizeof line && keyword_ok(line)) return true;
    length = snprintf(line, sizeof line, "keyword windowrulev2 float, class:^(%s)$", app_id);
    if (length > 0 && (size_t)length < sizeof line && keyword_ok(line)) return true;
    return false;
}

int hyprland_monitors(HyprMonitor *out, int max)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static HyprMonitor cached[8];
    static int cached_count;
    static unsigned long long cached_at;
    static bool cached_once;
    if (!out || max <= 0) return 0;
    unsigned long long now = now_milliseconds();
    pthread_mutex_lock(&lock);
    // 两秒内用上次的结果：工作区只在插拔显示器和改布局时变，问得太勤没有必要。
    if (!cached_once || now - cached_at >= 2000)
    {
        hyprland_monitors_free(cached, cached_count);
        cached_count = 0;
        char *reply = hyprland_available() ? hyprland_request("j/monitors") : NULL;
        if (reply)
        {
            wchar_t *text = wide_from_utf8(reply, strlen(reply));
            free(reply);
            if (text)
            {
                cached_count = hyprland_parse_monitors(text, cached, (int)(sizeof cached / sizeof cached[0]));
                free(text);
            }
        }
        cached_once = true;
        cached_at = now;
    }
    int count = cached_count < max ? cached_count : max;
    for (int index = 0; index < count; index++)
    {
        out[index] = cached[index];
        out[index].name = wcs_dup(cached[index].name);
    }
    pthread_mutex_unlock(&lock);
    return count;
}