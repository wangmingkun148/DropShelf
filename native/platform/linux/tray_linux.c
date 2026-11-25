// 通知区域图标：StatusNotifierItem（KDE、quickshell、waybar 等都认）+ 标准 dbusmenu 菜单。
// 托盘图标点击打开主窗口，右键菜单里可以新建置物架或退出。
// 没有 StatusNotifierWatcher 时什么都不做，程序照常运行（Hyprland 裸桌面本来就没有托盘）。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tray.h"
#include "log.h"

#include <gio/gio.h>
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SNI_INTERFACE "org.kde.StatusNotifierItem"
#define SNI_PATH "/StatusNotifierItem"
#define WATCHER_BUS "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define MENU_INTERFACE "com.canonical.dbusmenu"
#define MENU_PATH "/MenuBar"
#define NOTIFICATION_BUS "org.freedesktop.Notifications"

#define MENU_SHOW 1
#define MENU_NEW_SHELF 2
#define MENU_SEPARATOR 3
#define MENU_QUIT 4

enum
{
    MENU_PROP_LABEL,
    MENU_PROP_ENABLED,
    MENU_PROP_VISIBLE,
    MENU_PROP_TYPE,
    MENU_PROP_COUNT
};

static const char *menu_property_names[MENU_PROP_COUNT] = { "label", "enabled", "visible", "type" };

static const char sni_xml[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierItem'>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='WindowId' type='i' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='OverlayIconName' type='s' access='read'/>"
    "  <property name='AttentionIconName' type='s' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <method name='Activate'>"
    "   <arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/>"
    "  </method>"
    "  <method name='SecondaryActivate'>"
    "   <arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/>"
    "  </method>"
    "  <method name='ContextMenu'>"
    "   <arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/>"
    "  </method>"
    "  <method name='Scroll'>"
    "   <arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/>"
    "  </method>"
    "  <signal name='NewTitle'/>"
    "  <signal name='NewIcon'/>"
    "  <signal name='NewToolTip'/>"
    "  <signal name='NewStatus'><arg type='s'/></signal>"
    " </interface>"
    "</node>";

static const char menu_xml[] =
    "<node>"
    " <interface name='com.canonical.dbusmenu'>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <method name='GetLayout'>"
    "   <arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/>"
    "  </method>"
    "  <method name='GetGroupProperties'>"
    "   <arg type='ai' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='a(ia{sv})' direction='out'/>"
    "  </method>"
    "  <method name='GetProperty'>"
    "   <arg type='i' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='v' direction='out'/>"
    "  </method>"
    "  <method name='Event'>"
    "   <arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/>"
    "   <arg type='u' direction='in'/>"
    "  </method>"
    "  <method name='EventGroup'>"
    "   <arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShow'>"
    "   <arg type='i' direction='in'/><arg type='b' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShowGroup'>"
    "   <arg type='ai' direction='in'/><arg type='ai' direction='out'/><arg type='ai' direction='out'/>"
    "  </method>"
    "  <signal name='LayoutUpdated'>"
    "   <arg type='u'/><arg type='i'/>"
    "  </signal>"
    "  <signal name='ItemsPropertiesUpdated'>"
    "   <arg type='a(ia{sv})'/><arg type='a(ias)'/>"
    "  </signal>"
    " </interface>"
    "</node>";

static GDBusConnection *connection;
static GDBusNodeInfo *sni_info;
static GDBusNodeInfo *menu_info;
static guint sni_id, menu_id;
static char *bus_name;
static TrayActions actions;
static bool created;
static unsigned menu_revision = 1;
static wchar_t *hotkey_text_cache;   // 托盘标签里显示的快捷键

/// 把宽字符串转成 UTF-8；没有时返回空串。
static char *narrow(const wchar_t *text)
{
    if (!text) return g_strdup("");
    char *result = utf8_from_wide(text, NULL);
    return result ? result : g_strdup("");
}

static char *show_label(void)
{
    return g_strdup("显示 DropShelf");
}

static char *new_shelf_label(void)
{
    wchar_t *hotkey = actions.hotkey_text ? actions.hotkey_text() : NULL;
    char *result;
    if (hotkey && *hotkey)
    {
        char *text = narrow(hotkey);
        result = g_strdup_printf("新建置物架（%s）", text);
        g_free(text);
    }
    else result = g_strdup("新建置物架");
    free(hotkey);
    return result;
}

/// 找一个能用的托盘图标文件：装好的图标主题目录，或者开发目录里的原始 PNG。
static char *icon_path(void)
{
    const char *name = "io.github.wangmingkun148.DropShelf.png";

    GPtrArray *directories = g_ptr_array_new_with_free_func(g_free);
    const char *home = g_get_user_data_dir();
    if (home) g_ptr_array_add(directories, g_build_filename(home, "icons", "hicolor", NULL));
    const char *const *system = g_get_system_data_dirs();
    for (int index = 0; system && system[index]; index++)
        g_ptr_array_add(directories, g_build_filename(system[index], "icons", "hicolor", NULL));

    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe)
    {
        char *folder = g_path_get_dirname(exe);
        // 开发时在仓库的 linux/icons 下；装好（包括 AppImage）后在 ../share/icons 下。
        // 装好的程序不经过图标主题缓存，托盘只能靠 IconPixmap 显示，所以这里一定要找到文件。
        g_ptr_array_add(directories, g_build_filename(folder, "..", "share", "icons", "hicolor", NULL));
        g_ptr_array_add(directories, g_build_filename(folder, "..", "icons", "hicolor", NULL));
        g_ptr_array_add(directories, g_build_filename(folder, "..", "..", "linux", "icons", "hicolor", NULL));
        g_free(folder);
        g_free(exe);
    }

    static const int sizes[] = { 32, 48, 64, 128, 256 };
    char *found = NULL;
    for (unsigned at = 0; at < directories->len && !found; at++)
    {
        for (unsigned index = 0; index < G_N_ELEMENTS(sizes) && !found; index++)
        {
            char folder[32];
            snprintf(folder, sizeof folder, "%dx%d", sizes[index], sizes[index]);
            char *candidate = g_build_filename(directories->pdata[at], folder, "apps", name, NULL);
            log_trace(L"找托盘图标：%hs → %ls", candidate,
                g_file_test(candidate, G_FILE_TEST_IS_REGULAR) ? L"有" : L"没有");
            if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) found = candidate;
            else g_free(candidate);
        }
    }
    g_ptr_array_free(directories, TRUE);
    return found;
}

/// IconPixmap 属性的值：32 位的 ARGB 像素，按尺寸从大到小给几个。
/// 状态栏（quickshell 等）只在 IconName 为空时才看 IconPixmap，而 AppImage 里的图标
/// 不在系统图标主题里，所以 IconName 留空，全靠这里。
static GVariant *icon_pixmap(void)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    char *path = icon_path();
    if (path)
    {
        static const int sizes[] = { 32, 64, 128 };
        for (unsigned at = 0; at < G_N_ELEMENTS(sizes); at++)
        {
            GError *error = NULL;
            GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, sizes[at], sizes[at], TRUE, &error);
            if (!pixbuf)
            {
                if (error) g_error_free(error);
                continue;
            }
            int width = gdk_pixbuf_get_width(pixbuf);
            int height = gdk_pixbuf_get_height(pixbuf);
            int channels = gdk_pixbuf_get_n_channels(pixbuf);
            int stride = gdk_pixbuf_get_rowstride(pixbuf);
            const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
            bool has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
            gsize byte_count = (gsize)width * (gsize)height * 4;
            guint8 *data = g_malloc0(byte_count);
            for (int y = 0; y < height; y++)
            {
                const guchar *row = pixels + (gsize)y * (gsize)stride;
                for (int x = 0; x < width; x++)
                {
                    const guchar *pixel = row + (gsize)x * (gsize)channels;
                    guint8 alpha = has_alpha ? pixel[3] : 255;
                    guint8 *out = data + ((gsize)y * (gsize)width + (gsize)x) * 4;
                    // 网络字节序的 ARGB
                    out[0] = alpha;
                    out[1] = (guint8)(pixel[0] * alpha / 255);
                    out[2] = (guint8)(pixel[1] * alpha / 255);
                    out[3] = (guint8)(pixel[2] * alpha / 255);
                }
            }
            GBytes *bytes = g_bytes_new_take(data, byte_count);
            g_variant_builder_add(&builder, "(ii@ay)", width, height,
                g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, TRUE));
            g_bytes_unref(bytes);
            g_object_unref(pixbuf);
        }
        g_free(path);
    }
    return g_variant_builder_end(&builder);
}

// ───────────── 菜单 ─────────────

static bool menu_item_exists(int id)
{
    return id == 0 || id == MENU_SHOW || id == MENU_NEW_SHELF || id == MENU_SEPARATOR || id == MENU_QUIT;
}

static bool menu_is_separator(int id)
{
    return id == MENU_SEPARATOR;
}

static char *menu_label(int id)
{
    switch (id)
    {
        case 0: return NULL;
        case MENU_SHOW: return show_label();
        case MENU_NEW_SHELF: return new_shelf_label();
        case MENU_SEPARATOR: return NULL;
        case MENU_QUIT: return g_strdup("退出");
        default: return NULL;
    }
}

/// 一个菜单项的属性字典；请求了部分属性时只给这些。
static GVariant *menu_properties(int id, const char *const *requested, int requested_count)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    bool all = !requested || requested_count == 0;
    char *label = menu_label(id);
    for (int index = 0; index < MENU_PROP_COUNT; index++)
    {
        const char *name = menu_property_names[index];
        bool wanted = all;
        for (int at = 0; at < requested_count && !wanted; at++)
            if (requested[at] && strcmp(requested[at], name) == 0) wanted = true;
        if (!wanted) continue;
        switch (index)
        {
            case MENU_PROP_LABEL:
                g_variant_builder_add(&builder, "{sv}", name,
                    g_variant_new_string(id == 0 ? "root" : (label ? label : "")));
                break;
            case MENU_PROP_ENABLED:
                g_variant_builder_add(&builder, "{sv}", name, g_variant_new_boolean(true));
                break;
            case MENU_PROP_VISIBLE:
                g_variant_builder_add(&builder, "{sv}", name, g_variant_new_boolean(true));
                break;
            case MENU_PROP_TYPE:
                g_variant_builder_add(&builder, "{sv}", name,
                    g_variant_new_string(menu_is_separator(id) ? "separator" : "standard"));
                break;
        }
    }
    g_free(label);
    return g_variant_builder_end(&builder);
}

static GVariant *menu_layout_item(int id, const char *const *requested, int requested_count)
{
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (id == 0)
    {
        static const int items[] = { MENU_SHOW, MENU_NEW_SHELF, MENU_SEPARATOR, MENU_QUIT };
        for (unsigned index = 0; index < G_N_ELEMENTS(items); index++)
            g_variant_builder_add(&children, "v", menu_layout_item(items[index], requested, requested_count));
    }
    return g_variant_new("(i@a{sv}@av)", id, menu_properties(id, requested, requested_count),
        g_variant_builder_end(&children));
}

/// 菜单里的一项被点了。
static void menu_event(int id)
{
    switch (id)
    {
        case MENU_SHOW:
            if (actions.show_window) actions.show_window();
            break;
        case MENU_NEW_SHELF:
            if (actions.new_shelf) actions.new_shelf();
            break;
        case MENU_QUIT:
            if (actions.quit) actions.quit();
            break;
    }
}

static void on_menu_method(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
    const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)data;
    if (strcmp(method, "GetLayout") == 0)
    {
        gint32 parent = 0, depth = 0;
        const char *const *requested = NULL;
        gsize requested_count = 0;
        GVariant *names = NULL;
        g_variant_get(parameters, "(ii@as)", &parent, &depth, &names);
        if (g_variant_n_children(names) > 0)
        {
            requested = g_new0(const char *, g_variant_n_children(names) + 1);
            for (gsize index = 0; index < g_variant_n_children(names); index++)
            {
                const char *name = NULL;
                g_variant_get_child(names, index, "&s", &name);
                ((const char **)requested)[index] = name;
            }
            requested_count = g_variant_n_children(names);
        }
        GVariant *layout = menu_layout_item(parent, requested, (int)requested_count);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(u@(ia{sv}av))", menu_revision, layout));
        g_free((void *)requested);
        g_variant_unref(names);
        return;
    }
    if (strcmp(method, "GetGroupProperties") == 0)
    {
        GVariant *ids = NULL, *names = NULL;
        g_variant_get(parameters, "(@ai@as)", &ids, &names);
        gsize requested_count = g_variant_n_children(names);
        const char **requested = g_new0(const char *, requested_count + 1);
        for (gsize index = 0; index < requested_count; index++)
            g_variant_get_child(names, index, "&s", &requested[index]);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
        for (gsize index = 0; index < g_variant_n_children(ids); index++)
        {
            gint32 id = 0;
            g_variant_get_child(ids, index, "i", &id);
            if (!menu_item_exists(id)) continue;
            g_variant_builder_add(&builder, "(i@a{sv})", id,
                menu_properties(id, requested, (int)requested_count));
        }
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&builder)));
        g_free((void *)requested);
        g_variant_unref(ids);
        g_variant_unref(names);
        return;
    }
    if (strcmp(method, "GetProperty") == 0)
    {
        gint32 id = 0;
        const char *name = NULL;
        g_variant_get(parameters, "(i&s)", &id, &name);
        if (!menu_item_exists(id))
        {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "没有这一项");
            return;
        }
        const char *requested[1] = { name };
        GVariant *properties = menu_properties(id, requested, 1);
        GVariant *value = g_variant_lookup_value(properties, name, NULL);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(v)", value ? value : g_variant_new_string("")));
        if (value) g_variant_unref(value);
        g_variant_unref(properties);
        return;
    }
    if (strcmp(method, "Event") == 0)
    {
        gint32 id = 0;
        const char *event = NULL;
        GVariant *value = NULL;
        guint32 timestamp = 0;
        g_variant_get(parameters, "(i&s@vu)", &id, &event, &value, &timestamp);
        if (strcmp(event, "clicked") == 0 && menu_item_exists(id)) menu_event(id);
        g_variant_unref(value);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method, "EventGroup") == 0)
    {
        GVariant *updates = NULL;
        g_variant_get(parameters, "(@a(isvu))", &updates);
        for (gsize index = 0; index < g_variant_n_children(updates); index++)
        {
            gint32 id = 0;
            const char *event = NULL;
            guint32 timestamp = 0;
            g_variant_get_child(updates, index, "(i&svu)", &id, &event, NULL, &timestamp);
            if (strcmp(event, "clicked") == 0 && menu_item_exists(id)) menu_event(id);
        }
        g_variant_unref(updates);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0)));
        return;
    }
    if (strcmp(method, "AboutToShow") == 0)
    {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
        return;
    }
    if (strcmp(method, "AboutToShowGroup") == 0)
    {
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0),
                g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0)));
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "没有这个方法");
}

static GVariant *on_menu_property(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
    const char *property, GError **error, gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)data;
    (void)error;
    if (strcmp(property, "Version") == 0) return g_variant_new_uint32(3);
    if (strcmp(property, "Status") == 0) return g_variant_new_string("normal");
    if (strcmp(property, "TextDirection") == 0) return g_variant_new_string("ltr");
    if (strcmp(property, "IconThemePath") == 0) return g_variant_new_strv(NULL, 0);
    return NULL;
}

// ───────────── 托盘图标 ─────────────

static void activate_tray(void)
{
    if (actions.show_window) actions.show_window();
}

static void on_sni_method(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
    const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)parameters;
    (void)data;
    if (strcmp(method, "Activate") == 0 || strcmp(method, "SecondaryActivate") == 0) activate_tray();
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *on_sni_property(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
    const char *property, GError **error, gpointer data)
{
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)data;
    (void)error;
    if (strcmp(property, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (strcmp(property, "Id") == 0) return g_variant_new_string("dropshelf");
    if (strcmp(property, "Title") == 0) return g_variant_new_string("DropShelf");
    if (strcmp(property, "Status") == 0) return g_variant_new_string("Active");
    if (strcmp(property, "WindowId") == 0) return g_variant_new_int32(0);
    // 留空才让状态栏用下面的 IconPixmap；图标名字在 AppImage 里查不到，写上只会显示成粉黑格子。
    if (strcmp(property, "IconName") == 0) return g_variant_new_string("");
    if (strcmp(property, "IconPixmap") == 0) return icon_pixmap();
    if (strcmp(property, "OverlayIconName") == 0) return g_variant_new_string("");
    if (strcmp(property, "AttentionIconName") == 0) return g_variant_new_string("");
    if (strcmp(property, "ItemIsMenu") == 0) return g_variant_new_boolean(false);
    if (strcmp(property, "Menu") == 0) return g_variant_new_object_path(MENU_PATH);
    if (strcmp(property, "ToolTip") == 0)
    {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(s@a(iiay)ss)", "", g_variant_builder_end(&builder), "DropShelf", "");
    }
    return NULL;
}

static void tray_register(void)
{
    if (!connection) return;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(connection, WATCHER_BUS, WATCHER_PATH, WATCHER_BUS,
        "RegisterStatusNotifierItem", g_variant_new("(s)", bus_name), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (!reply)
    {
        log_info(L"通知区域不可用，托盘图标没有放上去");
        if (error) g_error_free(error);
        return;
    }
    g_variant_unref(reply);
    log_info(L"托盘图标已放上通知区域");
}

// ───────────── 通知 ─────────────

/// 发一条桌面通知；没有通知服务时安静地放弃。
static void send_notification(const wchar_t *title, const wchar_t *body)
{
    GDBusConnection *bus = connection ? connection : g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return;
    char *wide_title = narrow(title), *wide_body = narrow(body);
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(bus, NOTIFICATION_BUS, "/org/freedesktop/Notifications",
        NOTIFICATION_BUS, "Notify",
        g_variant_new("(susss@as@a{sv}i)", "DropShelf", 0, "io.github.wangmingkun148.DropShelf",
            wide_title, wide_body, g_variant_new_strv(NULL, 0), g_variant_builder_end(&hints), 5000),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (reply) g_variant_unref(reply);
    if (error) g_error_free(error);
    if (bus != connection) g_object_unref(bus);
    g_free(wide_title);
    g_free(wide_body);
}

// ───────────── 对外的接口 ─────────────

void tray_create(HWND window, const TrayActions *tray_actions)
{
    (void)window;
    if (created) return;
    if (tray_actions) actions = *tray_actions;
    GError *error = NULL;
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!connection)
    {
        log_info(L"连不上会话总线，托盘图标不可用");
        if (error) g_error_free(error);
        return;
    }

    bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
    GVariant *owned = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", bus_name, 0u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (!owned)
    {
        log_info(L"申请托盘总线名失败：%hs", error ? error->message : "");
        if (error) g_error_free(error);
        g_clear_object(&connection);
        g_clear_pointer(&bus_name, g_free);
        return;
    }
    g_variant_unref(owned);

    sni_info = g_dbus_node_info_new_for_xml(sni_xml, NULL);
    menu_info = g_dbus_node_info_new_for_xml(menu_xml, NULL);
    static const GDBusInterfaceVTable sni_vtable = { on_sni_method, on_sni_property, NULL, { 0 } };
    static const GDBusInterfaceVTable menu_vtable = { on_menu_method, on_menu_property, NULL, { 0 } };
    sni_id = g_dbus_connection_register_object(connection, SNI_PATH, sni_info->interfaces[0], &sni_vtable, NULL, NULL, NULL);
    menu_id = g_dbus_connection_register_object(connection, MENU_PATH, menu_info->interfaces[0], &menu_vtable, NULL, NULL, NULL);
    if (!sni_id || !menu_id)
    {
        log_info(L"托盘对象的注册失败");
        tray_destroy();
        return;
    }
    created = true;
    tray_register();
}

bool tray_handle_message(UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)message;
    (void)wparam;
    (void)lparam;
    return false;
}

void tray_refresh_text(void)
{
    free(hotkey_text_cache);
    hotkey_text_cache = NULL;
    if (!created || !connection) return;
    // 菜单标签是现算的，通知宿主刷新一下就行。
    GError *error = NULL;
    g_dbus_connection_emit_signal(connection, NULL, MENU_PATH, MENU_INTERFACE, "LayoutUpdated",
        g_variant_new("(ui)", menu_revision++, 0), &error);
    if (error) g_error_free(error);
}

void tray_show_still_running_hint(void)
{
    send_notification(L"DropShelf 还在运行", L"可以从通知区域的图标重新打开主窗口。");
}

void tray_show_notice(const wchar_t *title, const wchar_t *body)
{
    send_notification(title, body);
}

void tray_destroy(void)
{
    if (connection)
    {
        if (sni_id) g_dbus_connection_unregister_object(connection, sni_id);
        if (menu_id) g_dbus_connection_unregister_object(connection, menu_id);
        sni_id = menu_id = 0;
    }
    if (sni_info) g_dbus_node_info_unref(sni_info);
    if (menu_info) g_dbus_node_info_unref(menu_info);
    sni_info = menu_info = NULL;
    if (connection)
    {
        if (bus_name)
        {
            GError *error = NULL;
            GVariant *reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", bus_name), G_VARIANT_TYPE("(u)"),
                G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
            if (reply) g_variant_unref(reply);
            if (error) g_error_free(error);
        }
        g_object_unref(connection);
        connection = NULL;
    }
    g_clear_pointer(&bus_name, g_free);
    free(hotkey_text_cache);
    hotkey_text_cache = NULL;
    created = false;
}