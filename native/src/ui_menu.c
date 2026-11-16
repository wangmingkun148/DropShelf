// 弹出菜单：托盘图标的右键菜单，以及置物架的“更多”和右键菜单。外观与 C# 版的菜单样式相同。
// 可以带一层子菜单（“最近关闭的置物架”）：悬停一会儿或点按时在旁边展开。
// 主菜单和子菜单画在同一个窗口里，窗口大小一开始就按两者合起来的范围定好，展开子菜单时不用另开窗口，也不会抢走焦点。
#include "ui.h"

#include <stdlib.h>
#include <string.h>

// 尺寸（DIP）。菜单的方框四周留出画阴影的地方。
#define MARGIN_LEFT 12.0f
#define MARGIN_TOP 10.0f
#define MARGIN_RIGHT 12.0f
#define MARGIN_BOTTOM 16.0f
#define BOX_INSET 5.0f          // 边框 1 加内边距 4
#define BOX_RADIUS 8.0f
#define BOX_MINIMUM_WIDTH 180.0f
#define SUBMENU_MINIMUM_WIDTH 220.0f
#define SUBMENU_OVERLAP 3.0f    // 子菜单的方框压住主菜单右边一点（WPF 里弹出框往右偏 2，再减去主菜单的边框和内边距）
#define SUBMENU_RISE 6.0f       // 子菜单的方框比它的标题那一行高出这么多
#define ITEM_HEIGHT 32.0f
#define ITEM_GAP 1.0f           // 每项上下各留 1
#define ITEM_PADDING_LEFT 10.0f
#define ITEM_PADDING_RIGHT 12.0f
#define ICON_COLUMN 30.0f
#define GESTURE_GAP 28.0f
#define SEPARATOR_HEIGHT 9.0f   // 一条 1 高的线，上下各留 4
#define SUBMENU_DELAY 400       // 悬停多久展开或收起子菜单（毫秒，和系统的 MenuShowDelay 默认值相同）
#define SUBMENU_TIMER 1
#define GLYPH_CHEVRON L"\xE76C"

typedef struct Panel Panel;

typedef struct MenuItem
{
    int id;
    wchar_t *text;
    wchar_t *glyph;
    wchar_t *gesture;
    wchar_t *tooltip;
    float max_width;
    bool separator;
    bool disabled;
    bool checked;
    float top, height;          // 在方框里的位置（相对方框内侧的顶边）
    Panel *submenu;
} MenuItem;

struct Panel
{
    MenuItem *items;
    int count;
    float width, height;        // 方框的大小
    float left, top;            // 方框在窗口里的位置
    int highlighted;
    Anim fade;
};

static UiWindow *menu_window;
static Panel root;
static Panel *open_submenu;     // 展开着的子菜单
static int open_header = -1;    // 它在主菜单里是哪一项
static int pending_header = -1; // 计时到了要展开的那一项；-2 表示要收起
static UiMenuHandler menu_chosen;
static void *menu_context;
static bool menu_closing;
static const wchar_t *shown_tooltip;

static const GfxTextStyle item_style = { GFX_FONT_TEXT, 14, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
static const GfxTextStyle gesture_style = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_END, GFX_ALIGN_CENTER };
static const GfxTextStyle glyph_style = { GFX_FONT_ICON, 16, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
static const GfxTextStyle check_style = { GFX_FONT_ICON, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
static const GfxTextStyle chevron_style = { GFX_FONT_ICON, 11, 400, false, false, GFX_ALIGN_END, GFX_ALIGN_CENTER };

// ───────────── 内容 ─────────────

static void free_panel(Panel *panel)
{
    for (int index = 0; index < panel->count; index++)
    {
        MenuItem *item = &panel->items[index];
        free(item->text);
        free(item->glyph);
        free(item->gesture);
        free(item->tooltip);
        if (item->submenu)
        {
            free_panel(item->submenu);
            free(item->submenu);
        }
    }
    free(panel->items);
    *panel = (Panel){0};
    panel->highlighted = -1;
}

static void copy_items(Panel *panel, const UiMenuItem *items, int count, bool nested)
{
    panel->items = xcalloc((size_t)(count ? count : 1), sizeof(MenuItem));
    panel->count = count;
    panel->highlighted = -1;
    for (int index = 0; index < count; index++)
    {
        const UiMenuItem *source = &items[index];
        MenuItem *item = &panel->items[index];
        *item = (MenuItem){
            .id = source->id, .text = wcs_dup(source->text ? source->text : L""), .glyph = wcs_dup(source->glyph),
            .gesture = wcs_dup(source->gesture), .tooltip = wcs_dup(source->tooltip), .max_width = source->max_width,
            .separator = source->separator, .disabled = source->disabled, .checked = source->checked
        };
        // 只有一层子菜单。
        if (!nested && source->children && source->child_count > 0)
        {
            item->submenu = xcalloc(1, sizeof(Panel));
            copy_items(item->submenu, source->children, source->child_count, true);
        }
    }
}

static float text_width(const MenuItem *item)
{
    float width = gfx_text_measure(item->text, &item_style, 0).width;
    return item->max_width > 0 && width > item->max_width ? item->max_width : width;
}

/// 算出每一项的位置和方框的大小。
static void layout_panel(Panel *panel, float minimum_width)
{
    float width = 0, top = 0;
    for (int index = 0; index < panel->count; index++)
    {
        MenuItem *item = &panel->items[index];
        item->top = top;
        if (item->separator)
        {
            item->height = SEPARATOR_HEIGHT;
        }
        else
        {
            item->height = ITEM_HEIGHT + ITEM_GAP * 2;
            float needed = ITEM_PADDING_LEFT + ICON_COLUMN + text_width(item) + ITEM_PADDING_RIGHT;
            if (item->gesture && *item->gesture) needed += GESTURE_GAP + gfx_text_measure(item->gesture, &gesture_style, 0).width;
            else if (item->submenu) needed += GESTURE_GAP + gfx_text_measure(GLYPH_CHEVRON, &chevron_style, 0).width;
            if (needed > width) width = needed;
        }
        top += item->height;
    }
    panel->width = width + BOX_INSET * 2;
    if (panel->width < minimum_width) panel->width = minimum_width;
    panel->height = top + BOX_INSET * 2;
}

static GfxRect box_of(const Panel *panel)
{
    return (GfxRect){ panel->left, panel->top, panel->left + panel->width, panel->top + panel->height };
}

static GfxRect item_bounds(const Panel *panel, const MenuItem *item)
{
    float left = panel->left + BOX_INSET, top = panel->top + BOX_INSET + item->top;
    return (GfxRect){ left, top, panel->left + panel->width - BOX_INSET, top + item->height };
}

static bool selectable(const Panel *panel, int index)
{
    return index >= 0 && index < panel->count && !panel->items[index].separator && !panel->items[index].disabled;
}

static bool inside(GfxRect rect, float x, float y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

static int item_at(const Panel *panel, float x, float y)
{
    for (int index = 0; index < panel->count; index++)
        if (inside(item_bounds(panel, &panel->items[index]), x, y)) return selectable(panel, index) ? index : -1;
    return -1;
}

// ───────────── 状态 ─────────────

static void update_tooltip(void)
{
    const Panel *panel = open_submenu && open_submenu->highlighted >= 0 ? open_submenu : &root;
    const wchar_t *tooltip = panel->highlighted >= 0 ? panel->items[panel->highlighted].tooltip : NULL;
    if (tooltip == shown_tooltip) return;
    shown_tooltip = tooltip;
    ui_window_set_tooltip(menu_window, tooltip);
}

static void set_highlighted(Panel *panel, int index)
{
    if (index == panel->highlighted) return;
    panel->highlighted = index;
    update_tooltip();
    ui_window_invalidate(menu_window);
}

static void stop_timer(void)
{
    pending_header = -1;
    if (menu_window) KillTimer(ui_window_handle(menu_window), SUBMENU_TIMER);
}

static void schedule(int header)
{
    if (pending_header == header) return;
    pending_header = header;
    SetTimer(ui_window_handle(menu_window), SUBMENU_TIMER, SUBMENU_DELAY, NULL);
}

static void close_submenu(void)
{
    if (!open_submenu) return;
    open_submenu->highlighted = -1;
    open_submenu = NULL;
    open_header = -1;
    update_tooltip();
    ui_window_invalidate(menu_window);
}

static void open_submenu_at(int header, bool highlight_first)
{
    stop_timer();
    MenuItem *item = &root.items[header];
    if (!item->submenu || item->disabled) return;
    if (open_header != header)
    {
        close_submenu();
        open_submenu = item->submenu;
        open_header = header;
        open_submenu->highlighted = -1;
        anim_set(&open_submenu->fade, 0);
        anim_start(&open_submenu->fade, 1, 0.12, EASE_OF(EASE_CUBIC_OUT));
    }
    if (highlight_first)
        for (int index = 0; index < open_submenu->count; index++)
            if (selectable(open_submenu, index))
            {
                open_submenu->highlighted = index;
                break;
            }
    root.highlighted = header;
    update_tooltip();
    ui_window_invalidate(menu_window);
    ui_window_request_frame(menu_window);
}

void ui_menu_dismiss(void)
{
    if (!menu_window || menu_closing) return;
    menu_closing = true;
    stop_timer();
    ui_window_close(menu_window);
}

static void choose(const Panel *panel, int index)
{
    if (!selectable(panel, index)) return;
    // 先关菜单再做事：这一项要做的事可能又会打开窗口或菜单。
    int id = panel->items[index].id;
    UiMenuHandler chosen = menu_chosen;
    void *context = menu_context;
    ui_menu_dismiss();
    if (chosen) chosen(id, context);
}

// ───────────── 绘制 ─────────────

static void paint_panel(Panel *panel, double time, int open_index)
{
    if (anim_update(&panel->fade, time)) ui_window_request_frame(menu_window);
    float opacity = (float)panel->fade.value;
    bool fading = opacity < 1;
    if (fading) gfx_push_opacity(opacity);

    GfxRect box = box_of(panel);
    gfx_shadow(box, BOX_RADIUS, 20, 0, 5, COLOR_ARGB(51, 0, 0, 0));
    gfx_fill_rounded(box, BOX_RADIUS, theme_color(THEME_FLYOUT));
    gfx_stroke_rounded(box, BOX_RADIUS, theme_color(THEME_FLYOUT_BORDER), 1);

    for (int index = 0; index < panel->count; index++)
    {
        const MenuItem *item = &panel->items[index];
        GfxRect bounds = item_bounds(panel, item);
        if (item->separator)
        {
            // 分隔线横贯整个方框。
            float middle = (bounds.top + bounds.bottom) / 2;
            gfx_fill_rect((GfxRect){ box.left + 1, middle - 0.5f, box.right - 1, middle + 0.5f }, theme_color(THEME_DIVIDER));
            continue;
        }
        GfxRect row = { bounds.left, bounds.top + ITEM_GAP, bounds.right, bounds.bottom - ITEM_GAP };
        if (index == panel->highlighted || index == open_index) gfx_fill_rounded(row, 4, theme_color(THEME_SUBTLE_HOVER));
        Color color = theme_color(item->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_PRIMARY);
        float left = row.left + ITEM_PADDING_LEFT, right = row.right - ITEM_PADDING_RIGHT;
        GfxRect icon = { left, row.top, left + ICON_COLUMN, row.bottom };
        if (item->checked) gfx_text_draw(L"\xE73E", icon, &check_style, color);
        else if (item->glyph) gfx_text_draw(item->glyph, icon, &glyph_style, color);
        Color secondary = theme_color(item->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_SECONDARY);
        if (item->gesture && *item->gesture)
        {
            gfx_text_draw(item->gesture, (GfxRect){ left + ICON_COLUMN, row.top, right, row.bottom }, &gesture_style, secondary);
            right -= GESTURE_GAP + gfx_text_measure(item->gesture, &gesture_style, 0).width;
        }
        else if (item->submenu)
        {
            gfx_text_draw(GLYPH_CHEVRON, (GfxRect){ left + ICON_COLUMN, row.top, right, row.bottom }, &chevron_style, secondary);
            right -= GESTURE_GAP + gfx_text_measure(GLYPH_CHEVRON, &chevron_style, 0).width;
        }
        float text_right = left + ICON_COLUMN + text_width(item);
        if (text_right < right) right = text_right + 1;
        gfx_text_draw(item->text, (GfxRect){ left + ICON_COLUMN, row.top, right, row.bottom - 1 }, &item_style, color);
    }
    if (fading) gfx_pop_layer();
}

static void menu_paint(UiWindow *window, void *context, double time)
{
    (void)window;
    (void)context;
    paint_panel(&root, time, open_header);
    if (open_submenu) paint_panel(open_submenu, time, -1);
}

// ───────────── 鼠标和键盘 ─────────────

static void hover(float x, float y)
{
    if (open_submenu && inside(box_of(open_submenu), x, y))
    {
        // 移进了子菜单：不再收起它，主菜单上它的标题保持高亮。
        if (pending_header == -2) stop_timer();
        set_highlighted(open_submenu, item_at(open_submenu, x, y));
        return;
    }
    if (!inside(box_of(&root), x, y))
    {
        // 移到了菜单外面：子菜单开着就留着。
        if (!open_submenu) set_highlighted(&root, -1);
        else if (pending_header >= 0) stop_timer();
        return;
    }
    int index = item_at(&root, x, y);
    if (open_submenu) set_highlighted(open_submenu, -1);
    set_highlighted(&root, index >= 0 ? index : open_submenu ? open_header : -1);
    if (index >= 0 && root.items[index].submenu)
    {
        if (index != open_header) schedule(index);
        else stop_timer();
    }
    else if (open_submenu) schedule(-2);
    else stop_timer();
}

static bool menu_mouse(UiWindow *window, void *context, const UiMouseEvent *event)
{
    (void)window;
    (void)context;
    switch (event->kind)
    {
        case UI_MOUSE_MOVE: hover(event->x, event->y); break;
        case UI_MOUSE_LEAVE:
            if (!open_submenu) set_highlighted(&root, -1);
            else set_highlighted(open_submenu, -1);
            break;
        case UI_MOUSE_DOWN:
        {
            // 点在阴影那一圈上（或两个方框之外的空处）等于点到了菜单外面。
            bool in_root = inside(box_of(&root), event->x, event->y);
            bool in_submenu = open_submenu && inside(box_of(open_submenu), event->x, event->y);
            if (!in_root && !in_submenu) ui_menu_dismiss();
            break;
        }
        case UI_MOUSE_UP:
            if (open_submenu && inside(box_of(open_submenu), event->x, event->y))
            {
                choose(open_submenu, item_at(open_submenu, event->x, event->y));
                break;
            }
            {
                int index = item_at(&root, event->x, event->y);
                if (index >= 0 && root.items[index].submenu) open_submenu_at(index, false);
                else choose(&root, index);
            }
            break;
        default: break;
    }
    return true;
}

/// 从 from 往 step 的方向找下一个能选的项，到头了绕回来。
static int next_selectable(const Panel *panel, int from, int step)
{
    for (int tries = 0, index = from; tries < panel->count; tries++)
    {
        index = (index + step + panel->count) % panel->count;
        if (selectable(panel, index)) return index;
    }
    return -1;
}

static bool menu_key(UiWindow *window, void *context, unsigned key)
{
    (void)window;
    (void)context;
    // 子菜单里有高亮的项时，方向键和回车作用于子菜单。
    Panel *panel = open_submenu && open_submenu->highlighted >= 0 ? open_submenu : &root;
    switch (key)
    {
        case VK_ESCAPE:
        case VK_LEFT:
        {
            // 子菜单开着时先收起子菜单，回到它的标题上；Esc 再按一次才关掉整个菜单。
            if (open_submenu)
            {
                int header = open_header;
                close_submenu();
                set_highlighted(&root, header);
            }
            else if (key == VK_ESCAPE) ui_menu_dismiss();
            return true;
        }
        case VK_RIGHT:
            if (panel == &root && root.highlighted >= 0 && root.items[root.highlighted].submenu) open_submenu_at(root.highlighted, true);
            return true;
        case VK_DOWN:
            if (panel == &root && open_submenu) close_submenu();
            set_highlighted(panel, next_selectable(panel, panel->highlighted < 0 ? -1 : panel->highlighted, 1));
            return true;
        case VK_UP:
            if (panel == &root && open_submenu) close_submenu();
            set_highlighted(panel, next_selectable(panel, panel->highlighted < 0 ? 0 : panel->highlighted, -1));
            return true;
        case VK_RETURN:
        case VK_SPACE:
            if (panel == &root && root.highlighted >= 0 && root.items[root.highlighted].submenu) open_submenu_at(root.highlighted, true);
            else choose(panel, panel->highlighted);
            return true;
        default: return false;
    }
}

static bool menu_message(UiWindow *window, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    (void)window, (void)context, (void)lparam;
    if (message != WM_TIMER || wparam != SUBMENU_TIMER) return false;
    *result = 0;
    int header = pending_header;
    stop_timer();
    if (header == -2) close_submenu();
    else if (header >= 0) open_submenu_at(header, false);
    return true;
}

static void menu_activated(UiWindow *window, void *context, bool active)
{
    (void)window;
    (void)context;
    // 点到了别处，或切到了别的程序。
    if (!active) ui_menu_dismiss();
}

static void menu_closed(UiWindow *window, void *context)
{
    (void)context;
    if (window != menu_window) return;
    menu_window = NULL;
    menu_closing = false;
    open_submenu = NULL;
    open_header = pending_header = -1;
    shown_tooltip = NULL;
    free_panel(&root);
}

bool ui_menu_is_open(void)
{
    return menu_window != NULL;
}

// ───────────── 打开 ─────────────

void ui_menu_show(const UiMenuItem *items, int count, int x, int y, UiMenuHandler chosen, void *context)
{
    if (menu_window)
    {
        // 上一个菜单还开着：先关掉（它的窗口同步销毁）。
        ui_menu_dismiss();
        if (menu_window) return;
    }
    if (count <= 0) return;

    copy_items(&root, items, count, false);
    menu_chosen = chosen;
    menu_context = context;
    open_submenu = NULL;
    open_header = pending_header = -1;
    shown_tooltip = NULL;
    if (!gfx_initialize())
    {
        free_panel(&root);
        return;
    }
    layout_panel(&root, BOX_MINIMUM_WIDTH);
    for (int index = 0; index < root.count; index++)
        if (root.items[index].submenu) layout_panel(root.items[index].submenu, SUBMENU_MINIMUM_WIDTH);

    UiWindowOptions options = { 0 };
    options.title = L"DropShelf";
    options.popup = true;
    options.paint = menu_paint;
    options.mouse = menu_mouse;
    options.key = menu_key;
    options.activated = menu_activated;
    options.closed = menu_closed;
    options.message = menu_message;
    menu_window = ui_window_create(&options);
    if (!menu_window)
    {
        free_panel(&root);
        return;
    }

    // 先挪到光标所在的显示器上，再按那里的缩放比例和工作区决定朝哪边展开。
    ui_window_place(menu_window, x, y, MARGIN_LEFT + root.width + MARGIN_RIGHT, MARGIN_TOP + root.height + MARGIN_BOTTOM);
    float scale = ui_window_scale(menu_window);
    POINT anchor = { x, y };
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT work = monitor.rcWork;
    int box_w = (int)(root.width * scale + 0.5f), box_h = (int)(root.height * scale + 0.5f);
    // 默认方框的左上角在光标处；右边或下边放不下时翻到另一侧，还放不下就贴着工作区的边。
    int left = x + box_w > work.right ? x - box_w : x;
    int top = y + box_h > work.bottom ? y - box_h : y;
    if (left < work.left) left = work.left;
    if (top < work.top) top = work.top;

    // 子菜单的位置（相对主菜单方框的左上角，DIP）：默认在右边，放不下就放左边；下面放不下时往上挪。
    float work_left = (work.left - left) / scale, work_right = (work.right - left) / scale;
    float work_top = (work.top - top) / scale, work_bottom = (work.bottom - top) / scale;
    float min_x = 0, min_y = 0, max_x = root.width, max_y = root.height;
    for (int index = 0; index < root.count; index++)
    {
        Panel *submenu = root.items[index].submenu;
        if (!submenu) continue;
        float sub_left = root.width - SUBMENU_OVERLAP;
        if (sub_left + submenu->width > work_right && -(submenu->width - SUBMENU_OVERLAP) >= work_left) sub_left = -(submenu->width - SUBMENU_OVERLAP);
        float sub_top = BOX_INSET + root.items[index].top + ITEM_GAP - SUBMENU_RISE;
        if (sub_top + submenu->height > work_bottom) sub_top = work_bottom - submenu->height;
        if (sub_top < work_top) sub_top = work_top;
        submenu->left = sub_left;
        submenu->top = sub_top;
        if (sub_left < min_x) min_x = sub_left;
        if (sub_top < min_y) min_y = sub_top;
        if (sub_left + submenu->width > max_x) max_x = sub_left + submenu->width;
        if (sub_top + submenu->height > max_y) max_y = sub_top + submenu->height;
    }
    // 方框在窗口里的位置：最左上的方框留出阴影的边距。
    float origin_x = MARGIN_LEFT - min_x, origin_y = MARGIN_TOP - min_y;
    root.left = origin_x;
    root.top = origin_y;
    for (int index = 0; index < root.count; index++)
    {
        Panel *submenu = root.items[index].submenu;
        if (!submenu) continue;
        submenu->left += origin_x;
        submenu->top += origin_y;
    }
    float width = origin_x + max_x + MARGIN_RIGHT, height = origin_y + max_y + MARGIN_BOTTOM;
    ui_window_place(menu_window, left - (int)(origin_x * scale + 0.5f), top - (int)(origin_y * scale + 0.5f), width, height);

    anim_set(&root.fade, 0);
    anim_start(&root.fade, 1, 0.12, EASE_OF(EASE_CUBIC_OUT));
    ui_window_show(menu_window);
    ui_window_request_frame(menu_window);
}
