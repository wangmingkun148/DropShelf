#include "ui.h"
#include "ui_internal.h"
#include "lang.h"
#include "log.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

#define UI_WINDOW_CLASS L"DropShelfWindow"

// 较新的 DWM 属性，旧的 SDK 头文件里可能没有。
#define ATTRIBUTE_TRANSITIONS_FORCEDISABLED 3
#define ATTRIBUTE_USE_IMMERSIVE_DARK_MODE_OLD 19
#define ATTRIBUTE_USE_IMMERSIVE_DARK_MODE 20
#define ATTRIBUTE_WINDOW_CORNER_PREFERENCE 33
#define ATTRIBUTE_BORDER_COLOR 34
#define ATTRIBUTE_SYSTEMBACKDROP_TYPE 38
#define CORNER_DO_NOT_ROUND 1
#define BORDER_COLOR_NONE 0xFFFFFFFE

struct UiWindow
{
    HWND handle;
    UiWindowOptions options;
    GfxSurface *surface;
    UiElement *root;
    UiElement *hovered;         // 光标下的元素
    UiElement *captured;        // 按下鼠标时的元素，松开之前事件都交给它
    bool tracking_mouse;        // 已经请求了“光标离开窗口”的通知
    bool active;
    UiBackdrop applied_backdrop;
    bool backdrop_active;
    bool layout_dirty;
    bool frame_requested;       // 画的过程中有元素说还有动画在进行
    bool subscribed;            // 已经订阅逐帧回调
    bool rendering;
    bool animating;             // 正在调用 animate 回调（它可能改窗口大小，引起嵌套的重画）
    int message_depth;          // 正在处理的窗口消息有几层
    bool destroyed;             // 窗口已经销毁，等消息都处理完再释放这块内存
    HWND tooltip;               // 提示框（第一次需要时创建）
    wchar_t *tooltip_text;
    UiWindow *next;
};

static UiWindow *windows;
static UiWindow *creating;      // 正在 CreateWindowEx 里的窗口
static bool theme_listener_added;

// ───────────── 系统版本和窗口外观 ─────────────

static DWORD windows_build(void)
{
    static DWORD build;
    if (build) return build;
    // GetVersionEx 会按清单谎报版本，RtlGetVersion 给的是真的。
    typedef LONG (WINAPI *RtlGetVersionFunction)(OSVERSIONINFOW *);
    OSVERSIONINFOW version = { .dwOSVersionInfoSize = sizeof(version) };
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFunction get_version = ntdll ? (RtlGetVersionFunction)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    build = get_version && get_version(&version) == 0 ? version.dwBuildNumber : 19041;
    return build;
}

static bool is_windows_11(void)
{
    return windows_build() >= 22000;
}

static bool supports_system_backdrop(void)
{
    return windows_build() >= 22621;
}

/// 按主题设置窗口深浅色并启用系统材质。材质的深浅在启用时确定，之后只改深浅色不会重绘，
/// 所以先设深浅色，主题切换时（force）先去掉材质再重新启用。
static void apply_appearance(UiWindow *window, bool force)
{
    HWND handle = window->handle;
    BOOL dark = theme_is_dark();
    if (FAILED(DwmSetWindowAttribute(handle, ATTRIBUTE_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        DwmSetWindowAttribute(handle, ATTRIBUTE_USE_IMMERSIVE_DARK_MODE_OLD, &dark, sizeof(dark));

    if (window->options.popup)
    {
        // 显示和隐藏时不播放系统的淡入淡出；Windows 11 上不画圆角和边框，透明窗口里只看得到内容。
        BOOL disabled = TRUE;
        DwmSetWindowAttribute(handle, ATTRIBUTE_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
        if (is_windows_11())
        {
            int corner = CORNER_DO_NOT_ROUND;
            DwmSetWindowAttribute(handle, ATTRIBUTE_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            COLORREF border = BORDER_COLOR_NONE;
            DwmSetWindowAttribute(handle, ATTRIBUTE_BORDER_COLOR, &border, sizeof(border));
        }
        return;
    }

    // 低性能模式下不用材质：半透明的材质要由 Windows 和窗口背后的内容一起合成，用电池时显卡跟不上，动画会卡。
    UiBackdrop wanted = low_performance_enabled() || window->options.backdrop == 0 ? UI_BACKDROP_NONE : window->options.backdrop;
    if (!force && wanted == window->applied_backdrop) return;
    window->applied_backdrop = wanted;
    window->backdrop_active = false;
    if (!supports_system_backdrop()) return;

    int none = UI_BACKDROP_NONE;
    DwmSetWindowAttribute(handle, ATTRIBUTE_SYSTEMBACKDROP_TYPE, &none, sizeof(none));
    MARGINS margins = { 0, 0, 0, 0 };
    if (wanted != UI_BACKDROP_NONE)
    {
        // 整个窗口都当作“玻璃”，材质才会铺满客户区。
        margins = (MARGINS){ -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(handle, &margins);
        int value = wanted;
        window->backdrop_active = SUCCEEDED(DwmSetWindowAttribute(handle, ATTRIBUTE_SYSTEMBACKDROP_TYPE, &value, sizeof(value)));
    }
    else
    {
        DwmExtendFrameIntoClientArea(handle, &margins);
    }
}

// ───────────── 画 ─────────────

static void on_frame(void *context, double time);

static void render(UiWindow *window, double time)
{
    if (window->rendering || window->animating || !IsWindowVisible(window->handle)) return;
    // animate 里还有动画在进行时会请求下一帧，画的时候不能把这个请求清掉。
    bool animation_requested = false;
    if (window->options.animate)
    {
        // 动画先走到这一帧，窗口的位置和大小定下来以后再按新的大小画。
        window->frame_requested = false;
        window->animating = true;
        window->options.animate(window, window->options.context, time);
        window->animating = false;
        if (window->destroyed || !window->handle || !IsWindowVisible(window->handle)) return;
        animation_requested = window->frame_requested;
    }
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    window->rendering = true;
    window->frame_requested = animation_requested;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root && window->layout_dirty)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    window->layout_dirty = false;

    if (gfx_begin(window->surface, width, height, dpi))
    {
        bool transparent = window->options.popup || window->backdrop_active;
        gfx_clear(transparent ? COLOR_TRANSPARENT : theme_color(window->options.background));
        if (window->options.paint) window->options.paint(window, window->options.context, time);
        if (window->root) ui_paint(window->root, time);
        // 显卡设备丢失时已经重建，下一帧重画一次。
        if (!gfx_end(window->surface)) window->frame_requested = true;
    }
    window->rendering = false;

    // 还有动画在进行就接着订阅逐帧回调，否则退订，不占用处理器。
    if (window->frame_requested && !window->subscribed) frames_subscribe(on_frame, window);
    else if (!window->frame_requested && window->subscribed) frames_unsubscribe(on_frame, window);
    window->subscribed = window->frame_requested;
}

static void on_frame(void *context, double time)
{
    UiWindow *window = context;
    // 低性能模式下高刷新率的屏幕隔一帧画一次。
    if (low_performance_skip(time)) return;
    render(window, time);
}

void ui_window_request_frame(UiWindow *window)
{
    if (!window || window->destroyed) return;
    window->frame_requested = true;
    // 不在画的过程中调用时（比如刚开始一段动画），直接订阅，下一帧就会来画。
    if (!window->rendering && !window->animating && !window->subscribed)
    {
        frames_subscribe(on_frame, window);
        window->subscribed = true;
    }
}

void ui_window_invalidate(UiWindow *window)
{
    if (!window || !window->handle) return;
    window->layout_dirty = true;
    InvalidateRect(window->handle, NULL, FALSE);
}

void ui_window_repaint(UiWindow *window)
{
    if (window && window->handle) InvalidateRect(window->handle, NULL, FALSE);
}

// ───────────── 鼠标 ─────────────

static void send_mouse(UiElement *element, UiMouseKind kind, const UiMouseEvent *source)
{
    if (!element || !element->type->mouse) return;
    UiMouseEvent event = *source;
    event.kind = kind;
    element->type->mouse(element, &event);
}

static void set_hovered(UiWindow *window, UiElement *element, const UiMouseEvent *event)
{
    if (window->hovered == element) return;
    UiElement *previous = window->hovered;
    window->hovered = element;
    if (previous)
    {
        previous->hovered = false;
        send_mouse(previous, UI_MOUSE_LEAVE, event);
    }
    // 上面的回调可能改了光标下的元素（比如销毁了它），再确认一次。
    if (window->hovered == element && element)
    {
        element->hovered = true;
        send_mouse(element, UI_MOUSE_ENTER, event);
    }
    ui_window_repaint(window);
}

static UiMouseEvent make_mouse_event(UiWindow *window, UiMouseKind kind, LPARAM lparam)
{
    float scale = ui_window_scale(window);
    UiMouseEvent event = { .kind = kind };
    event.x = GET_X_LPARAM(lparam) / scale;
    event.y = GET_Y_LPARAM(lparam) / scale;
    return event;
}

static UiElement *element_at(UiWindow *window, float x, float y)
{
    return window->root ? ui_hit_test(window->root, x, y) : NULL;
}

static void on_mouse_move(UiWindow *window, LPARAM lparam)
{
    if (!window->tracking_mouse)
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, window->handle, 0 };
        window->tracking_mouse = TrackMouseEvent(&track) != 0;
    }
    UiMouseEvent event = make_mouse_event(window, UI_MOUSE_MOVE, lparam);
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    // 按住鼠标期间只有按下的那个元素算悬停，移出去就不算。
    if (window->captured) under = under == window->captured ? under : NULL;
    set_hovered(window, under, &event);
    send_mouse(window->captured ? window->captured : under, UI_MOUSE_MOVE, &event);
}

static void on_mouse_leave(UiWindow *window)
{
    window->tracking_mouse = false;
    UiMouseEvent event = { .kind = UI_MOUSE_LEAVE, .x = -1, .y = -1 };
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    if (!window->captured) set_hovered(window, NULL, &event);
}

static void on_mouse_down(UiWindow *window, LPARAM lparam, bool right)
{
    UiMouseEvent event = make_mouse_event(window, UI_MOUSE_DOWN, lparam);
    event.right_button = right;
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    set_hovered(window, under, &event);
    if (!under || under->disabled) return;
    if (!right)
    {
        window->captured = under;
        under->pressed = true;
        SetCapture(window->handle);
    }
    send_mouse(under, UI_MOUSE_DOWN, &event);
    ui_window_repaint(window);
}

static void on_mouse_up(UiWindow *window, LPARAM lparam, bool right)
{
    UiMouseEvent event = make_mouse_event(window, UI_MOUSE_UP, lparam);
    event.right_button = right;
    if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return;
    UiElement *under = element_at(window, event.x, event.y);
    if (right)
    {
        event.inside = true;
        if (under && !under->disabled) send_mouse(under, UI_MOUSE_UP, &event);
        return;
    }
    UiElement *captured = window->captured;
    if (!captured) return;
    window->captured = NULL;
    captured->pressed = false;
    ReleaseCapture();
    event.inside = under == captured;
    send_mouse(captured, UI_MOUSE_UP, &event);
    // 点击的处理里窗口可能已经关闭。
    if (!window->handle) return;
    set_hovered(window, element_at(window, event.x, event.y), &event);
    ui_window_repaint(window);
}

void ui_window_forget(UiWindow *window, UiElement *element)
{
    if (!window) return;
    if (window->hovered == element) window->hovered = NULL;
    if (window->captured == element)
    {
        window->captured = NULL;
        if (GetCapture() == window->handle) ReleaseCapture();
    }
    if (window->root == element) window->root = NULL;
}

// ───────────── 窗口消息 ─────────────

static int frame_thickness(HWND handle)
{
    UINT dpi = GetDpiForWindow(handle);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

/// 没有系统标题栏的窗口：顶边可以调整大小，顶部一条可以拖动。
static LRESULT hit_test(UiWindow *window, LPARAM lparam)
{
    LRESULT result = DefWindowProcW(window->handle, WM_NCHITTEST, 0, lparam);
    if (result != HTCLIENT) return result;
    POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    ScreenToClient(window->handle, &point);
    RECT client;
    GetClientRect(window->handle, &client);
    if (window->options.resizable && !IsZoomed(window->handle))
    {
        int border = frame_thickness(window->handle);
        if (point.y < border)
        {
            if (point.x < border) return HTTOPLEFT;
            if (point.x >= client.right - border) return HTTOPRIGHT;
            return HTTOP;
        }
    }
    float scale = ui_window_scale(window);
    float x = point.x / scale, y = point.y / scale;
    if (y < window->options.caption_height && !element_at(window, x, y)) return HTCAPTION;
    return HTCLIENT;
}

static void destroy_window_state(UiWindow *window)
{
    // 提示框属于这个窗口，系统随窗口一起销毁。
    window->tooltip = NULL;
    free(window->tooltip_text);
    window->tooltip_text = NULL;
    if (window->subscribed) frames_unsubscribe(on_frame, window);
    window->subscribed = false;
    for (UiWindow **link = &windows; *link; link = &(*link)->next)
    {
        if (*link != window) continue;
        *link = window->next;
        break;
    }
    UiElement *root = window->root;
    window->root = NULL;
    window->hovered = window->captured = NULL;
    window->handle = NULL;
    if (root) ui_element_destroy(root);
    gfx_surface_destroy(window->surface);
    window->surface = NULL;
    if (creating == window) creating = NULL;
    if (window->options.closed) window->options.closed(window, window->options.context);
    // 点击按钮关掉窗口时，外层还在处理这次点击的消息，等它们都返回了再释放。
    window->destroyed = true;
    if (window->message_depth == 0) free(window);
}

static LRESULT handle_message(UiWindow *window, HWND handle, UINT message, WPARAM wparam, LPARAM lparam);

static LRESULT CALLBACK window_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    UiWindow *window = (UiWindow *)GetWindowLongPtrW(handle, GWLP_USERDATA);
    if (message == WM_NCCREATE)
    {
        window = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        window->handle = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)window);
    }
    if (!window) return DefWindowProcW(handle, message, wparam, lparam);

    window->message_depth++;
    LRESULT result = handle_message(window, handle, message, wparam, lparam);
    if (--window->message_depth == 0 && window->destroyed) free(window);
    return result;
}

static LRESULT handle_message(UiWindow *window, HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    LRESULT hooked = 0;
    if (window->options.message && message != WM_NCDESTROY &&
        window->options.message(window, window->options.context, message, wparam, lparam, &hooked))
        return hooked;
    switch (message)
    {
        case WM_NCACTIVATE:
            // 没有边框的窗口不让系统重画非客户区，否则激活时会闪一下标题栏。
            if (window->options.frameless) return DefWindowProcW(handle, message, wparam, -1);
            break;
        case WM_NCCALCSIZE:
            // 没有边框：整个窗口都是客户区。
            if (window->options.frameless && wparam) return 0;
            if (!window->options.popup && wparam)
            {
                // 去掉系统的标题栏：左、右、下的边框照旧（调整大小用），顶上不留。
                NCCALCSIZE_PARAMS *parameters = (NCCALCSIZE_PARAMS *)lparam;
                LONG top = parameters->rgrc[0].top;
                DefWindowProcW(handle, message, wparam, lparam);
                parameters->rgrc[0].top = top;
                // 最大化时窗口比屏幕大出一圈边框，顶上也要让出来，否则内容被切掉一条。
                if (IsZoomed(handle)) parameters->rgrc[0].top += frame_thickness(handle);
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (window->options.popup || window->options.frameless) return HTCLIENT;
            return hit_test(window, lparam);
        case WM_MOUSEACTIVATE:
            if (window->options.no_activate) return MA_NOACTIVATE;
            break;
        case WM_GETMINMAXINFO:
            if (window->options.minimum_width > 0 || window->options.minimum_height > 0)
            {
                RECT outer, client;
                GetWindowRect(handle, &outer);
                GetClientRect(handle, &client);
                float scale = ui_window_scale(window);
                MINMAXINFO *info = (MINMAXINFO *)lparam;
                info->ptMinTrackSize.x = (LONG)(window->options.minimum_width * scale) + (outer.right - outer.left) - client.right;
                info->ptMinTrackSize.y = (LONG)(window->options.minimum_height * scale) + (outer.bottom - outer.top) - client.bottom;
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(handle, NULL);
            render(window, frames_time());
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                // 拖动边框调整大小时系统在自己的循环里，直接画，内容才跟得上窗口。
                window->layout_dirty = true;
                render(window, frames_time());
                if (window->options.moved && (wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED)) window->options.moved(window, window->options.context);
            }
            return 0;
        case WM_EXITSIZEMOVE:
            if (window->options.moved) window->options.moved(window, window->options.context);
            return 0;
        case WM_DPICHANGED:
        {
            // 挪到了缩放比例不同的显示器：按系统建议的位置和大小调整。
            const RECT *suggested = (const RECT *)lparam;
            SetWindowPos(handle, NULL, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            ui_window_invalidate(window);
            if (window->options.moved) window->options.moved(window, window->options.context);
            return 0;
        }
        case WM_ACTIVATE:
            window->active = LOWORD(wparam) != WA_INACTIVE;
            // 低性能模式的自动档随电源状态变化，窗口激活时再查一次要不要用材质。
            if (window->active) apply_appearance(window, false);
            ui_window_repaint(window);
            if (window->options.activated) window->options.activated(window, window->options.context, window->active);
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(window, lparam);
            return 0;
        case WM_MOUSELEAVE:
            on_mouse_leave(window);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            on_mouse_down(window, lparam, false);
            return 0;
        case WM_LBUTTONUP:
            on_mouse_up(window, lparam, false);
            return 0;
        case WM_RBUTTONDOWN:
            on_mouse_down(window, lparam, true);
            return 0;
        case WM_RBUTTONUP:
            on_mouse_up(window, lparam, true);
            return 0;
        case WM_CAPTURECHANGED:
            if (window->captured && (HWND)lparam != handle)
            {
                // 鼠标被别的窗口拿走：当作没有点成。
                UiElement *captured = window->captured;
                UiMouseEvent event = { .kind = UI_MOUSE_UP, .x = -1, .y = -1 };
                window->captured = NULL;
                captured->pressed = false;
                send_mouse(captured, UI_MOUSE_UP, &event);
                ui_window_repaint(window);
            }
            return 0;
        case WM_MOUSEWHEEL:
        {
            POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            ScreenToClient(handle, &point);
            float scale = ui_window_scale(window);
            UiMouseEvent event = { .kind = UI_MOUSE_WHEEL, .x = point.x / scale, .y = point.y / scale };
            event.wheel = GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA;
            if (window->options.mouse && window->options.mouse(window, window->options.context, &event)) return 0;
            // 滚轮交给光标下的元素，它不处理就一级级往上找。
            for (UiElement *element = element_at(window, event.x, event.y); element; element = element->parent)
            {
                if (!element->type->mouse) continue;
                element->type->mouse(element, &event);
                break;
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT)
            {
                if (window->options.cursor)
                {
                    POINT point;
                    GetCursorPos(&point);
                    ScreenToClient(handle, &point);
                    float scale = ui_window_scale(window);
                    LPCWSTR cursor = window->options.cursor(window, window->options.context, point.x / scale, point.y / scale);
                    if (cursor)
                    {
                        SetCursor(LoadCursorW(NULL, cursor));
                        return TRUE;
                    }
                }
                // 能点的控件上显示手形光标。
                bool clickable = window->hovered && !window->hovered->disabled;
                SetCursor(LoadCursorW(NULL, clickable ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (window->options.key && window->options.key(window, window->options.context, (unsigned)wparam)) return 0;
            break;
        case WM_CLOSE:
            DestroyWindow(handle);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
            destroy_window_state(window);
            return DefWindowProcW(handle, message, wparam, lparam);
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

// ───────────── 创建和放置 ─────────────

/// 界面语言变了：控件上的文字是现取的，重新排版并重画就是新的语言。
static void on_language_changed(void *context)
{
    (void)context;
    for (UiWindow *window = windows; window; window = window->next) ui_window_invalidate(window);
}

static void on_theme_changed(void *context)
{
    (void)context;
    for (UiWindow *window = windows; window; window = window->next)
    {
        apply_appearance(window, true);
        ui_window_invalidate(window);
    }
}

bool ui_window_snapshot(UiWindow *window, const wchar_t *path)
{
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || window->rendering) return false;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    if (!gfx_begin_image(width, height, dpi)) return false;
    window->rendering = true;
    double time = frames_time();
    gfx_clear(window->options.popup ? COLOR_TRANSPARENT : theme_color(window->options.background));
    if (window->options.paint) window->options.paint(window, window->options.context, time);
    if (window->root) ui_paint(window->root, time);
    window->rendering = false;
    return gfx_end_image(path);
}

void ui_window_warm(UiWindow *window)
{
    if (!window || !window->handle || window->rendering) return;
    RECT client;
    GetClientRect(window->handle, &client);
    int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    float dpi = (float)GetDpiForWindow(window->handle);
    float scale = dpi / 96.0f;
    if (window->root)
    {
        ui_measure(window->root, width / scale);
        ui_arrange(window->root, (GfxRect){ 0, 0, width / scale, height / scale });
    }
    if (!gfx_begin_image(width, height, dpi)) return;
    window->rendering = true;
    double time = frames_time();
    gfx_clear(COLOR_TRANSPARENT);
    if (window->options.paint) window->options.paint(window, window->options.context, time);
    if (window->root) ui_paint(window->root, time);
    window->rendering = false;
    HBITMAP bitmap = gfx_end_image_bitmap();
    if (bitmap) DeleteObject(bitmap);
}

void ui_snapshot_windows(const wchar_t *directory)
{
    directory_create(directory);
    int number = 1;
    for (UiWindow *window = windows; window; window = window->next, number++)
    {
        wchar_t name[32];
        swprintf(name, 32, L"window-%d.png", number);
        wchar_t *path = path_join(directory, name);
        ui_window_snapshot(window, path);
        free(path);
    }
}

void ui_refresh_window_appearance(void)
{
    for (UiWindow *window = windows; window; window = window->next)
    {
        apply_appearance(window, false);
        ui_window_invalidate(window);
    }
}

UiWindow *ui_window_create(const UiWindowOptions *options)
{
    if (!gfx_initialize()) return NULL;
    HINSTANCE instance = GetModuleHandleW(NULL);
    static bool registered;
    if (!registered)
    {
        WNDCLASSEXW class = { .cbSize = sizeof(class) };
        class.style = CS_DBLCLKS;
        class.lpfnWndProc = window_procedure;
        class.hInstance = instance;
        class.hCursor = LoadCursorW(NULL, IDC_ARROW);
        class.lpszClassName = UI_WINDOW_CLASS;
        if (!RegisterClassExW(&class)) return NULL;
        registered = true;
    }
    if (!theme_listener_added)
    {
        theme_add_listener(on_theme_changed, NULL);
        language_add_listener(on_language_changed, NULL);
        theme_listener_added = true;
    }

    UiWindow *window = xcalloc(1, sizeof(UiWindow));
    window->options = *options;
    window->layout_dirty = true;
    DWORD style, extended = WS_EX_NOREDIRECTIONBITMAP;
    if (options->popup)
    {
        style = WS_POPUP;
        extended |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        if (options->no_activate) extended |= WS_EX_NOACTIVATE;
    }
    else if (options->frameless)
    {
        // 带标题栏样式系统才画圆角和阴影；WM_NCCALCSIZE 里把整个窗口都算作客户区，标题栏并不显示。
        style = WS_OVERLAPPED | WS_CAPTION;
        extended |= WS_EX_TOOLWINDOW;
        if (options->no_activate) extended |= WS_EX_NOACTIVATE;
    }
    else
    {
        style = WS_OVERLAPPEDWINDOW;
        if (!options->resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    creating = window;
    HWND handle = CreateWindowExW(extended, UI_WINDOW_CLASS, options->title ? options->title : L"", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, instance, window);
    if (!handle)
    {
        log_win32_error(L"创建窗口失败", GetLastError());
        // 创建到一半失败时窗口已经走过销毁的流程，那里释放过了。
        if (creating == window) free(window);
        creating = NULL;
        return NULL;
    }
    creating = NULL;
    window->surface = gfx_surface_create(handle);
    window->next = windows;
    windows = window;
    if (options->icon)
    {
        SendMessageW(handle, WM_SETICON, ICON_BIG, (LPARAM)options->icon);
        SendMessageW(handle, WM_SETICON, ICON_SMALL, (LPARAM)options->icon);
    }
    apply_appearance(window, true);
    // 让系统按 WM_NCCALCSIZE 的新结果重新算一遍边框。
    SetWindowPos(handle, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (options->width > 0 && options->height > 0)
    {
        RECT outer;
        GetWindowRect(handle, &outer);
        ui_window_place(window, outer.left, outer.top, options->width, options->height);
    }
    return window;
}

HWND ui_window_handle(UiWindow *window)
{
    return window ? window->handle : NULL;
}

void ui_window_set_root(UiWindow *window, UiElement *root)
{
    UiElement *previous = window->root;
    window->root = NULL;
    window->hovered = window->captured = NULL;
    if (previous) ui_element_destroy(previous);
    window->root = root;
    if (root) ui_element_attach(root, window);
    ui_window_invalidate(window);
}

UiElement *ui_window_root(UiWindow *window)
{
    return window->root;
}

void ui_window_show(UiWindow *window)
{
    if (window->options.no_activate)
    {
        ShowWindow(window->handle, SW_SHOWNOACTIVATE);
    }
    else
    {
        if (IsIconic(window->handle)) ShowWindow(window->handle, SW_RESTORE);
        else ShowWindow(window->handle, SW_SHOW);
        SetForegroundWindow(window->handle);
    }
    ui_window_invalidate(window);
}

void ui_window_show_with(UiWindow *window, bool activate)
{
    if (!activate || window->options.no_activate)
    {
        ShowWindow(window->handle, SW_SHOWNOACTIVATE);
    }
    else
    {
        ShowWindow(window->handle, SW_SHOW);
        SetForegroundWindow(window->handle);
    }
    ui_window_invalidate(window);
}

void ui_window_set_topmost(UiWindow *window, bool topmost)
{
    if (!window || !window->handle) return;
    SetWindowPos(window->handle, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ui_window_set_border_color(UiWindow *window, const Color *color)
{
    if (!window || !window->handle || !is_windows_11()) return;
    COLORREF value = color ? RGB(color->r, color->g, color->b) : 0xFFFFFFFF;    // DWMWA_COLOR_DEFAULT
    DwmSetWindowAttribute(window->handle, ATTRIBUTE_BORDER_COLOR, &value, sizeof(value));
}

void ui_window_begin_move(UiWindow *window)
{
    if (!window || !window->handle) return;
    // 和 WPF 的 DragMove 一样：放开鼠标捕获，假装按在标题栏上，系统接着拖动窗口。
    if (window->captured)
    {
        window->captured->pressed = false;
        window->captured = NULL;
    }
    ReleaseCapture();
    SendMessageW(window->handle, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
}

// ───────────── 提示 ─────────────

/// 整个窗口作为一个提示区域：光标停住一会儿就显示当前的文字，文字换了先收起，光标再停住时显示新的。
static bool ensure_tooltip(UiWindow *window)
{
    if (window->tooltip) return true;
    static bool initialized;
    if (!initialized)
    {
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&controls);
        initialized = true;
    }
    window->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window->handle, NULL, GetModuleHandleW(NULL), NULL);
    if (!window->tooltip) return false;
    TTTOOLINFOW tool = { sizeof(tool) };
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = window->handle;
    tool.uId = (UINT_PTR)window->handle;
    tool.lpszText = L"";
    SendMessageW(window->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
    // 长的说明折成几行，宽度和 WPF 的提示差不多。
    SendMessageW(window->tooltip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)(int)(320 * ui_window_scale(window)));
    return true;
}

void ui_window_set_tooltip(UiWindow *window, const wchar_t *text)
{
    if (!window || !window->handle || wcs_equal(window->tooltip_text, text)) return;
    free(window->tooltip_text);
    window->tooltip_text = wcs_dup(text);
    if (!text || !*text)
    {
        if (window->tooltip)
        {
            SendMessageW(window->tooltip, TTM_POP, 0, 0);
            SendMessageW(window->tooltip, TTM_ACTIVATE, FALSE, 0);
        }
        return;
    }
    if (!ensure_tooltip(window)) return;
    SendMessageW(window->tooltip, TTM_POP, 0, 0);
    TTTOOLINFOW tool = { sizeof(tool) };
    tool.hwnd = window->handle;
    tool.uId = (UINT_PTR)window->handle;
    tool.lpszText = window->tooltip_text;
    SendMessageW(window->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
    SendMessageW(window->tooltip, TTM_ACTIVATE, TRUE, 0);
}

void ui_window_close(UiWindow *window)
{
    if (window && window->handle) DestroyWindow(window->handle);
}

void ui_close_all_windows(void)
{
    while (windows) DestroyWindow(windows->handle);
}

float ui_window_scale(UiWindow *window)
{
    UINT dpi = window && window->handle ? GetDpiForWindow(window->handle) : 96;
    return dpi > 0 ? dpi / 96.0f : 1.0f;
}

GfxSize ui_window_size(UiWindow *window)
{
    RECT client;
    GetClientRect(window->handle, &client);
    float scale = ui_window_scale(window);
    return (GfxSize){ client.right / scale, client.bottom / scale };
}

bool ui_window_is_active(UiWindow *window)
{
    return window->active;
}

bool ui_window_has_backdrop(UiWindow *window)
{
    return window->backdrop_active;
}

void ui_window_place(UiWindow *window, int x, int y, float width, float height)
{
    HWND handle = window->handle;
    if (width <= 0 || height <= 0)
    {
        SetWindowPos(handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    // 先挪过去，窗口才知道自己在哪个显示器上、缩放比例是多少；再按那里的比例定大小。
    SetWindowPos(handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT outer, client;
    GetWindowRect(handle, &outer);
    GetClientRect(handle, &client);
    float scale = ui_window_scale(window);
    int frame_width = (outer.right - outer.left) - client.right;
    int frame_height = (outer.bottom - outer.top) - client.bottom;
    SetWindowPos(handle, NULL, 0, 0, (int)(width * scale + 0.5f) + frame_width, (int)(height * scale + 0.5f) + frame_height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ui_window_center(UiWindow *window)
{
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    // 先挪到那个显示器上，让大小按它的缩放比例调整好，再居中。
    GfxSize size = ui_window_size(window);
    ui_window_place(window, monitor.rcWork.left, monitor.rcWork.top, size.width, size.height);
    RECT outer;
    GetWindowRect(window->handle, &outer);
    int x = monitor.rcWork.left + ((monitor.rcWork.right - monitor.rcWork.left) - (outer.right - outer.left)) / 2;
    int y = monitor.rcWork.top + ((monitor.rcWork.bottom - monitor.rcWork.top) - (outer.bottom - outer.top)) / 2;
    SetWindowPos(window->handle, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
