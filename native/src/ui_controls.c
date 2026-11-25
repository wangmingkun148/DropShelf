#include "ui.h"
#include "ui_internal.h"
#include "strings.h"

#include <stdlib.h>
#include <string.h>

// ───────────── 控件上的文字 ─────────────

static StoredText text_store(UiText text)
{
    return (StoredText){ text.string, text.string < 0 ? wcs_dup(text.literal ? text.literal : L"") : NULL };
}

static void text_free(StoredText *text)
{
    free(text->literal);
    text->literal = NULL;
}

/// 现在该显示的文字：文字表里的按当前语言现取。
static const wchar_t *text_get(const StoredText *text)
{
    return text->string >= 0 ? S((StringId)text->string) : text->literal;
}

StoredText ui_text_store(UiText text)
{
    return text_store(text);
}

void ui_text_free(StoredText *text)
{
    text_free(text);
}

const wchar_t *ui_text_get(const StoredText *text)
{
    return text_get(text);
}

// ───────────── 元素的公共部分 ─────────────

UiElement *ui_element_new(const UiElementType *type, size_t size)
{
    UiElement *element = xcalloc(1, size);
    element->type = type;
    element->opacity = 1;
    element->transform = gfx_matrix_identity();
    element->tooltip_string = -1;
    return element;
}

void ui_set_tooltip(UiElement *element, UiText text)
{
    free(element->tooltip_literal);
    element->tooltip_literal = NULL;
    element->tooltip_string = text.string;
    if (text.string < 0 && text.literal) element->tooltip_literal = wcs_dup(text.literal);
}

const wchar_t *ui_element_tooltip(const UiElement *element)
{
    for (; element; element = element->parent)
    {
        if (element->tooltip_string >= 0) return S((StringId)element->tooltip_string);
        if (element->tooltip_literal) return element->tooltip_literal;
    }
    return NULL;
}

bool ui_is_within(const UiElement *element, const UiElement *ancestor)
{
    for (; element; element = element->parent)
        if (element == ancestor) return true;
    return false;
}

void ui_clear_children(UiElement *parent)
{
    int count = parent->child_count;
    UiElement **children = parent->children;
    parent->children = NULL;
    parent->child_count = 0;
    for (int index = 0; index < count; index++)
    {
        children[index]->parent = NULL;
        ui_element_destroy(children[index]);
    }
    free(children);
    ui_invalidate(parent);
}

void ui_element_to_local(const UiElement *element, float *x, float *y)
{
    // 从最外层往里，一层层扣掉上级和它自己的变换（变换作用于元素本身和子元素）。
    const UiElement *chain[64];
    int depth = 0;
    for (const UiElement *ancestor = element; ancestor && depth < 64; ancestor = ancestor->parent) chain[depth++] = ancestor;
    for (int index = depth - 1; index >= 0; index--)
    {
        GfxMatrix inverse;
        if (chain[index]->transformed && gfx_matrix_invert(chain[index]->transform, &inverse)) gfx_matrix_apply(inverse, x, y);
    }
}

void ui_element_to_window(const UiElement *element, float *x, float *y)
{
    for (const UiElement *ancestor = element; ancestor; ancestor = ancestor->parent)
        if (ancestor->transformed) gfx_matrix_apply(ancestor->transform, x, y);
}

void ui_element_attach(UiElement *element, UiWindow *window)
{
    element->window = window;
    for (int index = 0; index < element->child_count; index++) ui_element_attach(element->children[index], window);
}

void ui_element_destroy(UiElement *element)
{
    if (!element) return;
    for (int index = 0; index < element->child_count; index++)
    {
        element->children[index]->parent = NULL;
        ui_element_destroy(element->children[index]);
    }
    free(element->children);
    ui_window_forget(element->window, element);
    if (element->type->destroy) element->type->destroy(element);
    free(element->tooltip_literal);
    free(element);
}

void ui_add(UiElement *parent, UiElement *child)
{
    parent->children = xrealloc(parent->children, (size_t)(parent->child_count + 1) * sizeof(UiElement *));
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    if (parent->window) ui_element_attach(child, parent->window);
    ui_invalidate(parent);
}

void ui_set_margin(UiElement *element, float left, float top, float right, float bottom)
{
    element->margin_left = left;
    element->margin_top = top;
    element->margin_right = right;
    element->margin_bottom = bottom;
}

void ui_set_disabled(UiElement *element, bool disabled)
{
    if (element->disabled == disabled) return;
    element->disabled = disabled;
    ui_window_repaint(element->window);
}

void ui_invalidate(UiElement *element)
{
    if (element && element->window) ui_window_invalidate(element->window);
}

static float maximum(float first, float second)
{
    return first > second ? first : second;
}

GfxSize ui_measure(UiElement *element, float available)
{
    GfxSize size = { 0, 0 };
    if (element->hidden)
    {
        element->desired = size;
        return size;
    }
    float inner = available - element->margin_left - element->margin_right;
    if (element->width > 0) inner = element->width;
    if (inner < 0) inner = 0;
    if (element->type->measure) size = element->type->measure(element, inner);
    if (element->width > 0) size.width = element->width;
    if (element->height > 0) size.height = element->height;
    size.width = maximum(size.width, element->minimum_width);
    size.height = maximum(size.height, element->minimum_height);
    element->desired = size;
    return size;
}

void ui_arrange(UiElement *element, GfxRect bounds)
{
    element->bounds = bounds;
    if (!element->hidden && element->type->arrange) element->type->arrange(element);
}

static GfxRect clip_bounds(const UiElement *element)
{
    const GfxRect *rect = &element->clip_rect;
    return rect->right > rect->left && rect->bottom > rect->top ? *rect : element->bounds;
}

void ui_paint(UiElement *element, double time)
{
    if (element->hidden || element->opacity <= 0.001f) return;
    if (element->transformed) gfx_push_transform(element->transform);
    bool faded = element->opacity < 0.999f;
    // 自己不画、只画子元素时，淡出和圆角裁剪合成一层：画出来一样，少一张中间位图。
    bool merged = faded && element->clip && element->clip_radius > 0 && !element->type->paint && !element->type->paint_over;
    if (faded && !merged) gfx_push_opacity(element->opacity);
    if (element->type->paint) element->type->paint(element, time);
    if (element->clip)
    {
        GfxRect clip = clip_bounds(element);
        if (element->clip_radius > 0) gfx_push_layer(clip, element->clip_radius, merged ? element->opacity : 1);
        else gfx_push_clip(clip);
    }
    if (!element->type->paints_children)
        for (int index = 0; index < element->child_count; index++) ui_paint(element->children[index], time);
    if (element->clip)
    {
        if (element->clip_radius > 0) gfx_pop_layer();
        else gfx_pop_clip();
    }
    if (element->type->paint_over) element->type->paint_over(element, time);
    if (faded && !merged) gfx_pop_layer();
    if (element->transformed) gfx_pop_transform();
}

static bool inside(GfxRect rect, float x, float y)
{
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

static UiElement *hit_test(UiElement *element, float x, float y, bool any)
{
    if (element->hidden || element->hit_through) return NULL;
    if (element->transformed)
    {
        GfxMatrix inverse;
        if (!gfx_matrix_invert(element->transform, &inverse)) return NULL;
        gfx_matrix_apply(inverse, &x, &y);
    }
    GfxRect bounds = element->bounds;
    bounds.top -= element->hit_extend_top;
    if (!inside(bounds, x, y)) return NULL;
    if (element->clip && !inside(clip_bounds(element), x, y)) return NULL;
    // 后加的子元素画在上面，先问它们。
    for (int index = element->child_count - 1; index >= 0; index--)
    {
        UiElement *hit = hit_test(element->children[index], x, y, any);
        if (hit) return hit;
    }
    return any || element->type->interactive ? element : NULL;
}

UiElement *ui_hit_test(UiElement *element, float x, float y)
{
    return hit_test(element, x, y, false);
}

UiElement *ui_hit_test_any(UiElement *element, float x, float y)
{
    return hit_test(element, x, y, true);
}

/// 元素连同外边距占多大。
static float outer_width(const UiElement *element)
{
    return element->hidden ? 0 : element->desired.width + element->margin_left + element->margin_right;
}

static float outer_height(const UiElement *element)
{
    return element->hidden ? 0 : element->desired.height + element->margin_top + element->margin_bottom;
}

// ───────────── 容器 ─────────────

typedef struct UiStack
{
    UiElement base;
    bool horizontal;
    float spacing;
    float padding_left, padding_top, padding_right, padding_bottom;
    ThemeColor fill, border;
    float radius;
} UiStack;

static int visible_children(const UiElement *element)
{
    int count = 0;
    for (int index = 0; index < element->child_count; index++)
        if (!element->children[index]->hidden) count++;
    return count;
}

static GfxSize stack_measure(UiElement *element, float available)
{
    UiStack *stack = (UiStack *)element;
    float inner = maximum(available - stack->padding_left - stack->padding_right, 0);
    int count = visible_children(element);
    float gaps = count > 1 ? stack->spacing * (count - 1) : 0;
    GfxSize size = { 0, 0 };
    if (!stack->horizontal)
    {
        for (int index = 0; index < element->child_count; index++)
        {
            UiElement *child = element->children[index];
            ui_measure(child, inner);
            size.width = maximum(size.width, outer_width(child));
            size.height += outer_height(child);
        }
        size.height += gaps;
    }
    else
    {
        // 先量不伸展的，剩下的宽度按份数分给伸展的（它们要知道自己的宽度才量得出高度，比如会换行的文字）。
        float used = gaps, total_grow = 0;
        for (int index = 0; index < element->child_count; index++)
        {
            UiElement *child = element->children[index];
            if (child->hidden) continue;
            if (child->grow > 0)
            {
                total_grow += child->grow;
                used += child->margin_left + child->margin_right;
                continue;
            }
            ui_measure(child, maximum(inner - used, 0));
            used += outer_width(child);
        }
        float rest = maximum(inner - used, 0);
        for (int index = 0; index < element->child_count; index++)
        {
            UiElement *child = element->children[index];
            if (child->hidden || child->grow <= 0) continue;
            ui_measure(child, rest * child->grow / total_grow + child->margin_left + child->margin_right);
            used += child->desired.width;
        }
        for (int index = 0; index < element->child_count; index++) size.height = maximum(size.height, outer_height(element->children[index]));
        // 有伸展的子元素时，这一行把可用的宽度占满。
        size.width = total_grow > 0 ? inner : used;
    }
    size.width += stack->padding_left + stack->padding_right;
    size.height += stack->padding_top + stack->padding_bottom;
    return size;
}

static void stack_arrange(UiElement *element)
{
    UiStack *stack = (UiStack *)element;
    GfxRect inner = {
        element->bounds.left + stack->padding_left, element->bounds.top + stack->padding_top,
        element->bounds.right - stack->padding_right, element->bounds.bottom - stack->padding_bottom
    };
    float inner_width = maximum(inner.right - inner.left, 0), inner_height = maximum(inner.bottom - inner.top, 0);
    int count = visible_children(element);
    float gaps = count > 1 ? stack->spacing * (count - 1) : 0;

    // 排列方向上：不伸展的用量出来的大小，剩下的按份数分给伸展的。
    float used = gaps, total_grow = 0;
    for (int index = 0; index < element->child_count; index++)
    {
        UiElement *child = element->children[index];
        if (child->hidden) continue;
        if (child->grow > 0)
        {
            total_grow += child->grow;
            used += stack->horizontal ? child->margin_left + child->margin_right : child->margin_top + child->margin_bottom;
        }
        else
        {
            used += stack->horizontal ? outer_width(child) : outer_height(child);
        }
    }
    float rest = maximum((stack->horizontal ? inner_width : inner_height) - used, 0);

    float cursor = stack->horizontal ? inner.left : inner.top;
    for (int index = 0; index < element->child_count; index++)
    {
        UiElement *child = element->children[index];
        if (child->hidden) continue;
        float main = stack->horizontal ? child->desired.width : child->desired.height;
        if (child->grow > 0) main = rest * child->grow / total_grow;

        // 另一个方向上：占满，或按量出来的大小靠一边、居中。
        float cross_space = stack->horizontal ? inner_height - child->margin_top - child->margin_bottom
                                              : inner_width - child->margin_left - child->margin_right;
        float cross_size = stack->horizontal ? child->desired.height : child->desired.width;
        float cross_start = stack->horizontal ? inner.top + child->margin_top : inner.left + child->margin_left;
        if (child->cross == UI_CROSS_STRETCH || cross_size > cross_space) cross_size = maximum(cross_space, 0);
        else if (child->cross == UI_CROSS_CENTER) cross_start += (cross_space - cross_size) / 2;
        else if (child->cross == UI_CROSS_END) cross_start += cross_space - cross_size;

        GfxRect bounds;
        if (stack->horizontal)
        {
            cursor += child->margin_left;
            bounds = (GfxRect){ cursor, cross_start, cursor + main, cross_start + cross_size };
            cursor += main + child->margin_right + stack->spacing;
        }
        else
        {
            cursor += child->margin_top;
            bounds = (GfxRect){ cross_start, cursor, cross_start + cross_size, cursor + main };
            cursor += main + child->margin_bottom + stack->spacing;
        }
        ui_arrange(child, bounds);
    }
}

static void stack_paint(UiElement *element, double time)
{
    (void)time;
    UiStack *stack = (UiStack *)element;
    gfx_fill_rounded(element->bounds, stack->radius, theme_color(stack->fill));
    gfx_stroke_rounded(element->bounds, stack->radius, theme_color(stack->border), 1);
}

static const UiElementType stack_type = { stack_measure, stack_arrange, stack_paint, NULL, NULL, false };

UiElement *ui_stack(bool horizontal, float spacing)
{
    UiStack *stack = (UiStack *)ui_element_new(&stack_type, sizeof(UiStack));
    stack->horizontal = horizontal;
    stack->spacing = spacing;
    stack->fill = stack->border = THEME_TRANSPARENT;
    return &stack->base;
}

void ui_stack_set_padding(UiElement *element, float left, float top, float right, float bottom)
{
    UiStack *stack = (UiStack *)element;
    stack->padding_left = left;
    stack->padding_top = top;
    stack->padding_right = right;
    stack->padding_bottom = bottom;
}

void ui_stack_set_background(UiElement *element, ThemeColor fill, ThemeColor border, float radius)
{
    UiStack *stack = (UiStack *)element;
    stack->fill = fill;
    stack->border = border;
    stack->radius = radius;
}

static const UiElementType spacer_type = { NULL, NULL, NULL, NULL, NULL, false };

UiElement *ui_spacer(void)
{
    UiElement *spacer = ui_element_new(&spacer_type, sizeof(UiElement));
    spacer->grow = 1;
    return spacer;
}

// ───────────── 文字 ─────────────

typedef struct UiLabel
{
    UiElement base;
    StoredText text;
    GfxTextStyle style;
    ThemeColor color;
} UiLabel;

static GfxSize label_measure(UiElement *element, float available)
{
    UiLabel *label = (UiLabel *)element;
    return gfx_text_measure(text_get(&label->text), &label->style, label->style.wrap ? available : 0);
}

static void label_paint(UiElement *element, double time)
{
    (void)time;
    UiLabel *label = (UiLabel *)element;
    // 上一级被禁用时文字跟着变淡。
    bool disabled = false;
    for (UiElement *ancestor = element; ancestor && !disabled; ancestor = ancestor->parent) disabled = ancestor->disabled;
    gfx_text_draw(text_get(&label->text), element->bounds, &label->style, theme_color(disabled ? THEME_TEXT_DISABLED : label->color));
}

static void label_destroy(UiElement *element)
{
    text_free(&((UiLabel *)element)->text);
}

static const UiElementType label_type = { label_measure, NULL, label_paint, NULL, label_destroy, false };

UiElement *ui_label(UiText text, UiTextKind kind)
{
    UiLabel *label = (UiLabel *)ui_element_new(&label_type, sizeof(UiLabel));
    label->text = text_store(text);
    label->color = THEME_TEXT_PRIMARY;
    label->style = (GfxTextStyle){ GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_START };
    switch (kind)
    {
        case UI_TEXT_TITLE: label->style.size = 28; label->style.weight = 600; break;
        case UI_TEXT_SUBTITLE: label->style.size = 20; label->style.weight = 600; break;
        case UI_TEXT_BODY_STRONG: label->style.weight = 600; break;
        case UI_TEXT_BODY: label->style.wrap = true; break;
        case UI_TEXT_CAPTION: label->style.size = 12; label->style.wrap = true; label->color = THEME_TEXT_SECONDARY; break;
        case UI_TEXT_GLYPH:
            label->style.font = GFX_FONT_ICON;
            label->style.size = 16;
            label->base.cross = UI_CROSS_CENTER;
            break;
    }
    return &label->base;
}

void ui_label_set_text(UiElement *element, UiText text)
{
    UiLabel *label = (UiLabel *)element;
    text_free(&label->text);
    label->text = text_store(text);
    ui_invalidate(element);
}

void ui_label_set_color(UiElement *element, ThemeColor color)
{
    ((UiLabel *)element)->color = color;
    ui_window_repaint(element->window);
}

void ui_label_set_size(UiElement *element, float size)
{
    ((UiLabel *)element)->style.size = size;
    ui_invalidate(element);
}

GfxTextStyle *ui_label_style(UiElement *element)
{
    return &((UiLabel *)element)->style;
}

const wchar_t *ui_label_text(UiElement *element)
{
    return text_get(&((UiLabel *)element)->text);
}

// ───────────── 按钮 ─────────────

typedef struct UiButton
{
    UiElement base;
    UiButtonKind kind;
    StoredText text;
    UiClickHandler clicked;
    void *context;
    wchar_t *glyph;             // 文字左边的图标
    float glyph_size;
    float font_size;            // 0 表示默认
    float padding_left, padding_top, padding_right, padding_bottom;
    ThemeColor color;           // THEME_COLOR_COUNT 表示按种类
} UiButton;

static bool button_is_glyph(UiButtonKind kind)
{
    return kind == UI_BUTTON_ICON || kind == UI_BUTTON_CAPTION || kind == UI_BUTTON_CAPTION_CLOSE;
}

static GfxTextStyle button_text_style(const UiButton *button)
{
    GfxTextStyle style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    if (button_is_glyph(button->kind))
    {
        style.font = GFX_FONT_ICON;
        style.size = button->kind == UI_BUTTON_ICON ? 16.0f : 10.0f;
    }
    if (button->font_size > 0) style.size = button->font_size;
    return style;
}

static const GfxTextStyle button_glyph_style = { GFX_FONT_ICON, 13, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static GfxSize button_measure(UiElement *element, float available)
{
    (void)available;
    UiButton *button = (UiButton *)element;
    if (button->kind == UI_BUTTON_ICON) return (GfxSize){ 32, 32 };
    if (button->kind == UI_BUTTON_CAPTION || button->kind == UI_BUTTON_CAPTION_CLOSE) return (GfxSize){ 46, 32 };
    GfxTextStyle style = button_text_style(button);
    GfxSize text = gfx_text_measure(text_get(&button->text), &style, 0);
    float glyph = button->glyph ? button->glyph_size + 8 : 0;
    // 内边距默认 12,5,12,6，最小高度 32。
    return (GfxSize){ text.width + glyph + button->padding_left + button->padding_right,
        maximum(text.height + button->padding_top + button->padding_bottom, 32) };
}

static void button_paint(UiElement *element, double time)
{
    (void)time;
    UiButton *button = (UiButton *)element;
    Color fill = COLOR_TRANSPARENT, border = COLOR_TRANSPARENT, text = theme_color(THEME_TEXT_PRIMARY);
    bool hovered = element->hovered && !element->disabled, pressed = element->pressed && element->hovered;
    float radius = theme_is_cards() ? 6.0f : 4.0f;
    switch (button->kind)
    {
        case UI_BUTTON_STANDARD:
            fill = theme_color(pressed ? THEME_CONTROL_PRESSED : hovered ? THEME_CONTROL_HOVER : THEME_CONTROL);
            border = theme_color(THEME_CONTROL_BORDER);
            if (pressed) text = theme_color(THEME_TEXT_SECONDARY);
            break;
        case UI_BUTTON_ACCENT:
            fill = theme_color(element->disabled ? THEME_CONTROL_ALT : pressed ? THEME_ACCENT_PRESSED : hovered ? THEME_ACCENT_HOVER : THEME_ACCENT);
            text = theme_color(THEME_TEXT_ON_ACCENT);
            break;
        case UI_BUTTON_SUBTLE:
        case UI_BUTTON_ICON:
            fill = theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT);
            if (pressed) text = theme_color(THEME_TEXT_SECONDARY);
            break;
        case UI_BUTTON_CAPTION:
            radius = 0;
            fill = theme_color(pressed ? THEME_SUBTLE_PRESSED : hovered ? THEME_SUBTLE_HOVER : THEME_TRANSPARENT);
            // 窗口不是当前窗口时标题栏按钮变淡。
            if (!ui_window_is_active(element->window)) text = theme_color(THEME_TEXT_TERTIARY);
            break;
        case UI_BUTTON_CAPTION_CLOSE:
            radius = 0;
            if (!ui_window_is_active(element->window)) text = theme_color(THEME_TEXT_TERTIARY);
            if (hovered || pressed)
            {
                fill = pressed ? COLOR_ARGB(0xC8, 0xC4, 0x2B, 0x1C) : COLOR_RGB(0xC4, 0x2B, 0x1C);
                text = COLOR_WHITE;
            }
            break;
    }
    if (button->color != THEME_COLOR_COUNT && !element->disabled && !pressed) text = theme_color(button->color);
    if (element->disabled) text = theme_color(THEME_TEXT_DISABLED);
    gfx_fill_rounded(element->bounds, radius, fill);
    gfx_stroke_rounded(element->bounds, radius, border, 1);
    GfxTextStyle style = button_text_style(button);
    GfxRect area = element->bounds;
    // 文字的内边距上 5 下 6，看起来比正中略高一点。
    if (!button_is_glyph(button->kind)) area.bottom -= button->padding_bottom - button->padding_top;
    if (button->glyph)
    {
        // 图标和文字作为一组居中。
        GfxSize label = gfx_text_measure(text_get(&button->text), &style, 0);
        float total = button->glyph_size + 8 + label.width;
        float left = (area.left + area.right - total) / 2;
        GfxTextStyle glyph_style = button_glyph_style;
        glyph_style.size = button->glyph_size;
        gfx_text_draw(button->glyph, (GfxRect){ left, area.top + 1, left + button->glyph_size + 8, area.bottom }, &glyph_style, text);
        area.left = left + button->glyph_size + 8;
        area.right = area.left + label.width + 1;
        style.align = GFX_ALIGN_START;
    }
    gfx_text_draw(text_get(&button->text), area, &style, text);
}

static void button_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiButton *button = (UiButton *)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button && !element->disabled && button->clicked)
        button->clicked(button->context);
}

static void button_destroy(UiElement *element)
{
    text_free(&((UiButton *)element)->text);
    free(((UiButton *)element)->glyph);
}

static const UiElementType button_type = { button_measure, NULL, button_paint, button_mouse, button_destroy, true };

UiElement *ui_button(UiButtonKind kind, UiText text, UiClickHandler clicked, void *context)
{
    UiButton *button = (UiButton *)ui_element_new(&button_type, sizeof(UiButton));
    button->kind = kind;
    button->text = text_store(text);
    button->clicked = clicked;
    button->context = context;
    button->padding_left = button->padding_right = 12;
    button->padding_top = 5;
    button->padding_bottom = 6;
    button->color = THEME_COLOR_COUNT;
    button->base.cross = kind == UI_BUTTON_CAPTION || kind == UI_BUTTON_CAPTION_CLOSE ? UI_CROSS_STRETCH : UI_CROSS_CENTER;
    return &button->base;
}

void ui_button_set_glyph(UiElement *element, const wchar_t *glyph, float size)
{
    UiButton *button = (UiButton *)element;
    free(button->glyph);
    button->glyph = wcs_dup(glyph);
    button->glyph_size = size > 0 ? size : 13;
    ui_invalidate(element);
}

void ui_button_set_font_size(UiElement *element, float size)
{
    ((UiButton *)element)->font_size = size;
    ui_invalidate(element);
}

void ui_button_set_padding(UiElement *element, float left, float top, float right, float bottom)
{
    UiButton *button = (UiButton *)element;
    button->padding_left = left;
    button->padding_top = top;
    button->padding_right = right;
    button->padding_bottom = bottom;
    ui_invalidate(element);
}

void ui_button_set_color(UiElement *element, ThemeColor color)
{
    ((UiButton *)element)->color = color;
    ui_window_repaint(element->window);
}

void ui_button_set_text(UiElement *element, UiText text)
{
    UiButton *button = (UiButton *)element;
    text_free(&button->text);
    button->text = text_store(text);
    ui_invalidate(element);
}

const wchar_t *ui_button_text(UiElement *element)
{
    return text_get(&((UiButton *)element)->text);
}

// ───────────── 开关 ─────────────
// 左边是“开/关”的文字，右边是 40×20 的滑轨；圆钮切换时滑过去（0.16 秒）。

#define TOGGLE_TRACK_WIDTH 40.0f
#define TOGGLE_TRACK_HEIGHT 20.0f
#define TOGGLE_TEXT_GAP 12.0f
#define TOGGLE_TRAVEL 20.0f

typedef struct UiToggle
{
    UiElement base;
    bool checked;
    Anim knob;              // 圆钮的位置：0 在左（关），1 在右（开）
    UiToggleHandler changed;
    void *context;
} UiToggle;

static const GfxTextStyle toggle_text_style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static float toggle_text_width(void)
{
    // 两种状态的文字取宽的那个，切换时开关不会左右跳。
    float on = gfx_text_measure(S(STR_ToggleOn), &toggle_text_style, 0).width;
    float off = gfx_text_measure(S(STR_ToggleOff), &toggle_text_style, 0).width;
    return maximum(maximum(on, off), 14);
}

static GfxSize toggle_measure(UiElement *element, float available)
{
    (void)element;
    (void)available;
    return (GfxSize){ toggle_text_width() + TOGGLE_TEXT_GAP + TOGGLE_TRACK_WIDTH, 24 };
}

static void toggle_paint(UiElement *element, double time)
{
    UiToggle *toggle = (UiToggle *)element;
    if (anim_update(&toggle->knob, time)) ui_window_request_frame(element->window);

    bool hovered = element->hovered && !element->disabled;
    GfxRect bounds = element->bounds;
    float middle = (bounds.top + bounds.bottom) / 2;
    GfxRect text = { bounds.left, bounds.top, bounds.right - TOGGLE_TRACK_WIDTH - TOGGLE_TEXT_GAP, bounds.bottom };
    gfx_text_draw(S(toggle->checked ? STR_ToggleOn : STR_ToggleOff), text, &toggle_text_style,
        theme_color(element->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_PRIMARY));

    GfxRect track = { bounds.right - TOGGLE_TRACK_WIDTH, middle - TOGGLE_TRACK_HEIGHT / 2, bounds.right, middle + TOGGLE_TRACK_HEIGHT / 2 };
    Color fill, border, knob;
    if (toggle->checked)
    {
        fill = theme_color(element->disabled ? THEME_TEXT_DISABLED : hovered ? THEME_ACCENT_HOVER : THEME_ACCENT);
        border = element->disabled ? COLOR_TRANSPARENT : theme_color(THEME_ACCENT);
        knob = theme_color(THEME_TEXT_ON_ACCENT);
    }
    else
    {
        fill = theme_color(THEME_CONTROL_ALT);
        border = theme_color(element->disabled ? THEME_TEXT_DISABLED : THEME_CONTROL_STRONG_BORDER);
        knob = theme_color(element->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_SECONDARY);
    }
    gfx_fill_rounded(track, TOGGLE_TRACK_HEIGHT / 2, fill);
    gfx_stroke_rounded(track, TOGGLE_TRACK_HEIGHT / 2, border, 1);
    // 圆钮直径 12，悬停时 14；圆心离滑轨左边 10，打开时再往右 20。
    float radius = hovered ? 7.0f : 6.0f;
    gfx_fill_ellipse(track.left + 10 + TOGGLE_TRAVEL * (float)toggle->knob.value, middle, radius, radius, knob);
}

static void toggle_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiToggle *toggle = (UiToggle *)element;
    if (event->kind != UI_MOUSE_UP || !event->inside || event->right_button || element->disabled) return;
    ui_toggle_set_checked(element, !toggle->checked);
    if (toggle->changed) toggle->changed(toggle->checked, toggle->context);
}

static const UiElementType toggle_type = { toggle_measure, NULL, toggle_paint, toggle_mouse, NULL, true };

UiElement *ui_toggle(bool checked, UiToggleHandler changed, void *context)
{
    UiToggle *toggle = (UiToggle *)ui_element_new(&toggle_type, sizeof(UiToggle));
    toggle->checked = checked;
    toggle->changed = changed;
    toggle->context = context;
    toggle->base.cross = UI_CROSS_CENTER;
    anim_set(&toggle->knob, checked ? 1 : 0);
    return &toggle->base;
}

void ui_toggle_set_checked(UiElement *element, bool checked)
{
    UiToggle *toggle = (UiToggle *)element;
    if (toggle->checked == checked) return;
    toggle->checked = checked;
    anim_start(&toggle->knob, checked ? 1 : 0, 0.16, EASE_OF(EASE_CUBIC_OUT));
    ui_window_request_frame(element->window);
    ui_window_repaint(element->window);
}

// ───────────── 分段选择 ─────────────
// 选中块垫在选项下面：切换时从原来的选项滑到新的选项，宽度跟着变，两个选项的文字颜色同时过渡（0.22 秒）。

#define SEGMENT_PADDING_X 14.0f
#define SEGMENT_INSET 3.0f      // 边框 1 加内边距 2

typedef struct UiSegmented
{
    UiElement base;
    StoredText *items;
    float *widths;          // 各个选项的宽度（含左右内边距）
    int count;
    int selected;
    int previous;           // 切换前选中的，动画从它那里滑过来
    int hovered_item;
    Anim slide;             // 0 在 previous，1 在 selected
    UiSelectHandler changed;
    void *context;
} UiSegmented;

static const GfxTextStyle segment_text_style = { GFX_FONT_TEXT, 13, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };

static GfxSize segmented_measure(UiElement *element, float available)
{
    (void)available;
    UiSegmented *segmented = (UiSegmented *)element;
    GfxSize size = { SEGMENT_INSET * 2, 0 };
    for (int index = 0; index < segmented->count; index++)
    {
        GfxSize text = gfx_text_measure(text_get(&segmented->items[index]), &segment_text_style, 0);
        segmented->widths[index] = text.width + SEGMENT_PADDING_X * 2;
        size.width += segmented->widths[index];
        size.height = maximum(size.height, text.height + 11 + SEGMENT_INSET * 2);
    }
    return size;
}

static GfxRect segment_bounds(const UiSegmented *segmented, int item)
{
    const GfxRect *bounds = &segmented->base.bounds;
    float left = bounds->left + SEGMENT_INSET;
    for (int index = 0; index < item && index < segmented->count; index++) left += segmented->widths[index];
    float width = item >= 0 && item < segmented->count ? segmented->widths[item] : 0;
    return (GfxRect){ left, bounds->top + SEGMENT_INSET, left + width, bounds->bottom - SEGMENT_INSET };
}

static int segment_at(const UiSegmented *segmented, float x)
{
    for (int index = 0; index < segmented->count; index++)
    {
        GfxRect bounds = segment_bounds(segmented, index);
        if (x >= bounds.left && x < bounds.right) return index;
    }
    return -1;
}

static void segmented_paint(UiElement *element, double time)
{
    UiSegmented *segmented = (UiSegmented *)element;
    if (anim_update(&segmented->slide, time)) ui_window_request_frame(element->window);
    float progress = (float)segmented->slide.value;

    // 简洁：浅底色加细边框；纸牌：只描一圈深色的边。
    bool cards = theme_is_cards();
    if (cards) gfx_stroke_rounded(element->bounds, 6, theme_color(THEME_CONTROL_STRONG_BORDER), 1);
    else
    {
        gfx_fill_rounded(element->bounds, 5, theme_color(THEME_CONTROL_ALT));
        gfx_stroke_rounded(element->bounds, 5, theme_color(THEME_CONTROL_BORDER), 1);
    }
    float inner_radius = cards ? 4.0f : 3.0f;

    // 选中块：在原来的和新的选项之间按进度过渡。
    if (segmented->selected >= 0)
    {
        GfxRect to = segment_bounds(segmented, segmented->selected);
        GfxRect from = segmented->previous >= 0 ? segment_bounds(segmented, segmented->previous) : to;
        GfxRect block = {
            from.left + (to.left - from.left) * progress, to.top,
            from.right + (to.right - from.right) * progress, to.bottom
        };
        gfx_fill_rounded(block, inner_radius, theme_color(element->disabled ? THEME_TEXT_DISABLED : THEME_ACCENT));
    }

    Color normal = theme_color(element->disabled ? THEME_TEXT_DISABLED : THEME_TEXT_PRIMARY);
    Color selected = theme_color(THEME_TEXT_ON_ACCENT);
    for (int index = 0; index < segmented->count; index++)
    {
        GfxRect bounds = segment_bounds(segmented, index);
        Color color = normal;
        if (index == segmented->selected) color = color_lerp(normal, selected, progress);
        else if (index == segmented->previous) color = color_lerp(selected, normal, progress);
        else if (index == segmented->hovered_item && element->hovered && !element->disabled)
            gfx_fill_rounded(bounds, inner_radius, theme_color(THEME_SUBTLE_HOVER));
        bounds.bottom -= 1;
        gfx_text_draw(text_get(&segmented->items[index]), bounds, &segment_text_style, color);
    }
}

static void segmented_mouse(UiElement *element, const UiMouseEvent *event)
{
    UiSegmented *segmented = (UiSegmented *)element;
    if (event->kind == UI_MOUSE_MOVE || event->kind == UI_MOUSE_ENTER || event->kind == UI_MOUSE_LEAVE)
    {
        int item = event->kind == UI_MOUSE_LEAVE ? -1 : segment_at(segmented, event->x);
        if (item != segmented->hovered_item)
        {
            segmented->hovered_item = item;
            ui_window_repaint(element->window);
        }
        return;
    }
    if (event->kind != UI_MOUSE_UP || !event->inside || event->right_button || element->disabled) return;
    int item = segment_at(segmented, event->x);
    if (item < 0 || item == segmented->selected) return;
    ui_segmented_set_selected(element, item);
    if (segmented->changed) segmented->changed(item, segmented->context);
}

static void segmented_destroy(UiElement *element)
{
    UiSegmented *segmented = (UiSegmented *)element;
    for (int index = 0; index < segmented->count; index++) text_free(&segmented->items[index]);
    free(segmented->items);
    free(segmented->widths);
}

static const UiElementType segmented_type = { segmented_measure, NULL, segmented_paint, segmented_mouse, segmented_destroy, true };

UiElement *ui_segmented(const UiText *items, int count, int selected, UiSelectHandler changed, void *context)
{
    UiSegmented *segmented = (UiSegmented *)ui_element_new(&segmented_type, sizeof(UiSegmented));
    segmented->items = xcalloc((size_t)count, sizeof(StoredText));
    segmented->widths = xcalloc((size_t)count, sizeof(float));
    for (int index = 0; index < count; index++) segmented->items[index] = text_store(items[index]);
    segmented->count = count;
    segmented->selected = selected;
    segmented->previous = -1;
    segmented->hovered_item = -1;
    segmented->changed = changed;
    segmented->context = context;
    segmented->base.cross = UI_CROSS_CENTER;
    anim_set(&segmented->slide, 1);
    return &segmented->base;
}

void ui_segmented_set_selected(UiElement *element, int selected)
{
    UiSegmented *segmented = (UiSegmented *)element;
    if (selected == segmented->selected || selected < 0 || selected >= segmented->count) return;
    segmented->previous = segmented->selected;
    segmented->selected = selected;
    // 第一次显示之前（还没排版）直接到位。
    bool laid_out = element->bounds.right > element->bounds.left;
    anim_set(&segmented->slide, 0);
    if (laid_out && segmented->previous >= 0) anim_start(&segmented->slide, 1, 0.22, EASE_STANDARD);
    else anim_set(&segmented->slide, 1);
    ui_window_request_frame(element->window);
    ui_window_repaint(element->window);
}

void ui_segmented_choose(UiElement *element, int selected)
{
    UiSegmented *segmented = (UiSegmented *)element;
    if (selected == segmented->selected || selected < 0 || selected >= segmented->count) return;
    ui_segmented_set_selected(element, selected);
    if (segmented->changed) segmented->changed(selected, segmented->context);
}

int ui_segmented_selected(UiElement *element)
{
    return ((UiSegmented *)element)->selected;
}

// ───────────── 设置卡片 ─────────────

UiElement *ui_settings_card(const wchar_t *glyph, UiText header, UiText description, UiElement *control, UiElement **description_label)
{
    if (description_label) *description_label = NULL;
    UiElement *card = ui_stack(true, 0);
    ui_stack_set_padding(card, 16, 11, 16, 11);
    // 纸牌风格下设置行是牌面上浅一点的一块（主窗口换风格时整个重建，这里按建的时候的风格取）。
    bool cards = theme_is_cards();
    ui_stack_set_background(card, cards ? THEME_SETTING_ROW : THEME_CARD, cards ? THEME_SETTING_ROW_BORDER : THEME_CARD_BORDER, 6);
    card->minimum_height = 66;
    ui_set_margin(card, 0, 0, 0, 4);

    if (glyph)
    {
        UiElement *icon = ui_label(UI_LIT(glyph), UI_TEXT_GLYPH);
        ui_label_set_size(icon, 20);
        ui_set_margin(icon, 4, 0, 20, 0);
        ui_add(card, icon);
    }
    UiElement *texts = ui_stack(false, 2);
    texts->grow = 1;
    texts->cross = UI_CROSS_CENTER;
    ui_set_margin(texts, 0, 0, 20, 0);
    ui_add(texts, ui_label(header, UI_TEXT_BODY));
    if (description.string >= 0 || description.literal)
    {
        UiElement *label = ui_label(description, UI_TEXT_CAPTION);
        ui_add(texts, label);
        if (description_label) *description_label = label;
    }
    ui_add(card, texts);
    if (control) ui_add(card, control);
    return card;
}
