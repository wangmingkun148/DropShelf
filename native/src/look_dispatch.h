// 两种风格各有一份的函数：simple_ 开头的是简洁，cards_ 开头的是纸牌。不分风格的代码需要指定其中一份时用这些名字，
// 一般情况调用不带前缀的名字（look_dispatch.c 按当前的风格转过去）。
#pragma once

#include "main_window_internal.h"
#include "shelf_window_internal.h"

void simple_pages_animate(double time);
void cards_pages_animate(double time);
void simple_pages_apply_calibration(int level);
void cards_pages_apply_calibration(int level);
void simple_pages_build(void);
void cards_pages_build(void);
float simple_pages_card_height(void);
float cards_pages_card_height(void);
void simple_pages_close(void);
void cards_pages_close(void);
UiElement *simple_pages_control(PagesControl control);
UiElement *cards_pages_control(PagesControl control);
void simple_pages_paint_tile_face(UiElement *tile, GfxMatrix matrix, float opacity);
void cards_pages_paint_tile_face(UiElement *tile, GfxMatrix matrix, float opacity);
void simple_pages_refresh(void);
void cards_pages_refresh(void);
float simple_pages_sensitivity_label_scale(int index);
float cards_pages_sensitivity_label_scale(int index);
bool simple_pages_tile_is_tall(int page);
bool cards_pages_tile_is_tall(int page);
const wchar_t *simple_pages_tile_summary(int page);
const wchar_t *cards_pages_tile_summary(int page);
bool simple_pages_update_card_height(float viewport, bool resized);
bool cards_pages_update_card_height(float viewport, bool resized);
void simple_shelf_end_drag(ShelfWindow *shelf);
void cards_shelf_end_drag(ShelfWindow *shelf);
void simple_shelf_finish_motion(ShelfWindow *shelf);
void cards_shelf_finish_motion(ShelfWindow *shelf);
void simple_shelf_hide_toast(ShelfWindow *shelf);
void cards_shelf_hide_toast(ShelfWindow *shelf);
void simple_shelf_layout(ShelfWindow *shelf);
void cards_shelf_layout(ShelfWindow *shelf);
void simple_shelf_notify(void *context, const wchar_t *message, bool success);
void cards_shelf_notify(void *context, const wchar_t *message, bool success);
void simple_shelf_rebuild_actions(ShelfWindow *shelf);
void cards_shelf_rebuild_actions(ShelfWindow *shelf);
void simple_shelf_remove_items(ShelfWindow *shelf, ShelfItem **items, int count);
void cards_shelf_remove_items(ShelfWindow *shelf, ShelfItem **items, int count);
void simple_shelf_repaint(ShelfWindow *shelf);
void cards_shelf_repaint(ShelfWindow *shelf);
void simple_shelf_reset_tiles(ShelfWindow *shelf);
void cards_shelf_reset_tiles(ShelfWindow *shelf);
void simple_shelf_restore_paths(ShelfWindow *shelf, const StrList *paths);
void cards_shelf_restore_paths(ShelfWindow *shelf, const StrList *paths);
float simple_shelf_scale(const ShelfWindow *shelf);
float cards_shelf_scale(const ShelfWindow *shelf);
void simple_shelf_set_hovered_action(ShelfWindow *shelf, int action, bool force);
void cards_shelf_set_hovered_action(ShelfWindow *shelf, int action, bool force);
void simple_shelf_show_toast(ShelfWindow *shelf, const wchar_t *message, bool success, ToastAction action);
void cards_shelf_show_toast(ShelfWindow *shelf, const wchar_t *message, bool success, ToastAction action);
void simple_shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height);
void cards_shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height);
void simple_shelf_update_title(ShelfWindow *shelf);
void cards_shelf_update_title(ShelfWindow *shelf);
float simple_shelf_width_for(int size);
float cards_shelf_width_for(int size);
void simple_shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths);
void cards_shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths);
void simple_shelf_window_apply_settings(ShelfWindow *shelf);
void cards_shelf_window_apply_settings(ShelfWindow *shelf);
void simple_shelf_window_close_animated(ShelfWindow *shelf);
void cards_shelf_window_close_animated(ShelfWindow *shelf);
void simple_shelf_window_close_now(ShelfWindow *shelf);
void cards_shelf_window_close_now(ShelfWindow *shelf);
ShelfWindow *simple_shelf_window_create(void);
ShelfWindow *cards_shelf_window_create(void);
bool simple_shelf_window_is_animating(const ShelfWindow *shelf);
bool cards_shelf_window_is_animating(const ShelfWindow *shelf);
bool simple_shelf_window_is_reusable(const ShelfWindow *shelf);
bool cards_shelf_window_is_reusable(const ShelfWindow *shelf);
bool simple_shelf_window_is_visible(const ShelfWindow *shelf);
bool cards_shelf_window_is_visible(const ShelfWindow *shelf);
void simple_shelf_window_release(ShelfWindow *shelf);
void cards_shelf_window_release(ShelfWindow *shelf);
ShelfWindow *simple_shelf_window_retain(ShelfWindow *shelf);
ShelfWindow *cards_shelf_window_retain(ShelfWindow *shelf);
void simple_shelf_window_show_at(ShelfWindow *shelf, POINT center, bool activate, ShelfEntranceKind entrance, ScreenEdge edge);
void cards_shelf_window_show_at(ShelfWindow *shelf, POINT center, bool activate, ShelfEntranceKind entrance, ScreenEdge edge);

// 两种风格签名不同、由调用方按风格指定的几个。
void simple_shelf_show_drop_overlay(ShelfWindow *shelf);
void cards_shelf_show_drop_overlay(ShelfWindow *shelf, IDataObject *data, int incoming);
void simple_shelf_window_leave_look(ShelfWindow *shelf);
void cards_shelf_window_leave_look(ShelfWindow *shelf);
void simple_shelf_window_adopt_look(ShelfWindow *shelf);
void cards_shelf_window_adopt_look(ShelfWindow *shelf);

// 只有纸牌有的（测试用）。
int cards_shelf_grid_columns_for(float width);
int cards_shelf_grid_rows_for(int count, float width);
float cards_shelf_grid_height(int rows);
int cards_shelf_grid_columns(int size);
