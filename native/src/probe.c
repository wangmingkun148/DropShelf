#include "probe.h"
#include "drop_data.h"
#include "log.h"

#include <math.h>
#include <shellscalingapi.h>
#include <stdlib.h>

#define PROBE_CLASS L"DropShelfProbe"
#define PROBE_SIZE 320.0                // 宽和高（DIP）
#define PROBE_TIMEOUT_MILLISECONDS 900
#define PROBE_TIMER 1
#define MESSAGE_DRAG_ENTERED (WM_APP + 1)   // wparam：是否可以接收

static HWND window;
static void (*on_detected)(void);

// ───────────── 拖放目标 ─────────────

static const wchar_t *const kind_names[] = { L"None", L"Files", L"VirtualFiles", L"Link", L"Image", L"Text" };

static HRESULT STDMETHODCALLTYPE target_query_interface(IDropTarget *self, REFIID id, void **object)
{
    if (IsEqualIID(id, &IID_IUnknown) || IsEqualIID(id, &IID_IDropTarget))
    {
        *object = self;
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

// 只有一个静态的对象，不用计数。
static ULONG STDMETHODCALLTYPE target_add_ref(IDropTarget *self)
{
    (void)self;
    return 2;
}

static ULONG STDMETHODCALLTYPE target_release(IDropTarget *self)
{
    (void)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE target_drag_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    (void)self, (void)keys, (void)point;
    DropContentKind kind = drop_classify(data);
    wchar_t *shelf = drop_source_shelf_id(data);
    bool from_shelf = shelf != NULL;
    free(shelf);
    if (log_trace_enabled())
    {
        wchar_t *formats = drop_format_names(data, 20);
        log_trace(L"探针收到拖放：%ls，来自置物架 %ls，格式 %ls", kind_names[kind], from_shelf ? L"True" : L"False", formats);
        free(formats);
    }
    bool accepted = kind != DROP_NONE && !from_shelf;
    *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    // 不在系统拖放回调中直接切换窗口，交给下一轮消息处理。
    PostMessageW(window, MESSAGE_DRAG_ENTERED, accepted, 0);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_drag_over(IDropTarget *self, DWORD keys, POINTL point, DWORD *effect)
{
    (void)self, (void)keys, (void)point;
    *effect = DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_drag_leave(IDropTarget *self)
{
    (void)self;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    (void)self, (void)data, (void)keys, (void)point;
    *effect = DROPEFFECT_NONE;
    return S_OK;
}

static IDropTargetVtbl target_functions = {
    target_query_interface, target_add_ref, target_release,
    target_drag_enter, target_drag_over, target_drag_leave, target_drop
};
static IDropTarget target = { &target_functions };

// ───────────── 窗口 ─────────────

static LRESULT CALLBACK probe_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_TIMER:
            if (wparam == PROBE_TIMER)
            {
                log_trace(L"探针窗口超时：没有收到拖放");
                probe_hide();
                return 0;
            }
            break;
        case MESSAGE_DRAG_ENTERED:
            probe_hide();
            if (wparam && on_detected) on_detected();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

bool probe_create(HINSTANCE instance, void (*detected)(void))
{
    on_detected = detected;
    WNDCLASSEXW class = { .cbSize = sizeof(class) };
    class.lpfnWndProc = probe_procedure;
    class.hInstance = instance;
    class.lpszClassName = PROBE_CLASS;
    class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    class.hbrBackground = CreateSolidBrush(RGB(128, 128, 128));
    if (!RegisterClassExW(&class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, PROBE_CLASS, L"DropShelf Probe",
        WS_POPUP, -32000, -32000, (int)PROBE_SIZE, (int)PROBE_SIZE, NULL, NULL, instance, NULL);
    if (!window)
    {
        log_win32_error(L"探针窗口创建失败", GetLastError());
        return false;
    }
    // 不透明度为 1/255：肉眼不可见，但仍能接收拖放（完全透明的窗口会被系统穿透）。
    SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA);
    HRESULT result = RegisterDragDrop(window, &target);
    if (FAILED(result))
    {
        log_info(L"探针窗口无法接收拖放，错误码 0x%08lX", (unsigned long)result);
        DestroyWindow(window);
        window = NULL;
        return false;
    }
    return true;
}

POINT probe_place(POINT center, RECT work, int width, int height)
{
    double maximum_left = fmax(work.left, (double)work.right - width);
    double maximum_top = fmax(work.top, (double)work.bottom - height);
    double left = fmin(fmax(center.x - width / 2.0, work.left), maximum_left);
    double top = fmin(fmax(center.y - height / 2.0, work.top), maximum_top);
    return (POINT){ (LONG)rint(left), (LONG)rint(top) };
}

void probe_show_at(POINT center)
{
    if (!window) return;
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    UINT dpi_x = 96, dpi_y = 96;
    double scale = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x > 0 ? dpi_x / 96.0 : 1.0;
    int size = (int)rint(PROBE_SIZE * scale);
    POINT position = probe_place(center, info.rcWork, size, size);
    SetWindowPos(window, HWND_TOPMOST, position.x, position.y, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    KillTimer(window, PROBE_TIMER);
    SetTimer(window, PROBE_TIMER, PROBE_TIMEOUT_MILLISECONDS, NULL);
}

void probe_hide(void)
{
    if (!window) return;
    KillTimer(window, PROBE_TIMER);
    if (IsWindowVisible(window)) ShowWindow(window, SW_HIDE);
}

void probe_destroy(void)
{
    if (!window) return;
    KillTimer(window, PROBE_TIMER);
    RevokeDragDrop(window);
    DestroyWindow(window);
    window = NULL;
}
