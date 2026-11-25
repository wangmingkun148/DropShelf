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
#include "ui_linux.h"

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
    bool entered;           // 已经给它发过 enter
    XdndSink *next;
};

typedef struct Atoms
{
    xcb_atom_t aware, enter, position, status, leave, drop, finished, selection, type_list, incr;
    xcb_atom_t action_copy, action_move;
    xcb_atom_t uri_list, image_png, image_jpeg, text_utf8, text_plain, utf8_string, text_html;
    xcb_atom_t data_property;
    xcb_atom_t targets, net_wm_pid;
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
    atoms->text_html = intern("text/html");
    atoms->data_property = intern(DATA_PROPERTY);
    atoms->targets = intern("TARGETS");
    atoms->net_wm_pid = intern("_NET_WM_PID");
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

// ───────────── 自己当 X11 拖动源 ─────────────
// 从置物架往 WPS 这类 XWayland 程序拖东西时，Hyprland 会把 Wayland 的拖动转成 Xdnd 发给对方，
// 可是拖动期间它不让 X11 程序读任何 selection（连剪贴板都读不到），对方拿不到数据，只能退回
// 粘贴剪贴板，或者干脆什么都不做。所以光标停在 XWayland 程序上时改成自己发 Xdnd：消息和数据
// 都直接由我们给，不经过合成器。

/// 光标在同一个 XWayland 窗口上停这么久，才把这次拖出接过来。
#define TAKEOVER_DWELL_MICROSECONDS 250000
/// 取消 GTK 的拖动以后等一会儿再发 XdndEnter：让 Hyprland 先把它的那次拖动收干净。
#define TAKEOVER_SETTLE_MICROSECONDS 160000
/// 每隔多久重新看一眼光标底下还是不是原来那个窗口。
#define SOURCE_RECHECK_MICROSECONDS 200000
/// 发过 XdndDrop 以后等 XdndFinished 的上限。
#define SOURCE_FINISH_TIMEOUT_MICROSECONDS 3000000
/// 自己送太久（看不到按键状态时会这样）就不再挂着。
#define SOURCE_TIMEOUT_MICROSECONDS 30000000
/// 放下以后隔多久送下一批：给接收方一点时间把上一批收完。
#define BATCH_DELAY_MILLISECONDS 450
/// 送下一批时等接收方回应 XdndPosition 的上限。
#define BATCH_ACCEPT_TIMEOUT_MICROSECONDS 1500000
/// 一次最多排这么多批：选中的东西再多也不会一直送个不停。
#define MAXIMUM_BATCHES 16
/// 发出 XdndDrop 以后对方取过哪些格式。
#define DROP_TYPE_URI_LIST 1u
#define DROP_TYPE_TEXT 2u
#define DROP_TYPE_HTML 4u

static struct
{
    // 置物架交过来的东西。
    bool prepared;
    char *uri_list;                 // text/uri-list 的内容（\r\n 分隔）
    char *text;                     // 纯文字；有文件时是路径列表
    char *html;                     // text/html：混合拖出时把文字和图片放在一起
    bool move_allowed;
    uint32_t drop_types;            // 发出 XdndDrop 以后对方取了哪些格式（DROP_TYPE_*）
    // 自动分批：一次拖出分成几批送给同一个窗口，不用鼠标按键。
    bool batching;
    guint batch_timer;
    GdkDragAction batch_report;      // 分批全部送完后告诉置物架的动作
    xcb_window_t batch_window;
    int batch_pid;
    RECT batch_rect;
    int batch_target_x, batch_target_y, batch_target_width, batch_target_height;
    int batch_point_x, batch_point_y;
    uint32_t batch_time;
    // 光标底下停在哪；两次轮询之间不做别的。
    int dwell_pid;
    gint64 dwell_since;
    // 取消 GTK 的拖动以后，等一小会儿再真正接管。
    bool starting;
    gint64 start_at;
    bool assisting;                  // 保留旧 Xdnd，只替它提供 selection 数据
    guint assist_clear_source;       // 拖动结束后延迟交回 selection
    // 接管以后。
    bool active;
    xcb_window_t window;            // 我们自己的 selection 窗口
    xcb_window_t target;            // 接收方 X 窗口
    xcb_window_t entered;           // 已经发过 XdndEnter 的窗口
    RECT target_rect;               // 接收方在逻辑坐标里的位置
    int target_pid;
    int target_x, target_y;         // 接收方窗口在 X 根坐标里的位置
    int target_width, target_height;
    bool accepted;                  // 对方接受了这次拖动
    uint32_t action;                // 对方答应的动作
    uint32_t proposed;              // 我们提议的动作
    uint32_t time;                  // 服务器时间，XdndPosition / XdndDrop 要带
    bool have_point;
    int point_x, point_y;
    bool waiting_finish;
    gint64 drop_at;
    gint64 started_at;
    gint64 checked_at;
    int owner_retries;
} source;

static void source_finish(bool dropped);
static gboolean source_batch_start(gpointer data);

/// 排着队、主拖动放下后一批一批送出去的内容。
typedef struct SourceBatch
{
    char *uri_list;
    char *text;
} SourceBatch;

static GQueue batches = G_QUEUE_INIT;

static void source_batch_free(SourceBatch *batch)
{
    if (!batch) return;
    g_free(batch->uri_list);
    g_free(batch->text);
    g_free(batch);
}

static void source_batches_clear(void)
{
    SourceBatch *batch;
    while ((batch = g_queue_pop_head(&batches)) != NULL) source_batch_free(batch);
}

/// 撤掉还没送出的批次。report 时把置物架那次拖出的收尾补上（分批排着的时候收尾是推迟的）。
static void source_batches_cancel(bool report)
{
    bool pending = source.batch_timer != 0;
    if (source.batch_timer)
    {
        g_source_remove(source.batch_timer);
        source.batch_timer = 0;
    }
    source_batches_clear();
    // 正在送某一批时收尾还没推迟：source_finish 会自己处理，这里不做。
    if (!pending) return;
    GdkDragAction action = source.batch_report;
    source.batch_report = 0;
    log_info(L"X11 拖动源：取消排着的分批");
    // 分批排着的时候，置物架那次拖出的收尾是推迟的；取消时补上，别让它一直停在“正在拖”。
    if (report) ui_drag_taken_over_finished(action);
}

static void source_clear_payload(void)
{
    g_free(source.uri_list);
    g_free(source.text);
    g_free(source.html);
    source.uri_list = NULL;
    source.text = NULL;
    source.html = NULL;
    source.prepared = false;
}

/// 路径里的特殊字节按 URI 的规矩转义：WPS 这类程序要的是能直接用的 file:// URI。
static void append_uri_path(GString *out, const char *path)
{
    static const char digits[] = "0123456789ABCDEF";
    for (const unsigned char *cursor = (const unsigned char *)path; *cursor; cursor++)
    {
        unsigned char byte = *cursor;
        bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '/';
        if (plain) g_string_append_c(out, (char)byte);
        else
        {
            g_string_append_c(out, '%');
            g_string_append_c(out, digits[byte >> 4]);
            g_string_append_c(out, digits[byte & 15]);
        }
    }
}

/// 把一组路径做成 text/uri-list 和一行一条的纯文字。
static void source_paths_build(const char *const *paths, int count, char **uri_list, char **text)
{
    *uri_list = NULL;
    *text = NULL;
    GString *list = g_string_new(NULL);
    GString *plain = g_string_new(NULL);
    for (int index = 0; paths && index < count; index++)
    {
        if (!paths[index] || !*paths[index]) continue;
        if (list->len > 0) g_string_append(list, "\r\n");
        g_string_append(list, "file://");
        append_uri_path(list, paths[index]);
        if (plain->len > 0) g_string_append_c(plain, '\n');
        g_string_append(plain, paths[index]);
    }
    if (list->len > 0)
    {
        g_string_append(list, "\r\n");
        *uri_list = g_string_free(list, FALSE);
        *text = g_string_free(plain, FALSE);
    }
    else
    {
        g_string_free(list, TRUE);
        g_string_free(plain, TRUE);
    }
}

void xdnd_source_set_payload(const char *const *paths, int count, const char *text, const char *html, bool move_allowed)
{
    if (source.active || source.starting) return;
    // 上一次拖出还留着 selection：新的一次要开始了，立刻收回来，别让旧数据再被取到。
    if (source.assist_clear_source)
    {
        g_source_remove(source.assist_clear_source);
        source.assist_clear_source = 0;
    }
    if (source.assisting && bridge.connection)
        xcb_set_selection_owner(bridge.connection, XCB_ATOM_NONE, bridge.atoms.selection, XCB_CURRENT_TIME);
    source.assisting = false;
    source_batches_cancel(true);
    source_clear_payload();
    source.move_allowed = move_allowed;
    source.html = html && *html ? g_strdup(html) : NULL;
    source.drop_types = 0;
    source_paths_build(paths, count, &source.uri_list, &source.text);
    if (!source.uri_list && !source.text && text && *text) source.text = g_strdup(text);
    source.prepared = source.uri_list != NULL || source.text != NULL || source.html != NULL;
    source.proposed = source.move_allowed ? bridge.atoms.action_move : bridge.atoms.action_copy;
    log_trace(L"X11 拖动源：准备好内容（%d 个文件，文字 %ls，HTML %ls，排着 %u 批，%ls），提议动作 0x%x（移动 0x%x，复制 0x%x）", count,
        source.text ? L"有" : L"无", source.html ? L"有" : L"无", g_queue_get_length(&batches),
        source.move_allowed ? L"可移动" : L"只复制", (unsigned)source.proposed,
        (unsigned)bridge.atoms.action_move, (unsigned)bridge.atoms.action_copy);
    if (source.text) log_trace(L"X11 拖动源：要拖出去的文字 %zu 字节", strlen(source.text));
}

void xdnd_source_add_batch(const char *const *paths, int count, const char *text)
{
    SourceBatch *batch = g_new0(SourceBatch, 1);
    source_paths_build(paths, count, &batch->uri_list, &batch->text);
    if (!batch->uri_list && !batch->text && text && *text) batch->text = g_strdup(text);
    if (!batch->uri_list && !batch->text)
    {
        source_batch_free(batch);
        return;
    }
    if (g_queue_get_length(&batches) >= MAXIMUM_BATCHES)
    {
        log_info(L"X11 拖动源：排着的批次到了上限 %d，后面的不送了", MAXIMUM_BATCHES);
        source_batch_free(batch);
        return;
    }
    g_queue_push_tail(&batches, batch);
    log_trace(L"X11 拖动源：排入一批（%ls）", batch->uri_list ? L"文件" : L"文字");
}

static bool path_is_image(const char *path)
{
    static const char *const extensions[] = {
        ".png", ".jpg", ".jpeg", ".jfif", ".gif", ".bmp", ".webp", ".svg", ".ico", ".tif", ".tiff"
    };
    size_t length = strlen(path);
    for (size_t index = 0; index < sizeof extensions / sizeof extensions[0]; index++)
    {
        size_t extension = strlen(extensions[index]);
        if (length > extension && g_ascii_strcasecmp(path + length - extension, extensions[index]) == 0) return true;
    }
    return false;
}

static void append_html_text(GString *out, const char *text)
{
    for (const char *cursor = text; *cursor; cursor++)
    {
        char character = *cursor;
        if (character == '&') g_string_append(out, "&amp;");
        else if (character == '<') g_string_append(out, "&lt;");
        else if (character == '>') g_string_append(out, "&gt;");
        else if (character == '"') g_string_append(out, "&quot;");
        else if (character == '\r')
        {
            if (cursor[1] != '\n') g_string_append(out, "<br>");
        }
        else if (character == '\n') g_string_append(out, "<br>");
        else g_string_append_c(out, character);
    }
}

char *xdnd_source_html_build(const char *text, const char *const *paths, int count)
{
    GString *out = g_string_new("<html><head><meta charset=\"utf-8\"></head><body>");
    bool any = false;
    if (text && *text)
    {
        g_string_append(out, "<div>");
        append_html_text(out, text);
        g_string_append(out, "</div>");
        any = true;
    }
    for (int index = 0; paths && index < count; index++)
    {
        const char *path = paths[index];
        if (!path || !*path || !path_is_image(path)) continue;
        // 图片用本地 file:// 地址：文档程序读得到本机文件，不用把整张图再塞进 selection。
        g_string_append(out, "<p><img src=\"file://");
        append_uri_path(out, path);
        g_string_append(out, "\"></p>");
        any = true;
    }
    g_string_append(out, "</body></html>");
    if (!any)
    {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static uint32_t window_pid(xcb_window_t window)
{
    if (!bridge.connection || !bridge.atoms.net_wm_pid) return 0;
    xcb_get_property_cookie_t cookie = xcb_get_property(bridge.connection, 0, window, bridge.atoms.net_wm_pid, XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(bridge.connection, cookie, NULL);
    uint32_t pid = 0;
    if (reply)
    {
        if (xcb_get_property_value_length(reply) >= 4) memcpy(&pid, xcb_get_property_value(reply), 4);
        free(reply);
    }
    return pid;
}

static bool window_aware(xcb_window_t window)
{
    if (!bridge.connection || !bridge.atoms.aware) return false;
    xcb_get_property_cookie_t cookie = xcb_get_property(bridge.connection, 0, window, bridge.atoms.aware, XCB_ATOM_ATOM, 0, 1);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(bridge.connection, cookie, NULL);
    bool aware = false;
    if (reply)
    {
        if (xcb_get_property_value_length(reply) >= 4)
        {
            xcb_atom_t version = XCB_ATOM_NONE;
            memcpy(&version, xcb_get_property_value(reply), 4);
            aware = version >= 3;
        }
        free(reply);
    }
    return aware;
}

/// 在 X 的顶层窗口里找 pid 那个程序的主窗口：同进程的窗口可能有好几个，取大小最接近的。
static bool source_find_target(int pid, const RECT *rect)
{
    if (!bridge.connection || !bridge.screen || pid <= 0 || !rect) return false;
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;
    if (width <= 0 || height <= 0) return false;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(bridge.connection, xcb_query_tree(bridge.connection, bridge.screen->root), NULL);
    if (!tree) return false;
    xcb_window_t best = XCB_NONE;
    int best_score = 0;
    xcb_window_t best_x = 0, best_y = 0;
    int best_width = 0, best_height = 0;
    int count = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);
    for (int index = 0; index < count; index++)
    {
        xcb_window_t window = children[index];
        if (!window || window_pid(window) != (uint32_t)pid) continue;
        if (!window_aware(window)) continue;
        xcb_get_window_attributes_reply_t *attributes =
            xcb_get_window_attributes_reply(bridge.connection, xcb_get_window_attributes(bridge.connection, window), NULL);
        bool viewable = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
        free(attributes);
        if (!viewable) continue;
        xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(bridge.connection, xcb_get_geometry(bridge.connection, window), NULL);
        if (!geometry) continue;
        int score = abs((int)geometry->width - width) + abs((int)geometry->height - height);
        if (!best || score < best_score)
        {
            best = window;
            best_score = score;
            best_x = geometry->x;
            best_y = geometry->y;
            best_width = geometry->width;
            best_height = geometry->height;
        }
        free(geometry);
    }
    free(tree);
    if (!best) return false;
    source.target = best;
    source.target_pid = pid;
    source.target_rect = *rect;
    source.target_x = best_x;
    source.target_y = best_y;
    source.target_width = best_width;
    source.target_height = best_height;
    return true;
}

static int source_type_atoms(xcb_atom_t *out, int max)
{
    int count = 0;
    // HTML 放最前面：认 HTML 的程序（文档程序）拿到它就能一次把文字和图片都插进去。
    if (source.html && count < max) out[count++] = bridge.atoms.text_html;
    if (source.uri_list && count < max) out[count++] = bridge.atoms.uri_list;
    if (source.text)
    {
        if (count < max) out[count++] = bridge.atoms.utf8_string;
        if (count < max) out[count++] = bridge.atoms.text_plain;
    }
    return count;
}

static bool source_create_window(void)
{
    if (source.window) return true;
    if (!bridge.connection || !bridge.screen) return false;
    // 只用来占着 XdndSelection 和收 XdndStatus，什么都不画。
    source.window = xcb_generate_id(bridge.connection);
    uint32_t values[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_create_window(bridge.connection, XCB_COPY_FROM_PARENT, source.window, bridge.screen->root, -10, -10, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, values);
    xcb_flush(bridge.connection);
    return true;
}

static bool source_take_ownership(void)
{
    xcb_set_selection_owner(bridge.connection, source.window, bridge.atoms.selection, XCB_CURRENT_TIME);
    xcb_flush(bridge.connection);
    xcb_get_selection_owner_reply_t *reply =
        xcb_get_selection_owner_reply(bridge.connection, xcb_get_selection_owner(bridge.connection, bridge.atoms.selection), NULL);
    if (!reply) return false;
    bool owned = reply->owner == source.window;
    free(reply);
    if (!owned) log_info(L"X11 拖动源：XdndSelection 没抢到");
    return owned;
}

bool xdnd_source_copy_only(void)
{
    // 混合拖出（带 HTML、排着几批）不能只替 Hyprland 的拖动提供数据：放下以后还要由我们自己
    // 跟对方一批一批地谈完，第几批送到了、还差几批都只有我们知道。
    if (source.html || !g_queue_is_empty(&batches)) return false;
    return source.prepared && !source.move_allowed;
}

bool xdnd_source_assist_begin(void)
{
    if (!bridge.connection || !source.prepared || source.active || source.starting || source.assisting) return false;
    if (!source_create_window() || !source_take_ownership()) return false;
    source.assisting = true;
    log_info(L"X11 拖动源：保留 Hyprland 的拖动，接管它的数据 selection");
    return true;
}

/// 松手以后接收方可能还在读数据；等够了再放手，下一次拖动开始前也会立刻清掉。
#define ASSIST_CLEAR_MICROSECONDS 5000000

static gboolean assist_clear(gpointer data)
{
    (void)data;
    source.assist_clear_source = 0;
    if (!source.assisting) return G_SOURCE_REMOVE;
    source.assisting = false;
    if (bridge.connection && source.window && bridge.atoms.selection)
    {
        // 这期间可能已经有别的程序开始拖动、接管了 selection，别把它清掉。
        xcb_get_selection_owner_reply_t *reply =
            xcb_get_selection_owner_reply(bridge.connection, xcb_get_selection_owner(bridge.connection, bridge.atoms.selection), NULL);
        bool ours = reply && reply->owner == source.window;
        free(reply);
        if (ours)
        {
            xcb_set_selection_owner(bridge.connection, XCB_ATOM_NONE, bridge.atoms.selection, XCB_CURRENT_TIME);
            xcb_flush(bridge.connection);
            log_info(L"X11 拖动源：交回拖动的数据 selection");
        }
    }
    return G_SOURCE_REMOVE;
}

void xdnd_source_assist_finish(void)
{
    if (!source.assisting || source.assist_clear_source) return;
    source.assist_clear_source = g_timeout_add(ASSIST_CLEAR_MICROSECONDS / 1000, assist_clear, NULL);
}

static void source_send_enter(xcb_window_t window)
{
    if (!window) return;
    xcb_atom_t all[8];
    int total = source_type_atoms(all, (int)(sizeof all / sizeof all[0]));
    bool in_property = total > 3;
    if (in_property)
        xcb_change_property(bridge.connection, XCB_PROP_MODE_REPLACE, source.window, bridge.atoms.type_list, XCB_ATOM_ATOM, 32,
            (uint32_t)total, all);
    uint32_t data[5];
    memset(data, 0, sizeof data);
    data[0] = source.window;
    data[1] = ((uint32_t)XDND_VERSION << 24) | (in_property ? 1u : 0u);
    if (total > 0) data[2] = all[0];
    if (total > 1) data[3] = all[1];
    if (total > 2) data[4] = all[2];
    send_message(window, bridge.atoms.enter, data);
    source.entered = window;
    source.accepted = false;
    source.action = 0;
    source.have_point = false;
}

static void source_send_leave(void)
{
    if (!source.entered) return;
    uint32_t data[5];
    memset(data, 0, sizeof data);
    data[0] = source.window;
    send_message(source.entered, bridge.atoms.leave, data);
    source.entered = 0;
    source.accepted = false;
    source.action = 0;
    source.have_point = false;
}

/// 光标在接收方窗口里的本地坐标 -> X 根坐标。缩放过的屏幕上两边的尺寸不一样，按比例折算。
static bool source_root_position(int *root_x, int *root_y)
{
    POINT cursor;
    if (!GetCursorPos(&cursor)) return false;
    int width = source.target_rect.right - source.target_rect.left;
    int height = source.target_rect.bottom - source.target_rect.top;
    double scale_x = width > 0 && source.target_width > 0 ? (double)source.target_width / width : 1.0;
    double scale_y = height > 0 && source.target_height > 0 ? (double)source.target_height / height : 1.0;
    *root_x = source.target_x + (int)lround((cursor.x - source.target_rect.left) * scale_x);
    *root_y = source.target_y + (int)lround((cursor.y - source.target_rect.top) * scale_y);
    return true;
}

static void source_send_position(void)
{
    if (!source.active || !source.entered) return;
    int root_x = 0, root_y = 0;
    // 自动分批时鼠标早就离开了：还用上次放下的那个点，光标现在的坐标跟接收窗口没关系。
    if (source.batching)
    {
        root_x = source.point_x;
        root_y = source.point_y;
    }
    else if (!source_root_position(&root_x, &root_y)) return;
    if (source.have_point && root_x == source.point_x && root_y == source.point_y) return;
    source.have_point = true;
    source.point_x = root_x;
    source.point_y = root_y;
    uint32_t data[5];
    memset(data, 0, sizeof data);
    data[0] = source.window;
    data[2] = ((uint32_t)root_x << 16) | ((uint32_t)root_y & 0xffff);
    data[3] = source.time;
    data[4] = source.proposed;
    send_message(source.entered, bridge.atoms.position, data);
}

static void source_send_drop(void)
{
    if (!source.entered || !source.accepted)
    {
        log_info(L"X11 拖动源：接收方没接受，不放下");
        source_finish(false);
        return;
    }
    uint32_t data[5];
    memset(data, 0, sizeof data);
    data[0] = source.window;
    data[2] = source.time;
    send_message(source.entered, bridge.atoms.drop, data);
    log_info(L"X11 拖动源：发出 XdndDrop");
    source.waiting_finish = true;
    source.drop_at = g_get_monotonic_time();
}

static bool source_start(void)
{
    if (!bridge.connection || !source.target) return false;
    if (!source_create_window() || !source_take_ownership()) return false;
    source.active = true;
    source.waiting_finish = false;
    source.have_point = false;
    source.owner_retries = 0;
    source.started_at = g_get_monotonic_time();
    source.checked_at = source.started_at;
    source_send_enter(source.target);
    source_send_position();
    log_info(L"X11 拖动源：接管这次拖出，目标窗口 0x%x（pid %d，%dx%d）", (unsigned)source.target, source.target_pid,
        source.target_width, source.target_height);
    return true;
}

/// 排着的批次不再送了：清掉队列，把置物架那次拖出的收尾补上。
static void source_batches_abort(void)
{
    source_batches_cancel(false);
    GdkDragAction action = source.batch_report;
    source.batch_report = 0;
    ui_drag_taken_over_finished(action);
}

static void source_finish(bool dropped)
{
    if (!source.active && !source.starting) return;
    bool batching = source.batching;
    GdkDragAction action = 0;
    if (batching) action = source.batch_report;
    else if (dropped && source.accepted)
        action = source.action == bridge.atoms.action_move && source.move_allowed ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
    log_info(L"X11 拖动源：%ls（动作 0x%x）", dropped ? L"放下完成" : L"取消", (unsigned)action);
    // 还有排着的批次、而且这一批真的放下了：记下窗口和落点，等一下接着送下一批。
    // 主拖动被拒或者取消时就不再送了，剩下的内容留在置物架上。
    bool next = dropped && source.accepted && !g_queue_is_empty(&batches);
    if (next)
    {
        source.batch_window = source.target;
        source.batch_pid = source.target_pid;
        source.batch_rect = source.target_rect;
        source.batch_target_x = source.target_x;
        source.batch_target_y = source.target_y;
        source.batch_target_width = source.target_width;
        source.batch_target_height = source.target_height;
        source.batch_time = source.time;
        source.batch_point_x = source.point_x;
        source.batch_point_y = source.point_y;
        if (!batching)
        {
            // 主拖动刚松手：落点就是最后一次 XdndPosition 报的点；没报过才按光标现算。
            // 后面几批都落在同一个点。
            if (!source.have_point)
            {
                int root_x = 0, root_y = 0;
                if (source_root_position(&root_x, &root_y))
                {
                    source.batch_point_x = root_x;
                    source.batch_point_y = root_y;
                }
            }
            source.batch_report = action;
        }
    }
    if (source.active && bridge.connection)
    {
        if (!dropped) source_send_leave();
        // 松手以后对方可能还在读数据，等它回过 XdndFinished（或者等够了）再把 selection 交回去。
        if (source.window && bridge.atoms.selection)
            xcb_set_selection_owner(bridge.connection, XCB_ATOM_NONE, bridge.atoms.selection, XCB_CURRENT_TIME);
        xcb_flush(bridge.connection);
    }
    source.starting = false;
    source.active = false;
    source.batching = false;
    source.target = 0;
    source.entered = 0;
    source.accepted = false;
    source.action = 0;
    source.waiting_finish = false;
    source.have_point = false;
    source.owner_retries = 0;
    source.dwell_pid = 0;
    source.dwell_since = 0;
    source.drop_types = 0;
    if (next)
    {
        log_info(L"X11 拖动源：还有 %u 批，%d 毫秒后接着送下一批", g_queue_get_length(&batches), BATCH_DELAY_MILLISECONDS);
        source.batch_timer = g_timeout_add(BATCH_DELAY_MILLISECONDS, source_batch_start, NULL);
        return;
    }
    source_batches_clear();
    source.batch_report = 0;
    ui_drag_taken_over_finished(action);
}

/// 送下一批：取出排在最前面的一批，重新和同一个窗口走一遍 Xdnd，不需要鼠标按键。
static gboolean source_batch_start(gpointer data)
{
    (void)data;
    source.batch_timer = 0;
    SourceBatch *batch = g_queue_pop_head(&batches);
    if (!batch)
    {
        source_batches_abort();
        return G_SOURCE_REMOVE;
    }
    if (source.active || source.starting || source.assisting || !bridge.connection)
    {
        log_info(L"X11 拖动源：下一批开始前状态变了，剩下 %u 批不送了", g_queue_get_length(&batches) + 1);
        source_batch_free(batch);
        source_batches_abort();
        return G_SOURCE_REMOVE;
    }
    source.batching = true;
    source.active = true;
    source.target = source.batch_window;
    source.target_pid = source.batch_pid;
    source.target_rect = source.batch_rect;
    source.target_x = source.batch_target_x;
    source.target_y = source.batch_target_y;
    source.target_width = source.batch_target_width;
    source.target_height = source.batch_target_height;
    source.point_x = source.batch_point_x;
    source.point_y = source.batch_point_y;
    source.have_point = false;
    source.time = source.batch_time;
    source.entered = 0;
    source.waiting_finish = false;
    source.accepted = false;
    source.action = 0;
    source.started_at = g_get_monotonic_time();
    source.checked_at = source.started_at;
    source.owner_retries = 0;
    source.drop_types = 0;
    g_free(source.uri_list);
    source.uri_list = batch->uri_list;
    batch->uri_list = NULL;
    g_free(source.text);
    source.text = batch->text;
    batch->text = NULL;
    g_free(source.html);
    source.html = NULL;
    source_batch_free(batch);
    source.prepared = source.uri_list != NULL || source.text != NULL;
    source.move_allowed = false;
    source.proposed = bridge.atoms.action_copy;
    if (!source_create_window() || !source_take_ownership())
    {
        log_info(L"X11 拖动源：下一批抢不到 XdndSelection，剩下的不送了");
        source_finish(false);
        return G_SOURCE_REMOVE;
    }
    source_send_enter(source.target);
    source_send_position();
    log_info(L"X11 拖动源：自动再送一批（%ls，还有 %u 批）到窗口 0x%x", source.uri_list ? L"文件" : L"文字",
        g_queue_get_length(&batches), (unsigned)source.target);
    return G_SOURCE_REMOVE;
}

static void source_handle_status(xcb_client_message_event_t *event)
{
    if (!source.active || !source.entered) return;
    if ((xcb_window_t)event->data.data32[0] != source.entered) return;
    bool accepted = (event->data.data32[1] & 1) != 0;
    uint32_t action = accepted ? event->data.data32[4] : 0;
    log_trace(L"X11 拖动源：XdndStatus 接受 %d，动作 0x%x（我们提议 0x%x）", accepted ? 1 : 0,
        (unsigned)event->data.data32[4], (unsigned)source.proposed);
    if (!accepted && source.proposed == bridge.atoms.action_move)
    {
        // 文档程序（WPS 这类）往往只肯复制，不接受移动。改成复制重新进入一次——
        // 它们只在进入时认一次动作，光换个位置不行。不然这次拖出去一点反应都没有。
        source.proposed = bridge.atoms.action_copy;
        log_info(L"X11 拖动源：接收方不接受移动，改按复制重新进入");
        source_send_leave();
        source_send_enter(source.target);
        source_send_position();
        return;
    }
    if (accepted && action != bridge.atoms.action_move) action = bridge.atoms.action_copy;
    if (accepted != source.accepted || action != source.action)
        log_info(L"X11 拖动源：接收方%ls%ls", accepted ? L"接受" : L"不接受",
            accepted ? (action == bridge.atoms.action_move ? L"（按移动）" : L"（按复制）") : L"");
    source.accepted = accepted;
    source.action = accepted ? action : 0;
}

static void source_handle_finished(xcb_client_message_event_t *event)
{
    if (!source.waiting_finish) return;
    bool success = event->data.data32[1] != 0;
    if (success && event->data.data32[2] == bridge.atoms.action_move) source.action = bridge.atoms.action_move;
    log_info(L"X11 拖动源：对方收完了%ls", success ? L"" : L"（没收下）");
    source_finish(success);
}

static void source_send_notify(xcb_selection_request_event_t *request, xcb_atom_t property)
{
    xcb_selection_notify_event_t notify;
    memset(&notify, 0, sizeof notify);
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.time = request->time;
    notify.requestor = request->requestor;
    notify.selection = request->selection;
    notify.target = request->target;
    notify.property = property;
    xcb_send_event(bridge.connection, 0, request->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&notify);
    xcb_flush(bridge.connection);
}

static void source_serve_selection(xcb_selection_request_event_t *request)
{
    if (!bridge.connection || !source.window) return;
    if (request->owner != source.window)
    {
        source_send_notify(request, XCB_ATOM_NONE);
        return;
    }
    xcb_atom_t property = request->property ? request->property : request->target;
    if (request->target == bridge.atoms.targets)
    {
        xcb_atom_t atoms[8];
        int count = 0;
        atoms[count++] = bridge.atoms.targets;
        if (source.html) atoms[count++] = bridge.atoms.text_html;
        if (source.uri_list) atoms[count++] = bridge.atoms.uri_list;
        if (source.text)
        {
            atoms[count++] = bridge.atoms.utf8_string;
            atoms[count++] = bridge.atoms.text_plain;
            atoms[count++] = bridge.atoms.text_utf8;
        }
        xcb_change_property(bridge.connection, XCB_PROP_MODE_REPLACE, request->requestor, property, XCB_ATOM_ATOM, 32, (uint32_t)count, atoms);
        source_send_notify(request, property);
        return;
    }
    const char *data = NULL;
    xcb_atom_t type = XCB_ATOM_NONE;
    uint32_t drop_type = 0;
    if (request->target == bridge.atoms.uri_list && source.uri_list)
    {
        data = source.uri_list;
        type = bridge.atoms.uri_list;
        drop_type = DROP_TYPE_URI_LIST;
    }
    else if (request->target == bridge.atoms.text_html && source.html)
    {
        data = source.html;
        type = bridge.atoms.text_html;
        drop_type = DROP_TYPE_HTML;
    }
    else if ((request->target == bridge.atoms.utf8_string || request->target == bridge.atoms.text_plain
                 || request->target == bridge.atoms.text_utf8) && source.text)
    {
        data = source.text;
        type = bridge.atoms.utf8_string;
        drop_type = DROP_TYPE_TEXT;
    }
    if (!data)
    {
        log_trace(L"X11 拖动源：对方要了不支持的格式（atom 0x%x）", (unsigned)request->target);
        source_send_notify(request, XCB_ATOM_NONE);
        return;
    }
    // 发出 XdndDrop 以后才来要的格式才是对方真的在取：记下来，出问题时从日志里能看出它取了什么。
    if (source.waiting_finish)
    {
        if (!(source.drop_types & drop_type)) log_info(L"X11 拖动源：对方放下时取了%ls", type == bridge.atoms.uri_list ? L"文件列表"
            : type == bridge.atoms.text_html ? L"HTML" : L"文字");
        source.drop_types |= drop_type;
    }
    size_t length = strlen(data);
    xcb_change_property(bridge.connection, XCB_PROP_MODE_REPLACE, request->requestor, property, type, 8, (uint32_t)length, data);
    log_info(L"X11 拖动源：对方来要数据（%ls），给了 %zu 字节",
        type == bridge.atoms.uri_list ? L"文件列表" : type == bridge.atoms.text_html ? L"HTML" : L"文字", length);
    source_send_notify(request, property);
}

static void source_handle_selection_clear(xcb_selection_clear_event_t *event)
{
    if (event->selection != bridge.atoms.selection || event->owner == source.window) return;
    if (source.assisting && !source.assist_clear_source)
    {
        // 拖到一半 selection 又被别人（多半是 Hyprland 原来那次拖动）抢回去：
        // 不马上拿回来，目标程序就会读到拿不到数据的那一边，退回粘贴剪贴板。
        log_trace(L"X11 拖动源：XdndSelection 被 0x%x 抢走了，重新接管", (unsigned)event->owner);
        source_take_ownership();
        return;
    }
    if (!source.active) return;
    // Hyprland 结束它那次拖动时会把 selection 收回去，下一拍再抢回来。
    log_trace(L"X11 拖动源：XdndSelection 被 0x%x 抢走了", (unsigned)event->owner);
    source.owner_retries = 0;
}

static void source_tick(gint64 now)
{
    if (source.starting)
    {
        if (now < source.start_at) return;
        source.starting = false;
        if (!source_start()) source_finish(false);
        return;
    }
    if (!source.active || !bridge.connection) return;
    if (source.batching)
    {
        // 自动分批不跟着鼠标走，也不看按键：接受了就放下，等 XdndFinished 收尾。
        if (source.waiting_finish)
        {
            if (now - source.drop_at >= SOURCE_FINISH_TIMEOUT_MICROSECONDS)
            {
                log_info(L"X11 拖动源：这一批等 XdndFinished 等太久，按已放下收尾");
                source_finish(true);
            }
            return;
        }
        if (source.accepted)
        {
            source_send_drop();
            return;
        }
        if (now - source.started_at >= BATCH_ACCEPT_TIMEOUT_MICROSECONDS)
        {
            log_info(L"X11 拖动源：接收方不理这一批，剩下的不送了");
            source_finish(false);
        }
        return;
    }
    if (source.waiting_finish)
    {
        if (now - source.drop_at >= SOURCE_FINISH_TIMEOUT_MICROSECONDS)
        {
            log_info(L"X11 拖动源：等 XdndFinished 等太久，按已放下收尾");
            source_finish(true);
        }
        return;
    }
    if (now - source.started_at >= SOURCE_TIMEOUT_MICROSECONDS)
    {
        log_info(L"X11 拖动源：拖得太久，收起来");
        source_finish(false);
        return;
    }
    if (now - source.checked_at >= SOURCE_RECHECK_MICROSECONDS)
    {
        source.checked_at = now;
        xcb_get_selection_owner_reply_t *owner =
            xcb_get_selection_owner_reply(bridge.connection, xcb_get_selection_owner(bridge.connection, bridge.atoms.selection), NULL);
        bool ours = owner && owner->owner == source.window;
        free(owner);
        if (!ours && source.owner_retries < 5)
        {
            source.owner_retries++;
            if (!source_take_ownership())
            {
                log_info(L"X11 拖动源：XdndSelection 抢不回来，取消");
                source_finish(false);
                return;
            }
        }
        POINT cursor;
        HyprClientAt client;
        if (!GetCursorPos(&cursor) || !hyprland_client_at(cursor.x, cursor.y, &client) || !client.found || !client.xwayland
            || client.pid <= 0)
        {
            log_info(L"X11 拖动源：光标离开 X11 程序，取消");
            source_finish(false);
            return;
        }
        if (client.pid != source.target_pid)
        {
            if (!source_find_target(client.pid, &client.rect))
            {
                log_info(L"X11 拖动源：找不到新的接收窗口，取消");
                source_finish(false);
                return;
            }
            xcb_window_t window = source.target;
            source_send_leave();
            source_send_enter(window);
        }
    }
    source_send_position();
    if (gesture_left_button_state() == 0) source_send_drop();
}

bool xdnd_source_should_take_over(void)
{
    if (!bridge.connection || !bridge.atoms.selection) return false;
    if (source.active || source.starting || source.assisting || !source.prepared) return false;
    if (!hyprland_available()) return false;
    POINT cursor;
    if (!GetCursorPos(&cursor)) return false;
    HyprClientAt client;
    if (!hyprland_client_at(cursor.x, cursor.y, &client) || !client.found || !client.xwayland || client.pid <= 0)
    {
        log_trace(L"接管检查：光标（%d,%d）下没有 XWayland 窗口（找到 %d，xwayland %d，pid %d）", (int)cursor.x,
            (int)cursor.y, client.found ? 1 : 0, client.xwayland ? 1 : 0, client.pid);
        source.dwell_pid = 0;
        source.dwell_since = 0;
        return false;
    }
    log_trace(L"接管检查：光标（%d,%d）下是 XWayland pid %d，窗口 %d,%d %dx%d", (int)cursor.x, (int)cursor.y, client.pid,
        (int)client.rect.left, (int)client.rect.top, (int)(client.rect.right - client.rect.left),
        (int)(client.rect.bottom - client.rect.top));
    gint64 now = g_get_monotonic_time();
    // 移动模式要换成自己的拖动源（会取消 GTK 那次），得确认光标真的停在 XWayland 程序上，
    // 免得只是路过就把拖动打断了。复制模式只是接管 selection，不打断拖动，看到目标就立刻接管。
    bool copy_only = !source.move_allowed;
    if (source.dwell_pid != client.pid || source.dwell_since == 0)
    {
        source.dwell_pid = client.pid;
        source.dwell_since = now;
        if (!copy_only) return false;
    }
    if (!copy_only && now - source.dwell_since < TAKEOVER_DWELL_MICROSECONDS) return false;
    // 真找到那个 X 窗口才动手：找不到就别把 GTK 的拖动打断。
    if (!source_find_target(client.pid, &client.rect))
    {
        log_trace(L"接管检查：pid %d 没有 XdndAware 的可见窗口", client.pid);
        source.dwell_pid = 0;
        source.dwell_since = 0;
        return false;
    }
    log_info(L"X11 拖动源：光标停在 XWayland 程序上（pid %d，窗口 0x%x）", client.pid, (unsigned)source.target);
    return true;
}

bool xdnd_source_begin(void)
{
    if (!bridge.connection || !source.target || source.active || source.starting) return false;
    source.starting = true;
    source.start_at = g_get_monotonic_time() + TAKEOVER_SETTLE_MICROSECONDS;
    source.dwell_pid = 0;
    source.dwell_since = 0;
    return true;
}

bool xdnd_source_active(void)
{
    return source.active || source.starting;
}

void xdnd_source_abort(void)
{
    if (source.assisting)
    {
        xdnd_source_assist_finish();
        return;
    }
    if (source.batch_timer)
    {
        source_batches_cancel(true);
        return;
    }
    if (!source.active && !source.starting) return;
    source_finish(false);
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
    drag_action = bridge.atoms.action_copy;
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
    // 和 Windows 一样一律按复制收：不能把来源自己提议的动作原样答应回去——WPS 和 Obsidian 提议的总是移动，
    // 答应了它们松手后会删掉原文。
    drag_action = bridge.atoms.action_copy;
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
            else if (source.window && message->window == source.window)
            {
                if (message->type == bridge.atoms.status) source_handle_status(message);
                else if (message->type == bridge.atoms.finished) source_handle_finished(message);
            }
            else log_trace(L"X11 拖放：客户端消息 0x%x（窗口 0x%x，不是我们）", (unsigned)message->type, (unsigned)message->window);
        }
        // 其它事件（X 错误、配置变化）不用管。
        else if (type == XCB_SELECTION_NOTIFY) handle_selection_notify((xcb_selection_notify_event_t *)event);
        else if (type == XCB_PROPERTY_NOTIFY) handle_property_notify((xcb_property_notify_event_t *)event);
        else if (type == XCB_SELECTION_REQUEST) source_serve_selection((xcb_selection_request_event_t *)event);
        else if (type == XCB_SELECTION_CLEAR) source_handle_selection_clear((xcb_selection_clear_event_t *)event);
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
    if (!probe_active && !sinks && !drag_active && !xdnd_source_active())
    {
        if (bridge.mapped) hide_window();
        return G_SOURCE_CONTINUE;
    }
    // 置物架开着但没人在拖：不用往下走。下面每拍都要问 Hyprland 光标在哪（一次 IPC），
    // 只有按住左键、X 那边也真有拖动时才值得问。
    if (!probe_active && !drag_active && !xdnd_source_active() && (gesture_left_button_state() == 0 || !x_drag_owner_exists()))
    {
        set_target(NULL);
        if (bridge.mapped) hide_window();
        return G_SOURCE_CONTINUE;
    }
    gint64 now = g_get_monotonic_time();
    POINT cursor = {0};
    bool have_cursor = GetCursorPos(&cursor);
    // 自己当拖动源的时候，位置、换窗口、松手都靠这一拍盯着。
    source_tick(now);

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
    // 还要按住左键：有的程序（WPS）拖完不把 XdndSelection 交回来，之后一直像“正在拖”，
    // 只凭这个就把窗口挂上去的话，那块铺满屏幕的窗口会把置物架上的点击全吃掉，开关都点不动。
    // 真正拖动时左键一定是按着的；看不到按键状态的设备（负数）照旧，免得把正常拖动挡掉。
    bool holding = gesture_left_button_state() != 0;
    // 自己正从置物架往外拖：这时候左键按着、光标也在置物架上，条件全对，但窗口一挂上去
    // 落点就变成我们自己了，拖到哪儿都没有接收方——图片、文件都拖不出去。让它待在下面。
    bool outgoing = ui_active_drag() != NULL || xdnd_source_active();
    bool interested = drag_active || (dragging && !outgoing && holding && (probe_active || under != NULL));

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
    // 自己当拖动源的那套也一起收掉，不再回调置物架。
    if (source.assist_clear_source)
    {
        g_source_remove(source.assist_clear_source);
        source.assist_clear_source = 0;
    }
    source.starting = false;
    source.active = false;
    source.batching = false;
    source.assisting = false;
    if (source.batch_timer)
    {
        g_source_remove(source.batch_timer);
        source.batch_timer = 0;
    }
    source_batches_clear();
    source.batch_report = 0;
    source.target = 0;
    source.entered = 0;
    source.waiting_finish = false;
    source.dwell_pid = 0;
    source.dwell_since = 0;
    source_clear_payload();
    if (bridge.connection && source.window) xcb_destroy_window(bridge.connection, source.window);
    source.window = 0;
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
    bool (*frame)(void *context, int *x, int *y, int *width, int *height))
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