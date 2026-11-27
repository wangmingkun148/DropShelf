// 置物架的拖入、拖出与 X11 拖放适配（从 shelf_input.c 拆出），按风格各编译一遍（见 look_variant.h）。
#include "look_variant.h"
#include "shelf_window_internal.h"
#include "attachment_cache.h"
#ifdef _WIN32
#include "clipboard.h"
#else
#include "ui_linux.h"
#include "xdnd_linux.h"
#include <gdk/gdk.h>
#endif
#include "folder_watch.h"
#include "log.h"
#include "task.h"

#include <math.h>
#if defined(_WIN32) && defined(LOOK_VARIANT_CARDS)
#include <shellapi.h>
#endif
#include <stdlib.h>
#include <string.h>

#define OVERLAY_FADE_IN 0.120
#define OVERLAY_FADE_OUT 0.100
#define DRAG_OVER_INTERVAL 0.008        // 两次处理拖动悬停之间至少隔这么久（秒）
#define TILES_EXPAND_DURATION 0.180
#define TILES_COLLAPSE_DURATION 0.240
#define HOVER_DURATION 0.140

// ───────────── 拖入 ─────────────

#ifdef _WIN32
static DWORD preferred_effect(DWORD allowed)
{
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    return DROPEFFECT_COPY;
}

static void set_description(ShelfWindow *shelf, DropImageType type, const wchar_t *message)
{
    // 拖动途中系统不停地来问，说明没变时不用重新设置。
    if (shelf->has_last_description && shelf->last_description_type == type && wcs_equal(shelf->last_description, message)) return;
    shelf->has_last_description = true;
    shelf->last_description_type = type;
    shelf->last_description = message;
    if (shelf->drag_data) drop_set_description(shelf->drag_data, type, message);
}
#endif

static void ease_to(Anim *anim, double to, double duration, bool animate)
{
    if (!animate || duration <= 0) anim_set(anim, to);
    else if (anim->value != to || anim->running) anim_start(anim, to, duration, EASE_OF(EASE_CUBIC_OUT));
}

/// 展开时快速操作从按钮栏向上长高并淡入，按钮栏同时淡出。
static void apply_tiles(ShelfWindow *shelf, bool expanded, double duration)
{
    bool has_actions = shelf->actions.count > 0;
    double collapsed = has_actions ? SHELF_ACTION_BAR_HEIGHT : 0;
    // 卡片 94，上下外边距各 3，下面再留 7。
    double full = SHELF_ACTION_TILE_HEIGHT + 6 + 7;
    bool animate = duration > 0 && anim_enabled();
    ease_to(&shelf->tiles_height, expanded && has_actions ? full : collapsed, duration, animate);
    ease_to(&shelf->tiles_opacity, expanded ? 1 : 0, duration, animate);
    ease_to(&shelf->bar_opacity, expanded ? 0 : 1, duration, animate);
    shelf_request_frame(shelf);
    shelf_repaint(shelf);
}

void shelf_reset_tiles(ShelfWindow *shelf)
{
    shelf->tiles_expanded = false;
    apply_tiles(shelf, false, 0);
}

#ifdef _WIN32
static void set_tiles_expanded(ShelfWindow *shelf, bool expanded)
{
    if (expanded == shelf->tiles_expanded) return;
    shelf->tiles_expanded = expanded;
    apply_tiles(shelf, expanded, expanded ? TILES_EXPAND_DURATION : TILES_COLLAPSE_DURATION);
}
#endif

void shelf_set_hovered_action(ShelfWindow *shelf, int action, bool force)
{
    if (!force && action == shelf->hovered_action) return;
    shelf->hovered_action = action;
    bool animate = !force && anim_enabled();
    for (int index = 0; index < SHELF_MAX_ACTIONS; index++) ease_to(&shelf->tile_highlight[index], index == action ? 1 : 0, HOVER_DURATION, animate);
    ease_to(&shelf->frame_opacity, action < 0 ? 1 : 0.45, HOVER_DURATION, animate);
    shelf_request_frame(shelf);
    shelf_repaint(shelf);
}

#ifdef _WIN32
/// 拖到哪张卡片上（四周多留 3）；卡片没展开时是 -1。
static int hit_test_tiles(ShelfWindow *shelf, float x, float y)
{
    if (!shelf->tiles_expanded) return -1;
    shelf_layout(shelf);
    for (int index = 0; index < shelf->actions.count; index++)
    {
        GfxRect tile = shelf->layout.tiles[index];
        if (x >= tile.left - 3 && y >= tile.top - 3 && x <= tile.right + 3 && y <= tile.bottom + 3) return index;
    }
    return -1;
}
#endif

#ifdef LOOK_VARIANT_CARDS
#ifdef _WIN32
/// 拖进来的文件数；读不到文件列表（文字、链接、虚拟文件）时为 0，不显示数字。
static int incoming_count(IDataObject *data)
{
    FORMATETC request = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {0};
    if (IDataObject_GetData(data, &request, &medium) != S_OK) return 0;
    int count = 0;
    HDROP drop = (HDROP)GlobalLock(medium.hGlobal);
    if (drop)
    {
        count = (int)DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return count;
}
#endif

void shelf_show_drop_overlay(ShelfWindow *shelf, IDataObject *data, int incoming)
#else
void shelf_show_drop_overlay(ShelfWindow *shelf)
#endif
{
    shelf_finish_motion(shelf);
    if (shelf->ui) ui_window_set_tooltip(shelf->ui, NULL);
    shelf->drop_zone_text = shelf->drag_from_shelf ? STR_ReleaseToMoveHere : shelf->model->count > 0 ? STR_ReleaseToAdd : STR_ReleaseToHold;
#ifdef LOOK_VARIANT_CARDS
#ifdef _WIN32
    shelf->incoming = data ? incoming_count(data) : incoming;
#else
    // Linux 的拖入在进入时还拿不到文件列表（GTK 到松手时才给数据），卡片上就不显示文件数；0 表示不显示。
    (void)data;
    shelf->incoming = incoming;
#endif
    // 原来那一摞最上面两张从叠着的样子向左右各转开一点，中间的虚线空位浮起来，等松手。
    shelf_fan_out(shelf);
#endif
    // 平常拖入时不带合并的蓄力底色；合并的提示自己接着设置。
    if (shelf->merge_hint == MERGE_HINT_NONE) shelf_reset_merge_charge(shelf);
    shelf_reset_tiles(shelf);
    shelf->overlay_hit = true;
    if (!shelf->overlay_visible) anim_set(&shelf->overlay_opacity, 0);
    shelf->overlay_visible = true;
    // 出场动画期间直接到位：内容整体正在淡入，里面再交叉淡入淡出看不出来。
    bool entering = shelf->popping;
    if (entering)
    {
        anim_set(&shelf->overlay_opacity, 1);
        anim_set(&shelf->body_opacity, 0);
    }
    else
    {
        shelf_fade(&shelf->overlay_opacity, 1, OVERLAY_FADE_IN);
        shelf_fade(&shelf->body_opacity, 0, OVERLAY_FADE_OUT);
    }
    Color accent = theme_color(THEME_ACCENT);
    ui_window_set_border_color(shelf->ui, &accent);
    shelf_request_frame(shelf);
}

#ifdef LOOK_VARIANT_CARDS
/// 拖动离开或松手：放置区域淡出、内容区淡入。instant 时直接换回：松手后堆叠要从摊开的样子接着落牌（见 shelf_hold_fan），
/// 两边的牌正好重合，交叉淡化反而会让牌变淡一下。
static void end_drag(ShelfWindow *shelf, bool instant)
#else
void shelf_end_drag(ShelfWindow *shelf)
#endif
{
    if (shelf->handle) KillTimer(shelf->handle, SHELF_TIMER_LEAVE);
    shelf->drag_inside = false;
    shelf->drag_from_shelf = false;
    if (shelf->drag_data) IDataObject_Release(shelf->drag_data);
    shelf->drag_data = NULL;
    shelf->overlay_hit = false;
    shelf_set_hovered_action(shelf, -1, false);
#ifdef LOOK_VARIANT_CARDS
    if (instant)
    {
        anim_set(&shelf->body_opacity, 1);
        anim_set(&shelf->overlay_opacity, 0);
    }
    else
    {
        shelf_fade(&shelf->body_opacity, 1, OVERLAY_FADE_IN);
        shelf_fade(&shelf->overlay_opacity, 0, OVERLAY_FADE_OUT);
    }
#else
    shelf_fade(&shelf->body_opacity, 1, OVERLAY_FADE_IN);
    shelf_fade(&shelf->overlay_opacity, 0, OVERLAY_FADE_OUT);
#endif
    shelf_reset_tiles(shelf);
    if (shelf->ui) ui_window_set_border_color(shelf->ui, NULL);
    shelf_request_frame(shelf);
}

#ifdef LOOK_VARIANT_CARDS
// 纸牌风格里 end_drag 带一个 instant 参数，对外还是用同一个没有参数的名字。
void shelf_end_drag(ShelfWindow *shelf)
{
    end_drag(shelf, false);
}
#endif

#ifdef _WIN32
static ShelfWindow *target_shelf(IDropTarget *self)
{
    return ((struct ShelfDropTarget *)self)->shelf;
}

/// 屏幕上的点换成窗口里的位置（DIP）。
static void client_point(ShelfWindow *shelf, POINTL point, float *x, float *y)
{
    POINT client = { point.x, point.y };
    ScreenToClient(shelf->handle, &client);
    float scale = shelf_scale(shelf);
    *x = client.x / scale;
    *y = client.y / scale;
}

static void apply_drag(ShelfWindow *shelf, POINTL point, DWORD *effect)
{
    ShelfModel *model = shelf->model;
    if (shelf->drag_from_self)
    {
        *effect = DROPEFFECT_NONE;
    }
    else if (shelf->drag_kind == DROP_NONE || model->busy)
    {
        *effect = DROPEFFECT_NONE;
        set_description(shelf, DROP_IMAGE_WARNING, S(model->busy ? STR_PleaseWait : STR_UnsupportedContent));
    }
    else
    {
        float x, y;
        client_point(shelf, point, &x, &y);
        // 拖到底部按钮区域时展开快速操作；展开后区域变高，移回上方的放置区域再收起。
        // 出场动画期间先不展开：晃动呼出时光标还在来回甩，播完后下一次移动再判断。
        if (shelf->actions.count > 0 && !shelf->popping)
        {
            shelf_layout(shelf);
            set_tiles_expanded(shelf, y >= shelf->layout.tiles_host.top);
        }
        int action = hit_test_tiles(shelf, x, y);
        shelf_set_hovered_action(shelf, action, false);
        if (action >= 0 && !drop_can_provide_files(shelf->drag_kind))
        {
            *effect = DROPEFFECT_NONE;
            set_description(shelf, DROP_IMAGE_WARNING, S(STR_FilesOnlyAction));
        }
        else
        {
            *effect = preferred_effect(*effect);
            // 在快速操作上时光标旁写操作名称；在放置区域上不写，放置区域里已经写着“松开以添加”等。
            if (action >= 0) set_description(shelf, DROP_IMAGE_LABEL, shelf->action_labels[action]);
            else set_description(shelf, DROP_IMAGE_INVALID, L"");
        }
    }
    drop_helper_over(shelf->drop_helper, *effect);
}

static void update_drag(ShelfWindow *shelf, POINTL point, DWORD *effect)
{
    apply_drag(shelf, point, effect);
    shelf->drag_effects = *effect;
    QueryPerformanceCounter(&shelf->drag_over_handled);
}

static HRESULT STDMETHODCALLTYPE target_query_interface(IDropTarget *self, REFIID id, void **object)
{
    if (IsEqualIID(id, &IID_IUnknown) || IsEqualIID(id, &IID_IDropTarget))
    {
        // 交出去的指针也要算一份引用：系统用完会 Release，不加的话置物架会被提前释放。
        *object = self;
        IDropTarget_AddRef(self);
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

// 拖放目标嵌在置物架里，系统持有它时也就持有置物架：登记时系统留一份（注销时放掉），拖动经过时拖放循环也会另留一份。
// 拖动进行中置物架关闭了，系统手上的那份直到拖动结束才放掉，这期间置物架还在，回调看到 closed 后什么都不做。
static ULONG STDMETHODCALLTYPE target_add_ref(IDropTarget *self)
{
    ShelfWindow *shelf = target_shelf(self);
    shelf_window_retain(shelf);
    return (ULONG)shelf->references;
}

static ULONG STDMETHODCALLTYPE target_release(IDropTarget *self)
{
    ShelfWindow *shelf = target_shelf(self);
    ULONG remaining = shelf->references > 1 ? (ULONG)shelf->references - 1 : 0;
    shelf_window_release(shelf);
    return remaining;
}

static HRESULT STDMETHODCALLTYPE target_drag_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    (void)keys;
    ShelfWindow *shelf = target_shelf(self);
    if (shelf->closing || shelf->closed)
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    shelf->last_drag_over = GetTickCount64();
    bool fresh = !shelf->drag_inside;
    if (fresh)
    {
        shelf->drag_inside = true;
        if (shelf->drag_data) IDataObject_Release(shelf->drag_data);
        shelf->drag_data = data;
        IDataObject_AddRef(data);
        wchar_t *source = drop_source_shelf_id(data);
        shelf->drag_from_self = source && wcs_equal(source, shelf->model->id);
        free(source);
        // 另一个置物架上的项目：松手后由那个置物架交过来。
        shelf->drag_from_shelf = !shelf->drag_from_self && shelf_drag_session_from(data) != NULL;
        shelf->drag_kind = shelf->drag_from_self ? DROP_NONE : drop_classify(data);
#ifdef LOOK_VARIANT_CARDS
        if (!shelf->drag_from_self && shelf->drag_kind != DROP_NONE && !shelf->model->busy) shelf_show_drop_overlay(shelf, data, 0);
#else
        if (!shelf->drag_from_self && shelf->drag_kind != DROP_NONE && !shelf->model->busy) shelf_show_drop_overlay(shelf);
#endif
    }
    // 出场期间拖出窗口又回来时，拖入的状态还留着，只需要重新接上拖动图像和说明。
    if (fresh || shelf->image_left)
    {
        shelf->image_left = false;
        shelf->has_last_description = false;
        drop_helper_enter(shelf->drop_helper, shelf->handle, data, *effect & preferred_effect(*effect));
    }
    update_drag(shelf, point, effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_drag_over(IDropTarget *self, DWORD keys, POINTL point, DWORD *effect)
{
    (void)keys;
    ShelfWindow *shelf = target_shelf(self);
    if (shelf->closed)
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    shelf->last_drag_over = GetTickCount64();
    // 光标每动一下来源程序就问一次，间隔内的询问直接给出上次的结果。
    if (shelf_seconds_since(shelf->drag_over_handled) < DRAG_OVER_INTERVAL) *effect = shelf->drag_effects;
    else update_drag(shelf, point, effect);
    return S_OK;
}

static bool cursor_inside(ShelfWindow *shelf)
{
    RECT rect;
    POINT cursor;
    return shelf->handle && GetWindowRect(shelf->handle, &rect) && GetCursorPos(&cursor) && PtInRect(&rect, cursor);
}

static HRESULT STDMETHODCALLTYPE target_drag_leave(IDropTarget *self)
{
    ShelfWindow *shelf = target_shelf(self);
    if (shelf->closed) return S_OK;
    bool inside = cursor_inside(shelf);
    if (!inside)
    {
        drop_helper_leave(shelf->drop_helper);
        shelf->image_left = true;
    }
    // 出场动画期间真的拖出去了也先留着：晃动呼出时光标常常甩出窗口又回来；回来时接着用，没回来就由计时器收起。
    if (inside || shelf->popping) SetTimer(shelf->handle, SHELF_TIMER_LEAVE, 220, NULL);
    else shelf_end_drag(shelf);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
    (void)keys;
    ShelfWindow *shelf = target_shelf(self);
    if (shelf->closed)
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    float x, y;
    client_point(shelf, point, &x, &y);
    // 悬停的处理有间隔，松手时按松手的位置再判断一次落在哪张快速操作卡片上。
    int action = hit_test_tiles(shelf, x, y);
    bool from_self = shelf->drag_from_self;
    DropContentKind kind = shelf->drag_kind;
    ShelfDragSession *session = from_self ? NULL : shelf_drag_session_from(data);
#ifdef LOOK_VARIANT_CARDS
    // 松手时摊开的牌留在堆叠上，等新牌落进中间再合拢。来自另一个置物架的项目稍后才交过来，那时摊开的牌已经收好，这里照常淡回去。
    bool fan = action < 0 && !session && !from_self && !shelf->model->expanded && shelf_take_fan(shelf);
    end_drag(shelf, fan);
#else
    shelf_end_drag(shelf);
#endif

    if (from_self || kind == DROP_NONE || shelf->model->busy || shelf->closing || (action >= 0 && !drop_can_provide_files(kind)))
    {
        *effect = DROPEFFECT_NONE;
        drop_helper_drop(shelf->drop_helper, data, *effect);
        if (!from_self && kind == DROP_NONE) shelf_notify(shelf, S(STR_SourceProvidedNothing), false);
        return S_OK;
    }
    *effect = preferred_effect(*effect);
    drop_helper_drop(shelf->drop_helper, data, *effect);
    shelf->model->has_interacted = true;
    // 来自另一个置物架：记下放在了这里，拖动结束后那边把项目交过来；放到快速操作上时照常读取文件来处理。
    if (session && action < 0)
    {
        shelf_drag_session_drop_on(session, shelf, x, y);
        return S_OK;
    }

    if (action >= 0 && action < shelf->actions.count)
    {
        // 直接放到快速操作上：只要文件，读好后交给这个操作。
        Receive *receive = xcalloc(1, sizeof(Receive));
        receive->shelf = shelf_window_retain(shelf);
        receive->for_action = true;
        quick_action_clone(&receive->action, &shelf->actions.items[action]);
        shelf->receiving++;
        shelf_show_receiving(shelf, S(STR_ReadingDrop));
        drop_read(data, true, false, &shelf->model->lifetime_over, shelf_received, receive);
        return S_OK;
    }

#ifdef LOOK_VARIANT_CARDS
    if (fan) shelf_hold_fan(shelf);
#endif
    // 文件很多、在网络盘上，或磁盘很久没用时，读取可能要好几秒：这期间显示正在读取。
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->x = x;
    receive->y = y;
    shelf->receiving++;
    shelf_show_receiving(shelf, S(STR_ReadingDrop));
    int files = drop_read(data, false, true, &shelf->model->lifetime_over, shelf_received, receive);
    if (files > 1 && shelf->receiving > 0)
    {
        wchar_t *text = str_reading_files(files);
        shelf_show_receiving(shelf, text);
        free(text);
    }
    return S_OK;
}

static IDropTargetVtbl target_functions = {
    target_query_interface, target_add_ref, target_release, target_drag_enter, target_drag_over, target_drag_leave, target_drop
};

void shelf_register_drop_target(ShelfWindow *shelf)
{
    shelf->drop_target.base.lpVtbl = &target_functions;
    shelf->drop_target.shelf = shelf;
    shelf->drop_helper = drop_helper_create();
    HRESULT result = RegisterDragDrop(shelf->handle, &shelf->drop_target.base);
    shelf->drop_registered = SUCCEEDED(result);
    if (!shelf->drop_registered) log_error(L"置物架无法接收拖放\r\nHRESULT 0x%08lX", (unsigned long)result);
}

void shelf_revoke_drop_target(ShelfWindow *shelf)
{
    if (!shelf->drop_registered || !shelf->handle) return;
    shelf->drop_registered = false;
    RevokeDragDrop(shelf->handle);
    if (shelf->drag_data) IDataObject_Release(shelf->drag_data);
    shelf->drag_data = NULL;
}

// ───────────── 拖出 ─────────────

/// 拖出：被拖的牌或行变淡，取消或留在置物架上时恢复，移走的由移除动画接着淡出。
void shelf_begin_drag_out(ShelfWindow *shelf, ShelfItem **items, int count)
{
    ShelfModel *model = shelf->model;
    if (count == 0 || model->busy || shelf->dragging_out) return;
    shelf_window_retain(shelf);
    // 刚在别处改了名或挪了位置的文件先找回来，再检查还在不在；之后拖出期间不再跟着改名走。
    shelf_model_relink(model, items, count);
    shelf->dragging_out = true;
    model->dragging_out = true;
    ReleaseCapture();
    ui_window_set_tooltip(shelf->ui, NULL);

    ShelfItem **files = xalloc((size_t)count * sizeof(ShelfItem *));
    ShelfItem **others = xalloc((size_t)count * sizeof(ShelfItem *));
    int file_count = 0, other_count = 0;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (!shelf_item_is_file_system(item)) others[other_count++] = item;
        else if (shelf_item_refresh_availability(item)) files[file_count++] = item;
    }
    shelf_repaint(shelf);
    // 有文件要拖出时，一起选中的文本和链接各写成一个文件跟着走，不再被落下。
    StrList extra = {0};
    if (file_count > 0 && other_count > 0) shelf_model_write_for_drag(model, others, other_count, &extra);
    bool carried = file_count > 0 && extra.count == other_count && other_count > 0;
    ShelfItem **dragged = xalloc((size_t)count * sizeof(ShelfItem *));
    int dragged_count = 0;
    if (file_count > 0)
    {
        memcpy(dragged, files, (size_t)file_count * sizeof(ShelfItem *));
        dragged_count = file_count;
        if (carried)
        {
            memcpy(dragged + dragged_count, others, (size_t)other_count * sizeof(ShelfItem *));
            dragged_count += other_count;
        }
    }
    else
    {
        memcpy(dragged, others, (size_t)other_count * sizeof(ShelfItem *));
        dragged_count = other_count;
    }
    for (int index = 0; index < dragged_count; index++) shelf_item_retain(dragged[index]);

    shelf_dim_dragged(shelf, items, count);
    // 从列表里只拖出一部分时，下边伸出“新置物架”，放上去就拆成一个新的置物架。
    if (model->expanded && count < model->count) split_pad_show_for(shelf);
    ShelfDragSession session = { shelf, NULL, 0, 0, false };
    shelf_drag_session_begin(&session);
    DragOutRequest request = { shelf->handle, dragged, dragged_count, carried ? &extra : NULL, model->id, shelf->move_mode, shelf_scale(shelf) };
    // 拖进观察的文件夹的不算新文件。
    StrList names = {0};
    for (int index = 0; index < dragged_count; index++)
        if (shelf_item_is_file_system(dragged[index])) strlist_add(&names, dragged[index]->value);
    for (int index = 0; index < extra.count; index++) strlist_add(&names, extra.items[index]);
    folder_watch_drag_begin(&names);
    strlist_clear(&names);
    DragOutOutcome outcome = dragged_count > 0 ? drag_out(&request) : (DragOutOutcome){ DRAG_OUT_CANCELLED, false };
    folder_watch_drag_end();
    shelf_drag_session_end();
    SplitPlace place;
    bool has_place = split_pad_dismiss(&place);
    shelf->dragging_out = false;
    model->has_interacted = true;

    if (!shelf->closed)
    {
        if (session.target && !session.target->closing && !session.target->closed && session.target != shelf)
        {
            // 放到了另一个置物架上：项目交过去，这边按移走处理。
            shelf_receive(session.target, items, count, model, session.drop_x, session.drop_y);
            shelf_let_go(shelf, items, count);
        }
        else if (session.split && has_place)
        {
            POINT cursor;
            GetCursorPos(&cursor);
            shelf_split_off(shelf, items, count, &place, &cursor);
        }
        else if (outcome.result != DRAG_OUT_CANCELLED)
        {
            // 聊天软件和浏览器常在放下之后才读取文件，临时附件要多保留一会儿。
            for (int index = 0; index < dragged_count; index++)
                if (dragged[index]->temporary || !shelf_item_is_file_system(dragged[index])) shelf_model_keep_temporary_files(model);
            shelf_model_apply_drag_out(model, dragged, dragged_count, outcome.result, shelf_close_when_empty());
        }
        else if (outcome.refused && dragged_count > 0)
        {
            bool all_text = true;
            for (int index = 0; index < dragged_count && all_text; index++) all_text = !shelf_item_is_file_system(dragged[index]);
            POINT cursor;
            GetCursorPos(&cursor);
            if (all_text && !shelves_contain_point(cursor)) shelf_offer_convert(shelf, dragged, dragged_count);
        }
    }
    model->dragging_out = false;
    shelf_restore_dimmed(shelf);
    if (session.target) shelf_window_release(session.target);
    for (int index = 0; index < dragged_count; index++) shelf_item_release(dragged[index]);
    free(dragged);
    free(files);
    free(others);
    strlist_clear(&extra);
    shelf_window_release(shelf);
}

#else
// Linux 的拖入拖出走 GdkDrop / GdkDrag，接在 ui_window 的 GTK 后端上；读取结果和移除逻辑和 Windows 共用。

static GdkContentProvider *linux_drag_prepare(UiWindow *window, void *context, double x, double y, GdkDragAction *actions, void **data);
static void drag_out_finished(UiWindow *window, void *context, void *data, GdkDragAction action);

static DropContentKind classify_drop(GdkDrop *drop)
{
    GdkContentFormats *formats = gdk_drop_get_formats(drop);
    // 和 ui_window 里 drop target 的类型顺序保持一致：有图片就按图片接。
    if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) return DROP_IMAGE;
    if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST) || gdk_content_formats_contain_gtype(formats, G_TYPE_FILE))
        return DROP_FILES;
    if (gdk_content_formats_contain_gtype(formats, G_TYPE_STRING)) return DROP_TEXT;
    return DROP_NONE;
}

/// 文件列表里的本地路径放进 paths；网页一类的地址没有本地路径，放进 uris 当链接。
static void add_file_paths(GdkFileList *list, StrList *paths, StrList *uris)
{
    GSList *files = gdk_file_list_get_files(list);
    for (GSList *each = files; each; each = each->next)
    {
        char *path = g_file_get_path(G_FILE(each->data));
        if (!path)
        {
            // 浏览器拖链接、拖网页图片时给的是 http(s) 地址：没有本地文件，按链接收。
            char *uri = g_file_get_uri(G_FILE(each->data));
            if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
            {
                wchar_t *wide = wide_from_utf8(uri, strlen(uri));
                if (wide)
                {
                    strlist_add(uris, wide);
                    free(wide);
                }
            }
            if (uri) g_free(uri);
            continue;
        }
        wchar_t *wide = wide_from_utf8(path, strlen(path));
        if (wide)
        {
            strlist_add(paths, wide);
            free(wide);
        }
        g_free(path);
    }
    g_slist_free(files);
}

void shelf_receive_paths_at(ShelfWindow *shelf, const StrList *paths, float x, float y)
{
    if (paths->count == 0) return;
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->x = x;
    receive->y = y;
    shelf->receiving++;
    shelf_show_receiving(shelf, S(STR_ReadingDrop));
    drop_read_paths(paths, shelf_received, receive);
}

void shelf_receive_text_at(ShelfWindow *shelf, const wchar_t *text, float x, float y)
{
    wchar_t *trimmed = wcs_trim_dup(text ? text : L"");
    if (!trimmed || !*trimmed)
    {
        free(trimmed);
        return;
    }
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc(1, sizeof(DroppedEntry));
    result->entries[0].kind = text_is_single_url(trimmed) ? ITEM_LINK : ITEM_TEXT;
    result->entries[0].value = trimmed;
    result->count = 1;
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->x = x;
    receive->y = y;
    shelf->receiving++;
    shelf_received(result, receive);
}

/// 浏览器拖来的网页地址（链接、网页图片的地址）：没有本地文件，当成链接放进去。
static void receive_links_at(ShelfWindow *shelf, const StrList *uris, float x, float y)
{
    if (uris->count == 0) return;
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc((size_t)uris->count, sizeof(DroppedEntry));
    for (int index = 0; index < uris->count; index++)
    {
        result->entries[index].kind = ITEM_LINK;
        result->entries[index].value = wcs_dup(uris->items[index]);
    }
    result->count = uris->count;
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->x = x;
    receive->y = y;
    shelf->receiving++;
    shelf_received(result, receive);
}

void shelf_receive_texture_at(ShelfWindow *shelf, GdkTexture *texture, float x, float y)
{
    wchar_t *directory = attachment_cache_create();
    if (!directory) return;
    wchar_t *path = path_join(directory, L"image.png");
    size_t utf8_length = 0;
    char *utf8 = utf8_from_wide(path, &utf8_length);
    gboolean saved = utf8 ? gdk_texture_save_to_png(texture, utf8) : FALSE;
    free(utf8);
    if (!saved)
    {
        attachment_cache_release(directory);
        free(path);
        free(directory);
        return;
    }
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->with_facts = true;
    result->entries = xcalloc(1, sizeof(DroppedEntry));
    result->entries[0].kind = ITEM_FILE;
    result->entries[0].value = path;
    result->entries[0].has_facts = file_facts_read(path, &result->entries[0].facts);
    result->entries[0].type_name = file_type_name(path);
    result->count = 1;
    // 缓存文件夹交给置物架持有：项目移除后由它清理掉图片文件。
    strlist_add(&result->caches, directory);
    free(directory);
    Receive *receive = xcalloc(1, sizeof(Receive));
    receive->shelf = shelf_window_retain(shelf);
    receive->x = x;
    receive->y = y;
    shelf->receiving++;
    shelf_received(result, receive);
}

struct LinuxDragOut
{
    ShelfWindow *shelf;
    ShelfItem **items;
    int count;
    // 改由 X11 拖动源分几批送时，每项跟着第几批走（-1 是没送）；NULL 表示都在第一批里。
    int *parts;
    bool transferred;           // 已经交给别的置物架，结束时不要再按拖出的结果处理
};
typedef struct LinuxDragOut LinuxDragOut;
static LinuxDragOut *current_drag_out;

/// GTK（Wayland 源）和 X11 接放窗口（X11 源）最后都走到这里：按阶段更新置物架的拖动状态，松手时把数据放进去。
/// 和 Windows 一样，从别的程序拖进来的东西一律按复制收下，顶部的复制/移动开关只管拖出。
static bool shelf_drop_phase(ShelfWindow *shelf, UiDropPhase phase, DropContentKind incoming_kind, bool from_self, const GValue *value,
    double x, double y, GdkDragAction *action)
{
    if (shelf->closing || shelf->closed)
    {
        log_info(L"拖入：置物架已经关闭，忽略（阶段 %d）", (int)phase);
        return false;
    }
    switch (phase)
    {
        case UI_DROP_ENTER:
            shelf->drag_inside = true;
            shelf->drag_from_self = from_self;
            shelf->drag_from_shelf = false;
            shelf->drag_kind = from_self ? DROP_NONE : incoming_kind;
            if (!from_self && shelf->drag_kind != DROP_NONE && !shelf->model->busy)
            {
#ifdef LOOK_VARIANT_CARDS
                shelf_show_drop_overlay(shelf, NULL, 0);
#else
                shelf_show_drop_overlay(shelf);
#endif
            }
        {
            // 本程序自己的拖动也接：拖回原来的置物架按松手位置重排，拖到另一个置物架就交过去。
            bool accepted = !shelf->model->busy && (from_self || shelf->drag_kind != DROP_NONE);
            log_info(L"拖入：识别为 %ls，置物架%ls，%ls", from_self ? L"本程序自己的拖动" : shelf->drag_kind == DROP_FILES ? L"文件" :
                shelf->drag_kind == DROP_IMAGE ? L"图片" : shelf->drag_kind == DROP_TEXT ? L"文字" : L"不支持的内容",
                shelf->model->busy ? L"正忙" : L"空闲", accepted ? L"接受" : L"不接受");
            return accepted;
        }
        case UI_DROP_MOTION:
            log_trace(L"拖入：移动到（%g, %g）", x, y);
            return !shelf->model->busy && (from_self || shelf->drag_kind != DROP_NONE);
        case UI_DROP_LEAVE:
            log_info(L"拖入：离开置物架");
            shelf_end_drag(shelf);
            return true;
        case UI_DROP_DROP:
        {
            DropContentKind kind = shelf->drag_kind;
            bool self = shelf->drag_from_self;
            GType value_type = value ? G_VALUE_TYPE(value) : G_TYPE_INVALID;
            log_info(L"拖入：在（%g, %g）放下，识别为 %d，数据是 %hs，%ls", x, y, (int)kind,
                value_type != G_TYPE_INVALID && g_type_name(value_type) ? g_type_name(value_type) : "无",
                self ? L"本程序拖出的" : L"外部拖入");
            shelf_end_drag(shelf);
            if (self)
            {
                LinuxDragOut *out = current_drag_out;
                if (out && out->shelf == shelf && !shelf->model->busy)
                {
                    // 拖回原来的置物架：按松手的位置重新排序，堆叠里松手就放到最上面。
                    int row = -1;
                    ShelfTarget target = shelf_hit_test(shelf, (float)x, (float)y, &row);
                    int index = target == TARGET_ROW || target == TARGET_ROW_REMOVE ? row : shelf->model->count;
                    out->transferred = true;
                    shelf_model_move(shelf->model, out->items, out->count, index);
                    log_info(L"拖入：在原来的置物架上重排，放到第 %d 项", index);
                    if (action) *action = GDK_ACTION_MOVE;
                    return true;
                }
                // 从一个置物架拖到另一个置物架上：项目交过去，来源那边按移走处理。
                if (out && out->shelf != shelf && !shelf->model->busy && !out->shelf->closed && !out->shelf->closing)
                {
                    out->transferred = true;
                    shelf->model->has_interacted = true;
                    shelf_receive(shelf, out->items, out->count, out->shelf->model, (float)x, (float)y);
                    shelf_let_go(out->shelf, out->items, out->count);
                    if (action) *action = GDK_ACTION_MOVE;
                    return true;
                }
                return false;
            }
            // 没有数据（X11 那边的图片解不开、文件列表是空的）就是没收下，不能告诉来源成功了。
            if (kind == DROP_NONE || shelf->model->busy || !value) return false;
            shelf->model->has_interacted = true;
            GType type = value ? G_VALUE_TYPE(value) : G_TYPE_INVALID;
            if (type == GDK_TYPE_FILE_LIST)
            {
                StrList paths = {0}, uris = {0};
                add_file_paths((GdkFileList *)g_value_get_boxed(value), &paths, &uris);
                shelf_receive_paths_at(shelf, &paths, (float)x, (float)y);
                receive_links_at(shelf, &uris, (float)x, (float)y);
                strlist_clear(&paths);
                strlist_clear(&uris);
            }
            else if (type == G_TYPE_FILE)
            {
                GFile *file = g_value_get_object(value);
                StrList paths = {0}, uris = {0};
                char *path = g_file_get_path(file);
                if (path)
                {
                    wchar_t *wide = wide_from_utf8(path, strlen(path));
                    if (wide)
                    {
                        strlist_add(&paths, wide);
                        free(wide);
                    }
                    g_free(path);
                }
                else
                {
                    char *uri = g_file_get_uri(file);
                    if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
                    {
                        wchar_t *wide = wide_from_utf8(uri, strlen(uri));
                        if (wide)
                        {
                            strlist_add(&uris, wide);
                            free(wide);
                        }
                    }
                    if (uri) g_free(uri);
                }
                shelf_receive_paths_at(shelf, &paths, (float)x, (float)y);
                receive_links_at(shelf, &uris, (float)x, (float)y);
                strlist_clear(&paths);
                strlist_clear(&uris);
            }
            else if (type == G_TYPE_STRING)
            {
                const char *utf8 = g_value_get_string(value);
                wchar_t *wide = utf8 ? wide_from_utf8(utf8, strlen(utf8)) : NULL;
                if (wide)
                {
                    shelf_receive_text_at(shelf, wide, (float)x, (float)y);
                    free(wide);
                }
            }
            else if (type == GDK_TYPE_TEXTURE) shelf_receive_texture_at(shelf, g_value_get_object(value), (float)x, (float)y);
            return true;
        }
    }
    return false;
}

static bool linux_drop(UiWindow *window, void *context, UiDropPhase phase, GdkDrop *drop, const GValue *value, double x, double y,
    GdkDragAction *action)
{
    (void)window;
    ShelfWindow *shelf = context;
    // 拖动是不是本程序从置物架里拖出去的那一次，要认准同一个 GdkDrag。
    // 不能只看一个全局开关：合成器漏发结束通知时，那个开关会一直留着，把之后拖进来的东西全当成自己的。
    // 也不能只看“是不是本程序的窗口”：设置页和输入框里拖出来的文字是要放进置物架的，不算自己拖出。
    GdkDrag *source = drop ? gdk_drop_get_drag(drop) : NULL;
    // 拖出的来源置物架已经关掉的话，这次拖动早就该结束了，按外部拖入处理。
    bool from_self = source && source == ui_active_drag() && current_drag_out && !current_drag_out->shelf->closed;
    DropContentKind kind = DROP_NONE;
    if (phase == UI_DROP_ENTER)
    {
        GdkContentFormats *formats = drop ? gdk_drop_get_formats(drop) : NULL;
        char *format_text = formats ? gdk_content_formats_to_string(formats) : NULL;
        log_info(L"拖入：进入置物架（%g, %g），格式 %hs，允许动作 0x%x，来自本程序 %ls", x, y,
            format_text ? format_text : "", (unsigned)(drop ? gdk_drop_get_actions(drop) : 0), from_self ? L"是" : L"否");
        g_free(format_text);
        if (!from_self) kind = classify_drop(drop);
    }
    return shelf_drop_phase(shelf, phase, kind, from_self, value, x, y, action);
}

// ───────────── X11 应用的拖放 ─────────────
// Hyprland 不会把 X11 源的拖动转给原生 Wayland 窗口（见 xdnd_linux.c），X11 拖放桥在探针或置物架的
// 位置放一个看不见的 X11 窗口接住拖动。数据在这里变回和 GTK 一样的 GValue，再走同一个 shelf_drop_phase。

static DropContentKind xdnd_kind(XdndKind kind)
{
    switch (kind)
    {
        case XDND_KIND_FILES: return DROP_FILES;
        case XDND_KIND_TEXT: return DROP_TEXT;
        case XDND_KIND_IMAGE: return DROP_IMAGE;
        default: return DROP_NONE;
    }
}

static const wchar_t *xdnd_kind_name(XdndKind kind)
{
    switch (kind)
    {
        case XDND_KIND_FILES: return L"文件列表";
        case XDND_KIND_TEXT: return L"文字";
        case XDND_KIND_IMAGE: return L"图片";
        default: return L"不支持的内容";
    }
}

/// text/uri-list 里一行一个地址：本地文件变成路径，网页地址按链接收（和 GTK 那边一样）。
static GdkFileList *file_list_from_uri_list(const char *text)
{
    if (!text) return NULL;
    GSList *files = NULL;
    char **lines = g_strsplit(text, "\n", -1);
    for (int index = 0; lines[index]; index++)
    {
        char *line = lines[index];
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t')) line[--length] = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        if (g_str_has_prefix(line, "file://"))
        {
            char *path = g_filename_from_uri(line, NULL, NULL);
            if (path)
            {
                files = g_slist_append(files, g_file_new_for_path(path));
                g_free(path);
            }
        }
        else if (g_str_has_prefix(line, "http://") || g_str_has_prefix(line, "https://"))
            files = g_slist_append(files, g_file_new_for_uri(line));
    }
    g_strfreev(lines);
    if (!files) return NULL;
    GdkFileList *list = gdk_file_list_new_from_list(files);
    g_slist_free_full(files, g_object_unref);
    return list;
}

static void x11_drag_enter(void *context, XdndKind kind, double x, double y)
{
    ShelfWindow *shelf = context;
    GdkDragAction action = GDK_ACTION_COPY;
    log_info(L"X11 拖入：进入置物架（%g, %g），%ls", x, y, xdnd_kind_name(kind));
    shelf_drop_phase(shelf, UI_DROP_ENTER, xdnd_kind(kind), false, NULL, x, y, &action);
}

static void x11_drag_motion(void *context, XdndKind kind, double x, double y)
{
    ShelfWindow *shelf = context;
    GdkDragAction action = GDK_ACTION_COPY;
    shelf_drop_phase(shelf, UI_DROP_MOTION, xdnd_kind(kind), false, NULL, x, y, &action);
}

static void x11_drag_leave(void *context)
{
    ShelfWindow *shelf = context;
    log_info(L"X11 拖入：离开置物架");
    GdkDragAction action = 0;
    shelf_drop_phase(shelf, UI_DROP_LEAVE, DROP_NONE, false, NULL, 0, 0, &action);
}

static bool x11_drag_drop(void *context, const XdndPayload *payload, double x, double y)
{
    ShelfWindow *shelf = context;
    if (!payload) return false;
    GValue value = G_VALUE_INIT;
    bool have_value = false;
    if (payload->kind == XDND_KIND_FILES)
    {
        GdkFileList *list = file_list_from_uri_list(payload->text);
        if (list)
        {
            g_value_init(&value, GDK_TYPE_FILE_LIST);
            g_value_take_boxed(&value, list);
            have_value = true;
        }
    }
    else if (payload->kind == XDND_KIND_TEXT)
    {
        g_value_init(&value, G_TYPE_STRING);
        g_value_set_string(&value, payload->text ? payload->text : "");
        have_value = true;
    }
    else if (payload->kind == XDND_KIND_IMAGE && payload->image && payload->image_size > 0)
    {
        GBytes *bytes = g_bytes_new(payload->image, payload->image_size);
        GError *error = NULL;
        GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
        g_bytes_unref(bytes);
        if (texture)
        {
            g_value_init(&value, GDK_TYPE_TEXTURE);
            g_value_take_object(&value, texture);
            have_value = true;
        }
        else
        {
            log_info(L"X11 拖入：图片解不开（%hs）", error ? error->message : "?");
            g_clear_error(&error);
        }
    }
    log_info(L"X11 拖入：在（%g, %g）放下，%ls", x, y, xdnd_kind_name(payload->kind));
    GdkDragAction action = GDK_ACTION_COPY;
    bool accepted = shelf_drop_phase(shelf, UI_DROP_DROP, xdnd_kind(payload->kind), false, have_value ? &value : NULL, x, y, &action);
    if (G_IS_VALUE(&value)) g_value_unset(&value);
    return accepted;
}

static bool x11_drag_frame(void *context, int *x, int *y, int *width, int *height)
{
    ShelfWindow *shelf = context;
    if (!shelf || !shelf->ui) return false;
    return ui_window_frame(shelf->ui, x, y, width, height);
}

void shelf_register_drop_target(ShelfWindow *shelf)
{
    if (!shelf || !shelf->ui) return;
    ui_window_set_drop_handler(shelf->ui, linux_drop, shelf);
    ui_window_set_drag_handler(shelf->ui, linux_drag_prepare, drag_out_finished, shelf);
    // X11 应用的拖动接不到 Wayland 窗口上，给置物架也挂一个 X11 接放窗口。
    if (!shelf->xdnd_sink)
        shelf->xdnd_sink = xdnd_add_sink(shelf, x11_drag_enter, x11_drag_motion, x11_drag_leave, x11_drag_drop, x11_drag_frame);
}

void shelf_revoke_drop_target(ShelfWindow *shelf)
{
    if (!shelf || !shelf->ui) return;
    ui_window_set_drop_handler(shelf->ui, NULL, NULL);
    ui_window_set_drag_handler(shelf->ui, NULL, NULL, NULL);
    if (shelf->xdnd_sink)
    {
        xdnd_remove_sink(shelf->xdnd_sink);
        shelf->xdnd_sink = NULL;
    }
}

static GdkDragAction gdk_action(DWORD effects)
{
    GdkDragAction action = 0;
    if (effects & DROPEFFECT_COPY) action |= GDK_ACTION_COPY;
    if (effects & DROPEFFECT_MOVE) action |= GDK_ACTION_MOVE;
    if (effects & DROPEFFECT_LINK) action |= GDK_ACTION_LINK;
    return action;
}

// 拖出从哪个位置开始；标题栏、滚动条、按钮上不拖出。
static ShelfItem **drag_items_at(ShelfWindow *shelf, double x, double y, int *count)
{
    *count = 0;
    ShelfModel *model = shelf->model;
    int row = -1;
    ShelfTarget target = shelf_hit_test(shelf, (float)x, (float)y, &row);
    if (target == TARGET_STACK)
    {
        int total = model->count;
        if (total == 0) return NULL;
        ShelfItem **items = xalloc((size_t)total * sizeof(ShelfItem *));
        for (int index = 0; index < total; index++) items[index] = shelf_item_retain(model->items[index]);
        *count = total;
        return items;
    }
    if (target != TARGET_ROW || row < 0 || row >= model->count) return NULL;
    if (!model->items[row]->selected)
    {
        shelf_clear_selection(shelf);
        model->items[row]->selected = true;
    }
    ShelfItem **items = shelf_selected_items(shelf, count);
    for (int index = 0; index < *count; index++) shelf_item_retain(items[index]);
    return items;
}

static void drag_out_finished(UiWindow *window, void *context, void *data, GdkDragAction action)
{
    (void)window;
    (void)context;
    LinuxDragOut *out = data;
    if (!out) return;
    ShelfWindow *shelf = out->shelf;
    SplitPlace place;
    bool had_place = split_pad_dismiss(&place);
    bool split = split_pad_take_request();
    if (shelf && !shelf->closed)
    {
        shelf->dragging_out = false;
        shelf->model->dragging_out = false;
        shelf->model->has_interacted = true;
        if (!out->transferred && split && had_place)
        {
            // 松手在“新置物架”上：拆成一个新的置物架。
            POINT cursor;
            GetCursorPos(&cursor);
            shelf_split_off(shelf, out->items, out->count, &place, &cursor);
        }
        else if (!out->transferred && action != 0)
        {
            DragOutResult result = (action & GDK_ACTION_MOVE) ? DRAG_OUT_MOVED : (action & GDK_ACTION_LINK) ? DRAG_OUT_LINKED : DRAG_OUT_COPIED;
            // 改由 X11 拖动源送的时候，只算对方确认收下的那几批；被拒、超时、没送出去的项目留在置物架上。
            ShelfItem **sent = out->items;
            int sent_count = out->count;
            uint32_t delivered = 0;
            if (xdnd_source_delivered(&delivered))
            {
                sent = xalloc((size_t)out->count * sizeof(ShelfItem *));
                sent_count = 0;
                for (int index = 0; index < out->count; index++)
                {
                    int part = out->parts ? out->parts[index] : 0;
                    if (part >= 0 && part < 32 && (delivered >> part & 1u)) sent[sent_count++] = out->items[index];
                }
                log_info(L"拖出：X11 拖动源确认送到 %d / %d 项", sent_count, out->count);
            }
            // 聊天软件和浏览器常在放下之后才读取文件，临时附件多保留一会儿。
            for (int index = 0; index < sent_count; index++)
                if (sent[index]->temporary || !shelf_item_is_file_system(sent[index]))
                    shelf_model_keep_temporary_files(shelf->model);
            if (sent_count > 0) shelf_model_apply_drag_out(shelf->model, sent, sent_count, result, shelf_close_when_empty());
            if (sent != out->items) free(sent);
        }
        shelf_restore_dimmed(shelf);
    }
    current_drag_out = NULL;
    for (int index = 0; index < out->count; index++) shelf_item_release(out->items[index]);
    free(out->items);
    free(out->parts);
    shelf_window_release(out->shelf);
    free(out);
}

static GdkContentProvider *linux_drag_prepare(UiWindow *window, void *context, double x, double y, GdkDragAction *actions, void **data)
{
    (void)window;
    ShelfWindow *shelf = context;
    ShelfModel *model = shelf->model;
    if (shelf->closing || shelf->closed || model->busy || shelf->dragging_out)
    {
        log_trace(L"拖出：这次还不能开始（关闭 %ls，忙 %ls，已经在拖出 %ls）", shelf->closing || shelf->closed ? L"是" : L"否",
            model->busy ? L"是" : L"否", shelf->dragging_out ? L"是" : L"否");
        return NULL;
    }
    int count = 0;
    ShelfItem **items = drag_items_at(shelf, x, y, &count);
    if (!items || count == 0)
    {
        free(items);
        return NULL;
    }
    shelf_model_relink(model, items, count);
    shelf->dragging_out = true;
    model->dragging_out = true;
    ui_window_set_tooltip(shelf->ui, NULL);

    GdkContentProvider *provider = NULL;
    ShelfItem **files = xalloc((size_t)count * sizeof(ShelfItem *));
    ShelfItem **others = xalloc((size_t)count * sizeof(ShelfItem *));
    // 文件索引从前往后存，文本索引从后往前存。
    int *dragged_indices = xalloc((size_t)count * sizeof(int));
    int file_count = 0, other_count = 0;
    bool has_temporary = false;
    for (int index = 0; index < count; index++)
    {
        ShelfItem *item = items[index];
        if (!shelf_item_is_file_system(item))
        {
            dragged_indices[count - 1 - other_count] = index;
            others[other_count++] = item;
        }
        else if (shelf_item_refresh_availability(item))
        {
            dragged_indices[file_count] = index;
            files[file_count++] = item;
            has_temporary |= item->temporary;
        }
    }
    // 文件和文字一起选中时按设置决定送哪些。文档程序一次只肯插入一种内容：
    // 自动分批按置物架里的顺序一项一项地送；智能把文字和图片合成一份 HTML，放下后再补送文字。
    int mixed = app_settings()->mixed_drag;
    bool mixed_selection = file_count > 0 && other_count > 0;
    if (mixed_selection && mixed == MIXED_DRAG_TEXT) file_count = 0;
    bool mixed_text = file_count > 0 && other_count > 0 && mixed != MIXED_DRAG_FILES;
    bool mixed_smart = mixed_text && mixed == MIXED_DRAG_SMART;
    bool mixed_batch = mixed_text && mixed == MIXED_DRAG_BATCH;
    // 一起送的时候，文字和链接各写成一个文件跟着走：走 GTK 拖动的 Wayland 程序收到的还是文件列表。
    StrList extra = {0};
    if (mixed_text) shelf_model_write_for_drag(model, others, other_count, &extra);
    // 文字和链接连在一起：智能模式用来合成 HTML，也可以直接当作文字拖出。
    WStr joined = {0};
    for (int index = 0; index < other_count; index++)
    {
        if (others[index]->kind != ITEM_TEXT && others[index]->kind != ITEM_LINK) continue;
        if (joined.length > 0) wstr_append(&joined, L"\n");
        wstr_append(&joined, others[index]->value);
    }
    char *utf8_text = joined.length > 0 ? utf8_from_wide(joined.data, NULL) : NULL;
    wstr_free(&joined);
    bool text_planned = mixed_text && utf8_text && *utf8_text;
    // 分批送的时候每项跟着第几批走（和 items 对齐，-1 是没送），收尾时只移走对方确认收下的那几批。
    int *item_parts = NULL;
    if (mixed_text)
    {
        item_parts = xalloc((size_t)count * sizeof(int));
        for (int index = 0; index < count; index++) item_parts[index] = -1;
    }
    ShelfItem **dragged = xalloc((size_t)count * sizeof(ShelfItem *));
    int dragged_count = 0;
    if (file_count > 0)
    {
        memcpy(dragged, files, (size_t)file_count * sizeof(ShelfItem *));
        dragged_count = file_count;
        // 一起送的模式把文字也交给对方（HTML、补送或分批）；只送文件时文字留在置物架。
        if (text_planned)
        {
            memcpy(dragged + dragged_count, others, (size_t)other_count * sizeof(ShelfItem *));
            dragged_count += other_count;
        }
        for (int index = 0; index < dragged_count; index++) shelf_item_retain(dragged[index]);
        // 走 GTK 拖动的 Wayland 程序：临时 .txt 跟着文件列表一起给。
        StrList provider_paths = {0};
        for (int index = 0; index < file_count; index++) strlist_add(&provider_paths, files[index]->value);
        for (int index = 0; index < extra.count; index++) strlist_add(&provider_paths, extra.items[index]);
        GFile **objects = g_new0(GFile *, (gsize)provider_paths.count);
        int list_count = 0;
        for (int index = 0; index < provider_paths.count; index++)
        {
            char *utf8 = utf8_from_wide(provider_paths.items[index], NULL);
            if (!utf8) continue;
            objects[list_count++] = g_file_new_for_path(utf8);
            free(utf8);
        }
        strlist_clear(&provider_paths);
        GdkFileList *list = list_count > 0 ? gdk_file_list_new_from_array(objects, (gsize)list_count) : NULL;
        for (int index = 0; index < list_count; index++) g_object_unref(objects[index]);
        g_free(objects);
        // 自己的 X11 拖动源只给真正的文件：文字要么在 HTML 里，要么补送，不再混一个 .txt 进去。
        char **names = xalloc((size_t)(file_count ? file_count : 1) * sizeof(char *));
        int object_count = 0;
        for (int index = 0; index < file_count; index++)
        {
            char *utf8 = utf8_from_wide(files[index]->value, NULL);
            if (!utf8) continue;
            names[object_count++] = utf8;
        }
        if (list)
        {
            GdkContentProvider *files_provider = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list);
            provider = files_provider;
            // GdkFileList 只给出 text/plain;charset=utf-8，微信要找的是不带 charset 的 text/plain，
            // 找不到就退回粘贴剪贴板。补一份纯 text/plain（路径），它就会把路径当文字插进去。
            // 图片不加这份：微信优先认 image/png，多一份纯文字反而可能把它顶掉。
            GdkContentProvider *image = NULL;
            // 只拖一张图片时，再附一份图片格式。文件列表放前面，别的程序拿到的还是文件，行为不变。
            const char *image_mime = NULL;
            GBytes *image_bytes = object_count == 1 ? xdnd_read_drag_image(names[0], &image_mime) : NULL;
            if (image_bytes && list_count == 1 && extra.count == 0)
                image = gdk_content_provider_new_for_bytes(image_mime, image_bytes);
            GdkContentProvider *parts[2];
            int part_count = 1;
            parts[0] = files_provider;
            if (image)
                parts[part_count++] = image;
            else if (object_count > 0)
            {
                GString *plain = g_string_new(NULL);
                for (int index = 0; index < object_count; index++)
                {
                    if (plain->len > 0) g_string_append_c(plain, '\n');
                    g_string_append(plain, names[index]);
                }
                gsize length = plain->len;
                char *text = g_string_free(plain, FALSE);
                GBytes *bytes = g_bytes_new_take(text, length);
                parts[part_count++] = gdk_content_provider_new_for_bytes("text/plain", bytes);
                g_bytes_unref(bytes);
            }
            // union 直接接管传进去的 provider，别再 unref：实测它不额外持有引用，
            // 掉到零就没了，拖动时再去用就是访问已释放的内存。
            if (part_count > 1) provider = gdk_content_provider_new_union(parts, (gsize)part_count);
            // GdkFileList 是 boxed 类型，不是 GObject：用 g_boxed_free 释放，g_object_unref 会报
            // “g_object_unref: assertion 'G_IS_OBJECT (object)' failed”，而且是漏掉的。
            g_boxed_free(GDK_TYPE_FILE_LIST, list);
            *actions = gdk_action(drag_allowed_effects(shelf->move_mode, has_temporary || extra.count > 0));
            if (!*actions) *actions = GDK_ACTION_COPY;
            bool move = (*actions & GDK_ACTION_MOVE) != 0;
            if (mixed_batch)
            {
                // 自动分批：按置物架里的顺序一项一项地送。第一批跟着这次拖动走，
                // 后面的排进 X11 拖动源，真正放下以后一批一批自动送过去。
                bool first = true;
                int part = 0;
                for (int index = 0; index < count; index++)
                {
                    ShelfItem *item = items[index];
                    char *one = NULL;
                    if (shelf_item_is_file_system(item))
                    {
                        if (!shelf_item_refresh_availability(item)) continue;
                        one = utf8_from_wide(item->value, NULL);
                    }
                    else if (item->kind == ITEM_TEXT || item->kind == ITEM_LINK)
                    {
                        if (item->value && *item->value) one = utf8_from_wide(item->value, NULL);
                    }
                    if (!one || !*one)
                    {
                        free(one);
                        continue;
                    }
                    if (first)
                    {
                        if (shelf_item_is_file_system(item))
                        {
                            const char *first_mime = image_mime;
                            bool reuse_image = object_count == 1 && strcmp(one, names[0]) == 0;
                            GBytes *first_image = reuse_image ? image_bytes : xdnd_read_drag_image(one, &first_mime);
                            xdnd_source_set_payload((const char *const *)&one, 1, NULL, NULL, move, first_image, first_mime);
                            if (!reuse_image) g_clear_pointer(&first_image, g_bytes_unref);
                        }
                        else xdnd_source_set_payload(NULL, 0, one, NULL, false, NULL, NULL);
                        first = false;
                        item_parts[index] = 0;
                    }
                    else if (shelf_item_is_file_system(item) ? xdnd_source_add_batch((const char *const *)&one, 1, NULL)
                                                             : xdnd_source_add_batch(NULL, 0, one))
                        item_parts[index] = ++part;
                    free(one);
                }
                // 一个能送的都没有（文件全找不到了）：还是按整组文件交过去，至少不让这次拖动变成空的。
                if (first)
                {
                    xdnd_source_set_payload((const char *const *)names, object_count, NULL, NULL, move, image_bytes, image_mime);
                    for (int index = 0; index < count; index++)
                        if (shelf_item_is_file_system(items[index])) item_parts[index] = 0;
                }
            }
            else
            {
                char *html = mixed_smart ? xdnd_source_html_build(utf8_text, (const char *const *)names, object_count) : NULL;
                xdnd_source_set_payload((const char *const *)names, object_count, NULL, html, move, image_bytes, image_mime);
                // 智能：HTML 里已经有文字，可文档程序常常只肯从文件列表里插图片；放下后再补一批纯文字。
                // 文字算在补送的那批里：HTML 对方未必用了，补送那批被拒时文字留在置物架上。
                bool text_queued = text_planned && xdnd_source_add_batch(NULL, 0, utf8_text);
                if (item_parts)
                    for (int index = 0; index < count; index++)
                        item_parts[index] = shelf_item_is_file_system(items[index]) ? 0 : text_queued ? 1 : -1;
                free(html);
            }
            g_clear_pointer(&image_bytes, g_bytes_unref);
        }
        for (int index = 0; index < object_count; index++) free(names[index]);
        free(names);
    }
    else
    {
        // 没有文件：文字和链接连在一起当作文字拖出。
        for (int index = 0; index < other_count; index++)
            if (others[index]->kind == ITEM_TEXT || others[index]->kind == ITEM_LINK)
                dragged[dragged_count++] = shelf_item_retain(others[index]);
        if (utf8_text && *utf8_text)
        {
            // GTK 要的是 UTF-8：直接给 wchar_t 那边收到的是乱码，拖出去的文字也会坏掉。
            provider = gdk_content_provider_new_typed(G_TYPE_STRING, utf8_text);
            xdnd_source_set_payload(NULL, 0, utf8_text, NULL, false, NULL, NULL);
        }
        *actions = GDK_ACTION_COPY;
    }
    free(utf8_text);
    strlist_clear(&extra);
    free(files);
    free(others);
    if (!provider || dragged_count == 0)
    {
        if (provider) g_object_unref(provider);
        for (int index = 0; index < dragged_count; index++) shelf_item_release(dragged[index]);
        free(dragged);
        free(item_parts);
        free(dragged_indices);
        shelf->dragging_out = false;
        model->dragging_out = false;
        free(items);
        return NULL;
    }

    *data = xcalloc(1, sizeof(LinuxDragOut));
    LinuxDragOut *out = *data;
    out->shelf = shelf_window_retain(shelf);
    out->items = dragged;
    out->count = dragged_count;
    if (item_parts)
    {
        out->parts = xalloc((size_t)dragged_count * sizeof(int));
        for (int index = 0; index < dragged_count; index++)
        {
            int original = index < file_count ? dragged_indices[index] : dragged_indices[count - 1 - (index - file_count)];
            out->parts[index] = item_parts[original];
        }
        free(item_parts);
    }
    free(dragged_indices);
    current_drag_out = out;
    shelf_dim_dragged(shelf, items, count);
    // 从列表里只拖出一部分时，下边伸出“新置物架”，放上去就拆成一个新的置物架。
    if (model->expanded && count < model->count) split_pad_show_for(shelf);
    free(items);
    return provider;
}

// Linux 的拖出由 GTK 的拖动源在拖动阈值到达时发起（见 linux_drag_prepare），共享代码里到阈值时的入口不用做别的。
void shelf_begin_drag_out(ShelfWindow *shelf, ShelfItem **items, int count)
{
    (void)shelf;
    (void)items;
    (void)count;
}
#endif

