// 弹出菜单：托盘图标的右键菜单，以及之后置物架的“更多”和右键菜单。外观与 C# 版的菜单样式相同。
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
#define ITEM_HEIGHT 32.0f
#define ITEM_GAP 1.0f           // 每项上下各留 1
#define ITEM_PADDING_LEFT 10.0f
#define ITEM_PADDING_RIGHT 12.0f
#define ICON_COLUMN 30.0f
#define GESTURE_GAP 28.0f
#define SEPARATOR_HEIGHT 9.0f   // 一条 1 高的线，上下各留 4

typedef struct MenuItem
{
    int id;
    wchar_t *text;
    wchar_t *glyph;
    wchar_t *gesture;
    bool separator;
    bool disabled;
    bool checked;
    float top, height;          // 在方框里的位置（相对方框内侧的顶边）
} MenuItem;

static UiWindow *menu_window;
static MenuItem *menu_items;
static int menu_count;
static int menu_highlighted = -1;
static UiMenuHandler menu_chosen;
static void *menu_context;
static Anim menu_fade;
static float box_width, box_height;
static bool menu_closing;

static const GfxTextStyle item_style = { GFX_FONT_TEXT, 14, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
static const GfxTextStyle gesture_style = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_END, GFX_ALIGN_CENTER };
static const GfxTextStyle glyph_style = { GFX_FONT_ICON, 16, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
static const GfxTextStyle check_style = { GFX_FONT_ICON, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static void free_items(void)
{
    for (int index = 0; index < menu_count; index++)
    {
        free(menu_items[index].text);
        free(menu_items[index].glyph);
        free(menu_items[index].gesture);
    }
    free(menu_items);
    menu_items = NULL;
    menu_count = 0;
}

/// 算出每一项的位置和方框的大小。
static void layout_items(void)
{
    float width = 0, top = 0;
    for (int index = 0; index < menu_count; index++)
    {
        MenuItem *item = &menu_items[index];
        item->top = top;
        if (item->separator)
        {
            item->height = SEPARATOR_HEIGHT;
        }
        else
        {
            item->height = ITEM_HEIGHT + ITEM_GAP * 2;
            float needed = ITEM_PADDING_LEFT + ICON_COLUMN + gfx_text_measure(item->text, &item_style, 0).width + ITEM_PADDING_RIGHT;
            if (item->gesture && *item->gesture) needed += GESTURE_GAP + gfx_text_measure(item->gesture, &gesture_style, 0).width;
            if (needed > width) width = needed;
        }
        top += item->height;
    }
    box_width = width + BOX_INSET * 2;
    if (box_width < BOX_MINIMUM_WIDTH) box_width = BOX_MINIMUM_WIDTH;
    box_height = top + BOX_INSET * 2;
}

static GfxRect item_bounds(const MenuItem *item)
{
    float left = MARGIN_LEFT + BOX_INSET, top = MARGIN_TOP + BOX_INSET + item->top;
    return (GfxRect){ left, top, MARGIN_LEFT + box_width - BOX_INSET, top + item->height };
}

static bool selectable(int index)
{
    return index >= 0 && index < menu_count && !menu_items[index].separator && !menu_items[index].disabled;
}

static int item_at(float x, float y)
{
    for (int index = 0; index < menu_count; index++)
    {
        GfxRect bounds = item_bounds(&menu_items[index]);
        if (x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) return selectable(index) ? index : -1;
    }
    return -1;
}

static bool inside_box(float x, float y)
{
    return x >= MARGIN_LEFT && x < MARGIN_LEFT + box_width && y >= MARGIN_TOP && y < MARGIN_TOP + box_height;
}

static void set_highlighted(int index)
{
    if (index == menu_highlighted) return;
    menu_highlighted = index;
    ui_window_invalidate(menu_window);
}

void ui_menu_dismiss(void)
{
    if (!menu_window || menu_closing) return;
    menu_closing = true;
    ui_window_close(menu_window);
}

static void choose(int index)
{
    if (!selectable(index)) return;
    // 先关菜单再做事：这一项要做的事可能又会打开窗口或菜单。
    int id = menu_items[index].id;
    UiMenuHandler chosen = menu_chosen;
    void *context = menu_context;
    ui_menu_dismiss();
    if (chosen) chosen(id, context);
}

static void menu_paint(UiWindow *window, void *context, double time)
{
    (void)context;
    if (anim_update(&menu_fade, time)) ui_window_request_frame(window);
    float opacity = (float)menu_fade.value;
    bool fading = opacity < 1;
    if (fading) gfx_push_opacity(opacity);

    GfxRect box = { MARGIN_LEFT, MARGIN_TOP, MARGIN_LEFT + box_width, MARGIN_TOP + box_height };
    gfx_shadow(box, BOX_RADIUS, 20, 0, 5, COLOR_ARGB(51, 0, 0, 0));
    gfx_fill_rounded(box, BOX_RADIUS, theme_color(THEME_FLYOUT));
    gfx_stroke_rounded(box, BOX_RADIUS, theme_color(THEME_FLYOUT_BORDER), 1);

    for (int index = 0; index < menu_count; index++)
    {
        const MenuItem *item = &menu_items[index];
        GfxRect bounds = item_bounds(item);
        if (item->separator)
        {
            // 分隔线横贯整个方框。
            float middle = (bounds.top + bounds.bottom) / 2;
            gfx_fill_rect((GfxRect){ box.left + 1, middle - 0.5f, box.right - 1, middle + 0.5f }, theme_color(THEME_DIVIDER));
            continue;
        }
        GfxRect row = { bounds.left, bounds.top + ITEM_GAP, bounds.right, bounds.bottom - ITEM_GAP };
        if (index == menu_highlighted) gfx_fill_rounded(row, 4, theme_color(THEME_SUBTLE_HOVER));
        Color color = theme_color(item->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_PRIMARY);
        float left = row.left + ITEM_PADDING_LEFT, right = row.right - ITEM_PADDING_RIGHT;
        GfxRect icon = { left, row.top, left + ICON_COLUMN, row.bottom };
        if (item->checked) gfx_text_draw(L"", icon, &check_style, color);
        else if (item->glyph) gfx_text_draw(item->glyph, icon, &glyph_style, color);
        if (item->gesture && *item->gesture)
        {
            Color gesture = theme_color(item->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_SECONDARY);
            gfx_text_draw(item->gesture, (GfxRect){ left + ICON_COLUMN, row.top, right, row.bottom }, &gesture_style, gesture);
            right -= GESTURE_GAP + gfx_text_measure(item->gesture, &gesture_style, 0).width;
        }
        gfx_text_draw(item->text, (GfxRect){ left + ICON_COLUMN, row.top, right, row.bottom - 1 }, &item_style, color);
    }
    if (fading) gfx_pop_layer();
}

static bool menu_mouse(UiWindow *window, void *context, const UiMouseEvent *event)
{
    (void)window;
    (void)context;
    switch (event->kind)
    {
        case UI_MOUSE_MOVE: set_highlighted(item_at(event->x, event->y)); break;
        case UI_MOUSE_LEAVE: set_highlighted(-1); break;
        case UI_MOUSE_DOWN:
            // 点在阴影那一圈上等于点到了菜单外面。
            if (!inside_box(event->x, event->y)) ui_menu_dismiss();
            break;
        case UI_MOUSE_UP: choose(item_at(event->x, event->y)); break;
        default: break;
    }
    return true;
}

/// 从 from 往 step 的方向找下一个能选的项，到头了绕回来。
static int next_selectable(int from, int step)
{
    for (int tries = 0, index = from; tries < menu_count; tries++)
    {
        index = (index + step + menu_count) % menu_count;
        if (selectable(index)) return index;
    }
    return -1;
}

static bool menu_key(UiWindow *window, void *context, unsigned key)
{
    (void)window;
    (void)context;
    switch (key)
    {
        case VK_ESCAPE: ui_menu_dismiss(); return true;
        case VK_DOWN: set_highlighted(next_selectable(menu_highlighted < 0 ? -1 : menu_highlighted, 1)); return true;
        case VK_UP: set_highlighted(next_selectable(menu_highlighted < 0 ? 0 : menu_highlighted, -1)); return true;
        case VK_RETURN:
        case VK_SPACE: choose(menu_highlighted); return true;
        default: return false;
    }
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
    menu_highlighted = -1;
    free_items();
}

bool ui_menu_is_open(void)
{
    return menu_window != NULL;
}

void ui_menu_show(const UiMenuItem *items, int count, int x, int y, UiMenuHandler chosen, void *context)
{
    if (menu_window)
    {
        // 上一个菜单还开着：先关掉（它的窗口同步销毁）。
        ui_menu_dismiss();
        if (menu_window) return;
    }
    if (count <= 0) return;

    menu_items = xcalloc((size_t)count, sizeof(MenuItem));
    menu_count = count;
    for (int index = 0; index < count; index++)
    {
        menu_items[index] = (MenuItem){
            .id = items[index].id, .text = wcs_dup(items[index].text ? items[index].text : L""), .glyph = wcs_dup(items[index].glyph),
            .gesture = wcs_dup(items[index].gesture), .separator = items[index].separator, .disabled = items[index].disabled,
            .checked = items[index].checked
        };
    }
    menu_chosen = chosen;
    menu_context = context;
    menu_highlighted = -1;
    if (!gfx_initialize())
    {
        free_items();
        return;
    }
    layout_items();

    UiWindowOptions options = { 0 };
    options.title = L"DropShelf";
    options.popup = true;
    options.paint = menu_paint;
    options.mouse = menu_mouse;
    options.key = menu_key;
    options.activated = menu_activated;
    options.closed = menu_closed;
    menu_window = ui_window_create(&options);
    if (!menu_window)
    {
        free_items();
        return;
    }

    // 先挪到光标所在的显示器上定好大小，再按那里的缩放比例和工作区决定朝哪边展开。
    float width = MARGIN_LEFT + box_width + MARGIN_RIGHT, height = MARGIN_TOP + box_height + MARGIN_BOTTOM;
    ui_window_place(menu_window, x, y, width, height);
    float scale = ui_window_scale(menu_window);
    POINT anchor = { x, y };
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    int box_w = (int)(box_width * scale + 0.5f), box_h = (int)(box_height * scale + 0.5f);
    // 默认方框的左上角在光标处；右边或下边放不下时翻到另一侧，还放不下就贴着工作区的边。
    int left = x + box_w > monitor.rcWork.right ? x - box_w : x;
    int top = y + box_h > monitor.rcWork.bottom ? y - box_h : y;
    if (left < monitor.rcWork.left) left = monitor.rcWork.left;
    if (top < monitor.rcWork.top) top = monitor.rcWork.top;
    ui_window_place(menu_window, left - (int)(MARGIN_LEFT * scale + 0.5f), top - (int)(MARGIN_TOP * scale + 0.5f), 0, 0);

    anim_set(&menu_fade, 0);
    anim_start(&menu_fade, 1, 0.12, EASE_OF(EASE_CUBIC_OUT));
    ui_window_show(menu_window);
    ui_window_request_frame(menu_window);
}
