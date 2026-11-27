#include "look_variant.h"
#include "shelf_window_internal.h"
#include "shelf_card.h"
#ifdef LOOK_VARIANT_CARDS
#include "card_face.h"
#endif
#include "log.h"
#include "shelf_history.h"
#include "power.h"
#include "probe.h"
#include "task.h"
#include "theme.h"

#include <math.h>
#ifdef _WIN32
#include <shellscalingapi.h>
#endif
#include <stdlib.h>
#include <string.h>


// ───────────── 尺寸 ─────────────

static float resolve_width(int size)
{
    return size == SHELF_SIZE_SMALL ? 184.0f : size == SHELF_SIZE_LARGE ? 264.0f : 220.0f;
}

float shelf_width_for(int size)
{
    return resolve_width(size);
}

/// 缩略图堆叠区域的高度，按宽度等比计算（Math.Round 恰好一半时取偶数）。
static float resolve_stack_height(int size)
{
    return (float)rint(resolve_width(size) * 0.66);
}

#ifdef LOOK_VARIANT_CARDS
// ───────────── 平铺的格子，和 C# style 版的 CardGridPanel 相同 ─────────────

int shelf_grid_columns_for(float width)
{
    int columns = (int)(width / SHELF_SLOT_WIDTH);
    return columns < 1 ? 1 : columns;
}

int shelf_grid_columns(int size)
{
    return size == SHELF_SIZE_SMALL ? 4 : size == SHELF_SIZE_LARGE ? 6 : 5;
}

int shelf_grid_rows_for(int count, float width)
{
    int columns = shelf_grid_columns_for(width);
    return (count + columns - 1) / columns;
}

float shelf_grid_height(int rows)
{
    return rows <= 0 ? 0 : 2 * SHELF_GRID_INSET + rows * SHELF_ROW_PITCH - SHELF_ROW_GAP;
}

#endif
void shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height)
{
#ifdef LOOK_VARIANT_CARDS
    // 平铺时窗口按一行的牌数定宽：内容区左右各留 10，平铺区域四周还有 1，少算这 2 就会少放一列。
    *width = expanded ? shelf_grid_columns(shelf->size) * SHELF_SLOT_WIDTH + 22 : resolve_width(shelf->size);
    // 平铺最多显示两行，再多就滚动。
    int rows = shelf_grid_rows_for(shelf->model->count, *width - 22);
    rows = rows < 1 ? 1 : rows > 2 ? 2 : rows;
    float body = expanded ? shelf_grid_height(rows) : resolve_stack_height(shelf->size) + SHELF_CAPTION_HEIGHT;
#else
    *width = expanded ? resolve_width(shelf->size) + 80 : resolve_width(shelf->size);
    int count = shelf->model->count;
    int rows = count < 2 ? 2 : count > 6 ? 6 : count;
    float body = expanded ? rows * SHELF_ROW_HEIGHT + 6 : resolve_stack_height(shelf->size) + SHELF_CAPTION_HEIGHT;
#endif
    bool has_actions = shelf->actions.count > 0;
    float actions = has_actions ? SHELF_ACTION_BAR_HEIGHT : 10;
    // 拖入时上方的放置区域至少留 66，下方是展开的一行快速操作。
    float tiles = has_actions ? SHELF_ACTION_TILE_HEIGHT + 14 : 8;
    if (body < 66 + tiles - actions) body = 66 + tiles - actions;
    *height = SHELF_HEADER_HEIGHT + body + actions;
}

float shelf_scale(const ShelfWindow *shelf)
{
    return ui_window_scale(shelf->ui);
}

static RECT work_area_at(POINT point, double *scale)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = { .cbSize = sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    UINT dpi_x = 96, dpi_y = 96;
    if (scale) *scale = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y) == S_OK && dpi_x > 0 ? dpi_x / 96.0 : 1.0;
    return info.rcWork;
}

void shelf_keep_in_work_area(ShelfWindow *shelf)
{
    RECT rect;
    if (!shelf->handle || !GetWindowRect(shelf->handle, &rect)) return;
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    POINT probe = { rect.left + width / 2, rect.top + (height / 2 < 40 ? height / 2 : 40) };
    RECT work = work_area_at(probe, NULL);
    int max_left = work.right - width > work.left ? work.right - width : work.left;
    int max_top = work.bottom - height > work.top ? work.bottom - height : work.top;
    int left = rect.left < work.left ? work.left : rect.left > max_left ? max_left : rect.left;
    int top = rect.top < work.top ? work.top : rect.top > max_top ? max_top : rect.top;
    if (left != rect.left || top != rect.top) SetWindowPos(shelf->handle, NULL, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void shelf_fit_resized(RECT rect, int width, int height, int *left, int *top)
{
    int old_width = rect.right - rect.left, old_height = rect.bottom - rect.top;
    POINT probe = { rect.left + old_width / 2, rect.top + (old_height / 2 < 40 ? old_height / 2 : 40) };
    RECT work = work_area_at(probe, NULL);
    // 贴着工作区右边的继续贴着（往左长、往右缩），其余情况左边不动；顶边尽量不动，下面放不下时才往上移。
    int x = rect.right >= work.right - 1 && rect.left > work.left ? rect.right - width : rect.left;
    int max_left = work.right - width > work.left ? work.right - width : work.left;
    int max_top = work.bottom - height > work.top ? work.bottom - height : work.top;
    *left = x < work.left ? work.left : x > max_left ? max_left : x;
    *top = rect.top < work.top ? work.top : rect.top > max_top ? max_top : rect.top;
}

void shelf_update_window_size(ShelfWindow *shelf)
{
    float width, height;
    shelf_target_size(shelf, shelf->model->expanded, &width, &height);
    RECT rect;
    if (!shelf->handle || !GetWindowRect(shelf->handle, &rect)) return;
    float scale = shelf_scale(shelf);
    int pixel_width = (int)lroundf(width * scale), pixel_height = (int)lroundf(height * scale);
    if (rect.right - rect.left != pixel_width || rect.bottom - rect.top != pixel_height)
        SetWindowPos(shelf->handle, NULL, 0, 0, pixel_width, pixel_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // 滑入动画结束时会再检查一次，动画进行中不要把窗口拉回原处。
    if (IsWindowVisible(shelf->handle) && !shelf->sliding && !shelf->card) shelf_keep_in_work_area(shelf);
    ui_window_invalidate(shelf->ui);
}

static void update_thumbnail_size(ShelfWindow *shelf)
{
    // 卡片 84 减去内边距后显示区域约为 66；堆叠画布 160×116 随置物架大小缩放，两侧各留 10 的边距。
    float stack_scale = fminf((resolve_width(shelf->size) - 20) / SHELF_CANVAS_WIDTH, resolve_stack_height(shelf->size) / SHELF_CANVAS_HEIGHT);
#ifdef LOOK_VARIANT_CARDS
    shelf->model->thumbnail_size = (int)ceil(50 * fmaxf(stack_scale, 1) * shelf_scale(shelf));
#else
    shelf->model->thumbnail_size = (int)ceil(66 * stack_scale * shelf_scale(shelf));
#endif
}

void shelf_window_apply_settings(ShelfWindow *shelf)
{
    const AppSettings *settings = app_settings();
    bool resized = shelf->size != settings->quick_shelf_size;
    shelf->size = settings->quick_shelf_size;
    QuickActionList actions = quick_actions_visible(settings);
    if (!quick_action_list_same(&actions, &shelf->actions))
    {
        quick_action_list_free(&shelf->actions);
        shelf->actions = actions;
        shelf_rebuild_actions(shelf);
        resized = true;
    }
    else quick_action_list_free(&actions);
    shelf->move_mode = settings->drag_out_mode == TRANSFER_MOVE;
    update_thumbnail_size(shelf);
    if (resized || !IsWindowVisible(shelf->handle)) shelf_update_window_size(shelf);
    shelf_repaint(shelf);
}

void shelf_update_title(ShelfWindow *shelf)
{
    wchar_t *title = shelf_model_title(shelf->model);
    wchar_t *window_title = str_shelf_window_title(title);
    SetWindowTextW(shelf->handle, window_title);
    free(window_title);
    free(title);
}

void shelf_rebuild_actions(ShelfWindow *shelf)
{
    for (int index = 0; index < SHELF_MAX_ACTIONS; index++)
    {
        free(shelf->action_labels[index]);
        free(shelf->action_tooltips[index]);
        shelf->action_labels[index] = shelf->action_tooltips[index] = NULL;
        if (index >= shelf->actions.count) continue;
        shelf->action_labels[index] = quick_action_label(&shelf->actions.items[index]);
        shelf->action_tooltips[index] = quick_action_tooltip(&shelf->actions.items[index]);
    }
    // 正在拖入时操作换了：卡片收起，下一次移动再按新的判断。
    shelf_reset_tiles(shelf);
    shelf_set_hovered_action(shelf, -1, true);
    if (shelf->hovered == TARGET_ACTION)
    {
        shelf->hovered = TARGET_NONE;
        if (shelf->ui) ui_window_set_tooltip(shelf->ui, NULL);
    }
    shelf_repaint(shelf);
}

void shelf_repaint(ShelfWindow *shelf)
{
    if (shelf && shelf->ui) ui_window_invalidate(shelf->ui);
}

// ───────────── 文字 ─────────────

/// 一行文字的高度（按字号和粗细缓存）。
float shelf_line_height(float size, int weight)
{
    typedef struct LineHeight { float size; int weight; float height; } LineHeight;
    static LineHeight cache[8];
    static int count;
    for (int index = 0; index < count; index++)
        if (cache[index].size == size && cache[index].weight == weight) return cache[index].height;
    GfxTextStyle style = shelf_text_style(size, weight, GFX_ALIGN_START);
    style.ellipsis = false;
    float height = gfx_text_measure(L"Ag", &style, 0).height;
    if (height <= 0) return size * 1.33f;
    if (count < 8) cache[count++] = (LineHeight){ size, weight, height };
    return height;
}

float shelf_text_width(const wchar_t *text, float size, int weight, GfxFont font)
{
    GfxTextStyle style = { font, size, weight, false, false, GFX_ALIGN_START, GFX_ALIGN_START };
    return gfx_text_measure(text, &style, 0).width;
}

// ───────────── 排版 ─────────────

CardPose shelf_card_home(int card)
{
    // XAML 里三张牌的位置：最上面一张不转，第二张转 7° 往右挪，最下面一张转 -10° 往左挪；中心 = 左上角 + 42 + 平移。
    static const CardPose homes[3] = {
#ifdef LOOK_VARIANT_CARDS
        { 48 + 32, 12 + 43, 0, 1, 1 },
        { 48 + 32 + 11, 14 + 43 + 1, 7, 1, 1 },
        { 48 + 32 - 12, 16 + 43 + 2, -10, 1, 1 },
#else
        { 38 + 42, 16 + 42, 0, 1, 1 },
        { 38 + 42 + 11, 18 + 42 + 1, 7, 1, 1 },
        { 38 + 42 - 12, 20 + 42 + 2, -10, 1, 1 },
#endif
    };
    return homes[card];
}

void shelf_to_canvas(ShelfWindow *shelf, float x, float y, float *cx, float *cy)
{
    const ShelfLayout *layout = &shelf->layout;
    float scale = layout->canvas_scale > 0 ? layout->canvas_scale : 1;
    *cx = (x - layout->canvas_x) / scale;
    *cy = (y - layout->canvas_y) / scale;
}

#ifdef LOOK_VARIANT_CARDS
int shelf_grid_columns_now(ShelfWindow *shelf)
#else
int shelf_visible_rows(ShelfWindow *shelf)
#endif
{
#ifdef LOOK_VARIANT_CARDS
    return shelf->layout.columns > 0 ? shelf->layout.columns : 1;
#else
    float height = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    int rows = (int)floorf(height / SHELF_ROW_PITCH);
    return rows < 1 ? 1 : rows;
#endif
}

#ifdef LOOK_VARIANT_CARDS
void shelf_set_scroll_offset(ShelfWindow *shelf, float offset)
#else
void shelf_set_scroll(ShelfWindow *shelf, int first)
#endif
{
#ifdef LOOK_VARIANT_CARDS
    float viewport = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    float maximum = shelf->layout.extent - viewport;
    if (offset > maximum) offset = maximum;
    if (offset < 0) offset = 0;
    if (offset == shelf->scroll_offset) return;
    shelf->scroll_offset = offset;
#else
    int maximum = shelf->model->count - shelf_visible_rows(shelf);
    if (first > maximum) first = maximum;
    if (first < 0) first = 0;
    if (first == shelf->scroll_first) return;
    shelf->scroll_first = first;
#endif
    shelf_layout(shelf);
    shelf_repaint(shelf);
}

void shelf_scroll_into_view(ShelfWindow *shelf, int index)
{
    if (index < 0 || index >= shelf->model->count) return;
    shelf_layout(shelf);
#ifdef LOOK_VARIANT_CARDS
    float top = (float)(index / shelf_grid_columns_now(shelf)) * SHELF_ROW_PITCH;
    float bottom = top + SHELF_SLOT_HEIGHT + 2 * SHELF_GRID_INSET;
    float viewport = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    if (top < shelf->scroll_offset) shelf_set_scroll_offset(shelf, top);
    else if (bottom > shelf->scroll_offset + viewport) shelf_set_scroll_offset(shelf, bottom - viewport);
#else
    int rows = shelf_visible_rows(shelf);
    if (index < shelf->scroll_first) shelf_set_scroll(shelf, index);
    else if (index >= shelf->scroll_first + rows) shelf_set_scroll(shelf, index - rows + 1);
#endif
}

#ifdef LOOK_VARIANT_CARDS
GfxRect shelf_tile_rect(ShelfWindow *shelf, int index)
#else
GfxRect shelf_row_rect(ShelfWindow *shelf, int index)
#endif
{
#ifdef LOOK_VARIANT_CARDS
    const ShelfLayout *layout = &shelf->layout;
    int columns = shelf_grid_columns_now(shelf);
    float width = layout->list.right - layout->list.left - 2;
    // 多出的宽度平均分给列间距。
    float step = columns > 1 ? (width - SHELF_SLOT_WIDTH) / (columns - 1) : 0;
    int row = index / columns, column = index % columns;
    float left = layout->list.left + 1 + column * step;
    float top = layout->list.top + 1 + SHELF_GRID_INSET + row * SHELF_ROW_PITCH - shelf->scroll_offset;
    return shelf_rect_of(left, top, SHELF_SLOT_WIDTH, SHELF_SLOT_HEIGHT);
#else
    const GfxRect list = shelf->layout.list;
    float top = list.top + 1 + (float)(index - shelf->scroll_first) * SHELF_ROW_PITCH + 1;
    return (GfxRect){ list.left + 1, top, list.right - 1, top + SHELF_ROW_PITCH - 2 };
#endif
}

#ifdef LOOK_VARIANT_CARDS
GfxRect shelf_tile_card_rect(ShelfWindow *shelf, int index)
#else
GfxRect shelf_row_thumb_rect(ShelfWindow *shelf, int index)
#endif
{
#ifdef LOOK_VARIANT_CARDS
    GfxRect slot = shelf_tile_rect(shelf, index);
    float left = slot.left + (SHELF_SLOT_WIDTH - SHELF_CARD_WIDTH) / 2;
    return shelf_rect_of(left, slot.top, SHELF_CARD_WIDTH, SHELF_CARD_HEIGHT);
#else
    GfxRect row = shelf_row_rect(shelf, index);
    return shelf_rect_of(row.left + 6, row.top + 5, 32, 32);
#endif
}

#ifdef LOOK_VARIANT_CARDS
void shelf_visible_tiles(ShelfWindow *shelf, int *first, int *last)
{
    const ShelfLayout *layout = &shelf->layout;
    int columns = shelf_grid_columns_now(shelf), count = shelf->model->count;
    int rows = (count + columns - 1) / columns;
    float viewport = layout->list.bottom - layout->list.top - 2;
    int first_row = (int)floorf((shelf->scroll_offset - SHELF_GRID_INSET) / SHELF_ROW_PITCH) - 1;
    int last_row = (int)floorf((shelf->scroll_offset + viewport - SHELF_GRID_INSET) / SHELF_ROW_PITCH) + 1;
    if (first_row < 0) first_row = 0;
    if (last_row > rows - 1) last_row = rows - 1;
    *first = first_row * columns;
    *last = (last_row + 1) * columns - 1;
    if (*last > count - 1) *last = count - 1;
}

/// 平铺里牌右上角的移除按钮：墨色小圆，伸出牌角 7。
GfxRect shelf_tile_remove_rect(GfxRect card)
{
    return shelf_rect_of(card.right - 11, card.top - 7, 18, 18);
}
#endif

static void clear_stack_summary(ShelfWindow *shelf)
{
    free(shelf->stack_caption);
    free(shelf->stack_detail);
    shelf->stack_caption = shelf->stack_detail = NULL;
}

static float mode_width(ShelfWindow *shelf)
{
    float *cached = &shelf->mode_widths[shelf->move_mode];
    if (*cached > 0) return *cached;
    float icon = shelf_text_width(shelf->move_mode ? GLYPH_MOVE : GLYPH_COPY, 12, 400, GFX_FONT_ICON);
    float label = shelf_text_width(S(shelf->move_mode ? STR_TransferMove : STR_TransferCopy), 12, 400, GFX_FONT_TEXT);
#ifdef LOOK_VARIANT_CARDS
    float width = ceilf(1 + 7 + icon + 5 + label + 8 + 1);
#else
    float width = ceilf(1 + 6 + icon + 5 + label + 8 + 1);
#endif
    if (icon > 0 && label > 0) *cached = width;
    return width;
}

void shelf_layout(ShelfWindow *shelf)
{
    unsigned generation = language_generation();
    if (shelf->label_generation != generation)
    {
        shelf->label_generation = generation;
        clear_stack_summary(shelf);
        shelf->mode_widths[0] = shelf->mode_widths[1] = 0;
    }
    ShelfLayout *layout = &shelf->layout;
    GfxSize size = ui_window_size(shelf->ui);
    float width = size.width, height = size.height;
    layout->width = width;
    layout->height = height;

    // 标题栏：外边距 4，左边是置顶和移动/复制，右边是切换视图、更多和关闭，标题在中间。
#ifdef _WIN32
    layout->pin = shelf_rect_of(4, 7, 28, 30);
    layout->mode = shelf_rect_of(4 + 28 + 2, 10, mode_width(shelf), 24);
#else
    // Wayland 上没有“取消置顶”：层壳窗口本来就在所有普通窗口之上，置顶开关没得可关，索性不画。
    layout->pin = (GfxRect){ 0, 0, 0, 0 };
    layout->mode = shelf_rect_of(4, 10, mode_width(shelf), 24);
#endif
    layout->close = shelf_rect_of(width - 4 - 28, 7, 28, 30);
    layout->more = shelf_rect_of(layout->close.left - 28, 7, 28, 30);
    bool has_items = shelf->model->count > 0;
    layout->view = has_items ? shelf_rect_of(layout->more.left - 28, 7, 28, 30) : (GfxRect){ 0, 0, 0, 0 };
    float right = has_items ? layout->view.left : layout->more.left;
    layout->title = (GfxRect){ layout->mode.right + 4, 4, right - 4, 40 };

    // 有快速操作时内容区下面是按钮栏（外边距 8,6,8,8，按钮高 36，上面还有 2）。
    int action_count = shelf->actions.count;
    layout->body = (GfxRect){ 10, SHELF_HEADER_HEIGHT + 2, width - 10, action_count > 0 ? height - 52 : height - 10 };
    layout->stack = layout->body;
    if (shelf->stack_pinned) layout->stack = shelf_rect_of(layout->body.left, layout->body.top, shelf->stack_pin.width, shelf->stack_pin.height);
    layout->list = layout->body;
    if (shelf->list_pinned) layout->list = shelf_rect_of(layout->body.left, layout->body.top, shelf->list_pin.width, shelf->list_pin.height);

    // 堆叠：下面两行文字（外边距 2,2,2,4），上面是按比例缩放的 160×116 画布。
    float caption_height = shelf_line_height(13, 600), detail_height = shelf_line_height(11, 400);
    float panel = 2 + caption_height + 1 + detail_height + 4;
    GfxRect stack = layout->stack;
    float canvas_width = stack.right - stack.left, canvas_height = stack.bottom - stack.top - panel;
    float scale = fminf(canvas_width / SHELF_CANVAS_WIDTH, canvas_height / SHELF_CANVAS_HEIGHT);
    if (scale < 0.01f) scale = 0.01f;
    layout->canvas_scale = scale;
    layout->canvas_x = stack.left + (canvas_width - SHELF_CANVAS_WIDTH * scale) / 2;
    layout->canvas_y = stack.top + (canvas_height - SHELF_CANVAS_HEIGHT * scale) / 2;
    float caption_top = stack.bottom - panel + 2;
    layout->caption = (GfxRect){ stack.left + 2, caption_top, stack.right - 2, caption_top + caption_height };
    layout->detail = (GfxRect){ stack.left + 2, layout->caption.bottom + 1, stack.right - 2, layout->caption.bottom + 1 + detail_height };

    // 列表：超出时右边有一条细滚动条（盖在内容上）。
    GfxRect list = layout->list;
    int count = shelf->model->count;
#ifdef LOOK_VARIANT_CARDS
    float grid_width = list.right - list.left - 2, viewport = list.bottom - list.top - 2;
    layout->columns = shelf_grid_columns_for(grid_width);
    int grid_rows = (count + layout->columns - 1) / layout->columns;
    layout->extent = shelf_grid_height(grid_rows);
    float maximum = layout->extent - viewport > 0 ? layout->extent - viewport : 0;
    if (shelf->scroll_offset > maximum) shelf->scroll_offset = maximum;
    if (shelf->scroll_offset < 0) shelf->scroll_offset = 0;
    // 只给平铺里生成出来的牌（看得见的几行，上下各多一行）取缩略图。
#else
    int rows = (int)floorf((list.bottom - list.top - 2) / SHELF_ROW_PITCH);
    if (rows < 1) rows = 1;
    int maximum_first = count - rows > 0 ? count - rows : 0;
    if (shelf->scroll_first > maximum_first) shelf->scroll_first = maximum_first;
    if (shelf->scroll_first < 0) shelf->scroll_first = 0;
    // 只给列表里显示出来的行取缩略图（多备一行，滚动时不用等）。
#endif
    bool list_shown = shelf->model->expanded || shelf->switching;
#ifdef LOOK_VARIANT_CARDS
    int from = -1, to = -1;
    if (list_shown)
    {
        // 看得见的几行，上下各多备一行（含两端）。
        shelf_visible_tiles(shelf, &from, &to);
    }
#else
    int from = list_shown ? shelf->scroll_first : -1;
    int to = list_shown ? shelf->scroll_first + rows : -1;
#endif
    if (from != shelf->model->visible_from || to != shelf->model->visible_to)
    {
        shelf->model->visible_from = from;
        shelf->model->visible_to = to;
        shelf_model_update_thumbnails(shelf->model);
    }
#ifdef LOOK_VARIANT_CARDS
    layout->scrollable = layout->extent > viewport + 0.5f;
#else
    layout->scrollable = count * SHELF_ROW_PITCH > list.bottom - list.top - 2;
#endif
    if (layout->scrollable)
    {
        layout->scroll_track = (GfxRect){ list.right - 1 - 1 - 12, list.top + 1 + 2, list.right - 1 - 1, list.bottom - 1 - 2 };
        float track = layout->scroll_track.bottom - layout->scroll_track.top;
#ifdef LOOK_VARIANT_CARDS
        float length = fmaxf(track * viewport / layout->extent, 16);
        float top = layout->scroll_track.top + (maximum > 0 ? (track - length) * shelf->scroll_offset / maximum : 0);
#else
        float length = fmaxf(track * rows / count, 16);
        float top = layout->scroll_track.top + (maximum_first > 0 ? (track - length) * shelf->scroll_first / maximum_first : 0);
#endif
        float thumb = shelf->scroll_hovered || shelf->scroll_dragging ? 6.0f : 3.0f;
        float center = (layout->scroll_track.left + layout->scroll_track.right) / 2;
        layout->scroll_thumb = (GfxRect){ center - thumb / 2, top, center + thumb / 2, top + length };
    }

    // 快速操作按钮均分宽度，各留 2 的外边距；按钮栏上方有一条分隔线。
    float cell = action_count > 0 ? (width - 16) / (float)action_count : 0;
    layout->action_divider = (GfxRect){ 10, height - 50, width - 10, height - 49 };
    for (int index = 0; index < SHELF_MAX_ACTIONS; index++)
    {
        if (index >= action_count)
        {
            layout->action_buttons[index] = layout->tiles[index] = (GfxRect){ 0, 0, 0, 0 };
            continue;
        }
        float cell_left = 8 + cell * (float)index, cell_right = 8 + cell * (float)(index + 1);
        layout->action_buttons[index] = (GfxRect){ cell_left + 2, height - 44, cell_right - 2, height - 8 };
        // 拖入时展开的卡片和按钮一一对齐，底边离窗口底部 7 + 3。
        layout->tiles[index] = (GfxRect){ cell_left + 2, height - 10 - SHELF_ACTION_TILE_HEIGHT, cell_right - 2, height - 10 };
    }
    // 放置区域在卡片那一行上面；那一行收起时和按钮栏一样高，展开时长高。
    float tiles_height = action_count > 0 ? (float)shelf->tiles_height.value : 0;
    layout->tiles_host = (GfxRect){ 0, height - tiles_height, width, height };
    layout->drop_zone = (GfxRect){ 10, SHELF_HEADER_HEIGHT + 2, width - 10, height - tiles_height - 6 };
#ifdef LOOK_VARIANT_CARDS
    // 放置区域里：上面是摊开的牌（176 × 108 的画布按比例缩进去，最高 104），下面一行字。
    GfxRect zone = layout->drop_zone;
    float zone_text = shelf_line_height(12, 600);
    float row = zone.bottom - zone.top - 4 - zone_text;
    float box = row < 104 ? row : 104;
    if (box < 1) box = 1;
    float zone_width = zone.right - zone.left;
    float drop_scale = fminf(zone_width / 176, box / 108);
    layout->drop_canvas_scale = drop_scale > 0.01f ? drop_scale : 0.01f;
    layout->drop_canvas_x = zone.left + (zone_width - 176 * layout->drop_canvas_scale) / 2;
    layout->drop_canvas_y = zone.top + (row - box) / 2 + (box - 108 * layout->drop_canvas_scale) / 2;
    layout->drop_text = (GfxRect){ zone.left, zone.top + row + 4, zone.right, zone.top + row + 4 + zone_text };
#endif

    // 提示：底部，外边距 10，内边距 10,7,12,8；带按钮时右边是按钮。
    if (shelf->toast_text)
    {
        float inner_width = width - 20 - 2 - 22;
        float glyph = shelf_text_width(shelf->toast_success ? GLYPH_COMPLETED : GLYPH_WARNING, 14, 400, GFX_FONT_ICON) + 8;
        // 按钮：内边距 8，左外边距 8，右外边距 -6（伸进提示的内边距里）。
        float button = 0, action = 0;
        if (shelf->toast_action != TOAST_ACTION_NONE)
        {
            button = shelf_text_width(S(shelf->toast_action == TOAST_ACTION_UNDO ? STR_Undo : STR_MenuConvertToFile), 12, 600, GFX_FONT_TEXT) + 16;
            action = 8 + button - 6;
        }
        GfxTextStyle wrap = { GFX_FONT_TEXT, 12, 400, true, false, GFX_ALIGN_START, GFX_ALIGN_START };
        float text_area = inner_width - glyph - action;
        GfxSize text = gfx_text_measure(shelf->toast_text, &wrap, text_area > 10 ? text_area : 10);
        float content = fmaxf(text.height, shelf_line_height(14, 400));
        if (action > 0) content = fmaxf(content, 24 - 7);
        float toast_height = content + 15 + 2;
        layout->toast = (GfxRect){ 10, height - 10 - toast_height, width - 10, height - 10 };
        float inner_left = layout->toast.left + 1 + 10, inner_top = layout->toast.top + 1 + 7;
        layout->toast_glyph = shelf_rect_of(inner_left, inner_top + 1, glyph - 8, shelf_line_height(14, 400));
        layout->toast_text = shelf_rect_of(inner_left + glyph, inner_top + (content - text.height) / 2, text_area, text.height);
        float inner_right = layout->toast.right - 1 - 12;
        layout->toast_action = button > 0 ? shelf_rect_of(inner_right + 6 - button, inner_top + (content - 24) / 2, button, 24) : (GfxRect){ 0, 0, 0, 0 };
    }
}

// ───────────── 视图 ─────────────

static void refresh_posted(void *data)
{
    ShelfWindow *shelf = data;
    shelf->refresh_posted = false;
    if (!shelf->closing && !shelf->closed) shelf_refresh_views(shelf);
    shelf_window_release(shelf);
}

void shelf_request_refresh(ShelfWindow *shelf)
{
    // 一次操作里连续的变化只刷新一次，排在这次操作做完之后。
    if (shelf->refresh_posted) return;
    shelf->refresh_posted = true;
    task_post(refresh_posted, shelf_window_retain(shelf));
}

/// 数量或视图变化后更新界面：只是切换了堆叠和列表时播放切换动画，刚拖入文件时播放落下的动画。
/// 列表里多了几行、窗口要变高时先平滑长高再落下；列表里移除之后先补位，窗口等补完再变小。
void shelf_refresh_views(ShelfWindow *shelf)
{
    bool switched = !shelf->count_changed && shelf->shown_expanded != shelf->model->expanded;
#ifndef LOOK_VARIANT_CARDS
    bool counted = shelf->count_changed;
#endif
    bool landing = shelf->landing, returning = shelf->returning;
#ifdef LOOK_VARIANT_CARDS
    // 松手时摊开的牌，先记下来：下面收尾时会把它清掉。
    bool fan = shelf->fan_held;
#endif
    float landing_x = shelf->landing_x, landing_y = shelf->landing_y;
    int landing_added = shelf->landing_added;
    ShelfItem **returning_items = shelf->returning_items;
    int returning_count = shelf->returning_count;
    shelf->count_changed = false;
    shelf->landing = shelf->returning = false;
    shelf->returning_items = NULL;
    shelf->returning_count = 0;
    shelf_finish_motion(shelf);
    shelf->shown_expanded = shelf->model->expanded;
    bool animate = anim_enabled() && IsWindowVisible(shelf->handle) && shelf->model->count > 0;
    if (switched && animate)
    {
        shelf_animate_view_switch(shelf);
    }
    else
    {
#ifdef LOOK_VARIANT_CARDS
        shelf_update_window_size(shelf);
        // 不播动画时平铺开也停在最新的项目上。
#else
        double lead = counted && animate && shelf->model->expanded && !shelf->hold_size ? shelf_grow_for_rows(shelf) : 0;
        if (!shelf->hold_size && lead == 0) shelf_update_window_size(shelf);
        // 不播动画时展开列表也停在最新的项目上。
#endif
        if (switched && shelf->model->expanded && shelf->model->count > 0) shelf_scroll_into_view(shelf, shelf->model->count - 1);
#ifdef LOOK_VARIANT_CARDS
        if (returning && animate) shelf_play_return(shelf, shelf->returning_x, shelf->returning_y, returning_items, returning_count);
#else
        bool played = false;
        if (returning && animate) played |= shelf_play_return(shelf, shelf->returning_x, shelf->returning_y, returning_items, returning_count, lead);
#endif
        if (landing && animate)
#ifdef LOOK_VARIANT_CARDS
        {
            if (shelf->model->expanded) shelf_play_grid_landing(shelf, landing_x, landing_y, landing_added);
            else
            {
                shelf->fan_held = fan;
                shelf_play_landing(shelf, landing_x, landing_y, landing_added);
            }
        }
#else
            played |= shelf->model->expanded ? shelf_play_list_landing(shelf, landing_x, landing_y, landing_added, lead)
                                             : shelf_play_landing(shelf, landing_x, landing_y, landing_added);
        if (lead > 0 && !played) shelf_finish_after(shelf, lead);
#endif
    }
    for (int index = 0; index < returning_count; index++) shelf_item_release(returning_items[index]);
    free(returning_items);
    shelf_update_title(shelf);
    shelf_repaint(shelf);
}

static void on_model_changed(void *context, ShelfChange change)
{
    ShelfWindow *shelf = context;
    if (shelf->closed) return;
    switch (change)
    {
        case SHELF_CHANGED_ITEMS:
#ifdef LOOK_VARIANT_CARDS
            shelf->rank_suit_known = false;
#endif
            clear_stack_summary(shelf);
            if (shelf->model->count != shelf->shown_count)
            {
#ifdef LOOK_VARIANT_CARDS
                // 新牌落进这一摞：牌角的数字从下面滚上来。
                if (shelf->model->count > shelf->shown_count && shelf->shown_count > 0) shelf_roll_rank(shelf);
#endif
                shelf->shown_count = shelf->model->count;
                shelf->count_changed = true;
                shelf_request_refresh(shelf);
            }
            shelf_update_title(shelf);
            shelf_repaint(shelf);
            break;
        case SHELF_CHANGED_EXPANDED:
            shelf_request_refresh(shelf);
            break;
        case SHELF_CHANGED_BUSY:
            shelf_update_busy_overlay(shelf, false);
            break;
        case SHELF_CHANGED_ITEM_TEXT:
#ifdef LOOK_VARIANT_CARDS
            // 移动、改名或转成文件以后扩展名可能变了。
            shelf->rank_suit_known = false;
#endif
            clear_stack_summary(shelf);
            shelf_repaint(shelf);
            break;
    }
}

// ───────────── 窗口 ─────────────

static void on_activated(UiWindow *window, void *context, bool active)
{
    (void)window;
    ShelfWindow *shelf = context;
    if (active && !shelf->closing) shelf_model_refresh_availability_in_background(shelf->model);
}

static void on_moved(UiWindow *window, void *context)
{
    (void)window;
    ShelfWindow *shelf = context;
    // 换到了缩放比例不同的显示器：缩略图按新的像素大小取。
    update_thumbnail_size(shelf);
}

static void on_closed(UiWindow *window, void *context)
{
    (void)window;
    ShelfWindow *shelf = context;
    shelf->closed = true;
    shelf->closing = true;
    shelf_cancel_card(shelf);
    shelf_stop_slide(shelf);
    shelf_stop_pop(shelf);
    shelf_merge_forget(shelf);
    // 没播完的动画在这里收尾：待移除的项目先移除，不会记进最近关闭。
    shelf_finish_motion(shelf);
    shelf_revoke_drop_target(shelf);
    // 关闭时仍有文件的置物架记入最近关闭，误关或退出后可以恢复；合并到别的置物架之后关闭的不记，项目已经在那边了。
    if (!shelf->merged_away && shelf->model->count > 0) shelf_history_record(shelf->model->items, shelf->model->count);
    power_hold(shelf, false);
    shelf->ui = NULL;
    shelf->handle = NULL;
    shelf_removal_free(shelf->undo);
    shelf->undo = NULL;
    shelf_model_release_resources(shelf->model);
    shelves_on_closed(shelf);
    shelf_window_release(shelf);
}

ShelfWindow *shelf_window_create(void)
{
    ShelfWindow *shelf = xcalloc(1, sizeof(ShelfWindow));
    shelf->references = 1;
    ShelfModelEvents events = { shelf, on_model_changed, shelf_notify, shelf_remove_visibly, shelf_produced };
    shelf->model = shelf_model_new(&events);
    const AppSettings *settings = app_settings();
    shelf->actions = quick_actions_visible(settings);
    shelf->hovered_action = -1;
    anim_set(&shelf->bar_opacity, 1);
    anim_set(&shelf->frame_opacity, 1);
    anim_set(&shelf->tiles_opacity, 0);
    anim_set(&shelf->tiles_height, shelf->actions.count > 0 ? SHELF_ACTION_BAR_HEIGHT : 0);
    shelf->size = settings->quick_shelf_size;
    shelf->move_mode = settings->drag_out_mode == TRANSFER_MOVE;
    shelf->surface_opacity = 0;
    shelf->surface_scale = 1;
    shelf->origin_x = 0.5f;
    shelf->origin_y = 0.4f;
    shelf->mark_opacity = 1;
#ifdef LOOK_VARIANT_CARDS
    shelf->rank_opacity = 1;
#else
    shelf->badge_scale = 1;
#endif
    shelf->hovered_row = shelf->pressed_row = -1;
    shelf->selection_anchor = -1;
    shelf->drop_zone_text = STR_ReleaseToHold;
    anim_set(&shelf->overlay_opacity, 0);
    anim_set(&shelf->body_opacity, 1);
    anim_set(&shelf->busy_opacity, 0);
    anim_set(&shelf->toast_opacity, 0);
    anim_set(&shelf->toast_offset, 0);
    anim_set(&shelf->merge_dim, 1);
    shelf_reset_merge_charge(shelf);

    float width, height;
    shelf_target_size(shelf, false, &width, &height);
    UiWindowOptions options = {0};
    options.title = L"DropShelf";
    options.width = width;
    options.height = height;
    options.frameless = true;
#ifdef LOOK_VARIANT_CARDS
    options.backdrop = UI_BACKDROP_NONE;
#else
    options.backdrop = UI_BACKDROP_ACRYLIC;
#endif
    options.background = THEME_SHELF_SOLID;
    options.context = shelf;
    options.paint = shelf_paint;
    options.mouse = shelf_on_mouse;
    options.key = shelf_on_key;
    options.activated = on_activated;
    options.moved = on_moved;
    options.closed = on_closed;
    options.animate = shelf_animate;
    options.cursor = shelf_on_cursor;
    options.message = shelf_on_message;
    shelf->ui = ui_window_create(&options);
    if (!shelf->ui)
    {
        shelf_model_release(shelf->model);
        quick_action_list_free(&shelf->actions);
        free(shelf);
        return NULL;
    }
    shelf->handle = ui_window_handle(shelf->ui);
    // 窗口关闭时 on_closed 放掉这一份。
    shelf_window_retain(shelf);
    shelf_rebuild_actions(shelf);
    shelf_update_title(shelf);
    update_thumbnail_size(shelf);
    shelf_register_drop_target(shelf);
    ui_window_set_topmost(shelf->ui, true);
    return shelf;
}

ShelfWindow *shelf_window_retain(ShelfWindow *shelf)
{
    if (shelf) shelf->references++;
    return shelf;
}

void shelf_window_release(ShelfWindow *shelf)
{
    if (!shelf || --shelf->references > 0) return;
    shelf_model_release(shelf->model);
    clear_stack_summary(shelf);
    free(shelf->tweens);
    free(shelf->removing);
    free(shelf->rows_before);
#ifdef LOOK_VARIANT_CARDS
    free(shelf->rows_before_x);
#endif
    free(shelf->rows_before_y);
    free(shelf->toast_text);
    for (int index = 0; index < shelf->convert_count; index++) shelf_item_release(shelf->convert_items[index]);
    free(shelf->convert_items);
    // 撤销后还没来得及播放飞回动画就关闭了：刷新被跳过，放回的项目的引用在这里放掉。
    for (int index = 0; index < shelf->returning_count; index++) shelf_item_release(shelf->returning_items[index]);
    free(shelf->returning_items);
    for (int index = 0; index < shelf->menu_count; index++) shelf_item_release(shelf->menu_items[index]);
    free(shelf->menu_items);
    for (int index = 0; index < shelf->dimmed_count; index++) shelf_item_release(shelf->dimmed[index]);
    free(shelf->dimmed);
    for (int index = 0; index < SHELF_MAX_ACTIONS; index++)
    {
        free(shelf->action_labels[index]);
        free(shelf->action_tooltips[index]);
    }
    quick_action_list_free(&shelf->actions);
    drop_helper_destroy(shelf->drop_helper);
    free(shelf);
}

bool shelf_window_is_visible(const ShelfWindow *shelf)
{
    return shelf->handle && IsWindowVisible(shelf->handle);
}

bool shelf_window_is_reusable(const ShelfWindow *shelf)
{
    const ShelfModel *model = shelf->model;
    return !shelf->closing && !shelf->closed && model->count == 0 && !model->busy && !model->has_interacted;
}

bool shelf_window_is_animating(const ShelfWindow *shelf)
{
#ifdef LOOK_VARIANT_CARDS
    // 有牌在替它出场或离场时也算。
    return shelf->sliding || shelf->popping || shelf->card || shelf->motion_running || shelf->receiving > 0 || shelf->closing;
#else
    return shelf->sliding || shelf->popping || shelf->motion_running || shelf->receiving > 0 || shelf->closing;
#endif
}

// ───────────── 由牌替置物架出场 ─────────────

/// 出场的牌落定：置物架现身。
static void entrance_finished(void *context)
{
    ShelfWindow *shelf = context;
    shelf->card = NULL;
    window_set_cloaked(shelf->handle, false);
}

void shelf_deal_in(ShelfWindow *shelf, POINT cursor)
{
    bool from_peek = shelf->from_peek;
    shelf->from_peek = false;
    shelf->surface_opacity = 1;
    shelf->surface_scale = 1;
    shelf->surface_dx = shelf->surface_dy = 0;
    RECT rect = {0};
    GfxImage *face = shelf->handle && GetWindowRect(shelf->handle, &rect) ? ui_window_capture(shelf->ui) : NULL;
    ShelfCard *card = face ? shelf_card_rent() : NULL;
    if (!card)
    {
        gfx_image_release(face);
        window_set_cloaked(shelf->handle, false);
        shelf_repaint(shelf);
        return;
    }
    double scale = shelf_scale(shelf);
    RECT work = work_area_at((POINT){ rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 }, NULL);
    double width = rect.right - rect.left, height = rect.bottom - rect.top;
    CardMotion motion;
    if (from_peek)
    {
        const ShelfPopOrigin *origin = &shelf->peek_origin;
        motion = card_motion_peek_pop(rect, scale, origin->center, origin->width, origin->height, origin->angle, shelf->edge);
    }
    else if (shelf->entrance == ENTRANCE_SHAKE)
    {
        // 光标离置物架中心太远时按靠近中心的位置算，牌不会从老远飞过来。
        double dx = fmax(-0.35 * width, fmin(0.35 * width, cursor.x - (rect.left + width / 2))) / scale;
        double dy = fmax(-0.35 * height, fmin(0.35 * height, cursor.y - (rect.top + height / 2))) / scale;
        motion = card_motion_shake_in(dx, dy);
    }
    else motion = card_motion_edge_in(shelf->edge, rect, work, scale);
    shelf->flipped_on_horizontal_axis = motion.horizontal_axis;
    shelf->card = card;
    shelf_card_play(card, rect, work, scale, face, &motion, NULL, entrance_finished, shelf);
    gfx_image_release(face);
}

void shelf_finish_entrance(ShelfWindow *shelf)
{
    ShelfCard *card = shelf->card;
    if (!card) return;
    shelf->card = NULL;
    window_set_cloaked(shelf->handle, false);
    shelf_card_cancel(card);
}

void shelf_cancel_card(ShelfWindow *shelf)
{
    shelf_card_cancel(shelf->card);
    shelf->card = NULL;
}

/// 以 center 为中心、完整落在那个显示器的工作区内；跨缩放比例不同的显示器时按实际大小再放一次。
static void center_on(ShelfWindow *shelf, POINT center)
{
    float width, height;
    shelf_target_size(shelf, shelf->model->expanded, &width, &height);
    double scale;
    RECT work = work_area_at(center, &scale);
    int pixel_width = (int)lround(width * scale), pixel_height = (int)lround(height * scale);
    POINT place = probe_place(center, work, pixel_width, pixel_height);
    SetWindowPos(shelf->handle, NULL, place.x, place.y, pixel_width, pixel_height, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT actual;
    if (GetWindowRect(shelf->handle, &actual) && (actual.right - actual.left != pixel_width || actual.bottom - actual.top != pixel_height))
    {
        place = probe_place(center, work, actual.right - actual.left, actual.bottom - actual.top);
        SetWindowPos(shelf->handle, NULL, place.x, place.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void shelf_window_show_at(ShelfWindow *shelf, POINT center, bool activate, ShelfEntranceKind entrance, ScreenEdge edge)
{
    bool was_visible = shelf_window_is_visible(shelf);
    // 复用正在出场的置物架时先让它落定，否则下一帧会把窗口拉回原来的位置。
    if (was_visible)
    {
        shelf_stop_slide(shelf);
        shelf_finish_entrance(shelf);
    }
    // 由一张牌替置物架出场：置物架先隐身，照常显示和绘制，牌落定时直接现身。纸牌晃动和边缘呼出时都这样，
    // 简洁只在从观察文件夹的角点开时（牌从角那里转出来）。
#ifdef LOOK_VARIANT_CARDS
    bool dealt = !was_visible && anim_enabled() && entrance != ENTRANCE_PLAIN;
#else
    bool dealt = !was_visible && anim_enabled() && shelf->from_peek;
#endif
    if (!dealt) shelf->from_peek = false;
    if (dealt) window_set_cloaked(shelf->handle, true);
    // 刚放进去的文件还没排好时先排好，截下的牌面和窗口大小都是放好以后的。
    if (dealt && shelf->refresh_posted) shelf_refresh_views(shelf);
    RECT before = {0};
    if (was_visible) GetWindowRect(shelf->handle, &before);
    center_on(shelf, center);
    update_thumbnail_size(shelf);
    // 复用的空置物架挪到了别处：和新打开的一样播放出场动画；本来就在光标附近时不重播，免得在原处闪一下。
    RECT now;
    bool moved = was_visible && GetWindowRect(shelf->handle, &now) &&
                 abs(now.left - before.left) + abs(now.top - before.top) > 24 * shelf_scale(shelf);
    if (!was_visible || moved)
    {
        shelf->entrance = entrance;
        shelf->edge = edge;
        // 先把内容摆到出场的起点再显示，第一帧就不会闪出完整的内容。
        shelf->surface_opacity = dealt ? 1.0f : 0.0f;
    }
    if (!was_visible)
    {
        power_hold(shelf, true);
        ui_window_set_topmost(shelf->ui, shelf->model->pinned);
        ui_window_show_with(shelf->ui, activate);
        center_on(shelf, center);
    }
    else if (activate) SetForegroundWindow(shelf->handle);
    if (dealt) shelf_deal_in(shelf, center);
#ifdef LOOK_VARIANT_CARDS
    else if (!was_visible || moved) shelf_animate_in(shelf);
#else
    else if (!was_visible || moved) shelf_animate_in(shelf, center);
#endif
    shelf_repaint(shelf);
}

void shelf_window_close_now(ShelfWindow *shelf)
{
    if (shelf->closed || !shelf->ui) return;
    shelf->closing = true;
    shelf_revoke_drop_target(shelf);
    ui_window_close(shelf->ui);
}

static void close_posted(void *data)
{
    ShelfWindow *shelf = data;
    shelf_window_close_now(shelf);
    shelf_window_release(shelf);
}

void shelf_window_close_animated(ShelfWindow *shelf)
{
    if (shelf->closing || shelf->closed) return;
    shelf->closing = true;
    shelf_finish_entrance(shelf);
#ifdef LOOK_VARIANT_CARDS
    shelf_stop_slide(shelf);
    shelf_stop_pop(shelf);
    // 发牌、移除、补位和窗口大小的动画停在这一帧：截下的牌面和屏幕上一样。没做完的收尾（比如待移除的项目）在窗口关闭时补上。
#else
    // 内容里的动画停在这一帧；没做完的收尾（比如待移除的项目）在窗口关闭时补上。
#endif
    shelf_stop_motion(shelf);
#ifdef LOOK_VARIANT_CARDS
    if (!anim_enabled() || !shelf_window_is_visible(shelf) || !shelf_play_exit(shelf)) task_post(close_posted, shelf_window_retain(shelf));
#else
    if (!anim_enabled() || !shelf_window_is_visible(shelf))
    {
        task_post(close_posted, shelf_window_retain(shelf));
        return;
    }
    shelf_start_leave(shelf);
#endif
}

void shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths)
{
    shelf_add_paths(shelf, paths);
}

// ───────────── 换风格 ─────────────

void shelf_window_leave_look(ShelfWindow *shelf)
{
    // 按原来的风格把正在播的动画都走到终点、收起提示和拖入时的放置区域，切换时不留下半截。
    if (shelf->card) shelf_finish_entrance(shelf);
    shelf_finish_motion(shelf);
    shelf_hide_toast(shelf);
    shelf_end_drag(shelf);
}

void shelf_window_adopt_look(ShelfWindow *shelf)
{
    // 模型、窗口和拖放的回调都换成这种风格的一份。
    ShelfModelEvents events = { shelf, on_model_changed, shelf_notify, shelf_remove_visibly, shelf_produced };
    shelf->model->events = events;
    UiWindowOptions handlers = { 0 };
    handlers.paint = shelf_paint;
    handlers.mouse = shelf_on_mouse;
    handlers.key = shelf_on_key;
    handlers.activated = on_activated;
    handlers.moved = on_moved;
    handlers.closed = on_closed;
    handlers.animate = shelf_animate;
    handlers.cursor = shelf_on_cursor;
    handlers.message = shelf_on_message;
    ui_window_set_handlers(shelf->ui, &handlers);
    shelf_revoke_drop_target(shelf);
    shelf_register_drop_target(shelf);
#ifdef LOOK_VARIANT_CARDS
    ui_window_set_backdrop(shelf->ui, UI_BACKDROP_NONE);
    shelf->rank_opacity = 1;
    shelf->rank_shift = 0;
    shelf->rank_hidden = shelf->rank_rolling = false;
    shelf->rank_suit_known = false;
    shelf->fan_held = false;
    anim_set(&shelf->fan, 0);
#else
    ui_window_set_backdrop(shelf->ui, UI_BACKDROP_ACRYLIC);
    shelf->badge_scale = 1;
    shelf->badge_popping = false;
    shelf->mark_override = false;
    shelf->hold_size = false;
#endif
    // 这种风格的视图从静止的样子开始：没有滚动、没有固定大小，项目都在原位。
    shelf->mark_opacity = 1;
    shelf->scroll_first = 0;
    shelf->scroll_offset = 0;
    shelf->stack_pinned = shelf->list_pinned = false;
    shelf->switching = false;
    for (int card = 0; card < 3; card++) shelf->card_posed[card] = false;
    for (int index = 0; index < shelf->model->count; index++)
    {
        ShelfItem *item = shelf->model->items[index];
        shelf_item_motion_reset(item);
        anim_set(&item->lift, 0);
        anim_set(&item->dim, 1);
    }
    shelf->shown_expanded = shelf->model->expanded;
    update_thumbnail_size(shelf);
    shelf_refresh_views(shelf);
    shelf_update_window_size(shelf);
    shelf_repaint(shelf);
}
