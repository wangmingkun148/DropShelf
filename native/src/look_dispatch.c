// 不分风格的代码调用两种风格各有一份的函数时，按当前的风格转给 simple_ 或 cards_ 开头的那一份，见 look_variant.h。
#include "look_dispatch.h"
#include "theme.h"

static bool cards(void)
{
    return theme_look() == LOOK_CARDS;
}

void pages_animate(double time)
{
    if (cards()) cards_pages_animate(time);
    else simple_pages_animate(time);
}

void pages_apply_calibration(int level)
{
    if (cards()) cards_pages_apply_calibration(level);
    else simple_pages_apply_calibration(level);
}

void pages_build(void)
{
    if (cards()) cards_pages_build();
    else simple_pages_build();
}

float pages_card_height(void)
{
    return cards() ? cards_pages_card_height() : simple_pages_card_height();
}

void pages_close(void)
{
    if (cards()) cards_pages_close();
    else simple_pages_close();
}

UiElement *pages_control(PagesControl control)
{
    return cards() ? cards_pages_control(control) : simple_pages_control(control);
}

void pages_paint_tile_face(UiElement *tile, GfxMatrix matrix, float opacity)
{
    if (cards()) cards_pages_paint_tile_face(tile, matrix, opacity);
    else simple_pages_paint_tile_face(tile, matrix, opacity);
}

void pages_refresh(void)
{
    if (cards()) cards_pages_refresh();
    else simple_pages_refresh();
}

float pages_sensitivity_label_scale(int index)
{
    return cards() ? cards_pages_sensitivity_label_scale(index) : simple_pages_sensitivity_label_scale(index);
}

bool pages_tile_is_tall(int page)
{
    return cards() ? cards_pages_tile_is_tall(page) : simple_pages_tile_is_tall(page);
}

const wchar_t *pages_tile_summary(int page)
{
    return cards() ? cards_pages_tile_summary(page) : simple_pages_tile_summary(page);
}

bool pages_update_card_height(float viewport, bool resized)
{
    return cards() ? cards_pages_update_card_height(viewport, resized) : simple_pages_update_card_height(viewport, resized);
}

void shelf_end_drag(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_end_drag(shelf);
    else simple_shelf_end_drag(shelf);
}

void shelf_finish_motion(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_finish_motion(shelf);
    else simple_shelf_finish_motion(shelf);
}

void shelf_hide_toast(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_hide_toast(shelf);
    else simple_shelf_hide_toast(shelf);
}

void shelf_layout(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_layout(shelf);
    else simple_shelf_layout(shelf);
}

void shelf_notify(void *context, const wchar_t *message, bool success)
{
    if (cards()) cards_shelf_notify(context, message, success);
    else simple_shelf_notify(context, message, success);
}

void shelf_rebuild_actions(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_rebuild_actions(shelf);
    else simple_shelf_rebuild_actions(shelf);
}

void shelf_remove_items(ShelfWindow *shelf, ShelfItem **items, int count)
{
    if (cards()) cards_shelf_remove_items(shelf, items, count);
    else simple_shelf_remove_items(shelf, items, count);
}

void shelf_repaint(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_repaint(shelf);
    else simple_shelf_repaint(shelf);
}

void shelf_reset_tiles(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_reset_tiles(shelf);
    else simple_shelf_reset_tiles(shelf);
}

void shelf_restore_paths(ShelfWindow *shelf, const StrList *paths)
{
    if (cards()) cards_shelf_restore_paths(shelf, paths);
    else simple_shelf_restore_paths(shelf, paths);
}

float shelf_scale(const ShelfWindow *shelf)
{
    return cards() ? cards_shelf_scale(shelf) : simple_shelf_scale(shelf);
}

void shelf_set_hovered_action(ShelfWindow *shelf, int action, bool force)
{
    if (cards()) cards_shelf_set_hovered_action(shelf, action, force);
    else simple_shelf_set_hovered_action(shelf, action, force);
}

void shelf_show_toast(ShelfWindow *shelf, const wchar_t *message, bool success, ToastAction action)
{
    if (cards()) cards_shelf_show_toast(shelf, message, success, action);
    else simple_shelf_show_toast(shelf, message, success, action);
}

void shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height)
{
    if (cards()) cards_shelf_target_size(shelf, expanded, width, height);
    else simple_shelf_target_size(shelf, expanded, width, height);
}

void shelf_update_title(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_update_title(shelf);
    else simple_shelf_update_title(shelf);
}

float shelf_width_for(int size)
{
    return cards() ? cards_shelf_width_for(size) : simple_shelf_width_for(size);
}

void shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths)
{
    if (cards()) cards_shelf_window_add_paths(shelf, paths);
    else simple_shelf_window_add_paths(shelf, paths);
}

void shelf_window_apply_settings(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_window_apply_settings(shelf);
    else simple_shelf_window_apply_settings(shelf);
}

void shelf_window_close_animated(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_window_close_animated(shelf);
    else simple_shelf_window_close_animated(shelf);
}

void shelf_window_close_now(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_window_close_now(shelf);
    else simple_shelf_window_close_now(shelf);
}

ShelfWindow *shelf_window_create(void)
{
    return cards() ? cards_shelf_window_create() : simple_shelf_window_create();
}

bool shelf_window_is_animating(const ShelfWindow *shelf)
{
    return cards() ? cards_shelf_window_is_animating(shelf) : simple_shelf_window_is_animating(shelf);
}

bool shelf_window_is_reusable(const ShelfWindow *shelf)
{
    return cards() ? cards_shelf_window_is_reusable(shelf) : simple_shelf_window_is_reusable(shelf);
}

bool shelf_window_is_visible(const ShelfWindow *shelf)
{
    return cards() ? cards_shelf_window_is_visible(shelf) : simple_shelf_window_is_visible(shelf);
}

void shelf_window_release(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_window_release(shelf);
    else simple_shelf_window_release(shelf);
}

ShelfWindow *shelf_window_retain(ShelfWindow *shelf)
{
    return cards() ? cards_shelf_window_retain(shelf) : simple_shelf_window_retain(shelf);
}

void shelf_window_show_at(ShelfWindow *shelf, POINT center, bool activate, ShelfEntranceKind entrance, ScreenEdge edge)
{
    if (cards()) cards_shelf_window_show_at(shelf, center, activate, entrance, edge);
    else simple_shelf_window_show_at(shelf, center, activate, entrance, edge);
}

void shelf_window_leave_look(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_window_leave_look(shelf);
    else simple_shelf_window_leave_look(shelf);
}

void shelf_window_adopt_look(ShelfWindow *shelf)
{
    if (cards()) cards_shelf_window_adopt_look(shelf);
    else simple_shelf_window_adopt_look(shelf);
}
