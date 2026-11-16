// 合并和拆分置物架，和 C# 版 ShelfWindow.DragOut.cs 里“合并和拆分”的部分、ShelfDragSession 以及 SplitPadWindow 对应：
// 把项目拖到另一个置物架上就移过去；从列表里只拖出一部分时下边伸出“新置物架”，放上去就拆成一个新的置物架；
// 拖着标题栏把整个置物架拖到另一个上面，停一下再松手就合并过去。
#include "shelf_window_internal.h"
#include "drop_data.h"
#include "log.h"
#include "theme.h"

#include <math.h>
#include <shellscalingapi.h>
#include <stdlib.h>
#include <string.h>

#define DIM_DURATION 0.150
#define OVERLAY_FADE_IN 0.120
#define MERGE_DWELL 300                 // 在另一个置物架上停这么久才可以合并（毫秒）
#define MERGE_DIM 0.62                  // 松手就合并时这边内容的透明度
#define PAD_HEIGHT 48.0f
#define PAD_GAP 6.0f
#define PAD_SHOW_DURATION 0.160
#define PAD_HIDE_DURATION 0.110
#define PAD_HOT_DURATION 0.120
#define GLYPH_ADD L"\xE710"

static bool close_when_empty(void)
{
    return app_settings()->close_when_empty;
}

static void request_frame(ShelfWindow *shelf)
{
    if (shelf->ui) ui_window_request_frame(shelf->ui);
}

static void ease_to(Anim *anim, double to, double duration, Ease ease)
{
    if (!anim_enabled() || duration <= 0) anim_set(anim, to);
    else if (anim->value != to || anim->running) anim_start(anim, to, duration, ease);
}

static bool contains_screen_point(const ShelfWindow *shelf, POINT point)
{
    RECT rect;
    return shelf->handle && GetWindowRect(shelf->handle, &rect) && PtInRect(&rect, point);
}

/// 屏幕上的点换到窗口里（DIP），并限制在窗口里面：飞进来的牌从窗口边上出现，不会在看不见的地方起飞。
static void client_point_inside(ShelfWindow *shelf, POINT screen, float *x, float *y)
{
    POINT client = screen;
    ScreenToClient(shelf->handle, &client);
    float scale = shelf_scale(shelf);
    GfxSize size = ui_window_size(shelf->ui);
    float high_x = size.width - 16 > 16 ? size.width - 16 : 16, high_y = size.height - 16 > 16 ? size.height - 16 : 16;
    float px = client.x / scale, py = client.y / scale;
    *x = px < 16 ? 16 : px > high_x ? high_x : px;
    *y = py < 16 ? 16 : py > high_y ? high_y : py;
}

// ───────────── 拖出的那一次 ─────────────

static ShelfDragSession *current_session;

void shelf_drag_session_begin(ShelfDragSession *session)
{
    current_session = session;
}

void shelf_drag_session_end(void)
{
    current_session = NULL;
}

ShelfDragSession *shelf_drag_session_from(IDataObject *data)
{
    if (!current_session || !data) return NULL;
    wchar_t *source = drop_source_shelf_id(data);
    bool same = source && wcs_equal(source, current_session->source->model->id);
    free(source);
    return same ? current_session : NULL;
}

void shelf_drag_session_drop_on(ShelfDragSession *session, ShelfWindow *target, float x, float y)
{
    if (session->target) shelf_window_release(session->target);
    session->target = shelf_window_retain(target);
    session->drop_x = x;
    session->drop_y = y;
}

// ───────────── 交接 ─────────────

int shelf_receive(ShelfWindow *shelf, ShelfItem **items, int count, ShelfModel *from, float x, float y)
{
    if (shelf->closing || shelf->closed) return 0;
    shelf_finish_motion(shelf);
    int added = shelf_model_adopt(shelf->model, items, count, from);
    if (added > 0)
    {
        shelf->landing = true;
        shelf->landing_x = x;
        shelf->landing_y = y;
        shelf->landing_added = added;
    }
    else shelf_notify(shelf, S(STR_AlreadyOnShelf), false);
    return added;
}

void shelf_let_go(ShelfWindow *shelf, ShelfItem **items, int count)
{
    bool all = true;
    for (int index = 0; index < shelf->model->count && all; index++)
    {
        bool listed = false;
        for (int at = 0; at < count && !listed; at++) listed = items[at] == shelf->model->items[index];
        all = listed;
    }
    // 全部交出去且设置为自动关闭时，带着原来的样子淡出关闭（不先变成空置物架，也不记进最近关闭）。
    if (close_when_empty() && all)
    {
        shelf->merged_away = true;
        shelf_window_close_animated(shelf);
    }
    else shelf_remove_items(shelf, items, count);
}

// ───────────── 拆分 ─────────────

static RECT work_area_of(POINT point, float *scale)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    UINT dpi_x = 96, dpi_y = 96;
    *scale = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x > 0 ? dpi_x / 96.0f : 1.0f;
    return info.rcWork;
}

bool shelf_split_place(ShelfWindow *source, SplitPlace *place)
{
    RECT rect;
    if (!source->handle || !GetWindowRect(source->handle, &rect)) return false;
    float collapsed_width, collapsed_height;
    shelf_target_size(source, false, &collapsed_width, &collapsed_height);
    POINT middle = { rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
    float scale;
    RECT work = work_area_of(middle, &scale);
    int width = (int)lroundf(collapsed_width * scale), height = (int)lroundf(PAD_HEIGHT * scale), gap = (int)lroundf(PAD_GAP * scale);
    int shelf_height = (int)lroundf(collapsed_height * scale);
    // 按新置物架的高度看下面够不够：拆出来的置物架要完整地落在屏幕里，不能盖住来源。
    bool below = rect.bottom + gap + shelf_height <= work.bottom || rect.top - gap - shelf_height < work.top;
    int max_left = work.right - width > work.left ? work.right - width : work.left;
    int left = rect.left < work.left ? work.left : rect.left > max_left ? max_left : rect.left;
    int top = below ? rect.bottom + gap : rect.top - gap - height;
    place->rect = (RECT){ left, top, left + width, top + height };
    place->below = below;
    return true;
}

void shelf_split_off(ShelfWindow *shelf, ShelfItem **items, int count, const SplitPlace *place, const POINT *from)
{
    if (count == 0 || shelf->closing || shelf->closed) return;
    float collapsed_width, collapsed_height;
    shelf_target_size(shelf, false, &collapsed_width, &collapsed_height);
    int height = (int)lroundf(collapsed_height * shelf_scale(shelf));
    RECT rect = place->rect;
    // 新置物架在“新置物架”小窗口所在的位置展开：在下边时顶边对齐、在上边时底边对齐，不盖住这个置物架。
    POINT center = { rect.left + (rect.right - rect.left) / 2, place->below ? rect.top + height / 2 : rect.bottom - height / 2 };
    ShelfWindow *target = shelves_open_at(center, false, false, EDGE_NONE);
    if (!target || target == shelf) return;
    // 项目从光标处（没有时从新置物架靠近这边的一侧）飞进去，这边的行随后滑走、补位。
    POINT origin = from ? *from : (POINT){ center.x, place->below ? rect.top : rect.bottom };
    float x, y;
    client_point_inside(target, origin, &x, &y);
    shelf_receive(target, items, count, shelf->model, x, y);
    shelf_let_go(shelf, items, count);
}

void shelf_split_off_from_menu(ShelfWindow *shelf, ShelfItem **items, int count)
{
    SplitPlace place;
    if (shelf_split_place(shelf, &place)) shelf_split_off(shelf, items, count, &place, NULL);
}

// ───────────── “新置物架”小窗口 ─────────────
// 全程只有一个，轮流给各个置物架用；拖动结束就收起。

typedef struct SplitPad
{
    UiWindow *ui;
    HWND handle;
    struct PadDropTarget
    {
        IDropTarget base;
    } drop_target;
    DropTargetHelper *drop_helper;
    bool registered;
    bool shown, below, hot;
    Anim opacity, offset, fill;
} SplitPad;

static SplitPad pad;

static void pad_paint(UiWindow *window, void *context, double time)
{
    (void)context;
    bool running = anim_update(&pad.opacity, time);
    running |= anim_update(&pad.offset, time);
    running |= anim_update(&pad.fill, time);
    if (running) ui_window_request_frame(window);
    GfxSize size = ui_window_size(window);
    float opacity = (float)pad.opacity.value;
    if (opacity <= 0.001f) return;
    gfx_push_transform(gfx_matrix_translate(0, (float)pad.offset.value));
    gfx_push_opacity(opacity);
    GfxRect frame = { 6, 6, size.width - 6, size.height - 6 };
    // 虚线框带着淡淡的底色，拖到上面时再铺一层强调色。
    gfx_fill_rounded(frame, 8, theme_color(THEME_ACCENT_FAINT));
    gfx_stroke_rounded_dashed(frame, 8, theme_color(THEME_ACCENT), 1.4f, 4, 3);
    float fill = (float)pad.fill.value;
    if (fill > 0.001f)
    {
        Color subtle = theme_color(THEME_ACCENT_SUBTLE);
        subtle.a = (unsigned char)(subtle.a * fill);
        gfx_fill_rounded(frame, 8, subtle);
    }
    GfxTextStyle glyph = { GFX_FONT_ICON, 13, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    GfxTextStyle label = { GFX_FONT_TEXT, 12, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    const wchar_t *text = S(STR_SplitPadLabel);
    float glyph_width = gfx_text_measure(GLYPH_ADD, &glyph, 0).width, text_width = gfx_text_measure(text, &label, 0).width;
    float left = (size.width - glyph_width - 7 - text_width) / 2;
    Color accent = theme_color(THEME_ACCENT_TEXT);
    gfx_text_draw(GLYPH_ADD, (GfxRect){ left, frame.top + 1, left + glyph_width + 1, frame.bottom }, &glyph, accent);
    left += glyph_width + 7;
    gfx_text_draw(text, (GfxRect){ left, frame.top, left + text_width + 1, frame.bottom }, &label, accent);
    gfx_pop_layer();
    gfx_pop_transform();
}

static void pad_animate(UiWindow *window, void *context, double time)
{
    (void)window, (void)context, (void)time;
    // 淡出完了才真正藏起来；这期间又显示了就不藏。
    if (!pad.shown && !pad.opacity.running && pad.handle && IsWindowVisible(pad.handle)) ShowWindow(pad.handle, SW_HIDE);
}

static void set_hot(bool hot, bool animate)
{
    if (hot == pad.hot && animate) return;
    pad.hot = hot;
    if (animate && anim_enabled()) anim_start(&pad.fill, hot ? 1 : 0, PAD_HOT_DURATION, EASE_OF(EASE_LINEAR));
    else anim_set(&pad.fill, hot ? 1 : 0);
    if (pad.ui) ui_window_request_frame(pad.ui);
}

static HRESULT STDMETHODCALLTYPE pad_query_interface(IDropTarget *self, REFIID id, void **object)
{
    if (IsEqualIID(id, &IID_IUnknown) || IsEqualIID(id, &IID_IDropTarget))
    {
        *object = self;
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE pad_add_ref(IDropTarget *self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE pad_release(IDropTarget *self) { (void)self; return 1; }

static IDataObject *pad_data;

static void pad_update(IDataObject *data, DWORD *effect)
{
    bool valid = pad.shown && shelf_drag_session_from(data) != NULL;
    *effect = valid ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (valid) drop_set_description(data, DROP_IMAGE_LABEL, S(STR_SplitPadLabel));
    drop_helper_over(pad.drop_helper, *effect);
    set_hot(valid, true);
}

static HRESULT STDMETHODCALLTYPE pad_drag_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    (void)self, (void)keys, (void)point;
    if (pad_data) IDataObject_Release(pad_data);
    pad_data = data;
    IDataObject_AddRef(data);
    drop_helper_enter(pad.drop_helper, pad.handle, data, *effect & DROPEFFECT_COPY);
    pad_update(data, effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pad_drag_over(IDropTarget *self, DWORD keys, POINTL point, DWORD *effect)
{
    (void)self, (void)keys, (void)point;
    pad_update(pad_data, effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pad_drag_leave(IDropTarget *self)
{
    (void)self;
    drop_helper_leave(pad.drop_helper);
    set_hot(false, true);
    if (pad_data) IDataObject_Release(pad_data);
    pad_data = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pad_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    (void)self, (void)keys, (void)point;
    ShelfDragSession *session = shelf_drag_session_from(data);
    *effect = session && pad.shown ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    drop_helper_drop(pad.drop_helper, data, *effect);
    if (session && pad.shown) session->split = true;
    set_hot(false, false);
    if (pad_data) IDataObject_Release(pad_data);
    pad_data = NULL;
    return S_OK;
}

static IDropTargetVtbl pad_functions = { pad_query_interface, pad_add_ref, pad_release, pad_drag_enter, pad_drag_over, pad_drag_leave, pad_drop };

static void pad_closed(UiWindow *window, void *context)
{
    (void)window, (void)context;
    pad.ui = NULL;
    pad.handle = NULL;
    pad.shown = false;
}

static bool ensure_pad(void)
{
    if (pad.ui) return true;
    UiWindowOptions options = {0};
    options.title = L"DropShelf";
    options.width = 220;
    options.height = PAD_HEIGHT;
    options.frameless = true;
    options.no_activate = true;
    options.backdrop = UI_BACKDROP_NONE;
    options.background = THEME_SHELF_SOLID;
    options.paint = pad_paint;
    options.animate = pad_animate;
    options.closed = pad_closed;
    pad.ui = ui_window_create(&options);
    if (!pad.ui)
    {
        log_error(L"显示拆分用的窗口失败");
        return false;
    }
    pad.handle = ui_window_handle(pad.ui);
    ui_window_set_topmost(pad.ui, true);
    if (!pad.drop_helper) pad.drop_helper = drop_helper_create();
    pad.drop_target.base.lpVtbl = &pad_functions;
    pad.registered = SUCCEEDED(RegisterDragDrop(pad.handle, &pad.drop_target.base));
    return true;
}

void split_pad_show_for(ShelfWindow *source)
{
    SplitPlace place;
    if (!shelf_split_place(source, &place) || !ensure_pad()) return;
    pad.shown = true;
    pad.below = place.below;
    set_hot(false, false);
    int width = place.rect.right - place.rect.left, height = place.rect.bottom - place.rect.top;
    SetWindowPos(pad.handle, HWND_TOPMOST, place.rect.left, place.rect.top, width, height, SWP_NOACTIVATE);
    ui_window_show_with(pad.ui, false);
    // 跨显示器时系统会按新的缩放调整大小，显示后再摆一次。
    SetWindowPos(pad.handle, NULL, place.rect.left, place.rect.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    // 从来源置物架那一侧滑出来。
    if (anim_enabled())
    {
        anim_set(&pad.opacity, 0);
        anim_start(&pad.opacity, 1, PAD_SHOW_DURATION, EASE_OF(EASE_LINEAR));
        anim_set(&pad.offset, place.below ? -8 : 8);
        anim_start(&pad.offset, 0, PAD_SHOW_DURATION, EASE_OF(EASE_CUBIC_OUT));
    }
    else
    {
        anim_set(&pad.opacity, 1);
        anim_set(&pad.offset, 0);
    }
    ui_window_invalidate(pad.ui);
    ui_window_request_frame(pad.ui);
}

bool split_pad_dismiss(SplitPlace *place)
{
    if (!pad.ui || !pad.shown) return false;
    RECT rect;
    bool placed = GetWindowRect(pad.handle, &rect);
    if (placed && place)
    {
        place->rect = rect;
        place->below = pad.below;
    }
    pad.shown = false;
    drop_helper_leave(pad.drop_helper);
    if (anim_enabled()) anim_start(&pad.opacity, 0, PAD_HIDE_DURATION, EASE_OF(EASE_LINEAR));
    else anim_set(&pad.opacity, 0);
    ui_window_request_frame(pad.ui);
    return placed;
}

bool split_pad_is_shown(void)
{
    return pad.ui && pad.shown;
}

void split_pad_close_shared(void)
{
    if (pad_data) IDataObject_Release(pad_data);
    pad_data = NULL;
    if (pad.registered && pad.handle) RevokeDragDrop(pad.handle);
    pad.registered = false;
    if (pad.ui) ui_window_close(pad.ui);
    pad.ui = NULL;
    pad.handle = NULL;
    drop_helper_destroy(pad.drop_helper);
    pad.drop_helper = NULL;
}

// ───────────── 拖着标题栏合并 ─────────────

static bool can_receive_merge(const ShelfWindow *shelf)
{
    return shelf_window_is_visible(shelf) && !shelf->closing && !shelf->closed && !shelf->model->busy && !shelf->drag_inside;
}

/// 放置区域收起后回到平常拖入时的样子：没有蓄力的底色，写着字。
void shelf_reset_merge_charge(ShelfWindow *shelf)
{
    anim_set(&shelf->merge_charge_opacity, 0);
    anim_set(&shelf->merge_charge_scale, 1);
    anim_set(&shelf->drop_label_opacity, 1);
    anim_set(&shelf->drop_zone_scale, 1);
    shelf->pulse_phase = 0;
}

/// 蓄力：只亮出放置区域的边框，里面的底色在 MERGE_DWELL 内从中间涨满，还不写“松开以合并”，这时松手只是挪了窗口。
/// 内容照常显示：只是路过时不会把内容整个换掉又换回来。
static void begin_merge_charge(ShelfWindow *shelf)
{
    shelf_finish_motion(shelf);
    shelf_reset_tiles(shelf);
    shelf->overlay_hit = false;
    if (!shelf->overlay_visible) anim_set(&shelf->overlay_opacity, 0);
    shelf->overlay_visible = true;
    ease_to(&shelf->overlay_opacity, 1, OVERLAY_FADE_IN, EASE_OF(EASE_CUBIC_OUT));
    anim_set(&shelf->drop_label_opacity, 0);
    ease_to(&shelf->merge_charge_opacity, 1, OVERLAY_FADE_IN, EASE_OF(EASE_CUBIC_OUT));
    anim_set(&shelf->merge_charge_scale, 0.4);
    ease_to(&shelf->merge_charge_scale, 1, MERGE_DWELL / 1000.0, EASE_OF(EASE_SINE_IN_OUT));
    anim_set(&shelf->drop_zone_scale, 1);
    shelf->pulse_phase = 0;
    request_frame(shelf);
    shelf_repaint(shelf);
}

/// 停够了：内容淡出，写上“松开以合并”，边框变成强调色，涨满的放置区域轻轻弹一下，表示可以松手了。
static void arm_merge(ShelfWindow *shelf)
{
    shelf_show_drop_overlay(shelf);
    shelf->drop_zone_text = STR_ReleaseToMerge;
    shelf->overlay_hit = false;
    ease_to(&shelf->drop_label_opacity, 1, OVERLAY_FADE_IN, EASE_OF(EASE_CUBIC_OUT));
    anim_set(&shelf->merge_charge_opacity, 1);
    anim_set(&shelf->merge_charge_scale, 1);
    if (anim_enabled())
    {
        // 90 毫秒放大到 1.035，再用 170 毫秒回到原样。
        anim_set(&shelf->drop_zone_scale, 1);
        anim_start(&shelf->drop_zone_scale, 1.035, 0.090, EASE_OF(EASE_SINE_OUT));
        shelf->pulse_phase = 1;
    }
    request_frame(shelf);
    shelf_repaint(shelf);
}

void shelf_update_merge_pulse(ShelfWindow *shelf)
{
    if (shelf->pulse_phase != 1 || shelf->drop_zone_scale.running) return;
    shelf->pulse_phase = 2;
    anim_start(&shelf->drop_zone_scale, 1, 0.170, EASE_OF(EASE_SINE_IN_OUT));
}

static void show_merge_hint(ShelfWindow *shelf, MergeHint hint)
{
    if (hint == shelf->merge_hint || (shelf->closing && hint != MERGE_HINT_NONE)) return;
    shelf->merge_hint = hint;
    switch (hint)
    {
        case MERGE_HINT_CHARGING: begin_merge_charge(shelf); break;
        case MERGE_HINT_ARMED: arm_merge(shelf); break;
        default:
            if (!shelf->drag_inside) shelf_end_drag(shelf);
            break;
    }
}

static void set_reference(ShelfWindow **slot, ShelfWindow *shelf)
{
    if (*slot == shelf) return;
    ShelfWindow *previous = *slot;
    *slot = shelf_window_retain(shelf);
    shelf_window_release(previous);
}

/// 换合并的对象：对方显示放置区域和“松开以合并”，边框变成强调色；这边内容变淡、边框也变色并提示，表示松手就并过去。
static void set_merge_target(ShelfWindow *shelf, ShelfWindow *target)
{
    if (target == shelf->merge_target) return;
    if (shelf->merge_target) show_merge_hint(shelf->merge_target, MERGE_HINT_NONE);
    set_reference(&shelf->merge_target, target);
    if (target) show_merge_hint(target, MERGE_HINT_ARMED);
    ease_to(&shelf->merge_dim, target ? MERGE_DIM : 1, DIM_DURATION, EASE_OF(EASE_CUBIC_OUT));
    request_frame(shelf);
    if (target)
    {
        Color accent = theme_color(THEME_ACCENT);
        ui_window_set_border_color(shelf->ui, &accent);
        shelf_show_toast(shelf, S(STR_ReleaseToMerge), true, TOAST_ACTION_NONE);
    }
    else
    {
        ui_window_set_border_color(shelf->ui, NULL);
        shelf_hide_toast(shelf);
    }
}

/// 换蓄力的对象：原来的那个收起提示，新的开始蓄力并计时。
static void set_merge_candidate(ShelfWindow *shelf, ShelfWindow *candidate)
{
    if (shelf->handle) KillTimer(shelf->handle, SHELF_TIMER_MERGE);
    if (shelf->merge_candidate && shelf->merge_candidate != shelf->merge_target) show_merge_hint(shelf->merge_candidate, MERGE_HINT_NONE);
    set_reference(&shelf->merge_candidate, candidate);
    if (!candidate) return;
    show_merge_hint(candidate, MERGE_HINT_CHARGING);
    SetTimer(shelf->handle, SHELF_TIMER_MERGE, MERGE_DWELL, NULL);
}

/// 光标在哪个可以接收合并的置物架上（不算自己）。
static ShelfWindow *shelf_under_cursor(ShelfWindow *shelf)
{
    POINT cursor;
    GetCursorPos(&cursor);
    for (ShelfWindow *other = shelves_first(); other; other = other->next)
        if (other != shelf && can_receive_merge(other) && contains_screen_point(other, cursor)) return other;
    return NULL;
}

void shelf_merge_on_moved(ShelfWindow *shelf)
{
    if (!shelf->merge_watch) return;
    ShelfWindow *under = shelf_under_cursor(shelf);
    if (under && under == shelf->merge_target) return;
    set_merge_target(shelf, NULL);
    if (under != shelf->merge_candidate) set_merge_candidate(shelf, under);
}

void shelf_merge_on_dwell(ShelfWindow *shelf)
{
    if (shelf->handle) KillTimer(shelf->handle, SHELF_TIMER_MERGE);
    ShelfWindow *candidate = shelf->merge_candidate;
    if (!candidate) return;
    shelf_window_retain(candidate);
    set_reference(&shelf->merge_candidate, NULL);
    POINT cursor;
    GetCursorPos(&cursor);
    if (shelf->merge_watch && can_receive_merge(candidate) && contains_screen_point(candidate, cursor)) set_merge_target(shelf, candidate);
    else show_merge_hint(candidate, MERGE_HINT_NONE);
    shelf_window_release(candidate);
}

/// 合并：全部项目交给 target，从这个置物架所在的位置飞进它的堆叠或列表；这边带着原来的样子淡出关闭，不留下一个空置物架，也不记进最近关闭。
static void merge_into(ShelfWindow *shelf, ShelfWindow *target)
{
    int count = shelf->model->count;
    if (count == 0 || shelf->closing || target->closing || target->closed) return;
    shelf_finish_motion(shelf);
    ShelfItem **items = xalloc((size_t)count * sizeof(ShelfItem *));
    for (int index = 0; index < count; index++) items[index] = shelf_item_retain(shelf->model->items[index]);
    RECT rect;
    GetWindowRect(shelf->handle, &rect);
    POINT center = { rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
    float x, y;
    client_point_inside(target, center, &x, &y);
    int added = shelf_receive(target, items, count, shelf->model, x, y);
    if (added > 0 && added < count)
    {
        wchar_t *message = str_merged_shelves(added);
        shelf_notify(target, message, true);
        free(message);
    }
    for (int index = 0; index < count; index++) shelf_item_release(items[index]);
    free(items);
    shelf->merged_away = true;
    shelf_window_close_animated(shelf);
}

void shelf_move_by_hand(ShelfWindow *shelf)
{
    // 有东西的置物架拖到另一个置物架上松手就合并：移动途中留意光标下面是不是别的置物架。
    ShelfModel *model = shelf->model;
    shelf->merge_watch = model->count > 0 && !model->busy && !shelf->dragging_out && !shelf->closing;
    shelf_window_retain(shelf);
    ui_window_begin_move(shelf->ui);
    shelf->merge_watch = false;
    if (shelf->closed)
    {
        set_merge_candidate(shelf, NULL);
        set_reference(&shelf->merge_target, NULL);
        shelf_window_release(shelf);
        return;
    }
    // 松手时还在那个置物架上、并且已经停够了就合并：对方收起放置区域，这边不恢复原样，直接带着变淡的样子并过去。
    // 还在蓄力时松手只是挪了窗口，对方收起提示。
    ShelfWindow *target = shelf->merge_target;
    POINT cursor;
    GetCursorPos(&cursor);
    if (target && !target->closing && !target->closed && !target->model->busy && contains_screen_point(target, cursor))
    {
        shelf_window_retain(target);
        set_reference(&shelf->merge_target, NULL);
        show_merge_hint(target, MERGE_HINT_NONE);
        shelf_hide_toast(shelf);
        merge_into(shelf, target);
        shelf_window_release(target);
    }
    else set_merge_target(shelf, NULL);
    set_merge_candidate(shelf, NULL);
    shelf_window_release(shelf);
}

void shelf_merge_forget(ShelfWindow *shelf)
{
    if (shelf->handle) KillTimer(shelf->handle, SHELF_TIMER_MERGE);
    set_reference(&shelf->merge_candidate, NULL);
    set_reference(&shelf->merge_target, NULL);
}
