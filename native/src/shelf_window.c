#include "shelf_window_internal.h"
#include "shelf_card.h"
#include "card_face.h"
#include "log.h"
#include "shelf_history.h"
#include "power.h"
#include "probe.h"
#include "task.h"
#include "theme.h"

#include <math.h>
#include <shellscalingapi.h>
#include <stdlib.h>
#include <string.h>

// 图标字形
#define GLYPH_PIN L"\xE718"
#define GLYPH_PINNED L"\xE840"
#define GLYPH_MORE L"\xE712"
#define GLYPH_CLOSE L"\xE711"
#define GLYPH_DOWNLOAD L"\xE896"
#define GLYPH_COLLAPSE L"\xE81E"
#define GLYPH_SPREAD L"\xF0E2"
#define GLYPH_COPY L"\xE8C8"
#define GLYPH_MOVE L"\xE7C2"
#define GLYPH_COMPLETED L"\xE930"
#define GLYPH_WARNING L"\xE7BA"

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

void shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height)
{
    // 平铺时窗口按一行的牌数定宽：内容区左右各留 10，平铺区域四周还有 1，少算这 2 就会少放一列。
    *width = expanded ? shelf_grid_columns(shelf->size) * SHELF_SLOT_WIDTH + 22 : resolve_width(shelf->size);
    // 平铺最多显示两行，再多就滚动。
    int rows = shelf_grid_rows_for(shelf->model->count, *width - 22);
    rows = rows < 1 ? 1 : rows > 2 ? 2 : rows;
    float body = expanded ? shelf_grid_height(rows) : resolve_stack_height(shelf->size) + SHELF_CAPTION_HEIGHT;
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
    // 牌面去掉内边距后缩略图的位置是 50 × 50；堆叠画布 160×116 随置物架大小缩放，两侧各留 10 的边距。平铺里的牌不缩放，按堆叠里的取。
    float stack_scale = fminf((resolve_width(shelf->size) - 20) / SHELF_CANVAS_WIDTH, resolve_stack_height(shelf->size) / SHELF_CANVAS_HEIGHT);
    shelf->model->thumbnail_size = (int)ceil(50 * fmaxf(stack_scale, 1) * shelf_scale(shelf));
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

static GfxTextStyle text_style(float size, int weight, GfxAlign align)
{
    return (GfxTextStyle){ GFX_FONT_TEXT, size, weight, false, true, align, GFX_ALIGN_CENTER };
}

static GfxTextStyle glyph_style(float size)
{
    return (GfxTextStyle){ GFX_FONT_ICON, size, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
}

/// 一行文字的高度（按字号和粗细缓存）。
static float line_height(float size, int weight)
{
    typedef struct LineHeight { float size; int weight; float height; } LineHeight;
    static LineHeight cache[8];
    static int count;
    for (int index = 0; index < count; index++)
        if (cache[index].size == size && cache[index].weight == weight) return cache[index].height;
    GfxTextStyle style = text_style(size, weight, GFX_ALIGN_START);
    style.ellipsis = false;
    float height = gfx_text_measure(L"Ag", &style, 0).height;
    if (height <= 0) return size * 1.33f;
    if (count < 8) cache[count++] = (LineHeight){ size, weight, height };
    return height;
}

static float text_width(const wchar_t *text, float size, int weight, GfxFont font)
{
    GfxTextStyle style = { font, size, weight, false, false, GFX_ALIGN_START, GFX_ALIGN_START };
    return gfx_text_measure(text, &style, 0).width;
}

static GfxRect rect_of(float left, float top, float width, float height)
{
    return (GfxRect){ left, top, left + width, top + height };
}

static bool contains(GfxRect rect, float x, float y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

// ───────────── 排版 ─────────────

CardPose shelf_card_home(int card)
{
    // XAML 里三张竖版牌的位置：最上面一张不转，第二张转 7° 往右挪，最下面一张转 -10° 往左挪；中心 = 左上角 + 牌面的一半 + 平移。
    static const CardPose homes[3] = {
        { 48 + 32, 12 + 43, 0, 1, 1 },
        { 48 + 32 + 11, 14 + 43 + 1, 7, 1, 1 },
        { 48 + 32 - 12, 16 + 43 + 2, -10, 1, 1 },
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

int shelf_grid_columns_now(ShelfWindow *shelf)
{
    return shelf->layout.columns > 0 ? shelf->layout.columns : 1;
}

void shelf_set_scroll_offset(ShelfWindow *shelf, float offset)
{
    float viewport = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    float maximum = shelf->layout.extent - viewport;
    if (offset > maximum) offset = maximum;
    if (offset < 0) offset = 0;
    if (offset == shelf->scroll_offset) return;
    shelf->scroll_offset = offset;
    shelf_layout(shelf);
    shelf_repaint(shelf);
}

/// 平铺要显示某一张而它不在可见范围里：滚到它所在的那一行。
void shelf_scroll_into_view(ShelfWindow *shelf, int index)
{
    if (index < 0 || index >= shelf->model->count) return;
    shelf_layout(shelf);
    float top = (float)(index / shelf_grid_columns_now(shelf)) * SHELF_ROW_PITCH;
    float bottom = top + SHELF_SLOT_HEIGHT + 2 * SHELF_GRID_INSET;
    float viewport = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    if (top < shelf->scroll_offset) shelf_set_scroll_offset(shelf, top);
    else if (bottom > shelf->scroll_offset + viewport) shelf_set_scroll_offset(shelf, bottom - viewport);
}

GfxRect shelf_tile_rect(ShelfWindow *shelf, int index)
{
    const ShelfLayout *layout = &shelf->layout;
    int columns = shelf_grid_columns_now(shelf);
    float width = layout->list.right - layout->list.left - 2;
    // 多出的宽度平均分给列间距。
    float step = columns > 1 ? (width - SHELF_SLOT_WIDTH) / (columns - 1) : 0;
    int row = index / columns, column = index % columns;
    float left = layout->list.left + 1 + column * step;
    float top = layout->list.top + 1 + SHELF_GRID_INSET + row * SHELF_ROW_PITCH - shelf->scroll_offset;
    return rect_of(left, top, SHELF_SLOT_WIDTH, SHELF_SLOT_HEIGHT);
}

GfxRect shelf_tile_card_rect(ShelfWindow *shelf, int index)
{
    GfxRect slot = shelf_tile_rect(shelf, index);
    float left = slot.left + (SHELF_SLOT_WIDTH - SHELF_CARD_WIDTH) / 2;
    return rect_of(left, slot.top, SHELF_CARD_WIDTH, SHELF_CARD_HEIGHT);
}

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
    return rect_of(card.right - 11, card.top - 7, 18, 18);
}

void shelf_layout(ShelfWindow *shelf)
{
    ShelfLayout *layout = &shelf->layout;
    GfxSize size = ui_window_size(shelf->ui);
    float width = size.width, height = size.height;
    layout->width = width;
    layout->height = height;

    // 标题栏：外边距 4，左边是置顶和移动/复制，右边是切换视图、更多和关闭，标题在中间。
    layout->pin = rect_of(4, 7, 28, 30);
    const wchar_t *mode_text = S(shelf->move_mode ? STR_TransferMove : STR_TransferCopy);
    float chip = 1 + 7 + text_width(shelf->move_mode ? GLYPH_MOVE : GLYPH_COPY, 12, 400, GFX_FONT_ICON) + 5 + text_width(mode_text, 12, 400, GFX_FONT_TEXT) + 8 + 1;
    layout->mode = rect_of(4 + 28 + 2, 10, ceilf(chip), 24);
    layout->close = rect_of(width - 4 - 28, 7, 28, 30);
    layout->more = rect_of(layout->close.left - 28, 7, 28, 30);
    bool has_items = shelf->model->count > 0;
    layout->view = has_items ? rect_of(layout->more.left - 28, 7, 28, 30) : (GfxRect){ 0, 0, 0, 0 };
    float right = has_items ? layout->view.left : layout->more.left;
    layout->title = (GfxRect){ layout->mode.right + 4, 4, right - 4, 40 };

    // 有快速操作时内容区下面是按钮栏（外边距 8,6,8,8，按钮高 36，上面还有 2）。
    int action_count = shelf->actions.count;
    layout->body = (GfxRect){ 10, SHELF_HEADER_HEIGHT + 2, width - 10, action_count > 0 ? height - 52 : height - 10 };
    layout->stack = layout->body;
    if (shelf->stack_pinned) layout->stack = rect_of(layout->body.left, layout->body.top, shelf->stack_pin.width, shelf->stack_pin.height);
    layout->list = layout->body;
    if (shelf->list_pinned) layout->list = rect_of(layout->body.left, layout->body.top, shelf->list_pin.width, shelf->list_pin.height);

    // 堆叠：下面两行文字（外边距 2,2,2,4），上面是按比例缩放的 160×116 画布。
    float caption_height = line_height(13, 600), detail_height = line_height(11, 400);
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

    // 平铺：固定大小的格子从左到右、从上到下排开，列数由宽度决定；超出时右边有一条细滚动条（盖在内容上）。
    GfxRect list = layout->list;
    int count = shelf->model->count;
    float grid_width = list.right - list.left - 2, viewport = list.bottom - list.top - 2;
    layout->columns = shelf_grid_columns_for(grid_width);
    int grid_rows = (count + layout->columns - 1) / layout->columns;
    layout->extent = shelf_grid_height(grid_rows);
    float maximum = layout->extent - viewport > 0 ? layout->extent - viewport : 0;
    if (shelf->scroll_offset > maximum) shelf->scroll_offset = maximum;
    if (shelf->scroll_offset < 0) shelf->scroll_offset = 0;
    // 只给平铺里生成出来的牌（看得见的几行，上下各多一行）取缩略图。
    bool list_shown = shelf->model->expanded || shelf->switching;
    int from = -1, to = -1;
    if (list_shown)
    {
        // 看得见的几行，上下各多备一行（含两端）。
        shelf_visible_tiles(shelf, &from, &to);
    }
    if (from != shelf->model->visible_from || to != shelf->model->visible_to)
    {
        shelf->model->visible_from = from;
        shelf->model->visible_to = to;
        shelf_model_update_thumbnails(shelf->model);
    }
    layout->scrollable = layout->extent > viewport + 0.5f;
    if (layout->scrollable)
    {
        layout->scroll_track = (GfxRect){ list.right - 1 - 1 - 12, list.top + 1 + 2, list.right - 1 - 1, list.bottom - 1 - 2 };
        float track = layout->scroll_track.bottom - layout->scroll_track.top;
        float length = fmaxf(track * viewport / layout->extent, 16);
        float top = layout->scroll_track.top + (maximum > 0 ? (track - length) * shelf->scroll_offset / maximum : 0);
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
    // 放置区域里：上面是摊开的牌（176 × 108 的画布按比例缩进去，最高 104），下面一行字。
    GfxRect zone = layout->drop_zone;
    float zone_text = line_height(12, 600);
    float row = zone.bottom - zone.top - 4 - zone_text;
    float box = row < 104 ? row : 104;
    if (box < 1) box = 1;
    float zone_width = zone.right - zone.left;
    float drop_scale = fminf(zone_width / 176, box / 108);
    layout->drop_canvas_scale = drop_scale > 0.01f ? drop_scale : 0.01f;
    layout->drop_canvas_x = zone.left + (zone_width - 176 * layout->drop_canvas_scale) / 2;
    layout->drop_canvas_y = zone.top + (row - box) / 2 + (box - 108 * layout->drop_canvas_scale) / 2;
    layout->drop_text = (GfxRect){ zone.left, zone.top + row + 4, zone.right, zone.top + row + 4 + zone_text };

    // 提示：底部，外边距 10，内边距 10,7,12,8；带按钮时右边是按钮。
    if (shelf->toast_text)
    {
        float inner_width = width - 20 - 2 - 22;
        float glyph = text_width(shelf->toast_success ? GLYPH_COMPLETED : GLYPH_WARNING, 14, 400, GFX_FONT_ICON) + 8;
        // 按钮：内边距 8，左外边距 8，右外边距 -6（伸进提示的内边距里）。
        float button = 0, action = 0;
        if (shelf->toast_action != TOAST_ACTION_NONE)
        {
            button = text_width(S(shelf->toast_action == TOAST_ACTION_UNDO ? STR_Undo : STR_MenuConvertToFile), 12, 600, GFX_FONT_TEXT) + 16;
            action = 8 + button - 6;
        }
        GfxTextStyle wrap = { GFX_FONT_TEXT, 12, 400, true, false, GFX_ALIGN_START, GFX_ALIGN_START };
        float text_area = inner_width - glyph - action;
        GfxSize text = gfx_text_measure(shelf->toast_text, &wrap, text_area > 10 ? text_area : 10);
        float content = fmaxf(text.height, line_height(14, 400));
        if (action > 0) content = fmaxf(content, 24 - 7);
        float toast_height = content + 15 + 2;
        layout->toast = (GfxRect){ 10, height - 10 - toast_height, width - 10, height - 10 };
        float inner_left = layout->toast.left + 1 + 10, inner_top = layout->toast.top + 1 + 7;
        layout->toast_glyph = rect_of(inner_left, inner_top + 1, glyph - 8, line_height(14, 400));
        layout->toast_text = rect_of(inner_left + glyph, inner_top + (content - text.height) / 2, text_area, text.height);
        float inner_right = layout->toast.right - 1 - 12;
        layout->toast_action = button > 0 ? rect_of(inner_right + 6 - button, inner_top + (content - 24) / 2, button, 24) : (GfxRect){ 0, 0, 0, 0 };
    }
}

// ───────────── 画 ─────────────

static Color faded(Color color, float opacity)
{
    if (opacity >= 1) return color;
    if (opacity <= 0) return COLOR_TRANSPARENT;
    color.a = (uint8_t)lroundf(color.a * opacity);
    return color;
}

/// 置物架上的图标按钮：悬停时有淡淡的底色，按下时更淡。
static void paint_icon_button(ShelfWindow *shelf, GfxRect rect, ShelfTarget target, const wchar_t *glyph, float size, Color text)
{
    bool hovered = shelf->hovered == target, pressed = hovered && shelf->pressed == target;
    gfx_fill_rounded(rect, 6, theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
    GfxTextStyle style = glyph_style(size);
    gfx_text_draw(glyph, rect, &style, text);
}

/// 一张有底色、有描边的小牌（标题栏的“移动/复制”、底部的快速操作）：悬停时提亮、描边换成强调色，按下时变淡。
static void paint_chip_card(GfxRect rect, float radius, ThemeColor border, bool hovered, bool pressed, float opacity)
{
    if (pressed) opacity *= 0.8f;
    gfx_fill_rounded(rect, radius, faded(theme_color(hovered ? THEME_CARD_HOVER : THEME_CARD), opacity));
    gfx_stroke_rounded(rect, radius, faded(theme_color(hovered ? THEME_ACCENT : border), opacity), 1);
}

static void paint_header(ShelfWindow *shelf)
{
    const ShelfLayout *layout = &shelf->layout;
    // 置顶：开着时图标是强调色。
    paint_icon_button(shelf, layout->pin, TARGET_PIN, shelf->model->pinned ? GLYPH_PINNED : GLYPH_PIN, 14,
        theme_color(shelf->model->pinned ? THEME_ACCENT_TEXT : THEME_TEXT_PRIMARY));

    // 移动/复制：和底部的快速操作一样是一张有底色、有描边的小牌，一眼看出能点。
    bool hovered = shelf->hovered == TARGET_MODE, pressed = hovered && shelf->pressed == TARGET_MODE;
    paint_chip_card(layout->mode, 4, THEME_CONTROL_STRONG_BORDER, hovered, pressed, 1);
    Color text = faded(theme_color(THEME_TEXT_PRIMARY), pressed ? 0.8f : 1);
    const wchar_t *glyph = shelf->move_mode ? GLYPH_MOVE : GLYPH_COPY;
    float glyph_width = text_width(glyph, 12, 400, GFX_FONT_ICON);
    GfxTextStyle icon = glyph_style(12);
    gfx_text_draw(glyph, rect_of(layout->mode.left + 8, layout->mode.top + 1, glyph_width, 23), &icon, text);
    GfxTextStyle label = text_style(12, 400, GFX_ALIGN_START);
    gfx_text_draw(S(shelf->move_mode ? STR_TransferMove : STR_TransferCopy),
        (GfxRect){ layout->mode.left + 8 + glyph_width + 5, layout->mode.top, layout->mode.right, layout->mode.bottom - 1 }, &label, text);

    // 收起时堆叠下方已显示名称和数量，标题只在平铺时显示。
    if (shelf->model->expanded)
    {
        wchar_t *title = shelf_model_title(shelf->model);
        GfxTextStyle style = text_style(12, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(title, layout->title, &style, theme_color(THEME_TEXT_SECONDARY));
        free(title);
    }

    Color primary = theme_color(THEME_TEXT_PRIMARY);
    if (shelf->model->count > 0)
        paint_icon_button(shelf, layout->view, TARGET_VIEW, shelf->model->expanded ? GLYPH_COLLAPSE : GLYPH_SPREAD, 14,
            shelf->pressed == TARGET_VIEW && shelf->hovered == TARGET_VIEW ? theme_color(THEME_TEXT_SECONDARY) : primary);
    paint_icon_button(shelf, layout->more, TARGET_MORE, GLYPH_MORE, 14,
        shelf->pressed == TARGET_MORE && shelf->hovered == TARGET_MORE ? theme_color(THEME_TEXT_SECONDARY) : primary);
    paint_icon_button(shelf, layout->close, TARGET_CLOSE, GLYPH_CLOSE, 11,
        shelf->pressed == TARGET_CLOSE && shelf->hovered == TARGET_CLOSE ? theme_color(THEME_TEXT_SECONDARY) : primary);
}

/// 空置物架：桌上留着的一个空牌位（虚线的竖版牌，中间一个加号），下面写着放到这里或粘贴。
static void paint_empty(ShelfWindow *shelf, float opacity)
{
    GfxRect body = shelf->layout.body;
    float first = line_height(13, 400), second = line_height(11, 400);
    float total = 72 + 10 + first + 3 + second;
    float top = (body.top + body.bottom - total) / 2, center = (body.left + body.right) / 2;
    Color ink = faded(theme_color(THEME_TEXT_PRIMARY), opacity);
    GfxRect slot = { center - 27, top, center + 27, top + 72 };
    gfx_stroke_rounded_dashed(slot, 5, ink, 1.5f, 2, 1.3333f);
    float middle = top + 36;
    gfx_line(center, middle - 7, center, middle + 7, ink, 1.4f);
    gfx_line(center - 7, middle, center + 7, middle, ink, 1.4f);
    top += 72 + 10;
    GfxTextStyle style = text_style(13, 400, GFX_ALIGN_CENTER);
    gfx_text_draw(S(STR_DropHere), (GfxRect){ body.left + 8, top, body.right - 8, top + first }, &style, ink);
    top += first + 3;
    style.size = 11;
    gfx_text_draw(S(STR_OrPaste), (GfxRect){ body.left + 8, top, body.right - 8, top + second }, &style, faded(theme_color(THEME_TEXT_SECONDARY), opacity));
}

/// 牌上的内容：缩略图按比例放进去，没有缩略图时是类型图标；已删除或移动的项目内容变淡。
static void paint_item_picture(ShelfItem *item, GfxRect area, float glyph_size, float opacity)
{
    float content = item->available ? opacity : opacity * 0.5f;
    if (item->thumbnail)
    {
        float width = (float)gfx_image_width(item->thumbnail), height = (float)gfx_image_height(item->thumbnail);
        float ratio = fminf((area.right - area.left) / width, (area.bottom - area.top) / height);
        float w = width * ratio, h = height * ratio;
        float x = (area.left + area.right - w) / 2, y = (area.top + area.bottom - h) / 2;
        gfx_draw_image(item->thumbnail, (GfxRect){ x, y, x + w, y + h }, content);
        return;
    }
    GfxTextStyle style = glyph_style(glyph_size);
    gfx_text_draw(shelf_item_glyph(item), area, &style, faded(theme_color(THEME_TEXT_SECONDARY), content));
}

/// 一张竖版的牌，中心在 (0, 0)：牌面上印着缩略图。下面几张换成牌桌上的牌面底色和下层牌的描边。
static void paint_card_face(ShelfItem *item, bool top)
{
    float half_width = SHELF_CARD_WIDTH / 2, half_height = SHELF_CARD_HEIGHT / 2;
    GfxRect rect = { -half_width, -half_height, half_width, half_height };
    gfx_fill_rounded(rect, SHELF_CARD_RADIUS, theme_color(top ? THEME_SHELF_TILE : THEME_CARD));
    gfx_stroke_rounded(rect, SHELF_CARD_RADIUS, theme_color(top ? THEME_SHELF_TILE_BORDER : THEME_SHELF_UNDER_CARD_BORDER), 1.2f);
    if (item) paint_item_picture(item, (GfxRect){ rect.left + 7, rect.top + 20, rect.right - 7, rect.bottom - 16 }, 26, 1);
}

static GfxMatrix canvas_matrix(const ShelfLayout *layout, CardPose pose, float canvas_x, float canvas_y, float canvas_scale)
{
    (void)layout;
    GfxMatrix matrix = gfx_matrix_scale(pose.scale, pose.scale, 0, 0);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(pose.angle, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(pose.x, pose.y));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_scale(canvas_scale, canvas_scale, 0, 0));
    return gfx_matrix_multiply(matrix, gfx_matrix_translate(canvas_x, canvas_y));
}

static void paint_card(ShelfWindow *shelf, int card, ShelfItem *item)
{
    CardPose pose = shelf->card_posed[card] ? shelf->card_pose[card] : shelf_card_home(card);
    float opacity = pose.opacity * (float)item->dim.value;
    if (opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    gfx_push_transform(canvas_matrix(layout, pose, layout->canvas_x, layout->canvas_y, layout->canvas_scale));
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
    paint_card_face(item, card == 0);
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

/// 最上面一张的牌角：项目数和数量最多的文件类型，右下角倒过来再印一个；裁在牌面以内。
static void paint_rank(ShelfWindow *shelf, float opacity)
{
    if (shelf->rank_hidden || opacity <= 0.001f || shelf->model->count == 0) return;
    const ShelfLayout *layout = &shelf->layout;
    wchar_t count[16];
    swprintf(count, ARRAYSIZE(count), L"%d", shelf->model->count);
    if (!shelf->rank_suit_known)
    {
        shelf->rank_suit = shelf_model_suit(shelf->model);
        shelf->rank_suit_known = true;
    }
    FileSuit suit = shelf->rank_suit;
    gfx_push_transform(gfx_matrix_multiply(gfx_matrix_scale(layout->canvas_scale, layout->canvas_scale, 0, 0),
        gfx_matrix_translate(layout->canvas_x, layout->canvas_y)));
    GfxRect card = { 48, 12, 48 + SHELF_CARD_WIDTH, 12 + SHELF_CARD_HEIGHT };
    gfx_push_clip(card);
    static const CardCorner corner = { 15, 15, 6, 2 };
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    // 新牌落进来时数字从下面滚上来；花色和倒过来的那一个不动。
    card_paint_corner_rolling(count, suit, &corner, card.left + 5, card.top + 4, ink, opacity, shelf->rank_shift, shelf->rank_opacity);
    card_paint_corner_inverted(count, suit, &corner, card, 5, 4, ink, opacity);
    gfx_pop_clip();
    gfx_pop_transform();
}

static void paint_stack(ShelfWindow *shelf, float opacity)
{
    if (opacity < 0.999f) gfx_push_opacity(opacity);
    for (int card = 2; card >= 0; card--)
    {
        ShelfItem *item = shelf_model_from_end(shelf->model, card + 1);
        if (item) paint_card(shelf, card, item);
    }
    paint_rank(shelf, shelf->mark_opacity);
    if (shelf->mark_opacity > 0)
    {
        wchar_t *caption = shelf_model_stack_caption(shelf->model);
        wchar_t *detail = shelf_model_stack_detail(shelf->model);
        GfxTextStyle style = text_style(13, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(caption, shelf->layout.caption, &style, faded(theme_color(THEME_TEXT_PRIMARY), shelf->mark_opacity));
        style = text_style(11, 400, GFX_ALIGN_CENTER);
        gfx_text_draw(detail, shelf->layout.detail, &style, faded(theme_color(THEME_TEXT_SECONDARY), shelf->mark_opacity));
        free(caption);
        free(detail);
    }
    if (opacity < 0.999f) gfx_pop_layer();
}

/// 平铺里的一张牌：竖版小牌印着缩略图，牌角印扩展名和花色，名字最多两行写在下面；悬停时抬起，选中时换成强调色描边。
static void paint_tile(ShelfWindow *shelf, int index, ShelfItem *item)
{
    const ShelfItemMotion *motion = &item->motion;
    float opacity = motion->opacity * (float)item->dim.value;
    if (opacity <= 0.001f) return;
    GfxRect slot = shelf_tile_rect(shelf, index);
    GfxRect card = shelf_tile_card_rect(shelf, index);
    // 以牌面中心为原点缩放、转动，再平移；名字跟着一起动。
    float cx = (card.left + card.right) / 2, cy = (card.top + card.bottom) / 2;
    GfxMatrix matrix = gfx_matrix_scale(motion->scale, motion->scale, cx, cy);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(motion->angle, cx, cy));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(motion->dx, motion->dy));
    gfx_push_transform(matrix);
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
    bool hovered = shelf->hovered_row == index && (shelf->hovered == TARGET_ROW || shelf->hovered == TARGET_ROW_REMOVE) && !shelf->switching;
    // 牌抬起时露出的桌面投影。
    float lift = (float)item->lift.value;
    if (hovered) gfx_fill_rounded(card, SHELF_CARD_RADIUS, faded(theme_color(THEME_SHELF_UNDER_CARD_BORDER), 0.18f));
    GfxRect face = { card.left, card.top - 4 * lift, card.right, card.bottom - 4 * lift };
    gfx_fill_rounded(face, SHELF_CARD_RADIUS, theme_color(THEME_SHELF_TILE));
    if (item->selected) gfx_stroke_rounded(face, SHELF_CARD_RADIUS, theme_color(THEME_ACCENT), 2);
    else gfx_stroke_rounded(face, SHELF_CARD_RADIUS, theme_color(THEME_SHELF_TILE_BORDER), 1.2f);
    paint_item_picture(item, (GfxRect){ face.left + 7, face.top + 20, face.right - 7, face.bottom - 16 }, 26, 1);
    wchar_t *mark = shelf_item_card_mark(item);
    static const CardCorner corner = { 10, 10, 5, 2 };
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    FileSuit suit = shelf_item_suit(item);
    card_paint_corner(mark, suit, &corner, face.left + 4, face.top + 4, ink, 1);
    card_paint_corner_inverted(mark, suit, &corner, face, 4, 4, ink, 1);
    free(mark);
    // 右上角的移除：墨色小圆里一个叉，悬停时变红。
    if (hovered)
    {
        GfxRect remove = shelf_tile_remove_rect(face);
        float x = (remove.left + remove.right) / 2, y = (remove.top + remove.bottom) / 2;
        bool over = shelf->hovered == TARGET_ROW_REMOVE;
        Color paper = theme_color(THEME_SHELF_SOLID);
        gfx_fill_ellipse(x, y, 9, 9, theme_color(over ? THEME_CRITICAL : THEME_TEXT_PRIMARY));
        gfx_stroke_ellipse(x, y, 8.25f, 8.25f, paper, 1.5f);
        gfx_line(x - 3, y - 3, x + 3, y + 3, paper, 1.4f);
        gfx_line(x + 3, y - 3, x - 3, y + 3, paper, 1.4f);
    }
    // 名字：牌下面，最多两行；选中时铺一层淡淡的强调色。
    float name_opacity = motion->name_opacity * (item->available ? 1.0f : 0.5f);
    if (name_opacity > 0.001f)
    {
        GfxTextStyle name = { GFX_FONT_TEXT, 11, 400, true, true, GFX_ALIGN_CENTER, GFX_ALIGN_START, 14 };
        GfxSize size = gfx_text_measure(shelf_item_display_name(item), &name, SHELF_SLOT_WIDTH - 6);
        float height = fminf(size.height, 28), width = fminf(size.width, SHELF_SLOT_WIDTH - 6);
        float top = card.bottom + 5;
        if (item->selected)
        {
            float middle = (slot.left + slot.right) / 2;
            gfx_fill_rounded((GfxRect){ middle - width / 2 - 3, top, middle + width / 2 + 3, top + height }, 4,
                faded(theme_color(THEME_ACCENT_SUBTLE), name_opacity));
        }
        gfx_text_draw(shelf_item_display_name(item), (GfxRect){ slot.left + 3, top, slot.right - 3, top + height }, &name,
            faded(theme_color(THEME_TEXT_PRIMARY), name_opacity));
    }
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

static void paint_grid(ShelfWindow *shelf, float opacity)
{
    const ShelfLayout *layout = &shelf->layout;
    GfxRect list = layout->list;
    // 平铺里的内容只显示在平铺范围以内；第一行的牌抬起时上面留着 8 的空白，不会被裁掉。
    gfx_push_clip((GfxRect){ list.left + 1, list.top + 1, list.right - 1, list.bottom - 1 });
    if (opacity < 0.999f) gfx_push_opacity(opacity);
    int first, last;
    shelf_visible_tiles(shelf, &first, &last);
    for (int index = first; index <= last; index++) paint_tile(shelf, index, shelf->model->items[index]);
    if (layout->scrollable)
    {
        float alpha = shelf->scroll_hovered || shelf->scroll_dragging ? 1.0f : 0.55f;
        gfx_fill_rounded(layout->scroll_thumb, 3, faded(theme_color(THEME_SCROLL_THUMB), alpha));
    }
    if (opacity < 0.999f) gfx_pop_layer();
    gfx_pop_clip();
}

/// 快速操作按钮栏：一格一张描边的小牌；只有两个操作时按钮上写名称，四个时只有图标。
static void paint_action_bar(ShelfWindow *shelf, float opacity)
{
    int count = shelf->actions.count;
    if (count == 0 || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    bool large = count <= 2;
    for (int index = 0; index < count; index++)
    {
        GfxRect button = layout->action_buttons[index];
        bool hovered = shelf->hovered == TARGET_ACTION && shelf->hovered_row == index;
        bool pressed = hovered && shelf->pressed == TARGET_ACTION && shelf->pressed_row == index;
        paint_chip_card(button, 5, THEME_SHELF_UNDER_CARD_BORDER, hovered, pressed, opacity);
        Color text = faded(theme_color(THEME_TEXT_PRIMARY), opacity * (pressed ? 0.8f : 1));
        const wchar_t *glyph = quick_action_glyph(&shelf->actions.items[index]);
        if (!large)
        {
            GfxTextStyle icon = glyph_style(16);
            gfx_text_draw(glyph, button, &icon, text);
            continue;
        }
        // 图标和名称作为一组居中，放不下时名称末尾省略。
        const wchar_t *label = shelf->action_labels[index] ? shelf->action_labels[index] : L"";
        float glyph_width = text_width(glyph, 15, 400, GFX_FONT_ICON);
        float available = button.right - button.left - 8 - glyph_width - 6;
        float label_width = fminf(text_width(label, 12, 400, GFX_FONT_TEXT), available > 0 ? available : 0);
        float left = (button.left + button.right - glyph_width - 6 - label_width) / 2;
        GfxTextStyle icon = glyph_style(15);
        gfx_text_draw(glyph, (GfxRect){ left, button.top + 1, left + glyph_width, button.bottom + 1 }, &icon, text);
        GfxTextStyle style = text_style(12, 400, GFX_ALIGN_START);
        gfx_text_draw(label, (GfxRect){ left + glyph_width + 6, button.top, left + glyph_width + 6 + label_width + 1, button.bottom }, &style, text);
    }
}

/// 拖入时展开的快速操作卡片：图标在上，名称在下（最多两行）；拖到上面的那张铺上强调色、略微放大。
static void paint_tiles(ShelfWindow *shelf, float opacity)
{
    int count = shelf->actions.count;
    if (count == 0 || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    bool large = count <= 2;
    gfx_push_clip(layout->tiles_host);
    gfx_push_opacity(opacity);
    for (int index = 0; index < count; index++)
    {
        GfxRect tile = layout->tiles[index];
        float active = (float)shelf->tile_highlight[index].value;
        float scale = 1 + 0.04f * active;
        float cx = (tile.left + tile.right) / 2, cy = (tile.top + tile.bottom) / 2;
        gfx_push_transform(gfx_matrix_scale(scale, scale, cx, cy));
        gfx_fill_rounded(tile, 6, theme_color(THEME_CARD));
        gfx_stroke_rounded(tile, 6, theme_color(THEME_SHELF_UNDER_CARD_BORDER), 1);
        if (active > 0.001f)
        {
            // 强调色的一层连边框一起盖住。
            GfxRect cover = { tile.left - 0.5f, tile.top - 0.5f, tile.right + 0.5f, tile.bottom + 0.5f };
            gfx_fill_rounded(cover, 6.5f, faded(theme_color(THEME_ACCENT), active));
        }
        Color text = color_lerp(theme_color(THEME_TEXT_PRIMARY), theme_color(THEME_TEXT_ON_ACCENT), active);
        const wchar_t *glyph = quick_action_glyph(&shelf->actions.items[index]);
        const wchar_t *label = shelf->action_labels[index] ? shelf->action_labels[index] : L"";
        GfxTextStyle icon = glyph_style(large ? 22.0f : 20.0f);
        float glyph_height = gfx_text_measure(glyph, &icon, 0).height;
        float padding = large ? 6.0f : 2.0f;
        float label_width = tile.right - tile.left - 2 - padding * 2;
        GfxTextStyle wrap = { GFX_FONT_TEXT, 12, 400, true, true, GFX_ALIGN_CENTER, GFX_ALIGN_START, 15 };
        float label_height = fminf(gfx_text_measure(label, &wrap, label_width > 1 ? label_width : 1).height, 30);
        float top = cy - (glyph_height + 8 + label_height) / 2;
        gfx_text_draw(glyph, (GfxRect){ tile.left, top, tile.right, top + glyph_height }, &icon, text);
        top += glyph_height + 8;
        gfx_text_draw(label, (GfxRect){ tile.left + 1 + padding, top, tile.right - 1 - padding, top + label_height }, &wrap, text);
        gfx_pop_transform();
    }
    gfx_pop_layer();
    gfx_pop_clip();
}

/// 拖进来时原来那一摞最上面两张摊成小扇形，中间露出一个虚线空位，空位上标着带进来几张；下面写着松开会怎样。
static void paint_drop_overlay(ShelfWindow *shelf, float opacity)
{
    const ShelfLayout *layout = &shelf->layout;
    GfxRect zone = layout->drop_zone;
    float cx = (zone.left + zone.right) / 2, cy = (zone.top + zone.bottom) / 2;
    // 可以合并时整个放置区域轻轻弹一下。
    float pulse = (float)shelf->drop_zone_scale.value;
    bool pulsing = fabsf(pulse - 1) > 0.0005f;
    if (pulsing) gfx_push_transform(gfx_matrix_scale(pulse, pulse, cx, cy));
    // 合并前的蓄力：拖着别的置物架停在这里时，强调色的底从中间涨满。
    float charge = opacity * (float)shelf->merge_charge_opacity.value;
    if (charge > 0.001f)
    {
        float scale = (float)shelf->merge_charge_scale.value;
        float half_width = (zone.right - zone.left) / 2 * scale, half_height = (zone.bottom - zone.top) / 2 * scale;
        gfx_fill_rounded((GfxRect){ cx - half_width, cy - half_height, cx + half_width, cy + half_height }, 8 * scale,
            faded(theme_color(THEME_ACCENT_SUBTLE), charge));
    }
    float label = opacity * (float)shelf->drop_label_opacity.value;
    if (label > 0.001f)
    {
        // 拖到快速操作上时整组牌变淡，文字不变。
        float group = label * (float)shelf->frame_opacity.value;
        float progress = (float)shelf->fan.value;
        gfx_push_opacity(group);
        for (int index = 1; index >= 0; index--)
        {
            ShelfItem *item = shelf_model_from_end(shelf->model, index + 1);
            if (!item) continue;
            CardPose pose = shelf_fan_pose(index, progress);
            gfx_push_transform(canvas_matrix(layout, pose, layout->drop_canvas_x, layout->drop_canvas_y, layout->drop_canvas_scale));
            paint_card_face(item, index == 0);
            gfx_pop_transform();
        }
        // 虚线空位：从略小、略低的地方浮起来。
        float slot_scale = 0.9f + 0.1f * progress, slot_shift = 6 - 9 * progress;
        CardPose slot = { 56 + 32, 10 + 43 + slot_shift, 0, slot_scale, 1 };
        gfx_push_transform(canvas_matrix(layout, slot, layout->drop_canvas_x, layout->drop_canvas_y, layout->drop_canvas_scale));
        gfx_push_opacity(progress);
        GfxRect ghost = { -SHELF_CARD_WIDTH / 2, -SHELF_CARD_HEIGHT / 2, SHELF_CARD_WIDTH / 2, SHELF_CARD_HEIGHT / 2 };
        gfx_fill_rounded(ghost, SHELF_CARD_RADIUS, theme_color(THEME_ACCENT_FAINT));
        gfx_stroke_rounded_dashed(ghost, SHELF_CARD_RADIUS, theme_color(THEME_ACCENT), 1.5f, 2, 1.3333f);
        gfx_pop_layer();
        gfx_pop_transform();
        if (shelf->incoming > 0)
        {
            wchar_t text[16];
            swprintf(text, ARRAYSIZE(text), L"+%d", shelf->incoming);
            GfxTextStyle number = { GFX_FONT_NUMBER, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
            float width = text_width(text, 12, 400, GFX_FONT_NUMBER) + 14;
            gfx_push_transform(gfx_matrix_multiply(gfx_matrix_scale(layout->drop_canvas_scale, layout->drop_canvas_scale, 0, 0),
                gfx_matrix_translate(layout->drop_canvas_x, layout->drop_canvas_y)));
            GfxRect badge = { 132, 0, 132 + width, 20 };
            gfx_fill_rounded(badge, 10, theme_color(THEME_ACCENT));
            gfx_text_draw(text, badge, &number, theme_color(THEME_TEXT_ON_ACCENT));
            gfx_pop_transform();
        }
        gfx_pop_layer();
        GfxTextStyle style = text_style(12, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(S(shelf->drop_zone_text), layout->drop_text, &style, faded(theme_color(THEME_TEXT_PRIMARY), label));
    }
    if (pulsing) gfx_pop_transform();
    paint_tiles(shelf, opacity * (float)shelf->tiles_opacity.value);
}

static void paint_busy(ShelfWindow *shelf, double time)
{
    float opacity = (float)shelf->busy_opacity.value;
    if (opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    gfx_fill_rect((GfxRect){ 0, 0, layout->width, layout->height }, faded(theme_color(THEME_SMOKE), opacity));
    float text = line_height(12, 400);
    float top = (layout->height - 30 - 12 - text) / 2;
    float cx = layout->width / 2, cy = top + 15;
    // 不确定进度的环：0.9 秒转一圈。
    clock_tick(&shelf->ring_clock, time);
    float angle = (float)fmod(shelf->ring_clock.elapsed / 0.9 * 360.0, 360.0);
    gfx_stroke_ellipse(cx, cy, 13.5f, 13.5f, faded(theme_color(THEME_CONTROL_ALT), opacity), 3);
    gfx_arc(cx, cy, 13.5f, angle - 90, 90, faded(theme_color(THEME_ACCENT), opacity), 3);
    GfxTextStyle style = text_style(12, 400, GFX_ALIGN_CENTER);
    gfx_text_draw(shelf->model->busy_text, (GfxRect){ 0, top + 30 + 12, layout->width, top + 30 + 12 + text }, &style,
        faded(theme_color(THEME_TEXT_SECONDARY), opacity));
    ui_window_request_frame(shelf->ui);
}

static void paint_toast(ShelfWindow *shelf)
{
    float opacity = (float)shelf->toast_opacity.value;
    if (!shelf->toast_text || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    gfx_push_transform(gfx_matrix_translate(0, (float)shelf->toast_offset.value));
    gfx_push_opacity(opacity);
    gfx_fill_rounded(layout->toast, 8, theme_color(THEME_FLYOUT));
    gfx_stroke_rounded(layout->toast, 8, theme_color(THEME_FLYOUT_BORDER), 1);
    GfxTextStyle icon = glyph_style(14);
    gfx_text_draw(shelf->toast_success ? GLYPH_COMPLETED : GLYPH_WARNING, layout->toast_glyph, &icon,
        theme_color(shelf->toast_success ? THEME_SUCCESS : THEME_CAUTION));
    GfxTextStyle wrap = { GFX_FONT_TEXT, 12, 400, true, false, GFX_ALIGN_START, GFX_ALIGN_START };
    gfx_text_draw(shelf->toast_text, layout->toast_text, &wrap, theme_color(THEME_TEXT_PRIMARY));
    if (shelf->toast_action != TOAST_ACTION_NONE)
    {
        bool hovered = shelf->hovered == TARGET_TOAST_ACTION, pressed = hovered && shelf->pressed == TARGET_TOAST_ACTION;
        gfx_fill_rounded(layout->toast_action, 4, theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
        GfxTextStyle style = text_style(12, 600, GFX_ALIGN_CENTER);
        style.ellipsis = false;
        GfxRect label = layout->toast_action;
        label.bottom -= 1;
        gfx_text_draw(S(shelf->toast_action == TOAST_ACTION_UNDO ? STR_Undo : STR_MenuConvertToFile), label, &style,
            theme_color(pressed ? THEME_TEXT_SECONDARY : THEME_ACCENT_TEXT));
    }
    gfx_pop_layer();
    gfx_pop_transform();
}

static void paint(UiWindow *window, void *context, double time)
{
    ShelfWindow *shelf = context;
    (void)window;
    shelf_layout(shelf);
    const ShelfLayout *layout = &shelf->layout;

    // 整个内容一起缩放、淡入淡出（出场和离场），背景不动。
    float origin_x = layout->width * shelf->origin_x, origin_y = layout->height * shelf->origin_y;
    GfxMatrix surface = gfx_matrix_scale(shelf->surface_scale, shelf->surface_scale, origin_x, origin_y);
    surface = gfx_matrix_multiply(surface, gfx_matrix_translate(shelf->surface_dx, shelf->surface_dy));
    gfx_push_transform(surface);
    // 拖到别的置物架上、松手就合并时内容变淡。
    float surface_opacity = shelf->surface_opacity * (float)shelf->merge_dim.value;
    bool layered = surface_opacity < 0.999f;
    if (layered) gfx_push_opacity(surface_opacity);

    paint_header(shelf);
    float body = (float)shelf->body_opacity.value;
    if (body > 0.001f)
    {
        if (shelf->model->count == 0) paint_empty(shelf, body);
        bool show_list = shelf->switching || shelf->shown_expanded;
        bool show_stack = shelf->model->count > 0 && (shelf->switching || !shelf->shown_expanded);
        // 平铺压在堆叠上面，切换时牌飞过堆叠不会被挡住。
        if (show_stack) paint_stack(shelf, body);
        if (show_list) paint_grid(shelf, body);
    }
    paint_action_bar(shelf, (float)shelf->bar_opacity.value);
    float overlay = (float)shelf->overlay_opacity.value;
    if (shelf->overlay_visible && overlay > 0.001f) paint_drop_overlay(shelf, overlay);
    paint_busy(shelf, time);
    paint_toast(shelf);

    if (layered) gfx_pop_layer();
    gfx_pop_transform();
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

/// 数量或视图变化后更新界面：只是切换了堆叠和平铺时播放切换动画，刚拖入文件时播放落下的动画。
void shelf_refresh_views(ShelfWindow *shelf)
{
    bool switched = !shelf->count_changed && shelf->shown_expanded != shelf->model->expanded;
    bool landing = shelf->landing, returning = shelf->returning;
    // 松手时摊开的牌，先记下来：下面收尾时会把它清掉。
    bool fan = shelf->fan_held;
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
        shelf_update_window_size(shelf);
        // 不播动画时平铺开也停在最新的项目上。
        if (switched && shelf->model->expanded && shelf->model->count > 0) shelf_scroll_into_view(shelf, shelf->model->count - 1);
        if (returning && animate) shelf_play_return(shelf, shelf->returning_x, shelf->returning_y, returning_items, returning_count);
        if (landing && animate)
        {
            if (shelf->model->expanded) shelf_play_grid_landing(shelf, landing_x, landing_y, landing_added);
            else
            {
                shelf->fan_held = fan;
                shelf_play_landing(shelf, landing_x, landing_y, landing_added);
            }
        }
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
            shelf->rank_suit_known = false;
            if (shelf->model->count != shelf->shown_count)
            {
                // 新牌落进这一摞：牌角的数字从下面滚上来。
                if (shelf->model->count > shelf->shown_count && shelf->shown_count > 0) shelf_roll_rank(shelf);
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
            // 移动、改名或转成文件以后扩展名可能变了。
            shelf->rank_suit_known = false;
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
    shelf->rank_opacity = 1;
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
    options.backdrop = UI_BACKDROP_NONE;
    options.background = THEME_SHELF_SOLID;
    options.context = shelf;
    options.paint = paint;
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
    free(shelf->tweens);
    free(shelf->removing);
    free(shelf->rows_before);
    free(shelf->rows_before_x);
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
    // 有牌在替它出场或离场时也算。
    return shelf->sliding || shelf->popping || shelf->card || shelf->motion_running || shelf->receiving > 0 || shelf->closing;
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
    // 晃动和边缘呼出由一张牌替置物架出场：置物架先隐身，照常显示和绘制，牌落定时直接现身。
    bool dealt = !was_visible && anim_enabled() && entrance != ENTRANCE_PLAIN;
    if (dealt) window_set_cloaked(shelf->handle, true);
    RECT before = {0};
    if (was_visible) GetWindowRect(shelf->handle, &before);
    center_on(shelf, center);
    update_thumbnail_size(shelf);
    // 复用的空置物架挪到了别处：重播出场动画，不直接瞬移过去。已经显示着的窗口不再隐身改为发牌，直接升起；
    // 本来就在光标附近时不重播，免得在原处闪一下。
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
    else if (!was_visible || moved) shelf_animate_in(shelf);
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
    shelf_stop_slide(shelf);
    shelf_stop_pop(shelf);
    // 发牌、移除、补位和窗口大小的动画停在这一帧：截下的牌面和屏幕上一样。没做完的收尾（比如待移除的项目）在窗口关闭时补上。
    shelf_stop_motion(shelf);
    if (!anim_enabled() || !shelf_window_is_visible(shelf) || !shelf_play_exit(shelf)) task_post(close_posted, shelf_window_retain(shelf));
}

void shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths)
{
    shelf_add_paths(shelf, paths);
}
