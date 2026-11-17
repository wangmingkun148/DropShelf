#include "shelf_window_internal.h"
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
#define GLYPH_LIST L"\xE8FD"
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

void shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height)
{
    *width = expanded ? resolve_width(shelf->size) + 80 : resolve_width(shelf->size);
    int count = shelf->model->count;
    int rows = count < 2 ? 2 : count > 6 ? 6 : count;
    float body = expanded ? rows * SHELF_ROW_HEIGHT + 6 : resolve_stack_height(shelf->size) + SHELF_CAPTION_HEIGHT;
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
    if (IsWindowVisible(shelf->handle) && !shelf->sliding) shelf_keep_in_work_area(shelf);
    ui_window_invalidate(shelf->ui);
}

static void update_thumbnail_size(ShelfWindow *shelf)
{
    // 卡片 84 减去内边距后显示区域约为 66；堆叠画布 160×116 随置物架大小缩放，两侧各留 10 的边距。
    float stack_scale = fminf((resolve_width(shelf->size) - 20) / SHELF_CANVAS_WIDTH, resolve_stack_height(shelf->size) / SHELF_CANVAS_HEIGHT);
    shelf->model->thumbnail_size = (int)ceil(66 * stack_scale * shelf_scale(shelf));
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

// ───────────── 排版 ─────────────

CardPose shelf_card_home(int card)
{
    // XAML 里三张牌的位置：最上面一张不转，第二张转 7° 往右挪，最下面一张转 -10° 往左挪；中心 = 左上角 + 42 + 平移。
    static const CardPose homes[3] = {
        { 38 + 42, 16 + 42, 0, 1, 1 },
        { 38 + 42 + 11, 18 + 42 + 1, 7, 1, 1 },
        { 38 + 42 - 12, 20 + 42 + 2, -10, 1, 1 },
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

int shelf_visible_rows(ShelfWindow *shelf)
{
    float height = shelf->layout.list.bottom - shelf->layout.list.top - 2;
    int rows = (int)floorf(height / SHELF_ROW_PITCH);
    return rows < 1 ? 1 : rows;
}

void shelf_set_scroll(ShelfWindow *shelf, int first)
{
    int maximum = shelf->model->count - shelf_visible_rows(shelf);
    if (first > maximum) first = maximum;
    if (first < 0) first = 0;
    if (first == shelf->scroll_first) return;
    shelf->scroll_first = first;
    shelf_layout(shelf);
    shelf_repaint(shelf);
}

void shelf_scroll_into_view(ShelfWindow *shelf, int index)
{
    if (index < 0 || index >= shelf->model->count) return;
    shelf_layout(shelf);
    int rows = shelf_visible_rows(shelf);
    if (index < shelf->scroll_first) shelf_set_scroll(shelf, index);
    else if (index >= shelf->scroll_first + rows) shelf_set_scroll(shelf, index - rows + 1);
}

GfxRect shelf_row_rect(ShelfWindow *shelf, int index)
{
    const GfxRect list = shelf->layout.list;
    float top = list.top + 1 + (float)(index - shelf->scroll_first) * SHELF_ROW_PITCH + 1;
    return (GfxRect){ list.left + 1, top, list.right - 1, top + SHELF_ROW_PITCH - 2 };
}

GfxRect shelf_row_thumb_rect(ShelfWindow *shelf, int index)
{
    GfxRect row = shelf_row_rect(shelf, index);
    return rect_of(row.left + 6, row.top + 5, 32, 32);
}

static GfxRect row_remove_rect(GfxRect row)
{
    return rect_of(row.right - 6 - 26, row.top + 5 + 3, 26, 26);
}

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
    float icon = text_width(shelf->move_mode ? GLYPH_MOVE : GLYPH_COPY, 12, 400, GFX_FONT_ICON);
    float label = text_width(S(shelf->move_mode ? STR_TransferMove : STR_TransferCopy), 12, 400, GFX_FONT_TEXT);
    float width = ceilf(1 + 6 + icon + 5 + label + 8 + 1);
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
    layout->pin = rect_of(4, 7, 28, 30);
    layout->mode = rect_of(4 + 28 + 2, 10, mode_width(shelf), 24);
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

    // 列表：超出时右边有一条细滚动条（盖在内容上）。
    GfxRect list = layout->list;
    int count = shelf->model->count;
    int rows = (int)floorf((list.bottom - list.top - 2) / SHELF_ROW_PITCH);
    if (rows < 1) rows = 1;
    int maximum_first = count - rows > 0 ? count - rows : 0;
    if (shelf->scroll_first > maximum_first) shelf->scroll_first = maximum_first;
    if (shelf->scroll_first < 0) shelf->scroll_first = 0;
    // 只给列表里显示出来的行取缩略图（多备一行，滚动时不用等）。
    bool list_shown = shelf->model->expanded || shelf->switching;
    int from = list_shown ? shelf->scroll_first : -1;
    int to = list_shown ? shelf->scroll_first + rows : -1;
    if (from != shelf->model->visible_from || to != shelf->model->visible_to)
    {
        shelf->model->visible_from = from;
        shelf->model->visible_to = to;
        shelf_model_update_thumbnails(shelf->model);
    }
    layout->scrollable = count * SHELF_ROW_PITCH > list.bottom - list.top - 2;
    if (layout->scrollable)
    {
        layout->scroll_track = (GfxRect){ list.right - 1 - 1 - 12, list.top + 1 + 2, list.right - 1 - 1, list.bottom - 1 - 2 };
        float track = layout->scroll_track.bottom - layout->scroll_track.top;
        float length = fmaxf(track * rows / count, 16);
        float top = layout->scroll_track.top + (maximum_first > 0 ? (track - length) * shelf->scroll_first / maximum_first : 0);
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
    gfx_fill_rounded(rect, 4, theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
    GfxTextStyle style = glyph_style(size);
    gfx_text_draw(glyph, rect, &style, text);
}

static void paint_header(ShelfWindow *shelf)
{
    const ShelfLayout *layout = &shelf->layout;
    // 置顶：开着时图标是强调色。
    paint_icon_button(shelf, layout->pin, TARGET_PIN, shelf->model->pinned ? GLYPH_PINNED : GLYPH_PIN, 14,
        theme_color(shelf->model->pinned ? THEME_ACCENT_TEXT : THEME_TEXT_PRIMARY));

    // 移动/复制：和普通按钮一样有底色和边框，一眼看出能点。
    bool hovered = shelf->hovered == TARGET_MODE, pressed = hovered && shelf->pressed == TARGET_MODE;
    gfx_fill_rounded(layout->mode, 4, theme_color(pressed ? THEME_CONTROL_PRESSED : hovered ? THEME_CONTROL_HOVER : THEME_CONTROL));
    gfx_stroke_rounded(layout->mode, 4, theme_color(THEME_CONTROL_BORDER), 1);
    Color text = theme_color(pressed ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY);
    const wchar_t *glyph = shelf->move_mode ? GLYPH_MOVE : GLYPH_COPY;
    float glyph_width = text_width(glyph, 12, 400, GFX_FONT_ICON);
    GfxTextStyle icon = glyph_style(12);
    gfx_text_draw(glyph, rect_of(layout->mode.left + 7, layout->mode.top + 1, glyph_width, 23), &icon, text);
    GfxTextStyle label = text_style(12, 400, GFX_ALIGN_START);
    gfx_text_draw(S(shelf->move_mode ? STR_TransferMove : STR_TransferCopy),
        (GfxRect){ layout->mode.left + 7 + glyph_width + 5, layout->mode.top, layout->mode.right, layout->mode.bottom - 1 }, &label, text);

    // 收起时堆叠下方已显示名称和数量，标题只在展开列表时显示。
    if (shelf->model->expanded)
    {
        wchar_t *title = shelf_model_title(shelf->model);
        GfxTextStyle style = text_style(12, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(title, layout->title, &style, theme_color(THEME_TEXT_SECONDARY));
        free(title);
    }

    Color primary = theme_color(THEME_TEXT_PRIMARY);
    if (shelf->model->count > 0)
        paint_icon_button(shelf, layout->view, TARGET_VIEW, shelf->model->expanded ? GLYPH_COLLAPSE : GLYPH_LIST, 14,
            shelf->pressed == TARGET_VIEW && shelf->hovered == TARGET_VIEW ? theme_color(THEME_TEXT_SECONDARY) : primary);
    paint_icon_button(shelf, layout->more, TARGET_MORE, GLYPH_MORE, 14,
        shelf->pressed == TARGET_MORE && shelf->hovered == TARGET_MORE ? theme_color(THEME_TEXT_SECONDARY) : primary);
    paint_icon_button(shelf, layout->close, TARGET_CLOSE, GLYPH_CLOSE, 11,
        shelf->pressed == TARGET_CLOSE && shelf->hovered == TARGET_CLOSE ? theme_color(THEME_TEXT_SECONDARY) : primary);
}

static void paint_empty(ShelfWindow *shelf, float opacity)
{
    GfxRect body = shelf->layout.body;
    gfx_stroke_rounded_dashed(body, 10, faded(theme_color(THEME_CONTROL_STRONG_BORDER), 0.45f * opacity), 1.2f, 4, 3);
    float glyph = line_height(26, 400) , first = line_height(13, 400), second = line_height(11, 400);
    GfxTextStyle icon = glyph_style(26);
    GfxSize glyph_size = gfx_text_measure(GLYPH_DOWNLOAD, &icon, 0);
    glyph = glyph_size.height > 0 ? glyph_size.height : glyph;
    float total = glyph + 10 + first + 3 + second;
    float top = (body.top + body.bottom - total) / 2;
    gfx_text_draw(GLYPH_DOWNLOAD, (GfxRect){ body.left, top, body.right, top + glyph }, &icon, faded(theme_color(THEME_TEXT_SECONDARY), opacity));
    top += glyph + 10;
    GfxTextStyle style = text_style(13, 400, GFX_ALIGN_CENTER);
    gfx_text_draw(S(STR_DropHere), (GfxRect){ body.left + 8, top, body.right - 8, top + first }, &style, faded(theme_color(THEME_TEXT_PRIMARY), opacity));
    top += first + 3;
    style.size = 11;
    gfx_text_draw(S(STR_OrPaste), (GfxRect){ body.left + 8, top, body.right - 8, top + second }, &style, faded(theme_color(THEME_TEXT_TERTIARY), opacity));
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

static void paint_card(ShelfWindow *shelf, int card, ShelfItem *item)
{
    CardPose pose = shelf->card_posed[card] ? shelf->card_pose[card] : shelf_card_home(card);
    float opacity = pose.opacity * (float)item->dim.value;
    if (opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    GfxMatrix matrix = gfx_matrix_scale(pose.scale, pose.scale, 0, 0);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(pose.angle, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(pose.x, pose.y));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_scale(layout->canvas_scale, layout->canvas_scale, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(layout->canvas_x, layout->canvas_y));
    gfx_push_transform(matrix);
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
    float half = SHELF_CARD_SIZE / 2;
    GfxRect rect = { -half, -half, half, half };
    gfx_shadow(rect, 10, 10, 0, 1.5f, COLOR_ARGB(41, 0, 0, 0));
    gfx_fill_rounded(rect, 10, theme_color(THEME_SHELF_TILE));
    gfx_stroke_rounded(rect, 10, theme_color(THEME_SHELF_TILE_BORDER), 1);
    paint_item_picture(item, (GfxRect){ -half + 10, -half + 10, half - 10, half - 10 }, 34, 1);
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

static void paint_badge(ShelfWindow *shelf, float opacity)
{
    int count = shelf->mark_override ? shelf->mark_count : shelf->model->count;
    if (count < 2 || opacity <= 0) return;
    const ShelfLayout *layout = &shelf->layout;
    wchar_t text[16];
    swprintf(text, 16, L"%d", count);
    float width = fmaxf(24, text_width(text, 11, 600, GFX_FONT_TEXT) + 12 + 4);
    GfxRect rect = rect_of(106, 6, width, 24);
    float center_x = 106 + width / 2, center_y = 18;
    GfxMatrix matrix = gfx_matrix_scale(shelf->badge_scale, shelf->badge_scale, center_x, center_y);
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_scale(layout->canvas_scale, layout->canvas_scale, 0, 0));
    matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(layout->canvas_x, layout->canvas_y));
    gfx_push_transform(matrix);
    gfx_fill_rounded(rect, 12, faded(theme_color(THEME_ACCENT), opacity));
    gfx_stroke_rounded(rect, 12, faded(theme_color(THEME_SHELF_TILE), opacity), 2);
    GfxTextStyle style = text_style(11, 600, GFX_ALIGN_CENTER);
    style.ellipsis = false;
    gfx_text_draw(text, rect, &style, faded(theme_color(THEME_TEXT_ON_ACCENT), opacity));
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
    paint_badge(shelf, shelf->mark_opacity);
    if (shelf->mark_opacity > 0)
    {
        const wchar_t *caption, *detail;
        if (shelf->model->count == 1)
        {
            ShelfItem *item = shelf->model->items[0];
            caption = shelf_item_display_name(item);
            detail = shelf_item_detail(item);
        }
        else
        {
            if (!shelf->stack_caption)
            {
                shelf->stack_caption = shelf_model_stack_caption(shelf->model);
                shelf->stack_detail = shelf_model_stack_detail(shelf->model);
            }
            caption = shelf->stack_caption;
            detail = shelf->stack_detail;
        }
        GfxTextStyle style = text_style(13, 600, GFX_ALIGN_CENTER);
        gfx_text_draw(caption, shelf->layout.caption, &style, faded(theme_color(THEME_TEXT_PRIMARY), shelf->mark_opacity));
        style = text_style(11, 400, GFX_ALIGN_CENTER);
        gfx_text_draw(detail, shelf->layout.detail, &style, faded(theme_color(THEME_TEXT_SECONDARY), shelf->mark_opacity));
    }
    if (opacity < 0.999f) gfx_pop_layer();
}

static void paint_row(ShelfWindow *shelf, int index, ShelfItem *item)
{
    const ShelfItemMotion *motion = &item->motion;
    float opacity = motion->row_opacity * (float)item->dim.value * (item->available ? 1.0f : 0.5f);
    if (opacity <= 0.001f) return;
    GfxRect row = shelf_row_rect(shelf, index);
    gfx_push_transform(gfx_matrix_translate(motion->row_dx, motion->row_dy));
    bool layered = opacity < 0.999f;
    if (layered) gfx_push_opacity(opacity);
    bool hovered = shelf->hovered_row == index && (shelf->hovered == TARGET_ROW || shelf->hovered == TARGET_ROW_REMOVE);
    ThemeColor fill = item->selected ? THEME_ACCENT_SUBTLE : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT;
    gfx_fill_rounded(row, 6, theme_color(fill));

    // 行首图标：落下的动画里它单独飞进来。
    GfxRect thumb = rect_of(row.left + 6, row.top + 5, 32, 32);
    if (motion->thumb_opacity > 0.001f)
    {
        float cx = (thumb.left + thumb.right) / 2, cy = (thumb.top + thumb.bottom) / 2;
        GfxMatrix matrix = gfx_matrix_scale(motion->thumb_scale, motion->thumb_scale, cx, cy);
        matrix = gfx_matrix_multiply(matrix, gfx_matrix_rotate(motion->thumb_angle, cx, cy));
        matrix = gfx_matrix_multiply(matrix, gfx_matrix_translate(motion->thumb_dx, motion->thumb_dy));
        gfx_push_transform(matrix);
        paint_item_picture(item, thumb, 20, motion->thumb_opacity);
        gfx_pop_transform();
    }

    float text_right = row.right - 6 - 26 - 4;
    if (motion->text_opacity > 0.001f)
    {
        float name_height = line_height(13, 400), detail_height = line_height(11, 400);
        float top = thumb.top + (32 - name_height - detail_height) / 2;
        float left = row.left + 6 + 36 + 6 + motion->text_dx;
        GfxTextStyle style = text_style(13, 400, GFX_ALIGN_START);
        gfx_text_draw(shelf_item_display_name(item), (GfxRect){ left, top, text_right + motion->text_dx, top + name_height }, &style,
            faded(theme_color(THEME_TEXT_PRIMARY), motion->text_opacity));
        style = text_style(11, 400, GFX_ALIGN_START);
        gfx_text_draw(shelf_item_detail(item), (GfxRect){ left, top + name_height, text_right + motion->text_dx, top + name_height + detail_height },
            &style, faded(theme_color(THEME_TEXT_SECONDARY), motion->text_opacity));
    }

    // 行尾的移除按钮，悬停在这一行上时才显示。
    if (hovered && !shelf->switching)
    {
        GfxRect remove = row_remove_rect(row);
        bool over = shelf->hovered == TARGET_ROW_REMOVE;
        bool down = over && shelf->pressed == TARGET_ROW_REMOVE;
        gfx_fill_rounded(remove, 4, theme_color(down ? THEME_SUBTLE_PRESSED : over ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT));
        GfxTextStyle style = glyph_style(10);
        gfx_text_draw(GLYPH_CLOSE, remove, &style, theme_color(down ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY));
    }
    if (layered) gfx_pop_layer();
    gfx_pop_transform();
}

static void paint_list(ShelfWindow *shelf, float opacity)
{
    const ShelfLayout *layout = &shelf->layout;
    GfxRect list = layout->list;
    // 列表里的内容只显示在列表范围以内。
    gfx_push_clip((GfxRect){ list.left + 1, list.top + 1, list.right - 1, list.bottom - 1 });
    if (opacity < 0.999f) gfx_push_opacity(opacity);
    for (int index = shelf->scroll_first > 0 ? shelf->scroll_first - 1 : 0; index < shelf->model->count; index++)
    {
        GfxRect row = shelf_row_rect(shelf, index);
        if (row.top > list.bottom + 60) break;
        paint_row(shelf, index, shelf->model->items[index]);
    }
    if (layout->scrollable)
    {
        float alpha = shelf->scroll_hovered || shelf->scroll_dragging ? 1.0f : 0.55f;
        gfx_fill_rounded(layout->scroll_thumb, 3, faded(theme_color(THEME_SCROLL_THUMB), alpha));
    }
    if (opacity < 0.999f) gfx_pop_layer();
    gfx_pop_clip();
}

/// 快速操作按钮栏：上面一条分隔线；只有两个操作时按钮上写名称，四个时只有图标。
static void paint_action_bar(ShelfWindow *shelf, float opacity)
{
    int count = shelf->actions.count;
    if (count == 0 || opacity <= 0.001f) return;
    const ShelfLayout *layout = &shelf->layout;
    gfx_fill_rect(layout->action_divider, faded(theme_color(THEME_DIVIDER), opacity));
    bool large = count <= 2;
    for (int index = 0; index < count; index++)
    {
        GfxRect button = layout->action_buttons[index];
        bool hovered = shelf->hovered == TARGET_ACTION && shelf->hovered_row == index;
        bool pressed = hovered && shelf->pressed == TARGET_ACTION && shelf->pressed_row == index;
        gfx_fill_rounded(button, 4, faded(theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT), opacity));
        Color text = faded(theme_color(pressed ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY), opacity);
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
        gfx_fill_rounded(tile, 10, theme_color(THEME_CARD));
        gfx_stroke_rounded(tile, 10, theme_color(THEME_CARD_BORDER), 1);
        if (active > 0.001f)
        {
            // 强调色的一层连边框一起盖住。
            GfxRect cover = { tile.left - 0.5f, tile.top - 0.5f, tile.right + 0.5f, tile.bottom + 0.5f };
            gfx_fill_rounded(cover, 10.5f, faded(theme_color(THEME_ACCENT), active));
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

static void paint_drop_overlay(ShelfWindow *shelf, float opacity)
{
    GfxRect zone = shelf->layout.drop_zone;
    float cx = (zone.left + zone.right) / 2, cy = (zone.top + zone.bottom) / 2;
    // 可以合并时整个放置区域轻轻弹一下。
    float pulse = (float)shelf->drop_zone_scale.value;
    bool pulsing = fabsf(pulse - 1) > 0.0005f;
    if (pulsing) gfx_push_transform(gfx_matrix_scale(pulse, pulse, cx, cy));
    // 合并前的蓄力：拖着别的置物架停在这里时，底色从中间涨满。
    float charge = opacity * (float)shelf->merge_charge_opacity.value;
    if (charge > 0.001f)
    {
        float scale = (float)shelf->merge_charge_scale.value;
        float half_width = (zone.right - zone.left) / 2 * scale, half_height = (zone.bottom - zone.top) / 2 * scale;
        gfx_fill_rounded((GfxRect){ cx - half_width, cy - half_height, cx + half_width, cy + half_height }, 10 * scale,
            faded(theme_color(THEME_ACCENT_SUBTLE), charge));
    }
    // 拖到快速操作上时虚线框和底色变淡，文字不变。
    float frame = opacity * (float)shelf->frame_opacity.value;
    gfx_fill_rounded(zone, 10, faded(theme_color(THEME_ACCENT_FAINT), frame));
    gfx_stroke_rounded_dashed(zone, 10, faded(theme_color(THEME_ACCENT), frame), 1.6f, 4, 3);
    float label = opacity * (float)shelf->drop_label_opacity.value;
    if (label > 0.001f)
    {
        GfxTextStyle icon = glyph_style(22);
        float glyph = gfx_text_measure(GLYPH_DOWNLOAD, &icon, 0).height;
        float text = line_height(12, 400);
        float top = (zone.top + zone.bottom - glyph - 6 - text) / 2;
        Color color = faded(theme_color(THEME_ACCENT_TEXT), label);
        gfx_text_draw(GLYPH_DOWNLOAD, (GfxRect){ zone.left, top, zone.right, top + glyph }, &icon, color);
        GfxTextStyle style = text_style(12, 400, GFX_ALIGN_CENTER);
        gfx_text_draw(S(shelf->drop_zone_text), (GfxRect){ zone.left, top + glyph + 6, zone.right, top + glyph + 6 + text }, &style, color);
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
    if (ui_window_has_backdrop(shelf->ui))
        gfx_fill_rect((GfxRect){ 0, 0, layout->width, layout->height }, theme_color(THEME_SHELF_ACRYLIC_TINT));

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
        if (show_list) paint_list(shelf, body);
        if (show_stack) paint_stack(shelf, body);
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

/// 数量或视图变化后更新界面：只是切换了堆叠和列表时播放切换动画，刚拖入文件时播放落下的动画。
/// 列表里多了几行、窗口要变高时先平滑长高再落下；列表里移除之后先补位，窗口等补完再变小。
void shelf_refresh_views(ShelfWindow *shelf)
{
    bool switched = !shelf->count_changed && shelf->shown_expanded != shelf->model->expanded;
    bool counted = shelf->count_changed;
    bool landing = shelf->landing, returning = shelf->returning;
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
        double lead = counted && animate && shelf->model->expanded && !shelf->hold_size ? shelf_grow_for_rows(shelf) : 0;
        if (!shelf->hold_size && lead == 0) shelf_update_window_size(shelf);
        // 不播动画时展开列表也停在最新的项目上。
        if (switched && shelf->model->expanded && shelf->model->count > 0) shelf_scroll_into_view(shelf, shelf->model->count - 1);
        bool played = false;
        if (returning && animate) played |= shelf_play_return(shelf, shelf->returning_x, shelf->returning_y, returning_items, returning_count, lead);
        if (landing && animate)
            played |= shelf->model->expanded ? shelf_play_list_landing(shelf, landing_x, landing_y, landing_added, lead)
                                             : shelf_play_landing(shelf, landing_x, landing_y, landing_added);
        if (lead > 0 && !played) shelf_finish_after(shelf, lead);
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
            clear_stack_summary(shelf);
            if (shelf->model->count != shelf->shown_count)
            {
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
    shelf->badge_scale = 1;
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
    options.backdrop = UI_BACKDROP_ACRYLIC;
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
    clear_stack_summary(shelf);
    free(shelf->tweens);
    free(shelf->removing);
    free(shelf->rows_before);
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
    return shelf->sliding || shelf->popping || shelf->motion_running || shelf->receiving > 0 || shelf->closing;
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
    // 复用正在滑入的置物架时停止动画，否则下一帧会把窗口拉回原来的位置。
    if (was_visible) shelf_stop_slide(shelf);
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
        shelf->surface_opacity = 0;
    }
    if (!was_visible)
    {
        power_hold(shelf, true);
        ui_window_set_topmost(shelf->ui, shelf->model->pinned);
        ui_window_show_with(shelf->ui, activate);
        center_on(shelf, center);
    }
    else if (activate) SetForegroundWindow(shelf->handle);
    if (!was_visible || moved) shelf_animate_in(shelf, center);
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
    // 内容里的动画停在这一帧；没做完的收尾（比如待移除的项目）在窗口关闭时补上。
    shelf_stop_motion(shelf);
    if (!anim_enabled() || !shelf_window_is_visible(shelf))
    {
        task_post(close_posted, shelf_window_retain(shelf));
        return;
    }
    shelf_start_leave(shelf);
}

void shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths)
{
    shelf_add_paths(shelf, paths);
}
