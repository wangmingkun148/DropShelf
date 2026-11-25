// X11（XWayland）应用发起的拖放接不进来：Hyprland 只把 Wayland 源的拖动转给 X11 应用，
// 反方向没有实现——X11 源拖动时光标下的原生 Wayland 窗口（探针、置物架）收不到任何事件。
// 这里用一个看不见的 X11 输入窗口铺满整个屏幕，用 Xdnd 协议把拖动接住，
// 读出的文字、文件或图片交给置物架，和 Wayland 源走同一套处理。
// 窗口只在探针亮着、或者光标落在置物架上、并且确实有 X11 拖动的时候挂上去，别的拖动一概不碰。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "xdnd_linux.h"
#include "gesture_linux.h"
#include "log.h"
#include "platform_linux.h"

#include <gdk/gdk.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

/// 接放窗口建好时的初始大小；挂上去之前会改成整个 X 屏幕那么大（见 poll_tick）。
#define PROBE_SIZE 320
/// 多久看一次光标和置物架的位置：置物架在动、光标也在动，窗口要跟得上。
#define POLL_MILLISECONDS 40
/// 看不到全局按键时，探针只活这么久（和 Wayland 探针一样）。
#define PROBE_BLIND_TIMEOUT_MICROSECONDS 900000
/// 能看到全局左键时，按住就一直等着。
#define PROBE_TIMEOUT_MICROSECONDS 6000000
/// 拖动最多接这么久：来源程序崩了、或者松手事件丢了，也不会一直挂着。
#define DRAG_TIMEOUT_MICROSECONDS 30000000
/// 看到左键松开后再等一会儿：XdndDrop 通常和松开同时到，给合成器一点时间。
#define RELEASE_GRACE_MICROSECONDS 800000
/// 等来源程序送数据的上限。
#define READ_TIMEOUT_MILLISECONDS 10000
#define MAXIMUM_DATA ((gsize)64 * 1024 * 1024)
/// 我们支持的最高 Xdnd 版本。
#define XDND_VERSION 5
/// 我们要的 XdndSelection 数据先放在自己窗口的这个属性上。
#define DATA_PROPERTY "DROPSHELF_DND_DATA"

struct XdndSink
{
    void *context;
    void (*enter)(void *context, XdndKind kind, double x, double y);
    void (*motion)(void *context, XdndKind kind, double x, double y);
    void (*leave)(void *context);
    void (*drop)(void *context, const XdndPayload *payload, double x, double y);
    bool (*frame)(void *context, int *x, int *y, int *width, int *height);
    bool (*want_move)(void *context);   // 这次拖入按移动收尾（来源程序会删掉原文）
    bool entered;           // 已经给它发过 enter
    XdndSink *next;
};

typedef struct Atoms
{
    xcb_atom_t aware, enter, position, status, leave, drop, finished, selection, type_list, incr;
    xcb_atom_t action_copy, action_move;
    xcb_atom_t uri_list, image_png, image_jpeg, text_utf8, text_plain, utf8_string;
    xcb_atom_t data_property;
} Atoms;

static struct
{
    xcb_connection_t *connection;
    xcb_window_t window;
    xcb_screen_t *screen;
    Atoms atoms;
    bool mapped;
    int x, y, width, height;
    guint poll_source;
    guint io_source;
} bridge;

static XdndSink *sinks;
static XdndSink *target;            // 现在窗口盖着的置物架
static void (*detected_callback)(void);

// 探针：手势识别到晃动/边缘停留后，跟着光标等 X11 应用把拖动送上来。
static bool probe_active;
static gint64 probe_shown_at;
static bool detected;               // 这次手势已经通知过打开置物架

// 拖动本身。
static xcb_window_t drag_source;
static int drag_version;
static bool drag_active;
static XdndKind drag_kind;
static xcb_atom_t drag_type;
static uint32_t drag_action;        // 这次拖动答应来源的动作（复制/移动）
static gint64 drag_started_at;
static gint64 drag_released_at;
// 拖动位置用屏幕坐标（和置物架的窗口一样），不用 XdndPosition 里带的那套 X 根坐标。
static int drag_point_x, drag_point_y;
static bool drag_has_position;

// 读数据。
static bool reading;
static bool incremental;
static bool dropping;
static GByteArray *incoming;
static guint read_timeout;
static xcb_atom_t read_type;

static void hide_window(void);
static void update_hover(void);

// ───────────── X 连接和窗口 ─────────────

static xcb_atom_t intern(const char *name)
{
    if (!bridge.connection || !name) return XCB_ATOM_NONE;
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(bridge.connection, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(bridge.connection, cookie, NULL);
    if (!reply) return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static void intern_atoms(void)
{
    Atoms *atoms = &bridge.atoms;
    atoms->aware = intern("XdndAware");
    atoms->enter = intern("XdndEnter");
    atoms->position = intern("XdndPosition");
    atoms->status = intern("XdndStatus");
    atoms->leave = intern("XdndLeave");
    atoms->drop = intern("XdndDrop");
    atoms->finished = intern("XdndFinished");
    atoms->selection = intern("XdndSelection");
    atoms->type_list = intern("XdndTypeList");
    atoms->incr = intern("INCR");
    atoms->action_copy = intern("XdndActionCopy");
    atoms->action_move = intern("XdndActionMove");
    atoms->uri_list = intern("text/uri-list");
    atoms->image_png = intern("image/png");
    atoms->image_jpeg = intern("image/jpeg");
    atoms->text_utf8 = intern("text/plain;charset=utf-8");
    atoms->text_plain = intern("text/plain");
    atoms->utf8_string = intern("UTF8_STRING");
    atoms->data_property = intern(DATA_PROPERTY);
}

static void send_message(xcb_window_t destination, xcb_atom_t type, const uint32_t data[5])
{
    if (!bridge.connection) return;
    xcb_client_message_event_t event;
    memset(&event, 0, sizeof event);
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = destination;
    event.type = type;
    for (int index = 0; index < 5; index++) event.data.data32[index] = data[index];
    xcb_send_event(bridge.connection, 0, destination, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
    xcb_flush(bridge.connection);
}

static bool create_window(void)
{
    if (!bridge.connection) return false;
    // 窗口什么都不画（背景留空、从不绘制，合成器里看不见），只用来接 X11 的拖动。
    // 必须自己摆位置（override-redirect）：否则合成器的窗口管理会接管，ConfigureRequest 被忽略，
    // 窗口一直留在 (0,0)，盖不到光标下面。
    bridge.window = xcb_generate_id(bridge.connection);
    uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[2] = { 1, XCB_EVENT_MASK_PROPERTY_CHANGE };
    // 必须是 InputOutput：InputOnly 的覆盖窗口在 XWayland 里没有对应的 Wayland 表面，合成器排窗口时
    // 总是把有焦点的拖动源放在它上面；拖动源按 X 的窗口栈找光标底下的窗口，就永远找不到我们。
    xcb_create_window(bridge.connection, XCB_COPY_FROM_PARENT, bridge.window, bridge.screen->root, 0, 0, PROBE_SIZE, PROBE_SIZE, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);
    uint32_t version = XDND_VERSION;
    xcb_change_property(bridge.connection, XCB_PROP_MODE_REPLACE, bridge.window, bridge.atoms.aware, XCB_ATOM_ATOM, 32, 1, &version);
    // GTK 找接放目标时会认有 WM_STATE 的窗口；没有它就接着往下面的窗口找，会把拖动源自己当成目标。
    // 覆盖窗口自己写着玩，不归窗口管理器管。
    xcb_atom_t wm_state = intern("WM_STATE");
    uint32_t normal_state[2] = { 1, 0 };
    xcb_change_property(bridge.connection, XCB_PROP_MODE_REPLACE, bridge.window, wm_state, wm_state, 32, 2, normal_state);
    // 起个名字，出问题时用 xdotool search --name 找得到它。
    static const char title[] = "DropShelf X11 drop target";
    xcb_change_property(bridge.connection, XCB_PROP_MODE_REPLACE, bridge.window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
        (uint32_t)(sizeof title - 1), title);
    xcb_flush(bridge.connection);
    return true;
}

static void place_window(int x, int y, int width, int height)
{
    if (!bridge.connection || !bridge.window) return;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    bool same = bridge.mapped && x == bridge.x && y == bridge.y && width == bridge.width && height == bridge.height;
    if (!same)
    {
        log_trace(L"X11 拖放：接放窗口摆到 (%d,%d) %dx%d", x, y, width, height);
        bridge.x = x;
        bridge.y = y;
        bridge.width = width;
        bridge.height = height;
        uint32_t values[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height };
        xcb_configure_window(bridge.connection, bridge.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH
                | XCB_CONFIG_WINDOW_HEIGHT, values);
    }
    // 拖动源按 X 的窗口栈找光标底下最上面的窗口，合成器又总把有焦点的拖动源往上提，
    // 所以每一拍都把自己提回栈顶，光在换位置的时候提一次不够。
    xcb_configure_window(bridge.connection, bridge.window, XCB_CONFIG_WINDOW_STACK_MODE, (uint32_t[]){ XCB_STACK_MODE_ABOVE });
    if (!bridge.mapped)
    {
        log_trace(L"X11 拖放：挂上接放窗口 (%d,%d)", x, y);
        xcb_map_window(bridge.connection, bridge.window);
        bridge.mapped = true;
    }
    xcb_flush(bridge.connection);
}

static void hide_window(void)
{
    if (!bridge.connection || !bridge.window || !bridge.mapped) return;
    log_trace(L"X11 拖放：收起接放窗口");
    xcb_unmap_window(bridge.connection, bridge.window);
    xcb_flush(bridge.connection);
    bridge.mapped = false;
}

// ───────────── X 那边的拖动 ─────────────

/// X 的拖动源在拖动期间拥有 XdndSelection。有没有主人，就知道桌面上是不是正有个 X11 应用在拖东西，
/// 不用等它把拖动送到我们窗口上。
static bool x_drag_owner_exists(void)
{
    if (!bridge.connection || !bridge.atoms.selection) return false;
    xcb_get_selection_owner_cookie_t cookie = xcb_get_selection_owner(bridge.connection, bridge.atoms.selection);
    xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(bridge.connection, cookie, NULL);
    if (!reply) return false;
    bool owned = reply->owner != XCB_NONE;
    free(reply);
    return owned;
}

// ───────────── 置物架 ─────────────

static bool sink_frame(XdndSink *sink, int *x, int *y, int *width, int *height)
{
    if (!sink || !sink->frame) return false;
    return sink->frame(sink->context, x, y, width, height);
}

static bool sink_contains(XdndSink *sink, int point_x, int point_y, int *local_x, int *local_y)
{
    int x = 0, y = 0, width = 0, height = 0;
    if (!sink_frame(sink, &x, &y, &width, &height)) return false;
    int inside_x = point_x - x, inside_y = point_y - y;
    if (inside_x < 0 || inside_y < 0 || inside_x >= width || inside_y >= height) return false;
    if (local_x) *local_x = inside_x;
    if (local_y) *local_y = inside_y;
    return true;
}

static XdndSink *sink_at(int point_x, int point_y)
{
    for (XdndSink *sink = sinks; sink; sink = sink->next)
    {
        int local_x, local_y;
        if (sink_contains(sink, point_x, point_y, &local_x, &local_y)) return sink;
    }
    return NULL;
}

static void leave_target(void)
{
    if (!target) return;
    XdndSink *leaving = target;
    target = NULL;
    if (leaving->entered && leaving->leave)
    {
        leaving->entered = false;
        leaving->leave(leaving->context);
    }
    leaving->entered = false;
}

static void set_target(XdndSink *sink)
{
    if (target == sink) return;
    leave_target();
    target = sink;
}

/// 这次拖动答应来源什么动作：跟着置物架顶部的“复制/移动”开关走。
/// 答应“移动”时来源程序松手后会删掉原文；还没落到置物架上就按复制算。
static uint32_t current_action(void)
{
    // 只有文字和图片跟着开关走：文件拖进来只记路径，答应移动的话文件管理器会把原文件删掉。
    if (drag_kind != XDND_KIND_TEXT && drag_kind != XDND_KIND_IMAGE) return bridge.atoms.action_copy;
    XdndSink *sink = target;
    if (!sink)
    {
        POINT cursor;
        if (GetCursorPos(&cursor)) sink = sink_at(cursor.x, cursor.y);
    }
    if (sink && sink->want_move && sink->want_move(sink->context)) return bridge.atoms.action_move;
    return bridge.atoms.action_copy;
}

/// 光标（或拖动最后报的位置）在置物架上时给它 enter，离开时 leave；motion 由 XdndPosition 送。
static void update_hover(void)
{
    if (!drag_active || !target || drag_kind == XDND_KIND_NONE) return;
    int point_x = 0, point_y = 0;
    bool have_point = false;
    if (drag_has_position)
    {
        point_x = drag_point_x;
        point_y = drag_point_y;
        have_point = true;
    }
    else
    {
        POINT cursor;
        if (GetCursorPos(&cursor))
        {
            point_x = cursor.x;
            point_y = cursor.y;
            have_point = true;
        }
    }
    int local_x = 0, local_y = 0;
    bool inside = have_point && sink_contains(target, point_x, point_y, &local_x, &local_y);
    if (inside && !target->entered)
    {
        target->entered = true;
        if (target->enter) target->enter(target->context, drag_kind, local_x, local_y);
    }
    else if (!inside && target->entered)
    {
        target->entered = false;
        if (target->leave) target->leave(target->context);
    }
}

// ───────────── 拖动状态 ─────────────

static void reset_drag(void)
{
    drag_active = false;
    dropping = false;
    reading = false;
    drag_kind = XDND_KIND_NONE;
    drag_type = XCB_ATOM_NONE;
    drag_action = XCB_ATOM_NONE;
    drag_source = 0;
    drag_version = 0;
    drag_released_at = 0;
    drag_has_position = false;
    if (read_timeout)
    {
        g_source_remove(read_timeout);
        read_timeout = 0;
    }
    if (incoming)
    {
        g_byte_array_free(incoming, TRUE);
        incoming = NULL;
    }
    incremental = false;
    leave_target();
    // 这一次拖动已经结束，探针也不用再等了；置物架还在光标下面的话，下一次轮询会把窗口盖回去。
    probe_active = false;
    hide_window();
}

static void send_status(bool accepted, uint32_t action)
{
    uint32_t data[5];
    data[0] = bridge.window;
    data[1] = accepted ? 1 : 0;
    data[2] = 0;
    data[3] = 0;                            // 空矩形：整个窗口都可以放
    data[4] = accepted ? action : XCB_ATOM_NONE;
    log_trace(L"X11 拖放：回答 XdndStatus（接受 %d，动作 %u）给 0x%x", accepted ? 1 : 0, (unsigned)data[4],
        (unsigned)drag_source);
    send_message(drag_source, bridge.atoms.status, data);
}

static void send_finished(bool accepted)
{
    if (drag_version < 5 || !drag_source) return;
    uint32_t data[5];
    data[0] = bridge.window;
    data[1] = accepted ? 1 : 0;
    // 报告的动作要和 XdndStatus 里答应的一致，来源才按移动/复制收尾。
    data[2] = accepted ? (drag_action ? drag_action : bridge.atoms.action_copy) : XCB_ATOM_NONE;
    data[3] = 0;
    data[4] = 0;
    send_message(drag_source, bridge.atoms.finished, data);
}

static GArray *read_type_list(xcb_client_message_event_t *event)
{
    GArray *types = g_array_new(FALSE, FALSE, sizeof(xcb_atom_t));
    bool in_property = (event->data.data32[1] & 1) != 0;
    if (in_property)
    {
        xcb_get_property_cookie_t cookie =
            xcb_get_property(bridge.connection, 0, drag_source, bridge.atoms.type_list, XCB_ATOM_ATOM, 0, 1024);
        xcb_get_property_reply_t *reply = xcb_get_property_reply(bridge.connection, cookie, NULL);
        if (reply)
        {
            int count = xcb_get_property_value_length(reply) / 4;
            xcb_atom_t *values = xcb_get_property_value(reply);
            for (int index = 0; index < count && values; index++) g_array_append_val(types, values[index]);
            free(reply);
        }
    }
    else
    {
        for (int index = 2; index < 5; index++)
        {
            xcb_atom_t atom = event->data.data32[index];
            if (atom) g_array_append_val(types, atom);
        }
    }
    return types;
}

/// 把 XdndTypeList 里的原子写成一行名字：图片拖不进来这类问题，靠它就知道来源程序到底给了什么。
static char *types_text(GArray *types)
{
    GString *text = g_string_new(NULL);
    for (guint index = 0; index < types->len; index++)
    {
        xcb_atom_t atom = g_array_index(types, xcb_atom_t, index);
        if (!atom) continue;
        xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(bridge.connection, atom);
        xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(bridge.connection, cookie, NULL);
        if (!reply) continue;
        if (text->len > 0) g_string_append_c(text, ' ');
        g_string_append_len(text, xcb_get_atom_name_name(reply), xcb_get_atom_name_name_length(reply));
        free(reply);
    }
    return g_string_free(text, FALSE);
}

static void choose_type(GArray *types)
{
    static const struct
    {
        xcb_atom_t *atom;
        XdndKind kind;
    } candidates[] = {
        { &bridge.atoms.uri_list, XDND_KIND_FILES },
        { &bridge.atoms.image_png, XDND_KIND_IMAGE },
        { &bridge.atoms.image_jpeg, XDND_KIND_IMAGE },
        { &bridge.atoms.text_utf8, XDND_KIND_TEXT },
        { &bridge.atoms.utf8_string, XDND_KIND_TEXT },
        { &bridge.atoms.text_plain, XDND_KIND_TEXT },
    };
    drag_type = XCB_ATOM_NONE;
    drag_kind = XDND_KIND_NONE;
    for (size_t candidate = 0; candidate < sizeof candidates / sizeof candidates[0]; candidate++)
    {
        for (guint index = 0; index < types->len; index++)
        {
            xcb_atom_t atom = g_array_index(types, xcb_atom_t, index);
            if (atom && atom == *candidates[candidate].atom)
            {
                drag_type = atom;
                drag_kind = candidates[candidate].kind;
                return;
            }
        }
    }
}

// ───────────── 读数据 ─────────────

static const char *kind_name(XdndKind kind)
{
    switch (kind)
    {
        case XDND_KIND_FILES: return "text/uri-list";
        case XDND_KIND_TEXT: return "文字";
        case XDND_KIND_IMAGE: return "图片";
        default: return "不支持";
    }
}

static void deliver_data(void)
{
    XdndPayload payload;
    memset(&payload, 0, sizeof payload);
    payload.kind = drag_kind;
    char *text = NULL;
    if (drag_kind == XDND_KIND_FILES || drag_kind == XDND_KIND_TEXT)
    {
        text = g_strndup(incoming && incoming->len ? (const char *)incoming->data : "", incoming ? incoming->len : 0);
        payload.text = text;
    }
    else if (drag_kind == XDND_KIND_IMAGE)
    {
        payload.image = incoming ? incoming->data : NULL;
        payload.image_size = incoming ? incoming->len : 0;
        payload.image_mime = read_type == bridge.atoms.image_jpeg ? "image/jpeg" : "image/png";
    }
    log_info(L"X11 拖放：数据读完了，%ls %zu 字节", drag_kind == XDND_KIND_IMAGE ? L"图片" : drag_kind == XDND_KIND_FILES ? L"文件列表" : L"文字",
        (size_t)(incoming ? incoming->len : 0));
    if (target && target->drop)
    {
        int point_x = drag_has_position ? drag_point_x : 0, point_y = drag_has_position ? drag_point_y : 0;
        if (!drag_has_position)
        {
            POINT cursor;
            if (GetCursorPos(&cursor))
            {
                point_x = cursor.x;
                point_y = cursor.y;
            }
        }
        int local_x = 0, local_y = 0;
        if (sink_contains(target, point_x, point_y, &local_x, &local_y)) target->drop(target->context, &payload, local_x, local_y);
        else log_info(L"X11 拖放：松手时已经不在置物架上，丢弃");
    }
    send_finished(target != NULL);
    g_free(text);
    reset_drag();
}

static void fail_read(const wchar_t *reason)
{
    log_info(L"X11 拖放：读不到数据（%ls）", reason);
    send_finished(false);
    reset_drag();
}

static gboolean read_timed_out(gpointer data)
{
    (void)data;
    read_timeout = 0;
    if (reading) fail_read(L"等得太久");
    return G_SOURCE_REMOVE;
}

static void read_property_chunk(bool first)
{
    if (!reading || !bridge.connection) return;
    xcb_get_property_cookie_t cookie =
        xcb_get_property(bridge.connection, 1, bridge.window, bridge.atoms.data_property, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(bridge.connection, cookie, NULL);
    if (!reply)
    {
        fail_read(L"取属性的回复丢了");
        return;
    }
    bool incr = reply->type == bridge.atoms.incr;
    if (first && incr)
    {
        // 来源程序改用 INCR：数据会分成几段陆续写进这个属性。
        incremental = true;
        free(reply);
        return;
    }
    int length = xcb_get_property_value_length(reply);
    if (length < 0) length = 0;
    if (length > 0)
    {
        if (incoming->len + (gsize)length > MAXIMUM_DATA)
        {
            free(reply);
            fail_read(L"数据太大");
            return;
        }
        g_byte_array_append(incoming, xcb_get_property_value(reply), (guint)length);
    }
    bool complete = !incremental || length == 0;
    free(reply);
    if (complete) deliver_data();
}

// ───────────── X 事件 ─────────────

static void handle_enter(xcb_client_message_event_t *event)
{
    if (event->data.data32[0] == bridge.window) return;     // 我们自己的拖动？不会发生
    if (drag_active && event->data.data32[0] == drag_source) return;
    if (drag_active) reset_drag();

    drag_source = event->data.data32[0];
    drag_version = (int)(event->data.data32[1] >> 24);
    drag_active = true;
    drag_started_at = g_get_monotonic_time();
    drag_released_at = 0;
    drag_has_position = false;

    GArray *types = read_type_list(event);
    char *offered = types_text(types);
    log_trace(L"X11 拖放：来源给的类型有 %hs", offered);
    g_free(offered);
    choose_type(types);
    g_array_free(types, TRUE);
    if (drag_kind == XDND_KIND_NONE)
    {
        log_info(L"X11 拖放：没有能用的格式，不接");
        return;
    }
    log_info(L"X11 拖放：%hs 送上来（源窗口 0x%x，Xdnd %d）", kind_name(drag_kind), (unsigned)drag_source, drag_version);
    // Xdnd 要求回应 XdndEnter；GTK 的拖动源也靠这个才继续发 XdndPosition、才允许松手放下。
    drag_action = current_action();
    send_status(true, drag_action);

    if (!target)
    {
        POINT cursor;
        XdndSink *under = GetCursorPos(&cursor) ? sink_at(cursor.x, cursor.y) : NULL;
        if (!under && !detected && detected_callback)
        {
            detected = true;
            log_info(L"X11 拖放：确认 X11 应用正在拖东西，弹出置物架");
            detected_callback();
        }
    }
    update_hover();
}

static void handle_position(xcb_client_message_event_t *event)
{
    if (!drag_active || event->data.data32[0] != drag_source) return;
    // data32[2] 是 X 的根坐标，和置物架用的屏幕坐标差着偏移；拖动时鼠标底下就是光标，直接问光标在哪。
    POINT cursor;
    if (GetCursorPos(&cursor))
    {
        drag_point_x = cursor.x;
        drag_point_y = cursor.y;
        drag_has_position = true;
    }
    // 只要格式能用就一直接着：拖动源没收到接受就不肯放下，而光标一到 Wayland 表面（置物架）上，
    // X 那边的位置就冻住了，未必还会再来一个 XdndPosition。到底收不收，等松手时按真实光标的位置再定。
    bool accepted = drag_kind != XDND_KIND_NONE;
    // 收不收、按什么动作收，都由置物架顶部的“复制/移动”开关决定：移动时告诉来源按移动收尾，
    // 它松手后会删掉原文。不能把来源自己提议的动作原样答应回去——WPS 和 Obsidian 提议的总是移动。
    drag_action = current_action();
    send_status(accepted, drag_action);
    if (!accepted || !target) return;
    int local_x = 0, local_y = 0;
    if (drag_has_position && sink_contains(target, drag_point_x, drag_point_y, &local_x, &local_y))
    {
        if (!target->entered)
        {
            target->entered = true;
            if (target->enter) target->enter(target->context, drag_kind, local_x, local_y);
        }
        else if (target->motion) target->motion(target->context, drag_kind, local_x, local_y);
    }
    else if (target->entered)
    {
        target->entered = false;
        if (target->leave) target->leave(target->context);
    }
}

static void handle_leave(void)
{
    if (!drag_active) return;
    log_trace(L"X11 拖放：离开");
    leave_target();
}

static void handle_drop(xcb_client_message_event_t *event)
{
    if (!drag_active) return;
    uint32_t timestamp = event->data.data32[2];
    if (drag_kind == XDND_KIND_NONE || drag_type == XCB_ATOM_NONE || !target)
    {
        log_info(L"X11 拖放：放下时不接受，不读数据");
        send_finished(false);
        reset_drag();
        return;
    }
    if (reading) return;
    log_info(L"X11 拖放：放下，向来源程序要数据");
    dropping = true;
    reading = true;
    incremental = false;
    read_type = drag_type;
    incoming = g_byte_array_new();
    xcb_convert_selection(bridge.connection, bridge.window, bridge.atoms.selection, drag_type, bridge.atoms.data_property, timestamp);
    xcb_flush(bridge.connection);
    read_timeout = g_timeout_add(READ_TIMEOUT_MILLISECONDS, read_timed_out, NULL);
}

static void handle_selection_notify(xcb_selection_notify_event_t *event)
{
    if (!reading || event->requestor != bridge.window) return;
    if (event->property == XCB_ATOM_NONE)
    {
        fail_read(L"来源程序没有给数据");
        return;
    }
    read_property_chunk(true);
}

static void handle_property_notify(xcb_property_notify_event_t *event)
{
    if (!reading || !incremental || event->window != bridge.window) return;
    if (event->atom != bridge.atoms.data_property || event->state != XCB_PROPERTY_NEW_VALUE) return;
    read_property_chunk(false);
}

static gboolean on_x_event(GIOChannel *channel, GIOCondition condition, gpointer data)
{
    (void)channel;
    (void)data;
    if (!bridge.connection) return G_SOURCE_REMOVE;
    if (condition & (G_IO_HUP | G_IO_ERR))
    {
        log_info(L"X11 拖放：X 连接断了，拖放桥停用");
        bridge.io_source = 0;       // 正在处理的就是它，别让 xdnd_stop 再去删一遍
        xdnd_stop();
        return G_SOURCE_REMOVE;
    }
    while (true)
    {
        xcb_generic_event_t *event = xcb_poll_for_event(bridge.connection);
        if (!event) break;
        int type = event->response_type & ~0x80;
        if (type == 0)
        {
            xcb_generic_error_t *error = (xcb_generic_error_t *)event;
            log_trace(L"X11 拖放：X 报错 code %d，请求 %d，资源 0x%x", (int)error->error_code, (int)error->major_code,
                (unsigned)error->resource_id);
        }
        if (type == XCB_CLIENT_MESSAGE)
        {
            xcb_client_message_event_t *message = (xcb_client_message_event_t *)event;
            if (message->window == bridge.window)
            {
                if (message->type == bridge.atoms.enter) handle_enter(message);
                else if (message->type == bridge.atoms.position) handle_position(message);
                else if (message->type == bridge.atoms.leave) handle_leave();
                else if (message->type == bridge.atoms.drop)
                {
                    log_trace(L"X11 拖放：收到 XdndDrop");
                    handle_drop(message);
                }
            }
            else log_trace(L"X11 拖放：客户端消息 0x%x（窗口 0x%x，不是我们）", (unsigned)message->type, (unsigned)message->window);
        }
        // 其它事件（X 错误、配置变化）不用管。
        else if (type == XCB_SELECTION_NOTIFY) handle_selection_notify((xcb_selection_notify_event_t *)event);
        else if (type == XCB_PROPERTY_NOTIFY) handle_property_notify((xcb_property_notify_event_t *)event);
        free(event);
    }
    xcb_flush(bridge.connection);
    return G_SOURCE_CONTINUE;
}

// ───────────── 定时跟踪 ─────────────

static gboolean poll_tick(gpointer data)
{
    (void)data;
    if (!bridge.connection || !bridge.window) return G_SOURCE_CONTINUE;
    if (!probe_active && !sinks && !drag_active)
    {
        if (bridge.mapped) hide_window();
        return G_SOURCE_CONTINUE;
    }
    gint64 now = g_get_monotonic_time();
    POINT cursor = {0};
    bool have_cursor = GetCursorPos(&cursor);

    // 探针等不到拖动就收起来，和 Wayland 探针一样。
    if (probe_active && !drag_active)
    {
        int buttons = gesture_left_button_state();
        gint64 elapsed = now - probe_shown_at;
        if (buttons == 0 || elapsed >= (buttons < 0 ? PROBE_BLIND_TIMEOUT_MICROSECONDS : PROBE_TIMEOUT_MICROSECONDS))
        {
            log_info(L"X11 拖放：探针等了 %lld 毫秒没有等到拖动%s", (long long)(elapsed / 1000),
                buttons == 0 ? L"（左键已经松开）" : L"");
            probe_active = false;
        }
    }
    if (drag_active && !reading)
    {
        // 来源程序中途取消拖动时不会有 XdndDrop，靠左键松开判断。
        int buttons = gesture_left_button_state();
        if (buttons == 0)
        {
            if (!drag_released_at) drag_released_at = now;
            else if (now - drag_released_at >= RELEASE_GRACE_MICROSECONDS)
            {
                log_info(L"X11 拖放：左键已经松开，没有等到放下");
                reset_drag();
            }
        }
        else
        {
            drag_released_at = 0;
        }
        if (drag_active && now - drag_started_at >= DRAG_TIMEOUT_MICROSECONDS)
        {
            log_info(L"X11 拖放：接得太久，收起来");
            reset_drag();
        }
    }

    XdndSink *under = NULL;
    if (drag_active && target) under = target;
    else if (have_cursor) under = sink_at(cursor.x, cursor.y);
    if (under) set_target(under);
    else if (!drag_active && !probe_active) set_target(NULL);

    // 桌面上有没有 X11 应用在拖东西：拖动源在拖动期间拥有 XdndSelection，不用等它把拖动送到我们窗口上。
    bool dragging = x_drag_owner_exists();

    // 什么时候挂窗口：探针亮着的时候，或者光标正落在置物架上——都是用户要把东西放进置物架的时候。
    // 别的 X11 拖动（在 Obsidian 里挪图片、往桌面拖文件……）一概不碰，否则会被我们截下来。
    bool interested = drag_active || (dragging && (probe_active || under != NULL));

    // 接放窗口铺满整个 X 屏幕。拖动源是拿 X 光标的位置去找窗口的，而 XWayland 里的 X 光标
    // 会落后、跳来跳去（合成器给的坐标和光标真实位置能差几百像素），窗口跟着光标走就总有盖不住的时候；
    // 铺满屏幕就不用管光标到底报在哪，反正都在里面。
    if (interested) place_window(0, 0, bridge.screen->width_in_pixels, bridge.screen->height_in_pixels);
    else hide_window();
    if (drag_active) update_hover();
    return G_SOURCE_CONTINUE;
}

// ───────────── 对外接口 ─────────────

bool xdnd_start(void (*detected)(void))
{
    if (bridge.connection) return true;
    int screen_number = 0;
    bridge.connection = xcb_connect(NULL, &screen_number);
    if (!bridge.connection || xcb_connection_has_error(bridge.connection))
    {
        if (bridge.connection) xcb_disconnect(bridge.connection);
        bridge.connection = NULL;
        return false;
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(bridge.connection));
    for (int index = 0; index < screen_number && screens.rem; index++) xcb_screen_next(&screens);
    bridge.screen = screens.data;
    if (!bridge.screen)
    {
        xdnd_stop();
        return false;
    }
    detected_callback = detected;
    intern_atoms();
    if (!create_window() || !bridge.atoms.aware)
    {
        xdnd_stop();
        return false;
    }
    int fd = xcb_get_file_descriptor(bridge.connection);
    GIOChannel *channel = g_io_channel_unix_new(fd);
    bridge.io_source = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_x_event, NULL);
    g_io_channel_unref(channel);
    bridge.poll_source = g_timeout_add(POLL_MILLISECONDS, poll_tick, NULL);
    log_info(L"X11 拖放桥已就绪，接放窗口 0x%x", (unsigned)bridge.window);
    return true;
}

void xdnd_stop(void)
{
    if (bridge.poll_source)
    {
        g_source_remove(bridge.poll_source);
        bridge.poll_source = 0;
    }
    if (bridge.io_source)
    {
        g_source_remove(bridge.io_source);
        bridge.io_source = 0;
    }
    // 置物架那边还拿着 sink 指针（关窗口时会来注销），关掉桥时只断开连接，不再回调它们。
    target = NULL;
    reset_drag();
    if (bridge.connection)
    {
        if (bridge.window) xcb_destroy_window(bridge.connection, bridge.window);
        xcb_disconnect(bridge.connection);
    }
    bridge.connection = NULL;
    bridge.window = 0;
    bridge.mapped = false;
    bridge.screen = NULL;
    sinks = NULL;
    detected_callback = NULL;
    probe_active = false;
}

bool xdnd_drag_in_progress(void)
{
    return drag_active;
}

void xdnd_probe_show(void)
{
    if (!bridge.connection) return;
    probe_active = true;
    probe_shown_at = g_get_monotonic_time();
    detected = false;
    // 马上把接放窗口挂上去，别等下一拍：Wayland 探针一露面，XWayland 就没有光标位置可报了，
    // 拖动源不会再去重新找落点；接放窗口要赶在那之前就在光标底下。
    place_window(0, 0, bridge.screen->width_in_pixels, bridge.screen->height_in_pixels);
}

void xdnd_probe_hide(void)
{
    probe_active = false;
    if (!drag_active) hide_window();
}

XdndSink *xdnd_add_sink(void *context,
    void (*enter)(void *context, XdndKind kind, double x, double y),
    void (*motion)(void *context, XdndKind kind, double x, double y),
    void (*leave)(void *context),
    void (*drop)(void *context, const XdndPayload *payload, double x, double y),
    bool (*frame)(void *context, int *x, int *y, int *width, int *height),
    bool (*want_move)(void *context))
{
    if (!bridge.connection) return NULL;
    XdndSink *sink = calloc(1, sizeof *sink);
    if (!sink) return NULL;
    sink->context = context;
    sink->enter = enter;
    sink->motion = motion;
    sink->leave = leave;
    sink->drop = drop;
    sink->frame = frame;
    sink->want_move = want_move;
    sink->next = sinks;
    sinks = sink;
    return sink;
}

void xdnd_remove_sink(XdndSink *sink)
{
    if (!sink) return;
    if (target == sink) leave_target();
    for (XdndSink **link = &sinks; *link; link = &(*link)->next)
    {
        if (*link != sink) continue;
        *link = sink->next;
        free(sink);
        return;
    }
}