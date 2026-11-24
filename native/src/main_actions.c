// 快速操作页：置物架上显示的几个位置和更多操作。把操作卡片拖到位置上替换或交换；自定义操作拖到窗口底部删除。
// 和 C# 版 MainWindow.ActionDrag.cs、SettingsViewModel 里的快速操作部分对应。
#include "main_window_internal.h"
#include "lang.h"
#include "quick_action.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DRAG_THRESHOLD 4.0f
#define DELETE_ZONE_HEIGHT 120.0f
/// 内置操作拖进底部区域时最多陷进去的深度。
#define LOCKED_SINK 28.0f

typedef struct ActionCard
{
    UiElement base;
    QuickAction action;
    int slot;                   // 置物架上的位置；-1 是“更多操作”里的
    wchar_t *glyph_text;
    wchar_t *title;
    wchar_t *description;
    StringId position;
    UiElement *edit, *remove;   // 自定义操作的编辑、删除按钮，悬停时显示
    bool drop_target;
    StringId drop_hint;
} ActionCard;

static UiElement *slots_section, *slots_grid, *more_list, *add_button;
static wchar_t *actions_key;

// 拖动
static ActionCard *pressed_card;
static float press_x, press_y;
static bool dragging;
static ActionCard *drop_target;
static bool over_zone;
static float ghost_x, ghost_y;          // 跟随光标的卡片左上角（窗口坐标）
static Anim zone_opacity, glow_opacity, hint_opacity;
static Anim ghost_opacity, ghost_bounce;
static bool ghost_visible, ghost_bouncing;
static bool zone_custom;                // 拖着的是自定义操作（红色、可以删除），否则是内置操作（灰色）
static wchar_t *ghost_glyph, *ghost_title;

// ───────────── 修改设置 ─────────────

typedef struct AssignChange
{
    int index;
    const wchar_t *id;
} AssignChange;

/// 把操作放到指定位置。已在其他位置的操作与原位置的操作互换，避免同一个操作出现两次。
static void change_assign(AppSettings *settings, void *context)
{
    const AssignChange *change = context;
    quick_action_normalize_slots(&settings->quick_action_slots, settings->custom_actions, settings->custom_action_count);
    StrList *slots = &settings->quick_action_slots;
    if (change->index < 0 || change->index >= slots->count) return;
    int existing = strlist_index_of(slots, change->id, false);
    if (existing == change->index) return;
    wchar_t *id = wcs_dup(change->id);
    if (existing >= 0)
    {
        free(slots->items[existing]);
        slots->items[existing] = slots->items[change->index];
    }
    else free(slots->items[change->index]);
    slots->items[change->index] = id;
}

void actions_assign_slot(int index, const wchar_t *id)
{
    if (index < 0 || index >= QUICK_ACTION_SLOTS || !id) return;
    AssignChange change = { index, id };
    app_update_settings(change_assign, &change);
}

/// 删除自定义操作；它所在的位置原地换成一个没用到的内置操作，其他按钮不移动。
static void change_delete(AppSettings *settings, void *context)
{
    const wchar_t *id = context;
    quick_action_normalize_slots(&settings->quick_action_slots, settings->custom_actions, settings->custom_action_count);
    StrList *slots = &settings->quick_action_slots;
    int index = strlist_index_of(slots, id, false);
    if (index >= 0)
        for (int builtin = 0; builtin < quick_action_builtin_count(); builtin++)
        {
            const wchar_t *candidate = quick_action_builtin_id(builtin);
            if (strlist_index_of(slots, candidate, false) >= 0) continue;
            free(slots->items[index]);
            slots->items[index] = wcs_dup(candidate);
            break;
        }
    for (int custom = 0; custom < settings->custom_action_count; custom++)
    {
        if (!wcs_equal(settings->custom_actions[custom].id, id)) continue;
        custom_action_free(&settings->custom_actions[custom]);
        memmove(&settings->custom_actions[custom], &settings->custom_actions[custom + 1],
            (size_t)(settings->custom_action_count - custom - 1) * sizeof(CustomAction));
        settings->custom_action_count--;
        break;
    }
}

void actions_delete_custom(const wchar_t *id)
{
    wchar_t *copy = wcs_dup(id);
    app_update_settings(change_delete, copy);
    free(copy);
}

// ───────────── 卡片 ─────────────

static const GfxTextStyle glyph_style = { GFX_FONT_ICON, 20, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
static const GfxTextStyle title_style = { GFX_FONT_TEXT, 14, 400, false, true, GFX_ALIGN_CENTER, GFX_ALIGN_START };
static const GfxTextStyle small_style = { GFX_FONT_TEXT, 12, 400, false, true, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };

static float line_height(const GfxTextStyle *style)
{
    return gfx_text_measure(L"Ag", style, 0).height;
}

static GfxSize card_measure(UiElement *element, float available)
{
    ActionCard *card = (ActionCard *)element;
    if (card->edit)
    {
        ui_measure(card->edit, 32);
        ui_measure(card->remove, 32);
    }
    if (card->slot >= 0)
    {
        // 和置物架底部的按钮一样：图标在上，名称和位置在下。
        float height = 14 + line_height(&glyph_style) + 10 + line_height(&title_style) + 3 + 20 + 12;
        return (GfxSize){ available, height > 62 ? height : 62 };
    }
    GfxTextStyle description = small_style;
    float height = 10 + line_height(&title_style) + 2 + line_height(&description) + 10;
    return (GfxSize){ available, height > 62 ? height : 62 };
}

static void card_arrange(UiElement *element)
{
    ActionCard *card = (ActionCard *)element;
    if (!card->edit) return;
    GfxRect bounds = element->bounds;
    float right, top;
    if (card->slot >= 0)
    {
        right = bounds.right - 10 + 4;
        top = bounds.top + 14 - 8;
    }
    else
    {
        right = bounds.right - 12;
        top = (bounds.top + bounds.bottom) / 2 - 16;
    }
    ui_arrange(card->remove, (GfxRect){ right - 32, top, right, top + 32 });
    ui_arrange(card->edit, (GfxRect){ right - 32 - 4 - 32, top, right - 32 - 4, top + 32 });
}

static bool card_buttons_shown(ActionCard *card)
{
    return card->edit && (card->base.hovered || card->edit->hovered || card->remove->hovered) && !dragging;
}

static void card_paint(UiElement *element, double time)
{
    (void)time;
    ActionCard *card = (ActionCard *)element;
    GfxRect bounds = element->bounds;
    bool source = dragging && card == pressed_card;
    if (source) gfx_push_opacity(0.4f);
    bool cards = theme_is_cards();
    gfx_fill_rounded(bounds, 6, theme_color(card->drop_target ? THEME_ACCENT_FAINT : cards ? THEME_SETTING_ROW : THEME_CARD));
    gfx_stroke_rounded(bounds, 6, theme_color(card->drop_target ? THEME_ACCENT : cards ? THEME_SETTING_ROW_BORDER : THEME_CARD_BORDER), 1);
    Color primary = theme_color(THEME_TEXT_PRIMARY), secondary = theme_color(THEME_TEXT_SECONDARY);
    if (card->slot >= 0)
    {
        float y = bounds.top + 14;
        gfx_text_draw(card->glyph_text, (GfxRect){ bounds.left + 10, y, bounds.right - 10, y + line_height(&glyph_style) }, &glyph_style, primary);
        y += line_height(&glyph_style) + 10;
        gfx_text_draw(card->title, (GfxRect){ bounds.left + 10, y, bounds.right - 10, y + line_height(&title_style) }, &title_style, primary);
        y += line_height(&title_style) + 3;
        GfxRect row = { bounds.left + 10, y, bounds.right - 10, y + 20 };
        if (card->drop_target)
        {
            // 拖到上面时位置换成“替换/交换”。
            GfxTextStyle glyph = { GFX_FONT_ICON, 11, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
            GfxTextStyle text = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
            float glyph_width = gfx_text_measure(GLYPH_SWAP, &glyph, 0).width, text_width = gfx_text_measure(S(card->drop_hint), &text, 0).width;
            float width = 8 + glyph_width + 5 + text_width + 10;
            float left = (row.left + row.right - width) / 2;
            GfxRect pill = { left, row.top, left + width, row.bottom };
            gfx_fill_rounded(pill, 10, theme_color(THEME_ACCENT));
            Color on = theme_color(THEME_TEXT_ON_ACCENT);
            gfx_text_draw(GLYPH_SWAP, (GfxRect){ left + 8, row.top + 1, left + 8 + glyph_width + 1, row.bottom }, &glyph, on);
            gfx_text_draw(S(card->drop_hint), (GfxRect){ left + 8 + glyph_width + 5, row.top, pill.right, row.bottom - 1 }, &text, on);
        }
        else gfx_text_draw(S(card->position), row, &small_style, secondary);
    }
    else
    {
        GfxTextStyle glyph = glyph_style;
        glyph.align = GFX_ALIGN_START;
        glyph.vertical = GFX_ALIGN_CENTER;
        float glyph_width = gfx_text_measure(card->glyph_text, &glyph, 0).width;
        gfx_text_draw(card->glyph_text, (GfxRect){ bounds.left + 16, bounds.top, bounds.left + 16 + glyph_width + 1, bounds.bottom }, &glyph, primary);
        float left = bounds.left + 16 + glyph_width + 16, right = bounds.right - 12 - (card->edit ? 68 + 12 : 12);
        GfxTextStyle title = title_style, description = small_style;
        title.align = description.align = GFX_ALIGN_START;
        description.vertical = GFX_ALIGN_START;
        float title_height = line_height(&title), description_height = line_height(&description);
        float top = (bounds.top + bounds.bottom - title_height - 2 - description_height) / 2;
        gfx_text_draw(card->title, (GfxRect){ left, top, right, top + title_height }, &title, primary);
        top += title_height + 2;
        gfx_text_draw(card->description, (GfxRect){ left, top, right, top + description_height }, &description, secondary);
    }
    if (source) gfx_pop_layer();
    // 编辑、删除按钮只在悬停在卡片上时画出来。
    bool shown = card_buttons_shown(card);
    if (card->edit) card->edit->opacity = card->remove->opacity = shown ? 1.0f : 0.0f;
}

static void begin_drag(ActionCard *card);
static void end_drag(bool apply);
static void update_drag(float x, float y);

static void card_mouse(UiElement *element, const UiMouseEvent *event)
{
    ActionCard *card = (ActionCard *)element;
    float x = event->x, y = event->y;
    ui_element_to_window(element, &x, &y);
    switch (event->kind)
    {
        case UI_MOUSE_ENTER:
        case UI_MOUSE_LEAVE:
            main_repaint();
            break;
        case UI_MOUSE_DOWN:
            if (event->right_button) break;
            pressed_card = card;
            press_x = x;
            press_y = y;
            break;
        case UI_MOUSE_MOVE:
            if (pressed_card != card || !element->pressed) break;
            if (!dragging)
            {
                if (fabsf(x - press_x) < DRAG_THRESHOLD && fabsf(y - press_y) < DRAG_THRESHOLD) break;
                begin_drag(card);
            }
            update_drag(x, y);
            break;
        case UI_MOUSE_UP:
            if (!dragging)
            {
                pressed_card = NULL;
                break;
            }
            // 光标被别的窗口拿走时什么都不做。
            end_drag(event->inside || event->x >= 0 || event->y >= 0);
            break;
        default:
            break;
    }
}

static void card_destroy(UiElement *element)
{
    ActionCard *card = (ActionCard *)element;
    if (pressed_card == card) pressed_card = NULL;
    if (drop_target == card) drop_target = NULL;
    quick_action_free(&card->action);
    free(card->glyph_text);
    free(card->title);
    free(card->description);
}

static const UiElementType card_type = { card_measure, card_arrange, card_paint, card_mouse, card_destroy, true };

static void on_edit(void *context)
{
    ActionCard *card = context;
    if (card->action.is_custom) dialogs_edit_action(&card->action.custom);
}

static void on_remove(void *context)
{
    ActionCard *card = context;
    if (card->action.is_custom) dialogs_confirm_delete(&card->action.custom, card->title);
}

static void on_add(void *context)
{
    (void)context;
    dialogs_edit_action(NULL);
}

static UiElement *create_card(const QuickAction *action, int slot, StringId position)
{
    ActionCard *card = (ActionCard *)ui_element_new(&card_type, sizeof(ActionCard));
    quick_action_clone(&card->action, action);
    card->slot = slot;
    card->position = position;
    card->glyph_text = wcs_dup(quick_action_glyph(action));
    card->title = quick_action_title(action);
    card->description = quick_action_description(action);
    ui_set_tooltip(&card->base, UI_LIT(card->description));
    if (action->is_custom)
    {
        card->edit = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_EDIT), on_edit, card);
        ui_button_set_font_size(card->edit, 14);
        ui_set_tooltip(card->edit, UI_STR(STR_EditTooltip));
        card->remove = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_DELETE), on_remove, card);
        ui_button_set_font_size(card->remove, 14);
        ui_set_tooltip(card->remove, UI_STR(STR_DeleteTooltip));
        card->edit->opacity = card->remove->opacity = 0;
        ui_add(&card->base, card->edit);
        ui_add(&card->base, card->remove);
    }
    return &card->base;
}

static StringId slot_position(int index, int layout)
{
    if (layout == LAYOUT_TWO) return index == 0 ? STR_SlotLeft : STR_SlotRight;
    switch (index)
    {
        case 0: return STR_SlotFarLeft;
        case 1: return STR_SlotCenterLeft;
        case 2: return STR_SlotCenterRight;
        default: return STR_SlotFarRight;
    }
}

/// 操作的布局、位置、自定义操作的内容和界面语言：变了才重建卡片。
static wchar_t *build_key(const AppSettings *settings)
{
    WStr key = {0};
    wstr_appendf(&key, L"%d|%d|%p|", settings->quick_action_layout, language_is_chinese(), (void *)language_current_pack());
    for (int index = 0; index < settings->quick_action_slots.count; index++) wstr_appendf(&key, L"%ls,", settings->quick_action_slots.items[index]);
    for (int index = 0; index < settings->custom_action_count; index++)
    {
        const CustomAction *custom = &settings->custom_actions[index];
        wstr_appendf(&key, L"\x1E%ls|%d|%ls|%ls|%d|%d|%d|%ls|%ls", custom->id ? custom->id : L"", custom->kind, custom->name ? custom->name : L"",
            custom->folder ? custom->folder : L"", custom->folder_mode, custom->image_size_kb, custom->image_format,
            custom->program_path ? custom->program_path : L"", custom->arguments ? custom->arguments : L"");
    }
    return wstr_take(&key);
}

void actions_refresh(void)
{
    if (!slots_grid || dragging) return;
    const AppSettings *settings = app_settings();
    ui_set_disabled(add_button, settings->custom_action_count >= QUICK_ACTION_MAXIMUM_CUSTOM);
    wchar_t *key = build_key(settings);
    if (actions_key && wcscmp(key, actions_key) == 0)
    {
        free(key);
        return;
    }
    free(actions_key);
    actions_key = key;
    pressed_card = NULL;
    drop_target = NULL;

    int layout = settings->quick_action_layout;
    slots_section->hidden = layout == LAYOUT_OFF;
    ui_clear_children(slots_grid);
    ui_clear_children(more_list);
    // 置物架上显示的位置，按从左到右的顺序，排成一行。
    int visible = layout < settings->quick_action_slots.count ? layout : settings->quick_action_slots.count;
    ui_grid_set_columns(slots_grid, visible > 0 ? visible : 1);
    for (int index = 0; index < visible; index++)
    {
        QuickAction action;
        if (!quick_action_find(settings->quick_action_slots.items[index], settings, &action)) continue;
        ui_add(slots_grid, create_card(&action, index, slot_position(index, layout)));
        quick_action_free(&action);
    }
    // 没有显示在置物架上的操作，可以拖到上方的位置上。
    QuickActionList available = quick_actions_available(settings);
    for (int index = 0; index < available.count; index++)
    {
        bool shown = false;
        for (int slot = 0; slot < visible && !shown; slot++) shown = wcs_equal(settings->quick_action_slots.items[slot], available.items[index].id);
        if (shown) continue;
        UiElement *card = create_card(&available.items[index], -1, STR_COUNT);
        ui_set_margin(card, 0, 0, 0, 4);
        ui_add(more_list, card);
    }
    quick_action_list_free(&available);
    main_relayout();
}

void actions_build(UiElement *content)
{
    slots_section = ui_stack(false, 0);
    UiElement *header = ui_label(UI_STR(STR_CurrentActions), UI_TEXT_BODY_STRONG);
    ui_set_margin(header, 2, 24, 0, 2);
    ui_add(slots_section, header);
    UiElement *hint = ui_label(UI_STR(STR_DragToSwapHint), UI_TEXT_CAPTION);
    ui_set_margin(hint, 2, 0, 0, 10);
    ui_add(slots_section, hint);
    // 和置物架底部按钮的排列一致，排成一行。
    slots_grid = ui_grid(4, 4, 4);
    ui_add(slots_section, slots_grid);
    ui_add(content, slots_section);

    UiElement *more = ui_label(UI_STR(STR_MoreActions), UI_TEXT_BODY_STRONG);
    ui_set_margin(more, 2, 24, 0, 8);
    ui_add(content, more);
    more_list = ui_stack(false, 0);
    ui_add(content, more_list);
    add_button = ui_button(UI_BUTTON_STANDARD, UI_STR(STR_AddCustomAction), on_add, NULL);
    ui_button_set_glyph(add_button, GLYPH_ADD, 12);
    ui_button_set_padding(add_button, 12, 5, 14, 6);
    add_button->cross = UI_CROSS_START;
    ui_set_margin(add_button, 0, 6, 0, 0);
    ui_add(content, add_button);
    free(actions_key);
    actions_key = NULL;
    actions_refresh();
}

// ───────────── 拖动 ─────────────

bool actions_dragging(void)
{
    return dragging;
}

static void fade(Anim *anim, double to)
{
    anim_start(anim, to, 0.15, EASE_OF(EASE_LINEAR));
    main_request_frame();
}

static void begin_drag(ActionCard *card)
{
    dragging = true;
    ghost_visible = true;
    ghost_bouncing = false;
    anim_set(&ghost_opacity, 1);
    anim_set(&ghost_bounce, 0);
    free(ghost_glyph);
    free(ghost_title);
    ghost_glyph = wcs_dup(card->glyph_text);
    ghost_title = wcs_dup(card->title);
    zone_custom = card->action.is_custom;
    over_zone = false;
    anim_set(&glow_opacity, 0.35);
    anim_set(&hint_opacity, 0);
    fade(&zone_opacity, 1);
    main_repaint();
}

static void set_drop_target(ActionCard *target)
{
    if (target == pressed_card) target = NULL;
    if (target == drop_target) return;
    if (drop_target) drop_target->drop_target = false;
    drop_target = target;
    if (target)
    {
        target->drop_target = true;
        // 从另一个位置拖来是“交换”，从更多操作拖来是“替换”。
        target->drop_hint = pressed_card && pressed_card->slot >= 0 ? STR_DropHintSwap : STR_DropHintReplace;
    }
    main_repaint();
}

static void set_over_zone(bool value)
{
    if (value == over_zone) return;
    over_zone = value;
    fade(&glow_opacity, value ? 0.8 : 0.35);
    fade(&hint_opacity, value ? 1 : 0);
}

static ActionCard *slot_at(float x, float y)
{
    if (slots_section->hidden) return NULL;
    for (int index = 0; index < slots_grid->child_count; index++)
    {
        UiElement *child = slots_grid->children[index];
        float lx = x, ly = y;
        ui_element_to_local(child, &lx, &ly);
        GfxRect bounds = child->bounds;
        if (lx >= bounds.left && lx < bounds.right && ly >= bounds.top && ly < bounds.bottom) return (ActionCard *)child;
    }
    return NULL;
}

static void update_drag(float x, float y)
{
    GfxSize size = ui_window_size(M->window);
    float zone_top = size.height - DELETE_ZONE_HEIGHT;
    // 内置操作拖进底部区域时像碰到弹性边界：越往下越拖不动，最多陷进去一小段。
    float follow_y = zone_custom || y <= zone_top ? y : zone_top + LOCKED_SINK * (1 - expf(-(y - zone_top) / LOCKED_SINK));
    ghost_x = x + 12;
    ghost_y = follow_y + 8;
    set_over_zone(y >= zone_top);
    set_drop_target(over_zone ? NULL : slot_at(x, y));
    main_repaint();
}

static void end_drag(bool apply)
{
    ActionCard *card = pressed_card;
    ActionCard *target = drop_target;
    bool zone = over_zone;
    bool was_dragging = dragging;
    dragging = false;
    set_drop_target(NULL);
    set_over_zone(false);
    fade(&zone_opacity, 0);
    pressed_card = NULL;
    ghost_visible = false;
    if (!was_dragging || !card || !apply)
    {
        main_repaint();
        return;
    }
    if (zone && card->action.is_custom)
    {
        dialogs_confirm_delete(&card->action.custom, card->title);
    }
    else if (zone)
    {
        // 内置操作在底部松手：跟随的卡片弹回区域上方并淡出。
        GfxSize size = ui_window_size(M->window);
        float top = size.height - DELETE_ZONE_HEIGHT - 40;
        ghost_visible = true;
        ghost_bouncing = true;
        anim_set(&ghost_bounce, ghost_y);
        anim_start(&ghost_bounce, ghost_y < top ? ghost_y : top, 0.32, EASE_BACK_OF(0.5));
        anim_set(&ghost_opacity, 1);
        anim_start_delayed(&ghost_opacity, 0, 0.18, 0.16, EASE_OF(EASE_LINEAR));
        main_request_frame();
    }
    else if (target)
    {
        wchar_t *id = wcs_dup(card->action.id);
        int slot = target->slot;
        actions_assign_slot(slot, id);
        free(id);
    }
    main_repaint();
}

bool actions_escape(void)
{
    if (!dragging) return false;
    end_drag(false);
    return true;
}

void actions_animate(double time)
{
    bool running = anim_update(&zone_opacity, time);
    running |= anim_update(&glow_opacity, time);
    running |= anim_update(&hint_opacity, time);
    running |= anim_update(&ghost_bounce, time);
    running |= anim_update(&ghost_opacity, time);
    if (ghost_bouncing)
    {
        ghost_y = (float)ghost_bounce.value;
        if (!ghost_opacity.running && ghost_opacity.value <= 0.001)
        {
            ghost_bouncing = false;
            ghost_visible = false;
        }
    }
    if (running) main_request_frame();
}

/// 拖动快速操作时窗口底部发光：自定义操作为红色，拖进去后提示松开删除；内置操作为灰色，提示不能删除。
void actions_paint_overlay(double time)
{
    (void)time;
    float zone = (float)zone_opacity.value;
    GfxSize size = ui_window_size(M->window);
    if (zone > 0.001f)
    {
        Color color = zone_custom ? COLOR_RGB(0xE5, 0x00, 0x0F) : COLOR_RGB(0x80, 0x80, 0x80);
        GfxRect rect = { 0, size.height - DELETE_ZONE_HEIGHT, size.width, size.height };
        float glow = (float)glow_opacity.value * zone;
        GfxStop stops[3] = { { 0, color_with_alpha(color, (uint8_t)(255 * glow)) }, { 0.4f, color_with_alpha(color, (uint8_t)(128 * glow)) },
            { 1, color_with_alpha(color, 0) } };
        gfx_fill_rect_linear(rect, 0, rect.bottom, 0, rect.top, stops, 3);
        float hint = (float)hint_opacity.value * zone;
        if (hint > 0.001f)
        {
            const wchar_t *glyph = zone_custom ? GLYPH_DELETE : GLYPH_LOCK;
            const wchar_t *text = S(zone_custom ? STR_ReleaseToDelete : STR_BuiltInCannotDelete);
            GfxTextStyle glyph_style_small = { GFX_FONT_ICON, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
            GfxTextStyle text_style = { GFX_FONT_TEXT, 13, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
            float glyph_width = gfx_text_measure(glyph, &glyph_style_small, 0).width;
            GfxSize text_size = gfx_text_measure(text, &text_style, 0);
            float width = 12 + glyph_width + 8 + text_size.width + 14, height = text_size.height + 11;
            float left = (size.width - width) / 2, top = size.height - 28 - height;
            gfx_push_opacity(hint);
            gfx_fill_rounded((GfxRect){ left, top, left + width, top + height }, 14, color);
            gfx_text_draw(glyph, (GfxRect){ left + 12, top + 1, left + 12 + glyph_width + 1, top + height }, &glyph_style_small, COLOR_WHITE);
            gfx_text_draw(text, (GfxRect){ left + 12 + glyph_width + 8, top, left + width, top + height - 1 }, &text_style, COLOR_WHITE);
            gfx_pop_layer();
        }
    }
    float opacity = (float)ghost_opacity.value;
    if (ghost_visible && ghost_title && opacity > 0.001f)
    {
        // 跟随光标的卡片：强调色边框，带阴影。
        GfxTextStyle glyph = { GFX_FONT_ICON, 16, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        GfxTextStyle text = { GFX_FONT_TEXT, 13, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        float glyph_width = gfx_text_measure(ghost_glyph, &glyph, 0).width;
        GfxSize text_size = gfx_text_measure(ghost_title, &text, 0);
        float width = 12 + glyph_width + 10 + text_size.width + 16, height = text_size.height + 17;
        GfxRect rect = { ghost_x, ghost_y, ghost_x + width, ghost_y + height };
        gfx_push_opacity(opacity);
        gfx_shadow(rect, 8, 18, 0, 4, COLOR_ARGB(56, 0, 0, 0));
        gfx_fill_rounded(rect, 8, theme_color(THEME_FLYOUT));
        gfx_stroke_rounded(rect, 8, theme_color(THEME_ACCENT), 1);
        gfx_text_draw(ghost_glyph, (GfxRect){ rect.left + 12, rect.top + 1, rect.left + 12 + glyph_width + 1, rect.bottom }, &glyph,
            theme_color(THEME_ACCENT_TEXT));
        gfx_text_draw(ghost_title, (GfxRect){ rect.left + 12 + glyph_width + 10, rect.top, rect.right, rect.bottom - 1 }, &text,
            theme_color(THEME_TEXT_PRIMARY));
        gfx_pop_layer();
    }
}

void actions_close(void)
{
    dragging = false;
    pressed_card = drop_target = NULL;
    slots_section = slots_grid = more_list = add_button = NULL;
    free(actions_key);
    actions_key = NULL;
    free(ghost_glyph);
    free(ghost_title);
    ghost_glyph = ghost_title = NULL;
    ghost_visible = ghost_bouncing = false;
    anim_set(&zone_opacity, 0);
}

// ───────────── 测试用 ─────────────

int actions_slot_count(void)
{
    return slots_grid && !slots_section->hidden ? slots_grid->child_count : 0;
}

const wchar_t *actions_slot_id(int index)
{
    return index >= 0 && index < actions_slot_count() ? ((ActionCard *)slots_grid->children[index])->action.id : NULL;
}

StringId actions_slot_position(int index)
{
    return index >= 0 && index < actions_slot_count() ? ((ActionCard *)slots_grid->children[index])->position : STR_COUNT;
}

int actions_more_count(void)
{
    return more_list ? more_list->child_count : 0;
}

const wchar_t *actions_more_id(int index)
{
    return index >= 0 && index < actions_more_count() ? ((ActionCard *)more_list->children[index])->action.id : NULL;
}
