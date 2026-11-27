// 全局快捷键的 Linux 实现：走 xdg-desktop-portal 的 GlobalShortcuts（Hyprland 的 portal 支持）。
// 协议是异步的：调用返回一个请求句柄，真正的结果从 org.freedesktop.portal.Request 的 Response 信号里来。
// 拿不到 portal 时只记日志，用户可以自己在 Hyprland 配置里写 bind 调 dropshelf --shelf。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hotkey.h"
#include "hyprland_linux.h"
#include "log.h"
#include "strings.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL_BUS "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_INTERFACE "org.freedesktop.portal.GlobalShortcuts"
#define REQUEST_INTERFACE "org.freedesktop.portal.Request"
#define SESSION_INTERFACE "org.freedesktop.portal.Session"
#define SHORTCUT_ID "shelf"

static GDBusConnection *connection;
static void (*pressed_callback)(void);
static bool paused;
static HotkeyGesture current_gesture;
static char *session_handle;
static guint activation_subscription;
static unsigned request_counter;
static unsigned session_generation;
static bool hyprland_bound;      // 这个组合键是我们在 Hyprland 运行时绑的，退出时要撤掉
static char *bound_modifiers, *bound_key;

typedef struct SessionRequest
{
    unsigned generation;
} SessionRequest;

typedef void (*PortalResponse)(GVariant *results, bool ok, void *context);

typedef struct Request
{
    char *path;
    guint subscription;
    PortalResponse callback;
    void *context;
} Request;

// ───────────── 请求和回复 ─────────────

static char *new_token(void)
{
    return g_strdup_printf("dropshelf_%u_%u", (unsigned)getpid(), ++request_counter);
}

/// 请求路径是 /org/freedesktop/portal/desktop/request/<发送方>/<token>，发送方是去掉冒号、点换成下划线的连接名。
static char *request_path(const char *token)
{
    const char *unique = connection ? g_dbus_connection_get_unique_name(connection) : NULL;
    if (!unique) return NULL;
    GString *sender = g_string_new(unique[0] == ':' ? unique + 1 : unique);
    for (char *cursor = sender->str; *cursor; cursor++)
        if (*cursor == '.') *cursor = '_';
    char *path = g_strdup_printf("%s/request/%s/%s", PORTAL_PATH, sender->str, token);
    g_string_free(sender, TRUE);
    return path;
}

static void on_response(GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
    GVariant *parameters, gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    Request *request = data;
    guint32 response = 2;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    if (request->subscription) g_dbus_connection_signal_unsubscribe(connection, request->subscription);
    PortalResponse callback = request->callback;
    void *context = request->context;
    g_free(request->path);
    g_free(request);
    if (callback) callback(results, response == 0, context);
    g_variant_unref(results);
}

/// 调一个返回请求句柄的 portal 方法，结果到了再回调。
/// token 必须和调用参数里的 handle_token 一样，回复才会发到我们对上的路径。
static void call_with_response(const char *method, const char *token, GVariant *parameters, PortalResponse callback,
    void *context)
{
    if (!connection) return;
    char *path = request_path(token);
    if (!path) return;
    Request *request = g_new0(Request, 1);
    request->path = path;
    request->callback = callback;
    request->context = context;
    request->subscription = g_dbus_connection_signal_subscribe(connection, PORTAL_BUS, REQUEST_INTERFACE, "Response", path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_response, request, NULL);
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(connection, PORTAL_BUS, PORTAL_PATH, PORTAL_INTERFACE, method, parameters,
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error)
    {
        log_info(L"快捷键 portal 调用 %hs 失败：%hs", method, error->message);
        g_error_free(error);
        g_dbus_connection_signal_unsubscribe(connection, request->subscription);
        g_free(request->path);
        g_free(request);
    }
    else
    {
        const char *handle = NULL;
        g_variant_get(result, "(&o)", &handle);
        log_trace(L"门户 %hs 的请求句柄：%hs（监听 %hs）", method, handle ? handle : "", path);
        g_variant_unref(result);
    }
}

// ───────────── 快捷键写法 ─────────────

/// 虚拟键码转成 XKB 的键名（portal 用这个写法）。
static const char *key_name(unsigned key)
{
    static char buffer[8];
    if (key >= 'A' && key <= 'Z')
    {
        buffer[0] = (char)('a' + (key - 'A'));
        buffer[1] = 0;
        return buffer;
    }
    if (key >= '0' && key <= '9')
    {
        buffer[0] = (char)key;
        buffer[1] = 0;
        return buffer;
    }
    if (key >= VK_F1 && key <= VK_F24)
    {
        snprintf(buffer, sizeof buffer, "F%u", key - VK_F1 + 1);
        return buffer;
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
    {
        snprintf(buffer, sizeof buffer, "KP_%u", key - VK_NUMPAD0);
        return buffer;
    }
    switch (key)
    {
        case VK_SPACE: return "space";
        case VK_RETURN: return "Return";
        case VK_ESCAPE: return "Escape";
        case VK_TAB: return "Tab";
        case VK_BACK: return "BackSpace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "Page_Up";
        case VK_NEXT: return "Page_Down";
        case VK_LEFT: return "Left";
        case VK_UP: return "Up";
        case VK_RIGHT: return "Right";
        case VK_DOWN: return "Down";
        default: return NULL;
    }
}

/// 拼成 portal 的触发写法，例如 "CTRL+SHIFT+space"。
static char *trigger_text(HotkeyGesture gesture)
{
    const char *key = key_name(gesture.key);
    if (!key) return NULL;
    GString *result = g_string_new(NULL);
    if (gesture.modifiers & MOD_CONTROL) g_string_append(result, "CTRL+");
    if (gesture.modifiers & MOD_ALT) g_string_append(result, "ALT+");
    if (gesture.modifiers & MOD_SHIFT) g_string_append(result, "SHIFT+");
    if (gesture.modifiers & MOD_WIN) g_string_append(result, "LOGO+");
    g_string_append(result, key);
    return g_string_free(result, FALSE);
}

// ───────────── Hyprland 的运行时绑定 ─────────────
// Hyprland 的 GlobalShortcuts 只登记名字，按键仍要用户绑；为了按键直接可用，用 keyword bind
// 把组合键绑到“启动本程序并打开置物架”。只在运行时生效，不改用户的配置文件。

static unsigned hyprland_modmask(unsigned modifiers)
{
    unsigned mask = 0;
    if (modifiers & MOD_SHIFT) mask |= 1u;      // SHIFT
    if (modifiers & MOD_CONTROL) mask |= 4u;    // CTRL
    if (modifiers & MOD_ALT) mask |= 8u;        // ALT
    if (modifiers & MOD_WIN) mask |= 64u;       // SUPER
    return mask;
}

static char *modifiers_text(HotkeyGesture gesture)
{
    GString *text = g_string_new(NULL);
    if (gesture.modifiers & MOD_CONTROL) g_string_append(text, "CTRL ");
    if (gesture.modifiers & MOD_ALT) g_string_append(text, "ALT ");
    if (gesture.modifiers & MOD_SHIFT) g_string_append(text, "SHIFT ");
    if (gesture.modifiers & MOD_WIN) g_string_append(text, "SUPER ");
    if (text->len > 0) g_string_truncate(text, text->len - 1);
    return g_string_free(text, FALSE);
}

/// Hyprland 的按键写法：全部大写（和它自己列出的绑定一致，匹配时不分大小写）。
static char *hyprland_key_text(HotkeyGesture gesture)
{
    const char *name = key_name(gesture.key);
    if (!name) return NULL;
    char *text = g_strdup(name);
    for (char *cursor = text; *cursor; cursor++) *cursor = (char)g_ascii_toupper((unsigned char)*cursor);
    return text;
}

/// 按下快捷键时执行的命令：就是本程序带 --shelf 再启动一次（单实例会转给正在运行的自己）。
static char *executable_command(void)
{
    char path[4096];
    ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
    if (length <= 0) return g_strdup("dropshelf --shelf");
    path[length] = 0;
    return g_strdup_printf("'%s' --shelf", path);
}

// ───────────── 会话和绑定 ─────────────

static void close_session(void)
{
    if (activation_subscription)
    {
        g_dbus_connection_signal_unsubscribe(connection, activation_subscription);
        activation_subscription = 0;
    }
    if (!session_handle) return;
    GError *error = NULL;
    g_dbus_connection_call_sync(connection, PORTAL_BUS, session_handle, SESSION_INTERFACE, "Close", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) g_error_free(error);
    g_free(session_handle);
    session_handle = NULL;
}

static void on_activated(GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
    GVariant *parameters, gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    (void)data;
    const char *session = NULL;
    const char *shortcut = NULL;
    guint64 timestamp = 0;
    GVariant *options = NULL;
    g_variant_get(parameters, "(&o&st@a{sv})", &session, &shortcut, &timestamp, &options);
    g_variant_unref(options);
    if (paused) return;
    if (pressed_callback) pressed_callback();
}

static void shortcuts_bound(GVariant *results, bool ok, void *context)
{
    (void)context;
    if (!ok)
    {
        log_info(L"系统没有绑定全局快捷键，可以在 Hyprland 配置里加 bind = ..., exec, dropshelf --shelf");
        return;
    }
    // 回复里带每个快捷键最终用的触发组合，方便对照设置。
    GVariant *shortcuts = g_variant_lookup_value(results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
    if (!shortcuts) return;
    GVariantIter iter;
    g_variant_iter_init(&iter, shortcuts);
    const char *id = NULL;
    GVariant *details = NULL;
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &details))
    {
        (void)id;
        const char *trigger = NULL;
        if (g_variant_lookup(details, "trigger_description", "&s", &trigger) ||
            g_variant_lookup(details, "preferred_trigger", "&s", &trigger))
            log_info(L"全局快捷键已绑定：%hs", trigger);
        g_variant_unref(details);
    }
    g_variant_unref(shortcuts);
}

static void bind_shortcuts(void)
{
    if (!session_handle) return;
    char *trigger = trigger_text(current_gesture);
    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
    g_variant_builder_open(&shortcuts, G_VARIANT_TYPE("(sa{sv})"));
    g_variant_builder_add(&shortcuts, "s", SHORTCUT_ID);
    g_variant_builder_open(&shortcuts, G_VARIANT_TYPE_VARDICT);
    char *description = utf8_from_wide(S(STR_OpenShelf), NULL);
    g_variant_builder_add(&shortcuts, "{sv}", "description", g_variant_new_string(description ? description : "Open shelf"));
    free(description);
    if (trigger) g_variant_builder_add(&shortcuts, "{sv}", "preferred_trigger", g_variant_new_string(trigger));
    g_variant_builder_close(&shortcuts);
    g_variant_builder_close(&shortcuts);
    char *token = new_token();
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    g_free(trigger);
    call_with_response("BindShortcuts", token,
        g_variant_new("(oa(sa{sv})sa{sv})", session_handle, &shortcuts, "", &options), shortcuts_bound, NULL);
    g_free(token);
    // 绑定结果从绑定的响应里看；之后按下时是 Activated 信号。
    activation_subscription = g_dbus_connection_signal_subscribe(connection, PORTAL_BUS, PORTAL_INTERFACE, "Activated",
        session_handle, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_activated, NULL, NULL);
}

static void session_created(GVariant *results, bool ok, void *context)
{
    SessionRequest *request = context;
    bool current = request && request->generation == session_generation;
    free(request);
    if (!current) return;
    log_trace(L"门户的快捷键会话回复：%ls", ok ? L"成功" : L"被拒绝");
    if (!ok)
    {
        log_info(L"快捷键会话没有建立，可能被系统拒绝");
        return;
    }
    const char *handle = NULL;
    if (!g_variant_lookup(results, "session_handle", "&s", &handle) || !handle)
    {
        log_info(L"快捷键会话的回复里没有句柄");
        return;
    }
    session_handle = g_strdup(handle);
    bind_shortcuts();
}

static void create_session(void)
{
    char *token = new_token();
    char *session_token = g_strdup_printf("dropshelf_%u", (unsigned)getpid());
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token));
    g_free(session_token);
    SessionRequest *request = g_new0(SessionRequest, 1);
    request->generation = session_generation;
    log_trace(L"向门户请求快捷键会话");
    call_with_response("CreateSession", token, g_variant_new("(a{sv})", &options), session_created, request);
    g_free(token);
}

static bool ensure_connection(void)
{
    if (connection) return true;
    GError *error = NULL;
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!connection)
    {
        log_info(L"连不上会话总线，快捷键不可用：%hs", error ? error->message : "");
        if (error) g_error_free(error);
        return false;
    }
    // 先看一眼 portal 有没有 GlobalShortcuts，没有就别走后面那一串了。
    GDBusNodeInfo *info = NULL;
    GVariant *reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", PORTAL_BUS), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
    if (!reply)
    {
        log_info(L"没有 xdg-desktop-portal，快捷键要自己在 Hyprland 配置里绑定");
        if (error) g_error_free(error);
        g_clear_object(&connection);
        return false;
    }
    g_variant_unref(reply);
    (void)info;
    return true;
}

// ───────────── 对外的接口 ─────────────

void hotkey_service_start(HWND window, void (*pressed)(void))
{
    (void)window;
    pressed_callback = pressed;
}

/// 撤掉运行时绑定，记下的组合键也一起清掉。用户自己写进配置的绑定不动。
static void release_hyprland_bind(void)
{
    if (hyprland_bound && bound_modifiers && bound_key) hyprland_unbind_hotkey(bound_modifiers, bound_key);
    hyprland_bound = false;
    g_clear_pointer(&bound_modifiers, g_free);
    g_clear_pointer(&bound_key, g_free);
}

bool hotkey_service_register(HotkeyGesture gesture)
{
    current_gesture = gesture;
    session_generation++;
    close_session();
    release_hyprland_bind();
    log_trace(L"注册全局快捷键：修饰键 %u，键 %u", gesture.modifiers, gesture.key);
    if (hotkey_is_empty(gesture)) return true;
    if (!hotkey_is_valid(gesture)) return false;
    if (hyprland_available())
    {
        // 上次没正常退出时留下的绑定先清掉，免得换了组合键旧的一直留着。
        hyprland_clear_own_binds();
        char *key = hyprland_key_text(gesture);
        char *command = executable_command();
        HyprBindState state = key && command ?
            hyprland_bind_state(hyprland_modmask(gesture.modifiers), key, command) : HYPR_BIND_FREE;
        if (state == HYPR_BIND_OURS)
        {
            // 上次运行的绑定还在（或用户写进了配置），已经能用。
            log_info(L"全局快捷键已经绑好，直接沿用");
            g_free(key);
            g_free(command);
            paused = false;
            return true;
        }
        if (state == HYPR_BIND_OTHER)
        {
            log_info(L"设置里的全局快捷键已经被其他功能占用，请换一个组合键");
            g_free(key);
            g_free(command);
            return false;
        }
        char *modifiers = modifiers_text(gesture);
        hyprland_bound = hyprland_bind_hotkey(hyprland_modmask(gesture.modifiers), modifiers, key, command);
        g_free(key);
        g_free(command);
        if (hyprland_bound)
        {
            bound_modifiers = modifiers;
            bound_key = hyprland_key_text(gesture);
            log_info(L"全局快捷键已在 Hyprland 里绑定");
            paused = false;
            return true;
        }
        g_free(modifiers);
    }
    // 别的合成器（或者 Hyprland 绑定失败）走门户；Hyprland 上门户只登记名字，按键仍要用户自己绑。
    if (!ensure_connection()) return false;
    create_session();
    paused = false;
    return true;
}

void hotkey_service_set_paused(bool value)
{
    if (value == paused) return;
    paused = value;
    // 暂停时撤掉运行时绑定，把按键让给前台的游戏；恢复时再绑回去。
    // 门户那边的 Activated 由 paused 标志挡掉。
    if (hyprland_bound)
    {
        if (paused) hyprland_unbind_hotkey(bound_modifiers, bound_key);
        else
        {
            char *command = executable_command();
            hyprland_bind_hotkey(hyprland_modmask(current_gesture.modifiers), bound_modifiers, bound_key, command);
            g_free(command);
        }
    }
}

bool hotkey_service_handle_message(UINT message, WPARAM wparam)
{
    (void)message;
    (void)wparam;
    return false;
}

void hotkey_service_stop(void)
{
    release_hyprland_bind();
    close_session();
    pressed_callback = NULL;
    if (connection)
    {
        g_object_unref(connection);
        connection = NULL;
    }
}