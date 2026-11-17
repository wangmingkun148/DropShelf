// 界面底座里的其余控件：等宽网格、滚动区、滑块、输入框和快捷键输入框。样式与 C# 版 Controls.xaml 里的相同。
#include "ui.h"
#include "ui_internal.h"
#include "hotkey.h"
#include "strings.h"

#include <imm.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float maximum(float first, float second)
{
    return first > second ? first : second;
}

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

/// 鼠标事件的坐标（窗口交给元素时已经换算成元素自己的）。
static void local_point(const UiElement *element, const UiMouseEvent *event, float *x, float *y)
{
    (void)element;
    *x = event->x;
    *y = event->y;
}

// ───────────── 等宽网格 ─────────────

typedef struct UiGrid
{
    UiElement base;
    int columns;
    float column_gap, row_gap;
    float row_height;
} UiGrid;

static float grid_column_width(const UiGrid *grid, float width)
{
    return maximum((width - grid->column_gap * (grid->columns - 1)) / grid->columns, 0);
}

static GfxSize grid_measure(UiElement *element, float available)
{
    UiGrid *grid = (UiGrid *)element;
    float column = grid_column_width(grid, available);
    GfxSize size = { available, 0 };
    int visible = 0;
    float row = 0;
    for (int index = 0; index < element->child_count; index++)
    {
        UiElement *child = element->children[index];
        if (child->hidden) continue;
        ui_measure(child, column);
        row = maximum(row, grid->row_height > 0 ? grid->row_height : child->desired.height + child->margin_top + child->margin_bottom);
        if (++visible % grid->columns == 0)
        {
            size.height += row + grid->row_gap;
            row = 0;
        }
    }
    if (visible % grid->columns != 0) size.height += row;
    else if (visible > 0) size.height -= grid->row_gap;
    return size;
}

static void grid_arrange(UiElement *element)
{
    UiGrid *grid = (UiGrid *)element;
    float column = grid_column_width(grid, element->bounds.right - element->bounds.left);
    float top = element->bounds.top;
    // 先量出每一行的高度，同一行的一样高。
    for (int start = 0; start < element->child_count;)
    {
        float row = 0;
        int end = start, counted = 0;
        for (; end < element->child_count && counted < grid->columns; end++)
        {
            UiElement *child = element->children[end];
            if (child->hidden) continue;
            row = maximum(row, grid->row_height > 0 ? grid->row_height : child->desired.height + child->margin_top + child->margin_bottom);
            counted++;
        }
        int slot = 0;
        for (int index = start; index < end; index++)
        {
            UiElement *child = element->children[index];
            if (child->hidden) continue;
            float left = element->bounds.left + slot * (column + grid->column_gap);
            ui_arrange(child, (GfxRect){ left + child->margin_left, top + child->margin_top, left + column - child->margin_right,
                top + row - child->margin_bottom });
            slot++;
        }
        top += row + grid->row_gap;
        start = end;
    }
}

static const UiElementType grid_type = { grid_measure, grid_arrange, NULL, NULL, NULL, false };

UiElement *ui_grid(int columns, float column_gap, float row_gap)
{
    UiGrid *grid = (UiGrid *)ui_element_new(&grid_type, sizeof(UiGrid));
    grid->columns = columns > 0 ? columns : 1;
    grid->column_gap = column_gap;
    grid->row_gap = row_gap;
    return &grid->base;
}

void ui_grid_set_row_height(UiElement *element, float height)
{
    ((UiGrid *)element)->row_height = height;
    ui_invalidate(element);
}

void ui_grid_set_columns(UiElement *element, int columns)
{
    ((UiGrid *)element)->columns = columns > 0 ? columns : 1;
    ui_invalidate(element);
}

// ───────────── 滚动区 ─────────────
// 滚动条宽 12，离右边 1、上下各 2；滑块平时宽 3、透明度 0.55，光标移上去时宽 6、不透明。

#define SCROLL_BAR_WIDTH 12.0f
#define SCROLL_WHEEL_STEP 48.0f     // 和 WPF 一样，滚一格是三行（每行 16）

typedef struct UiScroll
{
    UiElement base;
    float offset;
    float extent;           // 最多能滚多远
    bool bar_hidden;
    Anim bar_opacity;       // 翻牌后滚动条淡入
    bool bar_hovered;
    bool dragging;
    float drag_start_y, drag_start_offset;
} UiScroll;

static GfxSize scroll_measure(UiElement *element, float available)
{
    UiElement *content = element->children[0];
    ui_measure(content, available);
    return (GfxSize){ available, content->desired.height + content->margin_top + content->margin_bottom };
}

static void scroll_arrange(UiElement *element)
{
    UiScroll *scroll = (UiScroll *)element;
    UiElement *content = element->children[0];
    float viewport = element->bounds.bottom - element->bounds.top;
    float height = content->desired.height + content->margin_top + content->margin_bottom;
    scroll->extent = maximum(height - viewport, 0);
    scroll->offset = clampf(scroll->offset, 0, scroll->extent);
    // 内容不够高时撑满可见范围，内容里靠下的部分（牌堆）总是贴着底边。
    float top = element->bounds.top - scroll->offset;
    ui_arrange(content, (GfxRect){ element->bounds.left + content->margin_left, top + content->margin_top,
        element->bounds.right - content->margin_right, top + maximum(height, viewport) - content->margin_bottom });
}

static bool scroll_thumb(const UiScroll *scroll, GfxRect *thumb, GfxRect *track)
{
    const GfxRect *bounds = &scroll->base.bounds;
    float viewport = bounds->bottom - bounds->top;
    if (scroll->extent <= 0.5f || viewport <= 8) return false;
    *track = (GfxRect){ bounds->right - 1 - SCROLL_BAR_WIDTH, bounds->top + 2, bounds->right - 1, bounds->bottom - 2 };
    float length = track->bottom - track->top;
    float size = maximum(length * viewport / (viewport + scroll->extent), 24);
    float position = track->top + (length - size) * scroll->offset / scroll->extent;
    float width = scroll->bar_hovered || scroll->dragging ? 6.0f : 3.0f;
    float middle = (track->left + track->right) / 2;
    *thumb = (GfxRect){ middle - width / 2, position, middle + width / 2, position + size };
    return true;
}

static void scroll_paint_over(UiElement *element, double time)
{
    UiScroll *scroll = (UiScroll *)element;
    if (anim_update(&scroll->bar_opacity, time)) ui_window_request_frame(element->window);
    GfxRect thumb, track;
    if (scroll->bar_hidden || !scroll_thumb(scroll, &thumb, &track)) return;
    Color color = theme_color(THEME_SCROLL_THUMB);
    float opacity = (scroll->bar_hovered || scroll->dragging ? 1.0f : 0.55f) * (float)scroll->bar_opacity.value;
    color.a = (uint8_t)(color.a * opacity);
    gfx_fill_rounded(thumb, 3, color);
}

static void scroll_by(UiScroll *scroll, float offset)
{
    float next = clampf(offset, 0, scroll->extent);
    if (next == scroll->offset) return;
    scroll->offset = next;
    ui_invalidate(&scroll->base);
}

static void scroll_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiScroll *scroll = (UiScroll *)element;
    float x, y;
    local_point(element, event, &x, &y);
    GfxRect thumb = {0}, track = {0};
    bool has_bar = !scroll->bar_hidden && scroll_thumb(scroll, &thumb, &track);
    switch (event->kind)
    {
        case UI_MOUSE_WHEEL:
            scroll_by(scroll, scroll->offset - event->wheel * SCROLL_WHEEL_STEP);
            break;
        case UI_MOUSE_DOWN:
            if (!has_bar || event->right_button || x < track.left) break;
            if (y >= thumb.top && y < thumb.bottom)
            {
                scroll->dragging = true;
                scroll->drag_start_y = y;
                scroll->drag_start_offset = scroll->offset;
            }
            else
            {
                // 点在滑块上下的空处：翻一页。
                float page = element->bounds.bottom - element->bounds.top;
                scroll_by(scroll, scroll->offset + (y < thumb.top ? -page : page));
            }
            ui_window_repaint(element->window);
            break;
        case UI_MOUSE_MOVE:
            if (scroll->dragging && has_bar)
            {
                float travel = (track.bottom - track.top) - (thumb.bottom - thumb.top);
                if (travel > 0) scroll_by(scroll, scroll->drag_start_offset + (y - scroll->drag_start_y) * scroll->extent / travel);
            }
            else
            {
                bool hovered = has_bar && x >= track.left;
                if (hovered != scroll->bar_hovered)
                {
                    scroll->bar_hovered = hovered;
                    ui_window_repaint(element->window);
                }
            }
            break;
        case UI_MOUSE_UP:
            scroll->dragging = false;
            ui_window_repaint(element->window);
            break;
        case UI_MOUSE_LEAVE:
            if (!scroll->dragging && scroll->bar_hovered)
            {
                scroll->bar_hovered = false;
                ui_window_repaint(element->window);
            }
            break;
        default:
            break;
    }
}

static const UiElementType scroll_type = {
    scroll_measure, scroll_arrange, NULL, scroll_mouse, NULL, true, NULL, NULL, NULL, false, scroll_paint_over, IDC_ARROW, true
};

UiElement *ui_scroll(UiElement *content)
{
    UiScroll *scroll = (UiScroll *)ui_element_new(&scroll_type, sizeof(UiScroll));
    scroll->base.clip = true;
    anim_set(&scroll->bar_opacity, 1);
    ui_add(&scroll->base, content);
    return &scroll->base;
}

UiElement *ui_scroll_content(UiElement *scroll)
{
    return scroll->children[0];
}

void ui_scroll_to_top(UiElement *element)
{
    ui_scroll_set_offset(element, 0);
}

float ui_scroll_offset(UiElement *element)
{
    return ((UiScroll *)element)->offset;
}

void ui_scroll_set_offset(UiElement *element, float offset)
{
    UiScroll *scroll = (UiScroll *)element;
    if (offset < 0) offset = 0;
    if (scroll->offset == offset) return;
    scroll->offset = offset;
    ui_invalidate(element);
}

float ui_scroll_extent(UiElement *element)
{
    return ((UiScroll *)element)->extent;
}

bool ui_scroll_hits_bar(UiElement *element, float x, float y)
{
    UiScroll *scroll = (UiScroll *)element;
    GfxRect thumb, track;
    ui_element_to_local(element, &x, &y);
    return !scroll->bar_hidden && scroll_thumb(scroll, &thumb, &track) && x >= track.left && x < track.right && y >= track.top && y < track.bottom;
}

void ui_scroll_set_bar_hidden(UiElement *element, bool hidden, bool fade_in)
{
    UiScroll *scroll = (UiScroll *)element;
    scroll->bar_hidden = hidden;
    if (!hidden && fade_in)
    {
        anim_set(&scroll->bar_opacity, 0);
        anim_start(&scroll->bar_opacity, 1, 0.15, EASE_OF(EASE_CUBIC_OUT));
        ui_window_request_frame(element->window);
    }
    ui_window_repaint(element->window);
}

// ───────────── 滑块 ─────────────
// 切牌滑块：高 32，轨道是一排 10 张小牌，滑钮是一张强调色的切牌卡（宽 14 的范围里一张 7 × 27 的牌，悬停或拖动时抽高到 31）。
// 切牌卡越过哪张牌的中线，那张就翻成牌背，退回时再翻回牌面。和 C# style 版的 DeckSlider 对应。

#define SLIDER_THUMB 14.0f
#define SLIDER_CARDS 10
/// 翻一张牌：先压扁到 0，换面后再展开，各 0.13 秒。
#define SLIDER_FLIP_SECONDS 0.26

typedef struct UiSlider
{
    UiElement base;
    int minimum, maximum, value, step;
    bool dragging;
    bool read_only;
    UiSliderHandler changed;
    void *context;
    bool down[SLIDER_CARDS];        // 翻成牌背的（翻牌途中是要翻成的那一面）
    Anim flip[SLIDER_CARDS];        // 翻牌的进度：0 到 0.5 压扁原来的一面，0.5 到 1 展开新的一面
} UiSlider;

static GfxSize slider_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 32 };
}

static float slider_fraction(const UiSlider *slider)
{
    int range = slider->maximum - slider->minimum;
    return range > 0 ? (float)(slider->value - slider->minimum) / range : 0;
}

static float slider_thumb_center(const UiSlider *slider)
{
    const GfxRect *bounds = &slider->base.bounds;
    return bounds->left + SLIDER_THUMB / 2 + (bounds->right - bounds->left - SLIDER_THUMB) * slider_fraction(slider);
}

/// 牌按滑钮能走到的范围等距排开，第 i 张的中线正好是取值 i×10+5 时滑钮的位置。
static bool slider_card_down(const UiSlider *slider, int index)
{
    int range = slider->maximum - slider->minimum;
    return range > 0 && (double)(slider->value - slider->minimum) / range >= (index + 0.5) / SLIDER_CARDS;
}

/// 值变了：越过中线的牌翻面。animate 为 false 时直接换面；翻到一半又退回时从当前宽度接着走。
static void slider_update_cards(UiSlider *slider, bool animate)
{
    for (int index = 0; index < SLIDER_CARDS; index++)
    {
        bool down = slider_card_down(slider, index);
        if (down == slider->down[index]) continue;
        slider->down[index] = down;
        Anim *flip = &slider->flip[index];
        if (!animate)
        {
            anim_set(flip, 1);
            continue;
        }
        // 翻到一半退回：从镜像的进度接着走，宽度不跳。
        double from = flip->running ? 1 - flip->value : 0;
        anim_set(flip, from);
        anim_start(flip, 1, SLIDER_FLIP_SECONDS * (1 - from), EASE_OF(EASE_LINEAR));
    }
}

static void slider_paint(UiElement *element, double time)
{
    UiSlider *slider = (UiSlider *)element;
    GfxRect bounds = element->bounds;
    float middle = (bounds.top + bounds.bottom) / 2;
    float travel = bounds.right - bounds.left - SLIDER_THUMB;
    ThemePattern back = theme_pattern(THEME_PATTERN_STACK_EDGE);
    Color face = theme_color(THEME_SHELF_TILE), face_border = theme_color(THEME_SHELF_TILE_BORDER), back_border = theme_color(THEME_CARD_BACK_LINE);
    Color pip = theme_color(THEME_TEXT_TERTIARY);
    for (int index = 0; index < SLIDER_CARDS; index++)
    {
        Anim *flip = &slider->flip[index];
        if (anim_update(flip, time)) ui_window_request_frame(element->window);
        double progress = flip->value;
        // 前半段压扁原来的一面（先慢后快），后半段展开新的一面（先快后慢）。
        bool down = progress < 0.5 ? !slider->down[index] : slider->down[index];
        double t = progress < 0.5 ? progress / 0.5 : (progress - 0.5) / 0.5;
        float width = (float)(progress < 0.5 ? 1 - t * t : 1 - (1 - t) * (1 - t));
        if (progress >= 1) width = 1;
        float center = bounds.left + SLIDER_THUMB / 2 + travel * (index + 0.5f) / SLIDER_CARDS;
        if (width < 0.01f) continue;
        gfx_push_transform(gfx_matrix_scale(width, 1, center, middle));
        GfxRect card = { center - 6, middle - 8, center + 6, middle + 8 };
        if (down)
        {
            gfx_fill_rounded_pattern(card, 2, &back);
            gfx_stroke_rounded(card, 2, back_border, 1);
        }
        else
        {
            gfx_fill_rounded(card, 2, face);
            gfx_stroke_rounded(card, 2, face_border, 1);
            float diamond[] = { center, middle - 2.1f, center + 2.1f, middle, center, middle + 2.1f, center - 2.1f, middle };
            gfx_polygon(diamond, 4, pip, COLOR_TRANSPARENT, 0);
        }
        gfx_pop_transform();
    }
    // 滑钮：外面一圈设置行的底色把它和后面的小牌隔开，里面是强调色的牌。
    float x = slider_thumb_center(slider);
    bool raised = slider->dragging || (element->hovered && !slider->read_only);
    float half = raised ? 15.5f : 13.5f;
    gfx_fill_rounded((GfxRect){ x - 3.5f, middle - half, x + 3.5f, middle + half }, 3.5f, theme_color(THEME_SETTING_ROW));
    gfx_fill_rounded((GfxRect){ x - 2, middle - half + 1.5f, x + 2, middle + half - 1.5f }, 2, theme_color(THEME_ACCENT));
}

static void slider_set(UiSlider *slider, int value, bool dragging)
{
    if (value < slider->minimum) value = slider->minimum;
    if (value > slider->maximum) value = slider->maximum;
    if (value == slider->value) return;
    slider->value = value;
    slider_update_cards(slider, slider->base.window != NULL);
    ui_window_repaint(slider->base.window);
    if (slider->changed) slider->changed(value, dragging, slider->context);
}

static int slider_value_at(const UiSlider *slider, float x)
{
    const GfxRect *bounds = &slider->base.bounds;
    float travel = bounds->right - bounds->left - SLIDER_THUMB;
    float fraction = travel > 0 ? clampf((x - bounds->left - SLIDER_THUMB / 2) / travel, 0, 1) : 0;
    return slider->minimum + (int)lroundf(fraction * (slider->maximum - slider->minimum));
}

static void slider_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiSlider *slider = (UiSlider *)element;
    if (slider->read_only || event->right_button) return;
    float x, y;
    local_point(element, event, &x, &y);
    switch (event->kind)
    {
        case UI_MOUSE_DOWN:
        {
            // 按在轨道上时滑钮先跳到那里（不算拖动），然后一样可以接着拖。
            bool on_thumb = fabsf(x - slider_thumb_center(slider)) <= SLIDER_THUMB / 2;
            if (!on_thumb) slider_set(slider, slider_value_at(slider, x), false);
            slider->dragging = true;
            ui_window_repaint(element->window);
            break;
        }
        case UI_MOUSE_MOVE:
            if (slider->dragging) slider_set(slider, slider_value_at(slider, x), true);
            break;
        case UI_MOUSE_UP:
            if (!slider->dragging) break;
            slider->dragging = false;
            ui_window_repaint(element->window);
            // 松手时再通知一次，调用方知道拖动结束了。
            if (slider->changed) slider->changed(slider->value, false, slider->context);
            break;
        default:
            break;
    }
}

static bool slider_key(UiElement *element, unsigned key)
{
    UiSlider *slider = (UiSlider *)element;
    if (slider->read_only) return false;
    switch (key)
    {
        case VK_LEFT: case VK_DOWN: slider_set(slider, slider->value - slider->step, false); return true;
        case VK_RIGHT: case VK_UP: slider_set(slider, slider->value + slider->step, false); return true;
        case VK_PRIOR: slider_set(slider, slider->value + slider->step * 2, false); return true;
        case VK_NEXT: slider_set(slider, slider->value - slider->step * 2, false); return true;
        case VK_HOME: slider_set(slider, slider->minimum, false); return true;
        case VK_END: slider_set(slider, slider->maximum, false); return true;
        default: return false;
    }
}

static const UiElementType slider_type = { slider_measure, NULL, slider_paint, slider_mouse, NULL, true, slider_key, NULL, NULL, true };

UiElement *ui_slider(int minimum_value, int maximum_value, int value, int step, UiSliderHandler changed, void *context)
{
    UiSlider *slider = (UiSlider *)ui_element_new(&slider_type, sizeof(UiSlider));
    slider->minimum = minimum_value;
    slider->maximum = maximum_value;
    slider->value = value;
    slider->step = step > 0 ? step : 1;
    slider->changed = changed;
    slider->context = context;
    for (int index = 0; index < SLIDER_CARDS; index++)
    {
        slider->down[index] = slider_card_down(slider, index);
        anim_set(&slider->flip[index], 1);
    }
    return &slider->base;
}

void ui_slider_set_value(UiElement *element, int value)
{
    UiSlider *slider = (UiSlider *)element;
    if (slider->dragging || slider->value == value) return;
    slider->value = value;
    slider_update_cards(slider, element->window != NULL);
    ui_window_repaint(element->window);
}

int ui_slider_face_down_count(UiElement *element)
{
    UiSlider *slider = (UiSlider *)element;
    int count = 0;
    for (int index = 0; index < SLIDER_CARDS; index++) count += slider->down[index];
    return count;
}

int ui_slider_value(UiElement *element)
{
    return ((UiSlider *)element)->value;
}

void ui_slider_set_read_only(UiElement *element, bool read_only)
{
    UiSlider *slider = (UiSlider *)element;
    slider->read_only = read_only;
    element->hit_through = read_only;
}

// ───────────── 输入框 ─────────────
// 内边距 10,5,10,6，最小高 32，圆角 6；有焦点时底色变亮，底部一条 2 像素的强调色。

#define TEXTBOX_PADDING_LEFT 12.0f   // 10 加上文字自己的 2
#define TEXTBOX_PADDING_RIGHT 10.0f
#define CARET_BLINK 0.53

typedef struct UiTextbox
{
    UiElement base;
    WStr text;
    int caret, anchor;          // 光标和选中范围的另一端
    float scroll;               // 文字太长时向左挪了多少
    StoredText placeholder;
    int max_length;
    bool selecting;
    double blink_start;
    UiTextHandler changed;
    UiClickHandler submitted;
    void *context;
} UiTextbox;

static const GfxTextStyle textbox_style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static const wchar_t *textbox_chars(const UiTextbox *box)
{
    return box->text.data ? box->text.data : L"";
}

static GfxSize textbox_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 32 };
}

static GfxRect textbox_area(const UiTextbox *box)
{
    const GfxRect *bounds = &box->base.bounds;
    return (GfxRect){ bounds->left + TEXTBOX_PADDING_LEFT, bounds->top + 1, bounds->right - TEXTBOX_PADDING_RIGHT, bounds->bottom - 2 };
}

/// 文字太长时挪一挪，让光标始终看得见。
static void textbox_keep_caret_visible(UiTextbox *box)
{
    GfxRect area = textbox_area(box);
    float width = area.right - area.left;
    float caret = gfx_text_caret_x(textbox_chars(box), &textbox_style, box->caret);
    float total = gfx_text_caret_x(textbox_chars(box), &textbox_style, (int)box->text.length);
    if (caret - box->scroll > width - 2) box->scroll = caret - width + 2;
    if (caret - box->scroll < 0) box->scroll = caret;
    if (box->scroll > 0 && total - box->scroll < width - 2) box->scroll = maximum(total - width + 2, 0);
    if (box->scroll < 0) box->scroll = 0;
}

/// 输入法的候选窗跟着光标走。
static void textbox_place_ime(UiTextbox *box)
{
    HWND handle = ui_window_handle(box->base.window);
    if (!handle) return;
    HIMC ime = ImmGetContext(handle);
    if (!ime) return;
    GfxRect area = textbox_area(box);
    float x = area.left + gfx_text_caret_x(textbox_chars(box), &textbox_style, box->caret) - box->scroll;
    float y = box->base.bounds.top + 6;
    ui_element_to_window(&box->base, &x, &y);
    float scale = ui_window_scale(box->base.window);
    COMPOSITIONFORM composition = { CFS_POINT, { (LONG)(x * scale), (LONG)(y * scale) }, { 0 } };
    ImmSetCompositionWindow(ime, &composition);
    CANDIDATEFORM candidate = { 0, CFS_CANDIDATEPOS, { (LONG)(x * scale), (LONG)((box->base.bounds.bottom) * scale) }, { 0 } };
    ImmSetCandidateWindow(ime, &candidate);
    ImmReleaseContext(handle, ime);
}

static void textbox_paint(UiElement *element, double time)
{
    UiTextbox *box = (UiTextbox *)element;
    GfxRect bounds = element->bounds;
    ThemeColor fill = element->focused ? THEME_FLYOUT : element->hovered ? THEME_CONTROL_HOVER : THEME_CONTROL;
    gfx_fill_rounded(bounds, 6, theme_color(fill));
    gfx_stroke_rounded(bounds, 6, theme_color(THEME_CONTROL_BORDER), 1);
    if (element->focused) gfx_fill_rounded_corners((GfxRect){ bounds.left + 1, bounds.bottom - 2, bounds.right - 1, bounds.bottom }, 0, 5, theme_color(THEME_ACCENT));

    GfxRect area = textbox_area(box);
    const wchar_t *text = textbox_chars(box);
    gfx_push_clip((GfxRect){ area.left - 2, area.top, area.right, area.bottom });
    if (box->text.length == 0)
    {
        GfxTextStyle style = textbox_style;
        style.ellipsis = true;
        gfx_text_draw(ui_text_get(&box->placeholder), area, &style, theme_color(THEME_TEXT_TERTIARY));
    }
    else
    {
        if (element->focused && box->anchor != box->caret)
        {
            int start = box->anchor < box->caret ? box->anchor : box->caret;
            int end = box->anchor < box->caret ? box->caret : box->anchor;
            float left = area.left + gfx_text_caret_x(text, &textbox_style, start) - box->scroll;
            float right = area.left + gfx_text_caret_x(text, &textbox_style, end) - box->scroll;
            Color selection = theme_color(THEME_ACCENT);
            selection.a = 0x66;
            float middle = (area.top + area.bottom) / 2;
            gfx_fill_rect((GfxRect){ left, middle - 10, right, middle + 10 }, selection);
        }
        GfxRect line = { area.left - box->scroll, area.top, area.left - box->scroll + 100000, area.bottom };
        gfx_text_draw(text, line, &textbox_style, theme_color(element->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_PRIMARY));
    }
    if (element->focused)
    {
        double phase = fmod(time - box->blink_start, CARET_BLINK * 2);
        if (phase < CARET_BLINK)
        {
            float x = floorf(area.left + gfx_text_caret_x(text, &textbox_style, box->caret) - box->scroll) + 0.5f;
            float middle = (area.top + area.bottom) / 2;
            gfx_line(x, middle - 9, x, middle + 9, theme_color(THEME_TEXT_PRIMARY), 1);
        }
        // 光标闪烁：到下一次亮灭时再画一帧。
        ui_window_request_frame(element->window);
    }
    gfx_pop_clip();
}

static void textbox_restart_blink(UiTextbox *box)
{
    box->blink_start = frames_time();
    ui_window_repaint(box->base.window);
}

static void textbox_notify(UiTextbox *box)
{
    textbox_keep_caret_visible(box);
    textbox_place_ime(box);
    textbox_restart_blink(box);
    if (box->changed) box->changed(textbox_chars(box), box->context);
}

static void textbox_delete_range(UiTextbox *box, int start, int end)
{
    if (end <= start) return;
    memmove(box->text.data + start, box->text.data + end, (box->text.length - (size_t)end + 1) * sizeof(wchar_t));
    box->text.length -= (size_t)(end - start);
    box->caret = box->anchor = start;
}

static bool textbox_delete_selection(UiTextbox *box)
{
    if (box->anchor == box->caret) return false;
    int start = box->anchor < box->caret ? box->anchor : box->caret;
    int end = box->anchor < box->caret ? box->caret : box->anchor;
    textbox_delete_range(box, start, end);
    return true;
}

static void textbox_insert(UiTextbox *box, const wchar_t *insert, size_t length)
{
    textbox_delete_selection(box);
    if (box->max_length > 0 && box->text.length + length > (size_t)box->max_length)
        length = box->text.length >= (size_t)box->max_length ? 0 : (size_t)box->max_length - box->text.length;
    if (length == 0) return;
    WStr next = {0};
    wstr_append_n(&next, textbox_chars(box), (size_t)box->caret);
    wstr_append_n(&next, insert, length);
    wstr_append(&next, textbox_chars(box) + box->caret);
    wstr_free(&box->text);
    box->text = next;
    box->caret = box->anchor = box->caret + (int)length;
}

static void textbox_copy(UiTextbox *box)
{
    if (box->anchor == box->caret) return;
    int start = box->anchor < box->caret ? box->anchor : box->caret;
    int end = box->anchor < box->caret ? box->caret : box->anchor;
    HWND handle = ui_window_handle(box->base.window);
    if (!OpenClipboard(handle)) return;
    EmptyClipboard();
    size_t bytes = (size_t)(end - start + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory)
    {
        wchar_t *target = GlobalLock(memory);
        memcpy(target, box->text.data + start, bytes - sizeof(wchar_t));
        target[end - start] = 0;
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

static void textbox_paste(UiTextbox *box)
{
    HWND handle = ui_window_handle(box->base.window);
    if (!OpenClipboard(handle)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *text = data ? GlobalLock(data) : NULL;
    if (text)
    {
        // 只取第一行，换行和制表符不进单行输入框。
        size_t length = 0;
        while (text[length] && text[length] != L'\r' && text[length] != L'\n') length++;
        wchar_t *line = wcs_dup_n(text, length);
        for (size_t index = 0; index < length; index++)
            if (line[index] == L'\t') line[index] = L' ';
        GlobalUnlock(data);
        textbox_insert(box, line, length);
        free(line);
    }
    CloseClipboard();
}

static int word_left(const wchar_t *text, int index)
{
    while (index > 0 && wchar_is_white_space(text[index - 1])) index--;
    while (index > 0 && !wchar_is_white_space(text[index - 1])) index--;
    return index;
}

static int word_right(const wchar_t *text, int index, int length)
{
    while (index < length && !wchar_is_white_space(text[index])) index++;
    while (index < length && wchar_is_white_space(text[index])) index++;
    return index;
}

static bool textbox_key(UiElement *element, unsigned key)
{
    UiTextbox *box = (UiTextbox *)element;
    if (key & UI_KEY_ALT) return false;
    bool shift = GetKeyState(VK_SHIFT) < 0, control = GetKeyState(VK_CONTROL) < 0;
    int length = (int)box->text.length;
    const wchar_t *text = textbox_chars(box);
    int caret = box->caret;
    switch (key)
    {
        case VK_LEFT:
            if (!shift && box->anchor != box->caret) caret = box->anchor < box->caret ? box->anchor : box->caret;
            else caret = control ? word_left(text, caret) : caret > 0 ? caret - 1 : 0;
            break;
        case VK_RIGHT:
            if (!shift && box->anchor != box->caret) caret = box->anchor > box->caret ? box->anchor : box->caret;
            else caret = control ? word_right(text, caret, length) : caret < length ? caret + 1 : length;
            break;
        case VK_HOME: caret = 0; break;
        case VK_END: caret = length; break;
        case VK_BACK:
            if (!textbox_delete_selection(box) && box->caret > 0)
                textbox_delete_range(box, control ? word_left(text, box->caret) : box->caret - 1, box->caret);
            textbox_notify(box);
            return true;
        case VK_DELETE:
            if (!textbox_delete_selection(box) && box->caret < length)
                textbox_delete_range(box, box->caret, control ? word_right(text, box->caret, length) : box->caret + 1);
            textbox_notify(box);
            return true;
        case VK_RETURN:
            if (box->submitted) box->submitted(box->context);
            return box->submitted != NULL;
        case 'A':
            if (!control) return false;
            box->anchor = 0;
            box->caret = length;
            textbox_keep_caret_visible(box);
            textbox_restart_blink(box);
            return true;
        case 'C':
        case VK_INSERT:
            if (!control) return false;
            textbox_copy(box);
            return true;
        case 'X':
            if (!control) return false;
            textbox_copy(box);
            if (textbox_delete_selection(box)) textbox_notify(box);
            return true;
        case 'V':
            if (!control) return false;
            textbox_paste(box);
            textbox_notify(box);
            return true;
        default:
            // 能输入字符的键交给 WM_CHAR；Esc、Tab 等交给窗口。
            return false;
    }
    box->caret = caret;
    if (!shift) box->anchor = caret;
    textbox_keep_caret_visible(box);
    textbox_place_ime(box);
    textbox_restart_blink(box);
    return true;
}

static void textbox_character(UiElement *element, wchar_t character)
{
    UiTextbox *box = (UiTextbox *)element;
    // 控制字符（回车、退格、Ctrl+A 等）由 textbox_key 处理。
    if (character < 0x20 || character == 0x7F) return;
    textbox_insert(box, &character, 1);
    textbox_notify(box);
}

static void textbox_focus(UiElement *element, bool focused)
{
    UiTextbox *box = (UiTextbox *)element;
    if (focused)
    {
        textbox_place_ime(box);
        textbox_restart_blink(box);
    }
    else
    {
        box->anchor = box->caret;
        ui_window_repaint(element->window);
    }
}

static void textbox_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiTextbox *box = (UiTextbox *)element;
    float x, y;
    local_point(element, event, &x, &y);
    GfxRect area = textbox_area(box);
    int index = gfx_text_hit(textbox_chars(box), &textbox_style, x - area.left + box->scroll);
    switch (event->kind)
    {
        case UI_MOUSE_DOWN:
            if (event->right_button) break;
            box->caret = index;
            if (GetKeyState(VK_SHIFT) >= 0) box->anchor = index;
            box->selecting = true;
            textbox_keep_caret_visible(box);
            textbox_place_ime(box);
            textbox_restart_blink(box);
            break;
        case UI_MOUSE_MOVE:
            if (!box->selecting || index == box->caret) break;
            box->caret = index;
            textbox_keep_caret_visible(box);
            textbox_restart_blink(box);
            break;
        case UI_MOUSE_UP:
            box->selecting = false;
            break;
        default:
            break;
    }
}

static void textbox_destroy(UiElement *element)
{
    UiTextbox *box = (UiTextbox *)element;
    wstr_free(&box->text);
    ui_text_free(&box->placeholder);
}

static const UiElementType textbox_type = {
    textbox_measure, NULL, textbox_paint, textbox_mouse, textbox_destroy, true, textbox_key, textbox_character, textbox_focus, true, NULL, IDC_IBEAM
};

UiElement *ui_textbox(const wchar_t *text, UiText placeholder, int max_length, UiTextHandler changed, UiClickHandler submitted, void *context)
{
    UiTextbox *box = (UiTextbox *)ui_element_new(&textbox_type, sizeof(UiTextbox));
    wstr_append(&box->text, text ? text : L"");
    box->caret = box->anchor = (int)box->text.length;
    box->placeholder = ui_text_store(placeholder);
    box->max_length = max_length;
    box->changed = changed;
    box->submitted = submitted;
    box->context = context;
    return &box->base;
}

const wchar_t *ui_textbox_text(UiElement *element)
{
    return textbox_chars((UiTextbox *)element);
}

void ui_textbox_set_text(UiElement *element, const wchar_t *text)
{
    UiTextbox *box = (UiTextbox *)element;
    if (wcs_equal(textbox_chars(box), text ? text : L"")) return;
    wstr_free(&box->text);
    wstr_append(&box->text, text ? text : L"");
    box->caret = box->anchor = (int)box->text.length;
    box->scroll = 0;
    textbox_keep_caret_visible(box);
    ui_window_repaint(element->window);
}

void ui_textbox_type(UiElement *element, const wchar_t *text)
{
    UiTextbox *box = (UiTextbox *)element;
    if (wcs_equal(textbox_chars(box), text ? text : L"")) return;
    ui_textbox_set_text(element, text);
    textbox_notify(box);
}

void ui_textbox_set_placeholder(UiElement *element, UiText placeholder)
{
    UiTextbox *box = (UiTextbox *)element;
    ui_text_free(&box->placeholder);
    box->placeholder = ui_text_store(placeholder);
    ui_window_repaint(element->window);
}

const wchar_t *ui_textbox_placeholder(UiElement *element)
{
    return ui_text_get(&((UiTextbox *)element)->placeholder);
}

// ───────────── 快捷键输入框 ─────────────
// 最小宽 180、高 32；左边键盘图标和快捷键，右边清除按钮。录制时边框变成 2 像素的强调色。

typedef struct UiHotkeyBox
{
    UiElement base;
    wchar_t *gesture;
    bool recording;
    wchar_t *display;           // 录制中显示的文字（已经按下的修饰键）；NULL 时显示提示
    StringId message;           // 录制中的提示；STR_COUNT 表示用 display
    UiElement *clear;
    UiHotkeyHandler changed;
    void *context;
} UiHotkeyBox;

static const GfxTextStyle hotkey_text_style = { GFX_FONT_TEXT, 13, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_CENTER };
static const GfxTextStyle hotkey_glyph_style = { GFX_FONT_ICON, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static const wchar_t *hotkey_box_text(const UiHotkeyBox *box)
{
    if (box->recording) return box->message != STR_COUNT ? S(box->message) : box->display;
    return box->gesture && *box->gesture ? box->gesture : S(STR_HotkeyNone);
}

static GfxSize hotkey_box_measure(UiElement *element, float available)
{
    (void)available;
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    ui_measure(box->clear, 26);
    float text = gfx_text_measure(hotkey_box_text(box), &hotkey_text_style, 0).width;
    return (GfxSize){ maximum(180, 10 + 22 + text + 8 + 26 + 2), 32 };
}

static void hotkey_box_arrange(UiElement *element)
{
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    float middle = (element->bounds.top + element->bounds.bottom) / 2;
    ui_arrange(box->clear, (GfxRect){ element->bounds.right - 3 - 26, middle - 13, element->bounds.right - 3, middle + 13 });
}

static void hotkey_box_paint(UiElement *element, double time)
{
    (void)time;
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    GfxRect bounds = element->bounds;
    gfx_fill_rounded(bounds, 6, theme_color(element->hovered || box->clear->hovered ? THEME_CONTROL_HOVER : THEME_CONTROL));
    if (box->recording) gfx_stroke_rounded(bounds, 6, theme_color(THEME_ACCENT), 2);
    else gfx_stroke_rounded(bounds, 6, theme_color(THEME_CONTROL_BORDER), 1);
    gfx_text_draw(L"\xE765", (GfxRect){ bounds.left + 10, bounds.top + 1, bounds.left + 32, bounds.bottom }, &hotkey_glyph_style,
        theme_color(THEME_TEXT_SECONDARY));
    gfx_text_draw(hotkey_box_text(box), (GfxRect){ bounds.left + 32, bounds.top, bounds.right - 34, bounds.bottom - 1 }, &hotkey_text_style,
        theme_color(box->recording ? THEME_ACCENT_TEXT : THEME_TEXT_PRIMARY));
}

static void hotkey_box_start(UiHotkeyBox *box)
{
    box->recording = true;
    box->message = STR_HotkeyPrompt;
    ui_invalidate(&box->base);
}

static void hotkey_box_stop(UiHotkeyBox *box)
{
    if (!box->recording) return;
    box->recording = false;
    free(box->display);
    box->display = NULL;
    ui_invalidate(&box->base);
}

static void hotkey_box_commit(UiHotkeyBox *box, const wchar_t *gesture)
{
    hotkey_box_stop(box);
    ui_hotkey_box_set(&box->base, gesture);
    if (box->changed) box->changed(gesture, box->context);
}

static void hotkey_box_clear(void *context)
{
    hotkey_box_commit(context, L"");
}

static void hotkey_box_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    if (event->kind == UI_MOUSE_DOWN && !event->right_button) hotkey_box_start(box);
}

static unsigned current_modifiers(void)
{
    unsigned modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) modifiers |= MOD_WIN;
    return modifiers;
}

static bool hotkey_box_key(UiElement *element, unsigned key)
{
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    key &= ~UI_KEY_ALT;
    if (!box->recording)
    {
        if (key != VK_RETURN && key != VK_SPACE) return false;
        hotkey_box_start(box);
        return true;
    }
    unsigned modifiers = current_modifiers();
    if (key == VK_ESCAPE && modifiers == 0)
    {
        hotkey_box_stop(box);
        return true;
    }
    if ((key == VK_BACK || key == VK_DELETE) && modifiers == 0)
    {
        hotkey_box_commit(box, L"");
        return true;
    }
    // 按住的修饰键先显示出来，等主键。
    if (hotkey_is_modifier_key(key) || key == VK_PROCESSKEY)
    {
        HotkeyGesture preview = { modifiers, 0 };
        wchar_t *text = hotkey_to_text(preview);
        free(box->display);
        box->display = NULL;
        box->message = STR_HotkeyPrompt;
        if (*text)
        {
            WStr display = {0};
            wstr_append(&display, text);
            wstr_append(&display, L"+…");
            box->display = wstr_take(&display);
            box->message = STR_COUNT;
        }
        free(text);
        ui_invalidate(element);
        return true;
    }
    HotkeyGesture gesture = { modifiers, key };
    if (!hotkey_is_valid(gesture))
    {
        box->message = STR_HotkeyNeedsModifier;
        ui_invalidate(element);
        return true;
    }
    wchar_t *text = hotkey_to_text(gesture);
    hotkey_box_commit(box, text);
    free(text);
    return true;
}

static void hotkey_box_focus(UiElement *element, bool focused)
{
    if (!focused) hotkey_box_stop((UiHotkeyBox *)element);
}

static void hotkey_box_destroy(UiElement *element)
{
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    free(box->gesture);
    free(box->display);
}

static const UiElementType hotkey_box_type = {
    hotkey_box_measure, hotkey_box_arrange, hotkey_box_paint, hotkey_box_mouse, hotkey_box_destroy, true, hotkey_box_key, NULL, hotkey_box_focus, true
};

UiElement *ui_hotkey_box(const wchar_t *gesture, UiHotkeyHandler changed, void *context)
{
    UiHotkeyBox *box = (UiHotkeyBox *)ui_element_new(&hotkey_box_type, sizeof(UiHotkeyBox));
    box->gesture = wcs_dup(gesture ? gesture : L"");
    box->message = STR_HotkeyPrompt;
    box->changed = changed;
    box->context = context;
    box->base.cross = UI_CROSS_CENTER;
    box->clear = ui_button(UI_BUTTON_ICON, UI_LIT(L"\xE711"), hotkey_box_clear, box);
    ui_button_set_font_size(box->clear, 10);
    box->clear->width = box->clear->height = 26;
    ui_set_tooltip(box->clear, UI_STR(STR_ClearHotkey));
    ui_add(&box->base, box->clear);
    return &box->base;
}

void ui_hotkey_box_set(UiElement *element, const wchar_t *gesture)
{
    UiHotkeyBox *box = (UiHotkeyBox *)element;
    if (wcs_equal(box->gesture, gesture ? gesture : L"")) return;
    free(box->gesture);
    box->gesture = wcs_dup(gesture ? gesture : L"");
    ui_invalidate(element);
}

bool ui_hotkey_box_is_recording(UiElement *element)
{
    return ((UiHotkeyBox *)element)->recording;
}
