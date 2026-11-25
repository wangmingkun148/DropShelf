// 主窗口的概览页和四个设置页（常规、手势与置物架、快速操作、外观）。和 C# 版 MainWindow.xaml 里的各页、SettingsViewModel 对应。
#include "look_variant.h"
#include "main_window_internal.h"
#include "ui_internal.h"
#include "file_dialog.h"
#include "file_ops.h"
#include "foreground.h"
#include "gesture.h"
#include "lang.h"
#include "onboarding_window.h"
#include "quick_action.h"
#include "shelf_history.h"
#include "task.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_CARD_HEIGHT 70.0f
#define MAX_CARD_HEIGHT 230.0f
/// 牌抽出后会超出牌堆标题约这么高；窗口够高时在卡片和牌堆之间留出这段空间。
#define FAN_HEADROOM 72.0f
#define TILE_GAP 6.0f
/// 卡片够高时换成大卡片的样式；回差让高度停在分界附近时不会来回切换。
#define TALL_HEIGHT 138.0f
#define TALL_HYSTERESIS 6.0f
#define SENSITIVITY_SAVE_DELAY 250

static struct
{
    UiElement *getting_started;
    UiElement *language, *language_pack_card, *language_pack;
    int language_pack_count;
    UiElement *startup, *startup_description;
    UiElement *hotkey, *hotkey_description;
    UiElement *performance, *performance_description;
    UiElement *trigger, *sensitivity_card, *sensitivity_description, *sensitivity_slider, *edge_dwell_card, *edge_dwell;
    UiElement *blocked_list;
    UiElement *watch_list, *add_folder_button, *peek_seconds, *peek_seconds_card;
    UiElement *shelf_size, *drag_out_mode, *mixed_drag, *close_when_empty;
    UiElement *respond_fullscreen;      // 只有 Linux 有：全屏时也响应晃动和快捷键
    UiElement *history_description, *clear_history;
    UiElement *layout, *layout_description;
    UiElement *look;
    UiElement *theme_tiles[3];
    UiElement *swatches;
    UiElement *add_app_button;
    Anim card_height;
    float card_target;
    Anim label_value;           // 灵敏度下三个字的大小按这个值
    int pending_sensitivity;    // 拖动中还没保存的灵敏度，-1 表示没有
    unsigned sensitivity_generation;
    bool sensitivity_dragging;
} P = { .pending_sensitivity = -1 };

// ───────────── 修改设置 ─────────────

typedef struct IntChange
{
    size_t offset;
    int value;
} IntChange;

static void change_int(AppSettings *settings, void *context)
{
    const IntChange *change = context;
    *(int *)((char *)settings + change->offset) = change->value;
}

static void set_int(size_t offset, int value)
{
    IntChange change = { offset, value };
    app_update_settings(change_int, &change);
}

#define SET_INT(field, value) set_int(offsetof(AppSettings, field), (value))

static void on_language(int index, void *context) { (void)context; SET_INT(language, index); }
static void on_performance(int index, void *context) { (void)context; SET_INT(performance_mode, index); }
// 换风格会重建整个主窗口，不能在选择框自己的点击处理里做：等这一轮消息处理完再改设置。
static void apply_look(void *data) { SET_INT(look, (int)(intptr_t)data); }
static void on_look(int index, void *context) { (void)context; task_post(apply_look, (void *)(intptr_t)index); }
static void on_trigger(int index, void *context) { (void)context; SET_INT(auto_open_trigger, index); }
static void on_edge_dwell(int index, void *context) { (void)context; SET_INT(edge_dwell, index); }
static void on_shelf_size(int index, void *context) { (void)context; SET_INT(quick_shelf_size, index); }
static void on_drag_out(int index, void *context) { (void)context; app_update_drag_out_mode(index); }
static void on_mixed_drag(int index, void *context) { (void)context; app_update_mixed_drag(index); }
static void on_layout(int index, void *context)
{
    (void)context;
    static const int layouts[] = { LAYOUT_OFF, LAYOUT_TWO, LAYOUT_FOUR };
    SET_INT(quick_action_layout, layouts[index]);
}

static void change_close_when_empty(AppSettings *settings, void *context) { settings->close_when_empty = *(bool *)context; }
static void on_close_when_empty(bool checked, void *context) { (void)context; app_update_settings(change_close_when_empty, &checked); }

static void change_respond_fullscreen(AppSettings *settings, void *context) { settings->respond_when_fullscreen = *(bool *)context; }
static void on_respond_fullscreen(bool checked, void *context) { (void)context; app_update_settings(change_respond_fullscreen, &checked); }

static bool startup_failed;

static void on_startup(bool checked, void *context)
{
    (void)context;
    // 写入注册表失败时设置保持原值，开关拨回去，说明换成失败的原因。
    startup_failed = !app_set_start_with_windows(checked);
    main_window_refresh();
}

static void change_hotkey(AppSettings *settings, void *context)
{
    free(settings->hotkey);
    settings->hotkey = wcs_dup(context);
}

static void on_hotkey(const wchar_t *gesture, void *context)
{
    (void)context;
    app_update_settings(change_hotkey, (void *)gesture);
}

static void change_language_pack(AppSettings *settings, void *context)
{
    free(settings->language_pack);
    settings->language_pack = context ? wcs_dup(context) : NULL;
}

static void on_language_pack(int index, void *context)
{
    (void)context;
    // 第一项是“不使用”，其余按语言包的顺序。
    const wchar_t *id = index > 0 && index - 1 < language_packs_count() ? language_pack_id(language_packs_at(index - 1)) : NULL;
    app_update_settings(change_language_pack, (void *)id);
}

static void change_getting_started(AppSettings *settings, void *context)
{
    (void)context;
    settings->show_getting_started = false;
}

static void on_dismiss_getting_started(void *context)
{
    (void)context;
    app_update_settings(change_getting_started, NULL);
}

static void on_view_tutorial(void *context)
{
    (void)context;
    onboarding_window_show();
}

static void on_open_shelf(void *context)
{
    (void)context;
    main_open_shelf();
}

static void on_back(void *context)
{
    (void)context;
    flip_back();
}

static void on_clear_history(void *context)
{
    (void)context;
    shelf_history_clear();
}

// ───────────── 灵敏度 ─────────────

static int current_sensitivity(void)
{
    return P.pending_sensitivity >= 0 ? P.pending_sensitivity : app_settings()->shake_sensitivity;
}

static void change_sensitivity(AppSettings *settings, void *context) { settings->shake_sensitivity = *(int *)context; }

static void save_sensitivity(void)
{
    if (P.pending_sensitivity < 0) return;
    int level = P.pending_sensitivity;
    P.pending_sensitivity = -1;
    app_update_settings(change_sensitivity, &level);
}

static void on_sensitivity_timer(void *data)
{
    // 拖动停下 0.25 秒后才保存，不会每移动一格就写一次设置文件。
    if ((unsigned)(uintptr_t)data == P.sensitivity_generation) save_sensitivity();
}

static void update_sensitivity_description(void)
{
    int reversals = 0, distance = 0;
    shake_describe(current_sensitivity(), &reversals, &distance);
    wchar_t *text = str_sensitivity_detail(reversals, distance);
    ui_label_set_text(P.sensitivity_description, UI_LIT(text));
    free(text);
}

static void on_sensitivity(int value, bool dragging, void *context)
{
    (void)context;
    // 三个字拖动时紧跟滑钮，点轨道或按方向键时用一小段动画跟上。
    if (dragging || P.sensitivity_dragging) anim_set(&P.label_value, value);
    else anim_start(&P.label_value, value, 0.22, EASE_OF(EASE_CUBIC_OUT));
    P.sensitivity_dragging = dragging;
    main_request_frame();
    if (value == current_sensitivity()) return;
    P.pending_sensitivity = value;
    update_sensitivity_description();
    P.sensitivity_generation++;
    task_delay(SENSITIVITY_SAVE_DELAY, on_sensitivity_timer, (void *)(uintptr_t)P.sensitivity_generation);
}

void pages_apply_calibration(int level)
{
    P.sensitivity_generation++;
    P.pending_sensitivity = -1;
    if (level < SHAKE_STEADY) level = SHAKE_STEADY;
    if (level > SHAKE_RESPONSIVE) level = SHAKE_RESPONSIVE;
    app_update_settings(change_sensitivity, &level);
}

static void on_calibrate(void *context)
{
    (void)context;
    dialogs_open_calibration();
}

/// 滑块下的三个字对准 0、50、100：像 macOS 程序坞，离滑钮 50 以内的字按余弦曲线放大、变清楚，两端的字从外侧放大，不会超出滑块。
static GfxSize sensitivity_labels_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 20 };
}

/// 第 index 个字离滑钮多近：0 是 50 以外，1 是正对着。
static double sensitivity_label_weight(int index)
{
    const int stops[3] = { SHAKE_STEADY, SHAKE_BALANCED, SHAKE_RESPONSIVE };
    double distance = P.label_value.value - stops[index];
    return fabs(distance) >= 50 ? 0 : 0.5 + 0.5 * cos(3.14159265358979 * distance / 50);
}

float pages_sensitivity_label_scale(int index)
{
    return (float)(0.85 + 0.35 * sensitivity_label_weight(index));
}

static void sensitivity_labels_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    const StringId names[3] = { STR_SensitivitySteady, STR_SensitivityBalanced, STR_SensitivityResponsive };
    const GfxAlign aligns[3] = { GFX_ALIGN_START, GFX_ALIGN_CENTER, GFX_ALIGN_END };
    for (int index = 0; index < 3; index++)
    {
        double weight = sensitivity_label_weight(index);
        float scale = pages_sensitivity_label_scale(index);
        float opacity = (float)(0.5 + 0.5 * weight);
        GfxTextStyle style = { GFX_FONT_TEXT, 12, 400, false, false, aligns[index], GFX_ALIGN_START };
        float width = gfx_text_measure(S(names[index]), &style, 0).width;
        float left = index == 0 ? bounds.left : index == 1 ? (bounds.left + bounds.right - width) / 2 : bounds.right - width;
        // 缩放的原点：左边的字在左上角，中间的在上边中点，右边的在右上角。
        float origin = index == 0 ? left : index == 1 ? left + width / 2 : left + width;
        gfx_push_transform(gfx_matrix_scale(scale, scale, origin, bounds.top));
        Color color = theme_color(THEME_TEXT_PRIMARY);
        color.a = (uint8_t)(color.a * opacity);
        style.align = GFX_ALIGN_START;
        gfx_text_draw(S(names[index]), (GfxRect){ left, bounds.top, left + width + 1, bounds.bottom }, &style, color);
        gfx_pop_transform();
    }
}

static const UiElementType sensitivity_labels_type = { sensitivity_labels_measure, NULL, sensitivity_labels_paint, NULL, NULL, false };

static UiElement *build_sensitivity_control(void)
{
    UiElement *column = ui_stack(false, 0);
    column->width = 200;
    column->cross = UI_CROSS_CENTER;
    P.sensitivity_slider = ui_slider(0, 100, current_sensitivity(), 5, on_sensitivity, NULL);
    ui_set_tooltip(P.sensitivity_slider, UI_STR(STR_ShakeSensitivity));
    ui_add(column, P.sensitivity_slider);
    UiElement *labels = ui_element_new(&sensitivity_labels_type, sizeof(UiElement));
    ui_set_margin(labels, 0, -3, 0, 0);
    ui_add(column, labels);
    anim_set(&P.label_value, current_sensitivity());
    UiElement *calibrate = ui_button(UI_BUTTON_SUBTLE, UI_STR(STR_CalibrateShake), on_calibrate, NULL);
    calibrate->height = 26;
    ui_button_set_padding(calibrate, 8, 0, 8, 1);
    ui_button_set_font_size(calibrate, 12);
    ui_button_set_color(calibrate, THEME_ACCENT_TEXT);
    ui_set_margin(calibrate, 0, 4, -8, -2);
    calibrate->cross = UI_CROSS_END;
    ui_set_tooltip(calibrate, UI_STR(STR_CalibrateShakeTooltip));
    ui_add(column, calibrate);
    return column;
}

// ───────────── 屏蔽的应用 ─────────────

static void change_blocked_add(AppSettings *settings, void *context)
{
    if (strlist_index_of(&settings->blocked_apps, context, true) < 0) strlist_add(&settings->blocked_apps, context);
}

static void change_blocked_remove(AppSettings *settings, void *context)
{
    int index = strlist_index_of(&settings->blocked_apps, context, true);
    if (index < 0) return;
    free(settings->blocked_apps.items[index]);
    memmove(&settings->blocked_apps.items[index], &settings->blocked_apps.items[index + 1],
        (size_t)(settings->blocked_apps.count - index - 1) * sizeof(wchar_t *));
    settings->blocked_apps.count--;
}

static StrList menu_apps;
#define MENU_BROWSE_APP 1000

static void on_blocked_menu(int id, void *context)
{
    (void)context;
    if (id == MENU_BROWSE_APP)
    {
        wchar_t *path = file_dialog_open(ui_window_handle(M->window), S(STR_ChooseProgram), S(STR_AppFilter), NULL);
        if (path) app_update_settings(change_blocked_add, (void *)path_file_name(path));
        free(path);
    }
    else if (id >= 0 && id < menu_apps.count)
    {
        app_update_settings(change_blocked_add, menu_apps.items[id]);
    }
    strlist_clear(&menu_apps);
}

/// 从有窗口的程序中选择，或浏览选择程序文件。
static void on_add_app(void *context)
{
    (void)context;
    strlist_clear(&menu_apps);
    StrList all = {0};
    foreground_windowed_apps(&all);
    const AppSettings *settings = app_settings();
    for (int index = 0; index < all.count; index++)
        if (strlist_index_of(&settings->blocked_apps, all.items[index], true) < 0) strlist_add(&menu_apps, all.items[index]);
    strlist_clear(&all);
    int count = menu_apps.count;
    UiMenuItem *items = xcalloc((size_t)count + 2, sizeof(UiMenuItem));
    int used = 0;
    for (int index = 0; index < count; index++) items[used++] = (UiMenuItem){ .id = index, .text = menu_apps.items[index] };
    if (count > 0) items[used++] = (UiMenuItem){ .separator = true };
    items[used++] = (UiMenuItem){ .id = MENU_BROWSE_APP, .text = S(STR_BrowseApp) };
    // 菜单出现在按钮下方。
    UiElement *button = P.add_app_button;
    float x = button->bounds.left, y = button->bounds.bottom;
    ui_element_to_window(button, &x, &y);
    POINT point = ui_window_to_screen(M->window, x, y);
    ui_menu_show(items, used, point.x, point.y, on_blocked_menu, NULL);
    free(items);
}

/// 列表上显示的程序名；删除按钮记着自己在第几个。
static StrList blocked_names;

static void on_remove_app(void *context)
{
    int index = (int)(intptr_t)context;
    if (index < 0 || index >= blocked_names.count) return;
    wchar_t *name = wcs_dup(blocked_names.items[index]);
    app_update_settings(change_blocked_remove, name);
    free(name);
}

static void rebuild_blocked_list(void)
{
    const AppSettings *settings = app_settings();
    // 列表没变时不重建（悬停状态不会闪）。
    if (P.blocked_list->child_count == blocked_names.count && strlist_equal(&blocked_names, &settings->blocked_apps, false)) return;
    ui_clear_children(P.blocked_list);
    strlist_clear(&blocked_names);
    strlist_clone(&blocked_names, &settings->blocked_apps);
    for (int index = 0; index < blocked_names.count; index++)
    {
        UiElement *remove = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_DELETE), on_remove_app, (void *)(intptr_t)index);
        ui_button_set_font_size(remove, 14);
        ui_set_tooltip(remove, UI_STR(STR_RemoveBlockedApp));
        UiElement *card = ui_settings_card(NULL, UI_LIT(blocked_names.items[index]), UI_NO_TEXT, remove, NULL);
        ui_stack_set_padding(card, 60, 4, 10, 4);
        card->minimum_height = 44;
        ui_add(P.blocked_list, card);
    }
}

// ───────────── 观察文件夹 ─────────────

static void on_add_watch_folder(void *context)
{
    (void)context;
    app_choose_watch_folder(ui_window_handle(M->window));
}

/// 列表上显示的文件夹；删除按钮记着自己在第几个。
static StrList watch_names;

static void on_remove_watch_folder(void *context)
{
    int index = (int)(intptr_t)context;
    if (index < 0 || index >= watch_names.count) return;
    wchar_t *folder = wcs_dup(watch_names.items[index]);
    app_unwatch_folder(folder);
    free(folder);
}

static void rebuild_watch_list(void)
{
    const AppSettings *settings = app_settings();
    if (P.watch_list->child_count == watch_names.count && strlist_equal(&watch_names, &settings->watched_folders, false)) return;
    ui_clear_children(P.watch_list);
    strlist_clear(&watch_names);
    strlist_clone(&watch_names, &settings->watched_folders);
    for (int index = 0; index < watch_names.count; index++)
    {
        UiElement *remove = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_DELETE), on_remove_watch_folder, (void *)(intptr_t)index);
        ui_button_set_font_size(remove, 14);
        ui_set_tooltip(remove, UI_STR(STR_StopWatchingFolder));
        // 名字在上，完整的位置在下：同名的文件夹也分得清。
        wchar_t *name = path_folder_name(watch_names.items[index]);
        UiElement *card = ui_settings_card(NULL, UI_LIT(name), UI_LIT(watch_names.items[index]), remove, NULL);
        free(name);
        ui_stack_set_padding(card, 60, 6, 10, 6);
        card->minimum_height = 52;
        ui_add(P.watch_list, card);
    }
}

/// 输入框里是 1–99 的整数时才保存；正在输入的空白或别的字不改设置，下次刷新时换回保存的值。
static bool parse_peek_seconds(const wchar_t *text, int *seconds)
{
    wchar_t *trimmed = wcs_trim_dup(text);
    int value = 0;
    bool ok = *trimmed != 0;
    for (const wchar_t *at = trimmed; ok && *at; at++)
    {
        ok = *at >= L'0' && *at <= L'9';
        if (ok) value = value * 10 + (*at - L'0');
        if (value > WATCH_PEEK_MAXIMUM_SECONDS) ok = false;
    }
    free(trimmed);
    ok = ok && value >= WATCH_PEEK_MINIMUM_SECONDS;
    if (ok) *seconds = value;
    return ok;
}

static void on_peek_seconds(const wchar_t *text, void *context)
{
    (void)context;
    int seconds = 0;
    if (parse_peek_seconds(text, &seconds) && seconds != app_settings()->watch_peek_seconds) SET_INT(watch_peek_seconds, seconds);
}

static UiElement *build_peek_seconds_control(void)
{
    UiElement *row = ui_stack(true, 8);
    wchar_t text[8];
    swprintf(text, ARRAYSIZE(text), L"%d", app_settings()->watch_peek_seconds);
    P.peek_seconds = ui_textbox(text, UI_NO_TEXT, 2, on_peek_seconds, NULL, NULL);
    P.peek_seconds->width = 56;
    ui_add(row, P.peek_seconds);
    UiElement *unit = ui_label(UI_STR(STR_SecondsUnit), UI_TEXT_BODY);
    unit->cross = UI_CROSS_CENTER;
    ui_add(row, unit);
    return row;
}

static void refresh_peek_seconds(const AppSettings *settings)
{
    int shown_value = 0;
    bool valid = parse_peek_seconds(ui_textbox_text(P.peek_seconds), &shown_value);
    // 正在输入时不打断（删空了准备重填也算）；其余时候显示保存的值。
    if (P.peek_seconds->focused && (valid || wcs_is_blank(ui_textbox_text(P.peek_seconds)))) return;
    if (valid && shown_value == settings->watch_peek_seconds) return;
    wchar_t text[8];
    swprintf(text, ARRAYSIZE(text), L"%d", settings->watch_peek_seconds);
    ui_textbox_set_text(P.peek_seconds, text);
}

// ───────────── 概览页的卡片 ─────────────

typedef struct PageTile
{
    UiElement base;
    int page;
    wchar_t *summary;
    StrList items;          // 摘要拆成的各项，大卡片上显示为标签
#ifdef LOOK_VARIANT_CARDS
    StrList glance;         // 卡片右侧画出的当前设置：常规是快捷键的各个按键，快速操作是底部各个按钮的图标
#endif
    bool tall;
    Anim tall_anim;         // 0 是一行的样式，1 是大卡片
#ifdef LOOK_VARIANT_CARDS
    Anim lift;              // 悬停时卡片抬起、底下的牌背多露出一点（0 到 1）
#endif
} PageTile;

static void tile_split_summary(PageTile *tile)
{
    strlist_clear(&tile->items);
    const wchar_t *text = tile->summary ? tile->summary : L"";
    static const wchar_t *const separators[] = { L" · ", L"、", L", " };
    const wchar_t *start = text;
    while (*start)
    {
        const wchar_t *next = NULL;
        size_t length = 0;
        for (int index = 0; index < 3; index++)
        {
            const wchar_t *found = wcsstr(start, separators[index]);
            if (found && (!next || found < next))
            {
                next = found;
                length = wcslen(separators[index]);
            }
        }
        size_t part = next ? (size_t)(next - start) : wcslen(start);
        wchar_t *item = wcs_dup_n(start, part);
        wchar_t *trimmed = wcs_trim_dup(item);
        free(item);
        if (*trimmed) strlist_add_owned(&tile->items, trimmed);
        else free(trimmed);
        if (!next) break;
        start = next + length;
    }
}

#ifdef LOOK_VARIANT_CARDS
// ───────────── 卡片右侧的当前设置 ─────────────

/// 键帽：一键一格，下沿加粗，数字用 Bahnschrift。返回画完后的右边。
static float keycaps_width(const StrList *keys)
{
    GfxTextStyle style = { GFX_FONT_NUMBER, 11, 400 };
    float width = 0;
    for (int index = 0; index < keys->count; index++) width += gfx_text_measure(keys->items[index], &style, 0).width + 12 + 3;
    return width;
}

static void paint_keycaps(const StrList *keys, float left, float middle, Color color)
{
    GfxTextStyle style = { GFX_FONT_NUMBER, 11, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    float height = gfx_text_measure(L"0", &style, 0).height + 1 + 3;
    float top = middle - height / 2;
    for (int index = 0; index < keys->count; index++)
    {
        float width = gfx_text_measure(keys->items[index], &style, 0).width + 12;
        GfxRect cap = { left, top, left + width, top + height };
        gfx_stroke_rounded(cap, 3, color, 1);
        gfx_fill_rect((GfxRect){ cap.left + 2, cap.bottom - 2, cap.right - 2, cap.bottom - 1 }, color);
        gfx_text_draw(keys->items[index], (GfxRect){ cap.left, cap.top + 1, cap.right, cap.bottom - 2 }, &style, color);
        left += width + 3;
    }
}

/// 把快捷键拆成一个个按键；没有快捷键或没注册上时为空，和 C# 版的 HotkeyKeys 相同。
static void hotkey_keys(StrList *keys)
{
    strlist_clear(keys);
    const AppSettings *settings = app_settings();
    if (!settings->hotkey || !*settings->hotkey || !app_is_hotkey_registered()) return;
    const wchar_t *start = settings->hotkey;
    for (const wchar_t *cursor = start;; cursor++)
    {
        if (*cursor && *cursor != L'+') continue;
        strlist_add_owned(keys, wcs_dup_n(start, (size_t)(cursor - start)));
        if (!*cursor) break;
        start = cursor + 1;
    }
}

/// 一段折线，线条用墨色、圆角连接（入门提示和卡片上的小图共用）。
static void line_art(const float *points, int count, float x, float y, float opacity)
{
    float moved[16];
    for (int index = 0; index < count * 2 && index < 16; index += 2)
    {
        moved[index] = points[index] + x;
        moved[index + 1] = points[index + 1] + y;
    }
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    ink.a = (uint8_t)(ink.a * opacity);
    gfx_polyline(moved, count, ink, 1.4f);
}

static float glance_width(const PageTile *tile)
{
    switch (tile->page)
    {
        case PAGE_GENERAL: return keycaps_width(&tile->glance);
        case PAGE_SHELF: return 74;
        case PAGE_ACTIONS: return tile->glance.count * 31.0f;
        default: return 3 * 22.0f;
    }
}

/// 卡片右侧直接画出当前设置：快捷键、呼出方式、底部按钮、主题。left 是左边，middle 是竖直的中线。
static void paint_glance(const PageTile *tile, float left, float middle)
{
    const AppSettings *settings = app_settings();
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    switch (tile->page)
    {
        case PAGE_GENERAL:
            paint_keycaps(&tile->glance, left, middle, ink);
            break;
        case PAGE_SHELF:
        {
            // 左边的折线是晃动，右边的箭头和竖线是屏幕边缘；没用到的那种变淡。
            float top = middle - 13;
            float shake[] = { 2, 13, 8, 5, 15, 21, 22, 5, 29, 21, 35, 13 };
            line_art(shake, 6, left, top, settings->auto_open_trigger == TRIGGER_EDGE ? 0.25f : 1);
            float edge_opacity = settings->auto_open_trigger == TRIGGER_SHAKE ? 0.25f : 1;
            float shaft[] = { 48, 13, 62, 13 }, head[] = { 57, 8, 62, 13, 57, 18 };
            line_art(shaft, 2, left, top, edge_opacity);
            line_art(head, 3, left, top, edge_opacity);
            Color accent = theme_color(THEME_ACCENT);
            accent.a = (uint8_t)(accent.a * edge_opacity);
            gfx_line(left + 71, top + 1, left + 71, top + 25, accent, 2.4f);
            break;
        }
        case PAGE_ACTIONS:
        {
            GfxTextStyle glyph = { GFX_FONT_ICON, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
            for (int index = 0; index < tile->glance.count; index++)
            {
                GfxRect slot = { left + index * 31.0f, middle - 13, left + index * 31.0f + 28, middle + 13 };
                gfx_stroke_rounded(slot, 4, ink, 1);
                gfx_text_draw(tile->glance.items[index], slot, &glyph, ink);
            }
            break;
        }
        default:
        {
            // 浅色、深色两张牌和一张强调色的牌；没选的主题变淡。
            Color border = theme_color(THEME_CONTROL_BORDER), accent = theme_color(THEME_ACCENT);
            GfxRect chip = { left, middle - 12, left + 18, middle + 12 };
            gfx_push_opacity(settings->theme == THEME_DARK ? 0.45f : 1);
            gfx_fill_rounded(chip, 3, COLOR_RGB(0xFB, 0xFA, 0xF6));
            gfx_stroke_rounded(chip, 3, border, 1);
            gfx_pop_layer();
            chip.left += 22, chip.right += 22;
            gfx_push_opacity(settings->theme == THEME_LIGHT ? 0.45f : 1);
            gfx_fill_rounded(chip, 3, COLOR_RGB(0x26, 0x25, 0x22));
            gfx_stroke_rounded(chip, 3, border, 1);
            gfx_pop_layer();
            chip.left += 22, chip.right += 22;
            gfx_fill_rounded(chip, 3, accent);
            break;
        }
    }
}

/// 卡片左边的图标框：一张描边的小牌，悬停时铺满强调色。
static void paint_emblem(GfxRect box, const wchar_t *glyph, float glyph_size, bool hovered)
{
    if (hovered) gfx_fill_rounded(box, 6, theme_color(THEME_ACCENT));
    else gfx_stroke_rounded(box, 6, theme_color(THEME_TEXT_PRIMARY), 1);
    GfxTextStyle icon = { GFX_FONT_ICON, glyph_size, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    gfx_text_draw(glyph, box, &icon, theme_color(hovered ? THEME_TEXT_ON_ACCENT : THEME_TEXT_PRIMARY));
}

/// 卡片就是点开后会翻过来的那张牌：bounds 是卡片的位置（悬停抬起之后的），翻牌时也按这个画。
#endif
static void tile_paint_at(PageTile *tile, GfxRect bounds, bool hovered, bool pressed)
{
    UiElement *element = &tile->base;
    float height = bounds.bottom - bounds.top;
#ifdef LOOK_VARIANT_CARDS
    if (pressed) gfx_push_opacity(0.85f);
#else
    if (pressed) gfx_push_opacity(0.8f);
#endif
    gfx_fill_rounded(bounds, TILE_RADIUS, theme_color(hovered ? THEME_CARD_HOVER : THEME_CARD));
#ifdef LOOK_VARIANT_CARDS
    gfx_stroke_rounded(bounds, TILE_RADIUS, theme_color(hovered ? THEME_ACCENT : THEME_CARD_BORDER), 1);
#else
    gfx_stroke_rounded(bounds, TILE_RADIUS, theme_color(THEME_CARD_BORDER), 1);
#endif
    gfx_push_layer((GfxRect){ bounds.left + 1, bounds.top + 1, bounds.right - 1, bounds.bottom - 1 }, TILE_RADIUS - 1, 1);
    const wchar_t *glyph = main_page_glyph(tile->page);
#ifdef LOOK_VARIANT_CARDS
    Color primary = theme_color(THEME_TEXT_PRIMARY), secondary = theme_color(THEME_TEXT_SECONDARY);
#else
    Color accent = theme_color(THEME_ACCENT_TEXT);
#endif

    // 背景的大图标随高度渐显。
    float ramp = (height - 140) / (230 - 140);
    ramp = ramp < 0 ? 0 : ramp > 1 ? 1 : ramp;
    if (ramp > 0)
    {
        GfxTextStyle big = { GFX_FONT_ICON, 150, 400, false, false, GFX_ALIGN_END, GFX_ALIGN_END };
#ifdef LOOK_VARIANT_CARDS
        Color faint = primary;
        faint.a = (uint8_t)(faint.a * 0.06f * ramp);
#else
        Color faint = accent;
        faint.a = (uint8_t)(faint.a * 0.07f * ramp);
#endif
        gfx_text_draw(glyph, (GfxRect){ bounds.left, bounds.top, bounds.right - 1 + 16, bounds.bottom - 1 + 24 }, &big, faint);
    }
    float tall = (float)tile->tall_anim.value;
#ifndef LOOK_VARIANT_CARDS
    Color primary = theme_color(THEME_TEXT_PRIMARY), secondary = theme_color(THEME_TEXT_SECONDARY);
#endif
    if (tall < 0.999f)
    {
        // 一行的样式：图标在左，标题和摘要在右，竖直居中。
        gfx_push_opacity(1 - tall);
#ifdef LOOK_VARIANT_CARDS
        float middle = (bounds.top + bounds.bottom) / 2;
        GfxRect box = { bounds.left + 14, middle - 20, bounds.left + 14 + 40, middle + 20 };
        paint_emblem(box, glyph, 19, hovered);
        float glance = glance_width(tile);
        float glance_left = bounds.right - 16 - glance;
        if (glance > 0) paint_glance(tile, glance_left, middle);
        float left = box.right + 14, right = glance > 0 ? glance_left - 12 : bounds.right - 16;
#else
        GfxTextStyle icon = { GFX_FONT_ICON, 22, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
        float icon_width = gfx_text_measure(glyph, &icon, 0).width;
        gfx_text_draw(glyph, (GfxRect){ bounds.left + 1 + 18, bounds.top, bounds.left + 19 + icon_width + 1, bounds.bottom }, &icon, accent);
        float left = bounds.left + 1 + 18 + icon_width + 16, right = bounds.right - 1 - 16;
#endif
        GfxTextStyle header = { GFX_FONT_TEXT, 14, 600, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
        GfxTextStyle summary = { GFX_FONT_TEXT, 12, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
        float header_height = gfx_text_measure(S(main_page_title(tile->page)), &header, 0).height;
        float summary_height = gfx_text_measure(L"Ag", &summary, 0).height;
        float top = (bounds.top + bounds.bottom - header_height - 3 - summary_height) / 2;
        gfx_text_draw(S(main_page_title(tile->page)), (GfxRect){ left, top, right, top + header_height }, &header, primary);
        top += header_height + 3;
        gfx_text_draw(tile->summary ? tile->summary : L"", (GfxRect){ left, top, right, top + summary_height }, &summary, secondary);
        gfx_pop_layer();
    }
    if (tall > 0.001f)
    {
        // 大卡片：图标方块在左上，标题和标签在下。
        gfx_push_opacity(tall);
#ifdef LOOK_VARIANT_CARDS
        GfxRect box = { bounds.left + 20, bounds.top + 20, bounds.left + 20 + 44, bounds.top + 20 + 44 };
        paint_emblem(box, glyph, 21, hovered);
        float left = bounds.left + 20, right = bounds.right - 18, bottom = bounds.bottom - 20;
#else
        GfxRect box = { bounds.left + 1 + 22, bounds.top + 1 + 22, bounds.left + 1 + 22 + 44, bounds.top + 1 + 22 + 44 };
        gfx_fill_rounded(box, 10, theme_color(THEME_ACCENT_FAINT));
        GfxTextStyle icon = { GFX_FONT_ICON, 22, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        gfx_text_draw(glyph, box, &icon, accent);
        float left = bounds.left + 1 + 22, right = bounds.right - 1 - 18, bottom = bounds.bottom - 1 - 20;
#endif
        GfxTextStyle chip = { GFX_FONT_TEXT, 12, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
#ifdef LOOK_VARIANT_CARDS
        // 标签从左往右排，排不下换行，每个标签右边和下边留 6。先算出有几行，从下往上摆。
        float chip_height = gfx_text_measure(L"Ag", &chip, 0).height + 9;
#else
        // 标签从左往右排，排不下换行。先算出有几行，从下往上摆。
        float chip_height = gfx_text_measure(L"Ag", &chip, 0).height + 7;
#endif
        int rows = 1;
        float x = left;
        for (int index = 0; index < tile->items.count; index++)
        {
#ifdef LOOK_VARIANT_CARDS
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 20;
#else
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 18;
#endif
            if (x > left && x + width > right)
            {
                rows++;
                x = left;
            }
            x += width + 6;
        }
#ifdef LOOK_VARIANT_CARDS
        float chips_top = bottom - rows * (chip_height + 6);
#else
        float chips_top = bottom - rows * chip_height;
#endif
        GfxTextStyle header = { GFX_FONT_TEXT, 16, 600, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
        float header_height = gfx_text_measure(S(main_page_title(tile->page)), &header, 0).height;
        float header_top = chips_top - 10 - header_height;
        gfx_text_draw(S(main_page_title(tile->page)), (GfxRect){ left, header_top, right, header_top + header_height }, &header, primary);
        x = left;
        float y = chips_top;
        for (int index = 0; index < tile->items.count; index++)
        {
#ifdef LOOK_VARIANT_CARDS
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 20;
#else
            float width = gfx_text_measure(tile->items.items[index], &chip, 0).width + 18;
#endif
            if (x > left && x + width > right)
            {
#ifdef LOOK_VARIANT_CARDS
                y += chip_height + 6;
#else
                y += chip_height;
#endif
                x = left;
            }
            GfxRect pill = { x, y, x + width, y + chip_height };
#ifdef LOOK_VARIANT_CARDS
            gfx_fill_rounded(pill, 4, theme_color(THEME_SETTING_ROW));
            gfx_stroke_rounded(pill, 4, theme_color(THEME_CARD_BORDER), 1);
#else
            gfx_fill_rounded(pill, 11, theme_color(THEME_SUBTLE_SELECTED));
#endif
            gfx_text_draw(tile->items.items[index], (GfxRect){ pill.left, pill.top, pill.right, pill.bottom - 1 }, &chip, secondary);
            x += width + 6;
        }
        gfx_pop_layer();
    }
    gfx_pop_layer();
    if (pressed) gfx_pop_layer();
    (void)element;
}

static void tile_paint(UiElement *element, double time)
{
    PageTile *tile = (PageTile *)element;
    if (anim_update(&tile->tall_anim, time)) ui_window_request_frame(element->window);
#ifdef LOOK_VARIANT_CARDS
    if (anim_update(&tile->lift, time)) ui_window_request_frame(element->window);
    // 底下露出一道牌背，悬停时卡片微微抬起，牌背多露出一点。
    float lift = (float)tile->lift.value;
    GfxRect bounds = element->bounds;
    ThemePattern back = theme_pattern(THEME_PATTERN_STACK_EDGE);
    gfx_fill_rounded_pattern((GfxRect){ bounds.left + 3 + 2 * lift, bounds.top + 3 + 2 * lift, bounds.right + 3 + 2 * lift, bounds.bottom + 3 + 2 * lift },
        TILE_RADIUS, &back);
    bounds.top -= 2 * lift;
    bounds.bottom -= 2 * lift;
    tile_paint_at(tile, bounds, element->hovered, element->pressed && element->hovered);
#else
    tile_paint_at(tile, element->bounds, element->hovered, element->pressed && element->hovered);
#endif
}

static void tile_arrange(UiElement *element)
{
    PageTile *tile = (PageTile *)element;
    float height = element->bounds.bottom - element->bounds.top;
    if (tile->tall ? height < TALL_HEIGHT - TALL_HYSTERESIS : height >= TALL_HEIGHT + TALL_HYSTERESIS)
    {
        tile->tall = !tile->tall;
        // 两种样式交叉淡入，不随高度连续变化。
        anim_start(&tile->tall_anim, tile->tall ? 1 : 0, 0.2, EASE_OF(EASE_LINEAR));
        ui_window_request_frame(element->window);
    }
}

static void tile_mouse(UiElement *element, const UiMouseEvent *event)
{
    PageTile *tile = (PageTile *)element;
#ifdef LOOK_VARIANT_CARDS
    if (event->kind == UI_MOUSE_ENTER || event->kind == UI_MOUSE_LEAVE)
    {
        anim_start(&tile->lift, event->kind == UI_MOUSE_ENTER ? 1 : 0, 0.12, EASE_OF(EASE_LINEAR));
        ui_window_request_frame(element->window);
    }
#endif
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) flip_open(tile->page);
}

static void tile_destroy(UiElement *element)
{
    PageTile *tile = (PageTile *)element;
    free(tile->summary);
    strlist_clear(&tile->items);
#ifdef LOOK_VARIANT_CARDS
    strlist_clear(&tile->glance);
#endif
}

static const UiElementType tile_type = { NULL, tile_arrange, tile_paint, tile_mouse, tile_destroy, true };

void pages_paint_tile_face(UiElement *element, GfxMatrix matrix, float opacity)
{
    if (opacity <= 0.001f) return;
    PageTile *tile = (PageTile *)element;
    gfx_push_transform(matrix);
    if (opacity < 0.999f) gfx_push_opacity(opacity);
    tile_paint_at(tile, element->bounds, false, false);
    if (opacity < 0.999f) gfx_pop_layer();
    gfx_pop_transform();
}

static void tile_set_summary(UiElement *element, wchar_t *summary)
{
    PageTile *tile = (PageTile *)element;
    if (wcs_equal(tile->summary, summary))
    {
        free(summary);
        return;
    }
    free(tile->summary);
    tile->summary = summary;
    tile_split_summary(tile);
    ui_window_repaint(element->window);
}

bool pages_update_card_height(float viewport, bool resized)
{
    UiElement *page = M->overview_page;
    if (!page || page->desired.height <= 0 || viewport <= 0) return false;
    // 用量出来的高度减去卡片区域本身，结果与卡片当前的高度无关。
    float grid = M->tile_grid->desired.height + M->tile_grid->margin_top + M->tile_grid->margin_bottom;
    float rest = page->desired.height - grid;
    float spare = viewport - PAGE_TOP - PAGE_MARGIN - rest;
    float headroom = M->history_section->hidden ? 0 : FAN_HEADROOM;
    float cards = (spare - headroom) / 2 - TILE_GAP;
    cards = floorf(cards < MIN_CARD_HEIGHT ? MIN_CARD_HEIGHT : cards > MAX_CARD_HEIGHT ? MAX_CARD_HEIGHT : cards);
    if (fabsf(cards - P.card_target) < 1) return false;
    P.card_target = cards;
    // 拖动窗口边缘时直接跟随；只有页面内容变了（如收起入门提示）才平滑过渡，否则动画落后于窗口边缘，整页都在抖。
    if (!resized && M->tiles[0]->height > 0 && anim_enabled())
    {
        anim_start(&P.card_height, cards, MOTION_SECONDS, MOTION_EASE);
        main_request_frame();
        return false;
    }
    anim_set(&P.card_height, cards);
    for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->height = cards;
    return true;
}

// ───────────── 页面的框架 ─────────────

static void section_header(UiElement *parent, StringId text, float top, float bottom)
{
    UiElement *header = ui_label(UI_STR(text), UI_TEXT_BODY_STRONG);
    ui_set_margin(header, 2, top, 0, bottom);
    ui_add(parent, header);
}

/// 概览页的内容：页边距和最大宽度，状态栏出现时左边距加宽。内容不够高时撑满可见范围，牌堆贴着窗口底边。
static GfxSize column_measure(UiElement *element, float available)
{
    float left, right;
    main_content_columns(&left, &right);
    UiElement *child = element->children[0];
    ui_measure(child, right - left);
    (void)available;
    return (GfxSize){ available, child->desired.height + PAGE_TOP + PAGE_MARGIN };
}

static void column_arrange(UiElement *element)
{
    float left, right;
    main_content_columns(&left, &right);
    UiElement *child = element->children[0];
    ui_arrange(child, (GfxRect){ left, element->bounds.top + PAGE_TOP, right, element->bounds.bottom - PAGE_MARGIN });
}

static const UiElementType column_type = { column_measure, column_arrange, NULL, NULL, NULL, false };

/// 设置页的外层：翻牌时整页的变换和裁剪放在这里，里面的牌按页面那张牌的位置摆。
static GfxSize host_measure(UiElement *element, float available)
{
    GfxRect area = main_sheet_area();
    ui_measure(element->children[0], area.right - area.left);
    return (GfxSize){ available, 0 };
}

static void host_arrange(UiElement *element)
{
    ui_arrange(element->children[0], main_sheet_area());
}

static const UiElementType host_type = { host_measure, host_arrange, NULL, NULL, NULL, false };

/// 设置页那张牌：整张铺一层很淡的强调色，左上角稍浓；圆角 12，上下留一点空，滚动条不会伸进圆角。
static void sheet_paint(UiElement *element, double time)
{
    (void)time;
    PageView *view = element->tag;
    GfxRect bounds = element->bounds;
#ifdef LOOK_VARIANT_CARDS
    gfx_fill_rounded(bounds, SHEET_RADIUS, theme_color(THEME_CARD));
    ThemePattern back = theme_pattern(THEME_PATTERN_SHEET_BORDER);
    if (!view->sheet_border_hidden) gfx_stroke_rounded_pattern(bounds, SHEET_RADIUS, &back, SHEET_BAND);
#else
    float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    GfxStop stops[2] = { { 0, theme_color(THEME_SHEET_GLOW) }, { 0.7f, theme_color(THEME_STACK_EDGE) } };
    gfx_fill_rounded_radial(bounds, SHEET_RADIUS, bounds.left, bounds.top, width * 1.1f, height * 0.7f, stops, 2);
    if (!view->sheet_border_hidden) gfx_stroke_rounded(bounds, SHEET_RADIUS, theme_color(THEME_SHEET_BORDER), 1);
#endif
}

static void sheet_arrange(UiElement *element)
{
    GfxRect bounds = element->bounds;
#ifdef LOOK_VARIANT_CARDS
    ui_arrange(element->children[0], (GfxRect){ bounds.left + SHEET_BAND, bounds.top + SHEET_BAND + 6, bounds.right - SHEET_BAND, bounds.bottom - SHEET_BAND - 6 });
#else
    ui_arrange(element->children[0], (GfxRect){ bounds.left + 1, bounds.top + 1 + 6, bounds.right - 1, bounds.bottom - 1 - 6 });
#endif
}

static GfxSize sheet_measure(UiElement *element, float available)
{
#ifdef LOOK_VARIANT_CARDS
    ui_measure(element->children[0], available - 2 * SHEET_BAND);
#else
    ui_measure(element->children[0], available - 2);
#endif
    return (GfxSize){ available, 0 };
}

static const UiElementType sheet_type = { sheet_measure, sheet_arrange, sheet_paint, NULL, NULL, false };

static UiElement *pages_build_header_row(int page, float bottom)
{
    UiElement *row = ui_stack(true, 0);
    ui_set_margin(row, 0, 0, 0, bottom);
    UiElement *back = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_BACK), on_back, NULL);
    back->width = back->height = 36;
    ui_button_set_font_size(back, 14);
    ui_set_margin(back, -6, 3, 8, 0);
    ui_set_tooltip(back, UI_STR(STR_BackTooltip));
    ui_add(row, back);
    UiElement *title = ui_label(UI_STR(main_page_title(page)), UI_TEXT_TITLE);
    title->cross = UI_CROSS_CENTER;
    ui_add(row, title);
    return row;
}

static void build_page_frame(int page, float header_bottom)
{
    PageView *view = &M->pages[page];
    view->title = main_page_title(page);
    view->glyph = main_page_glyph(page);
    view->content = ui_stack(false, 0);
#ifdef LOOK_VARIANT_CARDS
    // 牌里的留白：两侧 14 加上 10 的牌背边框，正好等于 SHEET_PADDING，内容和概览页对齐。
    ui_stack_set_padding(view->content, 14, 10, 14, 24);
#else
    ui_stack_set_padding(view->content, 23, 12, 23, 24);
#endif
    view->header = pages_build_header_row(page, header_bottom);
    ui_add(view->content, view->header);
    view->scroll = ui_scroll(view->content);
    view->sheet = ui_element_new(&sheet_type, sizeof(UiElement));
    view->sheet->tag = view;
    ui_add(view->sheet, view->scroll);
    view->host = ui_element_new(&host_type, sizeof(UiElement));
    view->host->hidden = true;
    ui_add(view->host, view->sheet);
}

static UiElement *add_card(UiElement *parent, const wchar_t *glyph, UiText header, UiText description, UiElement *control, UiElement **description_label)
{
    UiElement *card = ui_settings_card(glyph, header, description, control, description_label);
    ui_add(parent, card);
    return card;
}

static UiElement *segmented(const StringId *items, int count, int selected, UiSelectHandler changed)
{
    UiText texts[4];
    for (int index = 0; index < count; index++) texts[index] = UI_STR(items[index]);
    return ui_segmented(texts, count, selected, changed, NULL);
}

static UiElement *wide_button(StringId text, UiClickHandler clicked)
{
    UiElement *button = ui_button(UI_BUTTON_STANDARD, UI_STR(text), clicked, NULL);
    button->minimum_width = 96;
    return button;
}

// ───────────── 常规 ─────────────

static UiElement *build_language_pack_selector(void)
{
    int count = language_packs_count();
    UiText *texts = xcalloc((size_t)count + 1, sizeof(UiText));
    texts[0] = UI_STR(STR_LanguagePackNone);
    for (int index = 0; index < count; index++) texts[index + 1] = UI_LIT(language_pack_name(language_packs_at(index)));
    UiElement *selector = ui_segmented(texts, count + 1, 0, on_language_pack, NULL);
    free(texts);
    P.language_pack_count = count;
    return selector;
}

static void build_general_page(void)
{
    build_page_frame(PAGE_GENERAL, 20);
    UiElement *content = M->pages[PAGE_GENERAL].content;
    const AppSettings *settings = app_settings();
    // 语言名称始终用该语言本身书写，切换错了也能认出来。
    UiText languages[3] = { UI_STR(STR_FollowSystem), UI_LIT(L"中文"), UI_LIT(L"English") };
    P.language = ui_segmented(languages, 3, settings->language, on_language, NULL);
    add_card(content, GLYPH_LANGUAGE, UI_STR(STR_Language), UI_STR(STR_LanguageDescription), P.language, NULL);
    // 找到语言包时才显示：用别的语言显示界面。
    P.language_pack = build_language_pack_selector();
    P.language_pack_card = add_card(content, GLYPH_LANGUAGE, UI_STR(STR_LanguagePackHeader), UI_STR(STR_LanguagePackDescription), P.language_pack, NULL);
    P.startup = ui_toggle(settings->start_with_windows, on_startup, NULL);
    add_card(content, GLYPH_POWER, UI_STR(STR_StartWithWindows), UI_STR(STR_StartupDescription), P.startup, &P.startup_description);
    P.hotkey = ui_hotkey_box(settings->hotkey, on_hotkey, NULL);
    add_card(content, GLYPH_KEYBOARD, UI_STR(STR_GlobalHotkey), UI_STR(STR_HotkeyRegistered), P.hotkey, &P.hotkey_description);
    const StringId modes[3] = { STR_PerformanceAuto, STR_PerformanceOff, STR_PerformanceOn };
    P.performance = segmented(modes, 3, settings->performance_mode, on_performance);
    add_card(content, GLYPH_LIGHTNING, UI_STR(STR_LowPerformanceMode), UI_STR(STR_PerformanceDescriptionAuto), P.performance, &P.performance_description);
    add_card(content, GLYPH_HELP, UI_STR(STR_Tutorial), UI_STR(STR_TutorialDescription), wide_button(STR_ViewTutorial, on_view_tutorial), NULL);
}

// ───────────── 手势与置物架 ─────────────

static void build_shelf_page(void)
{
    build_page_frame(PAGE_SHELF, 20);
    UiElement *content = M->pages[PAGE_SHELF].content;
    const AppSettings *settings = app_settings();
    section_header(content, STR_AutoOpen, 0, 8);
    const StringId triggers[3] = { STR_TriggerShake, STR_TriggerEdge, STR_TriggerBoth };
    P.trigger = segmented(triggers, 3, settings->auto_open_trigger, on_trigger);
    add_card(content, GLYPH_EXPAND, UI_STR(STR_TriggerHeader), UI_STR(STR_TriggerDescription), P.trigger, NULL);
    P.sensitivity_card = add_card(content, GLYPH_LIGHTNING, UI_STR(STR_ShakeSensitivity), UI_LIT(L" "), build_sensitivity_control(),
        &P.sensitivity_description);
    const StringId dwells[3] = { STR_DwellShort, STR_DwellMedium, STR_DwellLong };
    P.edge_dwell = segmented(dwells, 3, settings->edge_dwell, on_edge_dwell);
    P.edge_dwell_card = add_card(content, GLYPH_TIMER, UI_STR(STR_EdgeDwellHeader), UI_STR(STR_EdgeDwellDescription), P.edge_dwell, NULL);
    P.add_app_button = wide_button(STR_AddApp, on_add_app);
    add_card(content, GLYPH_SHIELD, UI_STR(STR_BlockedAppsHeader), UI_STR(STR_BlockedAppsDescription), P.add_app_button, NULL);
    P.blocked_list = ui_stack(false, 0);
    ui_add(content, P.blocked_list);
#ifdef __linux__
    // Windows 版本来就只在屏蔽列表里暂停，没有全屏这一层，这个开关只给 Linux。
    P.respond_fullscreen = ui_toggle(settings->respond_when_fullscreen, on_respond_fullscreen, NULL);
    add_card(content, GLYPH_EXPAND, UI_STR(STR_RespondInFullscreen), UI_STR(STR_RespondInFullscreenDescription), P.respond_fullscreen, NULL);
#endif

    section_header(content, STR_Shelf, 24, 8);
    const StringId sizes[3] = { STR_SizeSmall, STR_SizeMedium, STR_SizeLarge };
    P.shelf_size = segmented(sizes, 3, settings->quick_shelf_size, on_shelf_size);
    add_card(content, GLYPH_VIEW_ALL, UI_STR(STR_Size), UI_STR(STR_SizeDescription), P.shelf_size, NULL);
    const StringId transfers[2] = { STR_TransferMove, STR_TransferCopy };
    P.drag_out_mode = segmented(transfers, 2, settings->drag_out_mode, on_drag_out);
    add_card(content, GLYPH_MOVE, UI_STR(STR_DragOutModeHeader), UI_STR(STR_DragOutModeDescription), P.drag_out_mode, NULL);
#ifdef __linux__
    // 一次拖放里 WPS 这类 X11 程序只肯插入一种内容，怎么把文件和文字都送过去只有 Linux 版管得上。
    const StringId mixed[4] = { STR_MixedDragSmart, STR_MixedDragBatch, STR_MixedDragFiles, STR_MixedDragText };
    P.mixed_drag = segmented(mixed, 4, settings->mixed_drag, on_mixed_drag);
    add_card(content, GLYPH_DOCUMENT, UI_STR(STR_MixedDragHeader), UI_STR(STR_MixedDragDescription), P.mixed_drag, NULL);
#endif
    P.close_when_empty = ui_toggle(settings->close_when_empty, on_close_when_empty, NULL);
    add_card(content, GLYPH_CLEAR, UI_STR(STR_CloseWhenEmpty), UI_STR(STR_CloseWhenEmptyDescription), P.close_when_empty, NULL);
    P.clear_history = wide_button(STR_Clear, on_clear_history);
    add_card(content, GLYPH_HISTORY, UI_STR(STR_ClearShelfHistory), UI_STR(STR_HistoryEmpty), P.clear_history, &P.history_description);

    section_header(content, STR_WatchFolders, 24, 8);
    P.add_folder_button = wide_button(STR_AddFolder, on_add_watch_folder);
    add_card(content, GLYPH_VIEW, UI_STR(STR_WatchedFoldersHeader), UI_STR(STR_WatchFoldersDescription), P.add_folder_button, NULL);
    P.watch_list = ui_stack(false, 0);
    ui_add(content, P.watch_list);
    P.peek_seconds_card = add_card(content, GLYPH_TIMER, UI_STR(STR_WatchPeekSecondsHeader), UI_STR(STR_WatchPeekSecondsDescription),
        build_peek_seconds_control(), NULL);
}

// ───────────── 快速操作 ─────────────

static void build_actions_page(void)
{
    build_page_frame(PAGE_ACTIONS, 8);
    UiElement *content = M->pages[PAGE_ACTIONS].content;
    UiElement *description = ui_label(UI_STR(STR_ActionsPageDescription), UI_TEXT_CAPTION);
    ui_label_set_size(description, 13);
    ui_set_margin(description, 0, 0, 0, 20);
    ui_add(content, description);
    const StringId layouts[3] = { STR_LayoutOff, STR_LayoutTwo, STR_LayoutFour };
    int layout = app_settings()->quick_action_layout;
    P.layout = segmented(layouts, 3, layout == LAYOUT_FOUR ? 2 : layout == LAYOUT_TWO ? 1 : 0, on_layout);
    add_card(content, GLYPH_VIEW_ALL, UI_STR(STR_ActionLayoutHeader), UI_STR(STR_ActionLayoutDescription), P.layout, &P.layout_description);
    actions_build(content);
}

// ───────────── 外观 ─────────────

typedef struct ThemeTile
{
    UiElement base;
    int theme;
} ThemeTile;

static GfxSize theme_tile_measure(UiElement *element, float available)
{
    (void)element;
    (void)available;
    GfxTextStyle label = { GFX_FONT_TEXT, 13, 400 };
    return (GfxSize){ 140 + 12 + 4, 92 + 8 + gfx_text_measure(L"Ag", &label, 0).height + 2 + 12 + 4 };
}

static void preview_box(GfxRect rect, float radius, uint32_t fill, uint32_t border)
{
    Color fill_color = COLOR_ARGB((uint8_t)(fill >> 24), (uint8_t)(fill >> 16), (uint8_t)(fill >> 8), (uint8_t)fill);
    Color border_color = COLOR_ARGB((uint8_t)(border >> 24), (uint8_t)(border >> 16), (uint8_t)(border >> 8), (uint8_t)border);
    gfx_fill_rounded(rect, radius, fill_color);
    if (border) gfx_stroke_rounded(rect, radius, border_color, 1);
}

#ifdef LOOK_VARIANT_CARDS
/// 预览中间那张竖放的牌。
static GfxRect centered_card(GfxRect outer)
#else
static GfxRect centered(GfxRect outer, float size)
#endif
{
#ifdef LOOK_VARIANT_CARDS
    float x = (outer.left + outer.right - 44) / 2, y = (outer.top + outer.bottom - 60) / 2;
    return (GfxRect){ x, y, x + 44, y + 60 };
#else
    float x = (outer.left + outer.right - size) / 2, y = (outer.top + outer.bottom - size) / 2;
    return (GfxRect){ x, y, x + size, y + size };
#endif
}

static void theme_tile_paint(UiElement *element, double time)
{
    (void)time;
    ThemeTile *tile = (ThemeTile *)element;
    GfxRect bounds = element->bounds;
    bool selected = app_settings()->theme == tile->theme;
    if (element->hovered) gfx_fill_rounded(bounds, 10, theme_color(THEME_SUBTLE_HOVER));
    if (selected) gfx_stroke_rounded(bounds, 10, theme_color(THEME_ACCENT), 2);
    GfxRect preview = { bounds.left + 8, bounds.top + 8, bounds.left + 8 + 140, bounds.top + 8 + 92 };
    switch (tile->theme)
    {
        case THEME_LIGHT:
#ifdef LOOK_VARIANT_CARDS
            preview_box(preview, 7, 0xFFE8E5DE, 0xFFD2CEC5);
            preview_box(centered_card(preview), 5, 0xFFFBFAF6, 0xFF15171C);
#else
            preview_box(preview, 7, 0xFFF9F7F1, 0x14000000);
            preview_box(centered(preview, 64), 8, 0xFFFFFFFF, 0x1A000000);
            preview_box(centered(preview, 30), 5, 0xFFF9F7F1, 0x26000000);
#endif
            break;
        case THEME_DARK:
#ifdef LOOK_VARIANT_CARDS
            preview_box(preview, 7, 0xFF1C1B19, 0xFF3A3834);
            preview_box(centered_card(preview), 5, 0xFF262522, 0xFFA6A39C);
#else
            preview_box(preview, 7, 0xFF202020, 0x33FFFFFF);
            preview_box(centered(preview, 64), 8, 0xFF2B2B2B, 0x26FFFFFF);
            preview_box(centered(preview, 30), 5, 0xFF3B3B3B, 0x33FFFFFF);
#endif
            break;
        default:
        {
            // 跟随系统：左浅右深。
            float middle = (preview.left + preview.right) / 2;
            gfx_push_layer(preview, 7, 1);
#ifdef LOOK_VARIANT_CARDS
            gfx_fill_rect((GfxRect){ preview.left, preview.top, middle, preview.bottom }, COLOR_RGB(0xE8, 0xE5, 0xDE));
            gfx_fill_rect((GfxRect){ middle, preview.top, preview.right, preview.bottom }, COLOR_RGB(0x1C, 0x1B, 0x19));
#else
            gfx_fill_rect((GfxRect){ preview.left, preview.top, middle, preview.bottom }, COLOR_RGB(0xF9, 0xF7, 0xF1));
            gfx_fill_rect((GfxRect){ middle, preview.top, preview.right, preview.bottom }, COLOR_RGB(0x20, 0x20, 0x20));
#endif
            gfx_pop_layer();
#ifdef LOOK_VARIANT_CARDS
            // 牌也是左右各一半：左边浅色的牌面配墨色的边，右边深色的牌面配浅灰的边。
            GfxRect card = centered_card(preview);
            float half = (card.left + card.right) / 2;
            gfx_push_clip((GfxRect){ card.left, card.top, half, card.bottom });
            preview_box(card, 5, 0xFFFBFAF6, 0xFF15171C);
            gfx_pop_clip();
            gfx_push_clip((GfxRect){ half, card.top, card.right, card.bottom });
            preview_box(card, 5, 0xFF262522, 0xFFA6A39C);
            gfx_pop_clip();
#else
            preview_box(centered(preview, 64), 8, 0x80808080, 0);
            preview_box(centered(preview, 30), 5, 0xFFFFFFFF, 0x33000000);
#endif
            break;
        }
    }
    StringId names[3] = { STR_FollowSystem, STR_ThemeLight, STR_ThemeDark };
    GfxTextStyle label = { GFX_FONT_TEXT, 13, 400, false, true, GFX_ALIGN_START, GFX_ALIGN_START };
    gfx_text_draw(S(names[tile->theme]), (GfxRect){ preview.left + 2, preview.bottom + 8, preview.right, bounds.bottom - 6 }, &label,
        theme_color(THEME_TEXT_PRIMARY));
}

static void theme_tile_mouse(UiElement *element, const UiMouseEvent *event)
{
    ThemeTile *tile = (ThemeTile *)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button && app_settings()->theme != tile->theme) SET_INT(theme, tile->theme);
}

static const UiElementType theme_tile_type = { theme_tile_measure, NULL, theme_tile_paint, theme_tile_mouse, NULL, true };

#ifdef LOOK_VARIANT_CARDS
/// 强调色色块的编号：先是默认的钴蓝、跟随 Windows，后面是 theme_accent_preset 的各个颜色。
#define SWATCH_COBALT (-2)
#define SWATCH_SYSTEM (-1)

#else
/// 强调色色块：选中时外圈和对勾。index 为 -1 是跟随 Windows。
#endif
typedef struct Swatch
{
    UiElement base;
    int index;
} Swatch;

static GfxSize swatch_measure(UiElement *element, float available)
{
    (void)element;
    (void)available;
#ifdef LOOK_VARIANT_CARDS
    return (GfxSize){ 30, 38 };
}

/// 色块代表的设置值：钴蓝是 NULL，跟随 Windows 是 "system"。
static const wchar_t *swatch_value(const Swatch *swatch)
{
    if (swatch->index == SWATCH_COBALT) return NULL;
    if (swatch->index == SWATCH_SYSTEM) return THEME_SYSTEM_ACCENT_VALUE;
    return theme_accent_preset(swatch->index);
#else
    return (GfxSize){ 32, 32 };
#endif
}

static bool swatch_selected(const Swatch *swatch)
{
#ifdef LOOK_VARIANT_CARDS
    const wchar_t *accent = app_settings()->accent_color, *value = swatch_value(swatch);
    if (!value) return !accent || !*accent;
    return accent && wcs_equal_ignore_case(accent, value);
#else
    const wchar_t *accent = app_settings()->accent_color;
    if (swatch->index < 0) return !accent || !*accent;
    return accent && wcs_equal_ignore_case(accent, theme_accent_preset(swatch->index));
#endif
}

static void swatch_paint(UiElement *element, double time)
{
    (void)time;
    Swatch *swatch = (Swatch *)element;
    GfxRect bounds = element->bounds;
#ifndef LOOK_VARIANT_CARDS
    float x = (bounds.left + bounds.right) / 2, y = (bounds.top + bounds.bottom) / 2;
#endif
    bool selected = swatch_selected(swatch);
    float ring = selected ? 1.0f : element->hovered ? 0.35f : 0;
    if (ring > 0)
    {
        Color color = theme_color(THEME_TEXT_PRIMARY);
        color.a = (uint8_t)(color.a * ring);
#ifdef LOOK_VARIANT_CARDS
        gfx_stroke_rounded(bounds, 6, color, 2);
#else
        gfx_stroke_ellipse(x, y, 16, 16, color, 2);
#endif
    }
#ifdef LOOK_VARIANT_CARDS
    Color fill = swatch->index == SWATCH_SYSTEM ? theme_color(THEME_SYSTEM_ACCENT) : THEME_COBALT;
#else
    Color fill = theme_color(THEME_SYSTEM_ACCENT);
#endif
    COLORREF rgb;
    if (swatch->index >= 0 && accent_color_parse(theme_accent_preset(swatch->index), &rgb)) fill = COLOR_RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
#ifdef LOOK_VARIANT_CARDS
    gfx_fill_rounded((GfxRect){ bounds.left + 5, bounds.top + 5, bounds.right - 5, bounds.bottom - 5 }, 3, fill);
#else
    gfx_fill_ellipse(x, y, 11, 11, fill);
#endif
    if (selected)
    {
        GfxTextStyle check = { GFX_FONT_ICON, 11, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        gfx_text_draw(GLYPH_CHECK, bounds, &check, COLOR_WHITE);
    }
}

static void change_accent(AppSettings *settings, void *context)
{
    free(settings->accent_color);
    settings->accent_color = context ? wcs_dup(context) : NULL;
}

static void swatch_mouse(UiElement *element, const UiMouseEvent *event)
{
    Swatch *swatch = (Swatch *)element;
    if (event->kind != UI_MOUSE_UP || !event->inside || event->right_button || swatch_selected(swatch)) return;
#ifdef LOOK_VARIANT_CARDS
    app_update_settings(change_accent, (void *)swatch_value(swatch));
#else
    app_update_settings(change_accent, swatch->index >= 0 ? (void *)theme_accent_preset(swatch->index) : NULL);
#endif
}

static const UiElementType swatch_type = { swatch_measure, NULL, swatch_paint, swatch_mouse, NULL, true };

static void build_appearance_page(void)
{
    build_page_frame(PAGE_APPEARANCE, 20);
    UiElement *content = M->pages[PAGE_APPEARANCE].content;
    UiText looks[2] = { UI_STR(STR_LookSimple), UI_STR(STR_LookCards) };
    P.look = ui_segmented(looks, 2, app_settings()->look, on_look, NULL);
    UiElement *look_card = add_card(content, GLYPH_STYLE, UI_STR(STR_Look), UI_STR(STR_LookDescription), P.look, NULL);
    ui_set_margin(look_card, 0, 0, 0, 20);
    section_header(content, STR_Theme, 0, 10);
    UiElement *themes = ui_stack(true, 12);
    for (int index = 0; index < 3; index++)
    {
        ThemeTile *tile = (ThemeTile *)ui_element_new(&theme_tile_type, sizeof(ThemeTile));
        tile->theme = index;
        P.theme_tiles[index] = &tile->base;
        ui_add(themes, &tile->base);
    }
    ui_add(content, themes);

    // 强调色：一张卡片，色块排在标题下面。
    UiElement *card = ui_stack(false, 0);
    ui_stack_set_padding(card, 20, 14, 16, 14);
#ifdef LOOK_VARIANT_CARDS
    ui_stack_set_background(card, THEME_SETTING_ROW, THEME_SETTING_ROW_BORDER, 6);
#else
    ui_stack_set_background(card, THEME_CARD, THEME_CARD_BORDER, 8);
#endif
    ui_set_margin(card, 0, 20, 0, 0);
    UiElement *row = ui_stack(true, 0);
    UiElement *icon = ui_label(UI_LIT(GLYPH_BRUSH), UI_TEXT_GLYPH);
    ui_label_set_size(icon, 20);
    ui_set_margin(icon, 0, 0, 20, 0);
    ui_add(row, icon);
    UiElement *texts = ui_stack(false, 0);
    texts->grow = 1;
    ui_add(texts, ui_label(UI_STR(STR_AccentColor), UI_TEXT_BODY));
    UiElement *description = ui_label(UI_STR(STR_AccentDescription), UI_TEXT_CAPTION);
    ui_set_margin(description, 0, 2, 0, 0);
    ui_add(texts, description);
    P.swatches = ui_stack(true, 4);
    ui_set_margin(P.swatches, -5, 10, 0, 0);
#ifdef LOOK_VARIANT_CARDS
    for (int index = SWATCH_COBALT; index < theme_accent_preset_count(); index++)
#else
    for (int index = -1; index < theme_accent_preset_count(); index++)
#endif
    {
        Swatch *swatch = (Swatch *)ui_element_new(&swatch_type, sizeof(Swatch));
        swatch->index = index;
#ifdef LOOK_VARIANT_CARDS
        if (index == SWATCH_COBALT) ui_set_tooltip(&swatch->base, UI_STR(STR_AccentCobalt));
#endif
        // 跟随 Windows 显示系统当前的强调色，并与固定颜色之间留出间隔。
#ifdef LOOK_VARIANT_CARDS
        if (index == SWATCH_SYSTEM)
#else
        if (index < 0)
#endif
        {
            ui_set_margin(&swatch->base, 0, 0, 12, 0);
            ui_set_tooltip(&swatch->base, UI_STR(STR_AccentFollowSystem));
        }
        ui_add(P.swatches, &swatch->base);
    }
    ui_add(texts, P.swatches);
    ui_add(row, texts);
    ui_add(card, row);
    ui_add(content, card);
}

// ───────────── 概览页 ─────────────

#ifdef LOOK_VARIANT_CARDS
/// 入门提示顶上的一行：左边是第几步，右上角的小图画出这一步怎么做（线条用墨色，重点用强调色）。tag 是第几步（1 到 3）。
static GfxSize tip_header_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 26 };
}

static void tip_rect(GfxRect rect, float angle, Color fill, bool dashed)
{
    Color ink = theme_color(THEME_TEXT_PRIMARY);
    if (angle != 0) gfx_push_transform(gfx_matrix_rotate(angle, (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2));
    gfx_fill_rounded(rect, 2, fill);
    if (dashed) gfx_stroke_rounded_dashed(rect, 2, ink, 1, 2, 2);
    else gfx_stroke_rounded(rect, 2, ink, 1);
    if (angle != 0) gfx_pop_transform();
}

static void tip_header_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    int step = (int)(intptr_t)element->tag;
    wchar_t rank[2] = { (wchar_t)(L'0' + step), 0 };
    GfxTextStyle style = { GFX_FONT_RANK, 24, 600, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };
    gfx_text_draw(rank, bounds, &style, theme_color(THEME_TEXT_PRIMARY));
    // 72 × 24 的小图靠右，竖直居中。
    float x = bounds.right - 72, y = (bounds.top + bounds.bottom) / 2 - 12;
    Color accent = theme_color(THEME_ACCENT), card = theme_color(THEME_CARD);
    switch (step)
    {
        case 1:
        {
            float shake[] = { 2, 12, 9, 4, 17, 20, 25, 4, 33, 20, 40, 12 }, shaft[] = { 50, 12, 62, 12 }, head[] = { 58, 8, 62, 12, 58, 16 };
            line_art(shake, 6, x, y, 1);
            line_art(shaft, 2, x, y, 1);
            line_art(head, 3, x, y, 1);
            gfx_line(x + 69, y + 1, x + 69, y + 23, accent, 2.4f);
            break;
        }
        case 2:
        {
            tip_rect((GfxRect){ x + 2.5f, y + 5.5f, x + 16.5f, y + 22.5f }, -8, card, false);
            tip_rect((GfxRect){ x + 8.5f, y + 4.5f, x + 22.5f, y + 21.5f }, 6, card, false);
            tip_rect((GfxRect){ x + 5.5f, y + 4.5f, x + 19.5f, y + 21.5f }, 0, accent, false);
            float shaft[] = { 30, 13, 64, 13 }, head[] = { 60, 9, 64, 13, 60, 17 };
            line_art(shaft, 2, x, y, 1);
            line_art(head, 3, x, y, 1);
            break;
        }
        default:
        {
            float shaft[] = { 20, 1, 20, 10 }, head[] = { 16, 7, 20, 11, 24, 7 };
            line_art(shaft, 2, x, y, 1);
            line_art(head, 3, x, y, 1);
            tip_rect((GfxRect){ x + 4.5f, y + 13.5f, x + 35.5f, y + 23.5f }, 0, accent, false);
            tip_rect((GfxRect){ x + 38.5f, y + 13.5f, x + 69.5f, y + 23.5f }, 0, COLOR_TRANSPARENT, true);
            break;
        }
    }
}

static const UiElementType tip_header_type = { tip_header_measure, NULL, tip_header_paint, NULL, NULL, false };

/// 三张入门提示按顺序编号，右上角的小图画出这一步怎么做。
static UiElement *tip_card(int step, StringId title, StringId body)
#else
static UiElement *tip_card(const wchar_t *glyph, StringId title, StringId body)
#endif
{
    UiElement *card = ui_stack(false, 0);
#ifdef LOOK_VARIANT_CARDS
    ui_stack_set_padding(card, 14, 12, 16, 16);
#else
    ui_stack_set_padding(card, 16, 16, 16, 18);
#endif
    ui_stack_set_background(card, THEME_CARD, THEME_CARD_BORDER, 8);
#ifdef LOOK_VARIANT_CARDS
    UiElement *top = ui_element_new(&tip_header_type, sizeof(UiElement));
    top->tag = (void *)(intptr_t)step;
    ui_add(card, top);
#else
    UiElement *icon = ui_label(UI_LIT(glyph), UI_TEXT_GLYPH);
    ui_label_set_size(icon, 22);
    ui_label_set_color(icon, THEME_ACCENT_TEXT);
    icon->cross = UI_CROSS_START;
    ui_add(card, icon);
#endif
    UiElement *header = ui_label(UI_STR(title), UI_TEXT_BODY_STRONG);
    ui_label_style(header)->wrap = true;
#ifdef LOOK_VARIANT_CARDS
    ui_set_margin(header, 0, 8, 0, 4);
#else
    ui_set_margin(header, 0, 12, 0, 4);
#endif
    ui_add(card, header);
    ui_add(card, ui_label(UI_STR(body), UI_TEXT_CAPTION));
    return card;
}

#ifdef LOOK_VARIANT_CARDS
/// 概览页右上角的“打开置物架”：强调色按钮，文字后面跟着快捷键的键帽（淡一些）。
typedef struct OpenButton
{
    UiElement base;
    StrList keys;
} OpenButton;

static const GfxTextStyle open_label_style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_CENTER };

static GfxSize open_button_measure(UiElement *element, float available)
{
    (void)available;
    OpenButton *button = (OpenButton *)element;
    GfxSize text = gfx_text_measure(S(STR_OpenShelf), &open_label_style, 0);
    float keys = button->keys.count > 0 ? 12 + keycaps_width(&button->keys) - 3 : 0;
    return (GfxSize){ 16 + text.width + keys + 10, text.height + 6 + 7 };
}

static void open_button_paint(UiElement *element, double time)
{
    (void)time;
    OpenButton *button = (OpenButton *)element;
    GfxRect bounds = element->bounds;
    bool pressed = element->pressed && element->hovered;
    gfx_fill_rounded(bounds, 6, theme_color(pressed ? THEME_ACCENT_PRESSED : element->hovered ? THEME_ACCENT_HOVER : THEME_ACCENT));
    Color text = theme_color(THEME_TEXT_ON_ACCENT);
    float width = gfx_text_measure(S(STR_OpenShelf), &open_label_style, 0).width;
    gfx_text_draw(S(STR_OpenShelf), (GfxRect){ bounds.left + 16, bounds.top, bounds.left + 16 + width + 1, bounds.bottom - 1 }, &open_label_style, text);
    if (button->keys.count == 0) return;
    gfx_push_opacity(0.75f);
    paint_keycaps(&button->keys, bounds.left + 16 + width + 12, (bounds.top + bounds.bottom) / 2, text);
    gfx_pop_layer();
}

static void open_button_mouse(UiElement *element, const UiMouseEvent *event)
{
    (void)element;
    if (event->kind == UI_MOUSE_UP && event->inside && !event->right_button) on_open_shelf(NULL);
}

static void open_button_destroy(UiElement *element)
{
    strlist_clear(&((OpenButton *)element)->keys);
}

static const UiElementType open_button_type = { open_button_measure, NULL, open_button_paint, open_button_mouse, open_button_destroy, true };
static UiElement *open_button;

/// 最近关闭的置物架下面的牌桌：铺满窗口宽度，从标题上方一点一直铺到窗口底边（被窗口底边裁掉）。
static void table_paint(UiElement *element, double time)
{
    (void)time;
    ThemePattern table = theme_pattern(THEME_PATTERN_TABLE);
    gfx_fill_rounded_pattern((GfxRect){ M->stage.left, element->bounds.top + 12, M->stage.right, element->bounds.top + 12 + 4000 }, 0, &table);
}

static GfxSize table_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 0 };
}

static const UiElementType table_type = { table_measure, NULL, table_paint, NULL, NULL, false };
static UiElement *history_tally;

#endif
static void build_overview(void)
{
    UiElement *page = ui_stack(false, 0);
    M->overview_page = page;

    // 标题行：窗口宽到显示左侧状态栏时，标题移到状态栏图标右侧，这一行收起。
    UiElement *header = ui_stack(true, 0);
    M->overview_header = header;
    UiElement *title = ui_label(UI_STR(STR_Overview), UI_TEXT_TITLE);
    title->cross = UI_CROSS_CENTER;
    title->grow = 1;
    ui_add(header, title);
#ifdef LOOK_VARIANT_CARDS
    UiElement *open = ui_element_new(&open_button_type, sizeof(OpenButton));
#else
    UiElement *open = ui_button(UI_BUTTON_ACCENT, UI_STR(STR_OpenShelf), on_open_shelf, NULL);
    ui_button_set_glyph(open, GLYPH_ADD, 13);
    ui_button_set_padding(open, 16, 6, 16, 7);
#endif
    open->cross = UI_CROSS_CENTER;
#ifdef LOOK_VARIANT_CARDS
    open_button = open;
#endif
    ui_add(header, open);
    ui_add(page, header);

    UiElement *body = ui_stack(false, 0);
    M->overview_body = body;
    P.getting_started = ui_stack(false, 0);
    UiElement *getting_header = ui_stack(true, 0);
    UiElement *getting_title = ui_label(UI_STR(STR_GettingStarted), UI_TEXT_BODY_STRONG);
    ui_set_margin(getting_title, 2, 24, 0, 8);
    getting_title->grow = 1;
    ui_add(getting_header, getting_title);
    UiElement *dismiss = ui_button(UI_BUTTON_ICON, UI_LIT(GLYPH_CLOSE), on_dismiss_getting_started, NULL);
    dismiss->width = dismiss->height = 28;
    ui_button_set_font_size(dismiss, 10);
    dismiss->cross = UI_CROSS_END;
    ui_set_margin(dismiss, 0, 0, 0, 4);
    ui_set_tooltip(dismiss, UI_STR(STR_DismissTooltip));
    ui_add(getting_header, dismiss);
    ui_add(P.getting_started, getting_header);
    UiElement *tips = ui_grid(3, 9, 0);
#ifdef LOOK_VARIANT_CARDS
    ui_add(tips, tip_card(1, STR_TipShakeTitle, STR_TipShakeBody));
    ui_add(tips, tip_card(2, STR_TipStackTitle, STR_TipStackBody));
    ui_add(tips, tip_card(3, STR_QuickActionsTitle, STR_TipActionsBody));
#else
    ui_add(tips, tip_card(GLYPH_MOUSE, STR_TipShakeTitle, STR_TipShakeBody));
    ui_add(tips, tip_card(GLYPH_LIBRARY, STR_TipStackTitle, STR_TipStackBody));
    ui_add(tips, tip_card(GLYPH_LIGHTNING, STR_QuickActionsTitle, STR_TipActionsBody));
#endif
    ui_add(P.getting_started, tips);
    ui_add(body, P.getting_started);

    section_header(body, STR_SettingsSection, 24, 8);
    M->tile_grid = ui_grid(2, TILE_GAP, TILE_GAP);
    ui_set_margin(M->tile_grid, 0, 0, 0, TILE_GAP);
    for (int index = 0; index < PAGE_COUNT; index++)
    {
        PageTile *tile = (PageTile *)ui_element_new(&tile_type, sizeof(PageTile));
        tile->page = index;
        tile->base.height = MIN_CARD_HEIGHT;
        M->tiles[index] = &tile->base;
        ui_add(M->tile_grid, &tile->base);
    }
    ui_add(body, M->tile_grid);
    ui_add(page, body);
    anim_set(&P.card_height, MIN_CARD_HEIGHT);
    P.card_target = MIN_CARD_HEIGHT;

    // 中间留空，最近关闭的置物架总是贴着窗口底边；下半截伸出页面，被窗口底边裁掉。
    ui_add(page, ui_spacer());
    M->history_section = ui_stack(false, 0);
#ifdef LOOK_VARIANT_CARDS
    ui_add(M->history_section, ui_element_new(&table_type, sizeof(UiElement)));
    // 标题和右侧“已保留几个 / 最多几个”都用牌桌上的文字颜色。
    UiElement *history_header = ui_stack(true, 0);
    ui_set_margin(history_header, 2, 24, 0, 8);
    UiElement *history_title = ui_label(UI_STR(STR_RecentShelves), UI_TEXT_BODY_STRONG);
    ui_label_set_color(history_title, THEME_TABLE_TEXT);
    history_title->grow = 1;
    ui_add(history_header, history_title);
    history_tally = ui_label(UI_LIT(L""), UI_TEXT_BODY);
    ui_label_style(history_tally)->font = GFX_FONT_NUMBER;
    ui_label_set_size(history_tally, 13);
    ui_label_set_color(history_tally, THEME_TABLE_TEXT);
    history_tally->cross = UI_CROSS_END;
    ui_set_margin(history_tally, 0, 0, 0, 1);
    ui_add(history_header, history_tally);
    ui_add(M->history_section, history_header);
#else
    section_header(M->history_section, STR_RecentShelves, 24, 8);
#endif
    M->fan = fan_create();
    M->fan->height = 165;
    ui_set_margin(M->fan, 0, 4, 0, -36);
    ui_add(M->history_section, M->fan);
    ui_add(page, M->history_section);

    UiElement *column = ui_element_new(&column_type, sizeof(UiElement));
    ui_add(column, page);
    M->overview_scroll = ui_scroll(column);
}

void pages_build(void)
{
    // 每次打开设置重新找一遍语言包：新放进文件夹的、刚改过的译文不用重启程序就能用上。
    wchar_t *data = settings_data_directory();
    wchar_t *packs = path_join(data, LANGUAGE_PACK_FOLDER);
    language_packs_reload(packs);
    language_set_pack(language_packs_find(app_settings()->language_pack));
    free(packs);
    free(data);
    P.pending_sensitivity = -1;
    startup_failed = false;
    build_overview();
    build_general_page();
    build_shelf_page();
    build_actions_page();
    build_appearance_page();
}

// ───────────── 刷新 ─────────────

void pages_refresh(void)
{
    if (!M->overview_page) return;
    const AppSettings *settings = app_settings();
    P.getting_started->hidden = !settings->show_getting_started;

    // 概览页四张卡片上的当前设置。
    bool has_hotkey = settings->hotkey && *settings->hotkey;
    tile_set_summary(M->tiles[PAGE_GENERAL], str_general_summary(settings->language, settings->start_with_windows, has_hotkey ? settings->hotkey : NULL));
    tile_set_summary(M->tiles[PAGE_SHELF], str_shelf_summary(settings->auto_open_trigger, settings->drag_out_mode, settings->watched_folders.count));
    QuickActionList visible = quick_actions_visible(settings);
    StrList labels = {0};
#ifdef LOOK_VARIANT_CARDS
    PageTile *general = (PageTile *)M->tiles[PAGE_GENERAL], *actions = (PageTile *)M->tiles[PAGE_ACTIONS];
    hotkey_keys(&general->glance);
    strlist_clear(&actions->glance);
    for (int index = 0; index < visible.count; index++)
    {
        strlist_add_owned(&labels, quick_action_label(&visible.items[index]));
        strlist_add(&actions->glance, quick_action_glyph(&visible.items[index]));
    }
#else
    for (int index = 0; index < visible.count; index++) strlist_add_owned(&labels, quick_action_label(&visible.items[index]));
#endif
    quick_action_list_free(&visible);
    tile_set_summary(M->tiles[PAGE_ACTIONS], str_actions_summary(&labels));
    strlist_clear(&labels);
    tile_set_summary(M->tiles[PAGE_APPEARANCE], str_appearance_summary(settings->look, settings->theme, settings->accent_color));

    // 常规。
    ui_segmented_set_selected(P.language, settings->language);
    if (language_packs_count() != P.language_pack_count)
    {
        UiElement *card = P.language_pack_card;
        UiElement *old = P.language_pack;
        P.language_pack = build_language_pack_selector();
        // 换掉卡片最后的控件。
        for (int index = 0; index < card->child_count; index++)
        {
            if (card->children[index] != old) continue;
            card->children[index] = P.language_pack;
            P.language_pack->parent = card;
            ui_element_attach(P.language_pack, card->window);
            old->parent = NULL;
            ui_element_destroy(old);
            break;
        }
    }
    P.language_pack_card->hidden = language_packs_count() == 0;
    int pack_index = 0;
    for (int index = 0; index < language_packs_count(); index++)
        if (settings->language_pack && wcs_equal_ignore_case(language_pack_id(language_packs_at(index)), settings->language_pack)) pack_index = index + 1;
    ui_segmented_set_selected(P.language_pack, pack_index);
    ui_toggle_set_checked(P.startup, settings->start_with_windows);
    ui_label_set_text(P.startup_description, UI_STR(startup_failed ? STR_StartupFailed : STR_StartupDescription));
    ui_hotkey_box_set(P.hotkey, settings->hotkey);
    ui_label_set_text(P.hotkey_description, UI_STR(!has_hotkey ? STR_HotkeyNotSetDescription
                                                    : app_is_hotkey_registered() ? STR_HotkeyRegistered : STR_HotkeyTaken));
    ui_label_set_color(P.hotkey_description, has_hotkey && !app_is_hotkey_registered() ? THEME_CRITICAL : THEME_TEXT_SECONDARY);
    ui_segmented_set_selected(P.performance, settings->performance_mode);
    ui_segmented_set_selected(P.look, settings->look);
    ui_label_set_text(P.performance_description, UI_STR(settings->performance_mode == PERFORMANCE_NORMAL ? STR_PerformanceDescriptionNormal
                                                        : settings->performance_mode == PERFORMANCE_LOW ? STR_PerformanceDescriptionLow
                                                                                                         : STR_PerformanceDescriptionAuto));

    // 手势与置物架。
    ui_segmented_set_selected(P.trigger, settings->auto_open_trigger);
    P.sensitivity_card->hidden = !auto_open_uses_shake(settings->auto_open_trigger);
    P.edge_dwell_card->hidden = !auto_open_uses_edge(settings->auto_open_trigger);
    if (P.pending_sensitivity < 0 && ui_slider_value(P.sensitivity_slider) != settings->shake_sensitivity)
    {
        ui_slider_set_value(P.sensitivity_slider, settings->shake_sensitivity);
        anim_start(&P.label_value, settings->shake_sensitivity, 0.22, EASE_OF(EASE_CUBIC_OUT));
        main_request_frame();
    }
    update_sensitivity_description();
    ui_segmented_set_selected(P.edge_dwell, settings->edge_dwell);
    rebuild_blocked_list();
#ifdef __linux__
    ui_toggle_set_checked(P.respond_fullscreen, settings->respond_when_fullscreen);
#endif
    ui_segmented_set_selected(P.shelf_size, settings->quick_shelf_size);
    ui_segmented_set_selected(P.drag_out_mode, settings->drag_out_mode);
#ifdef __linux__
    ui_segmented_set_selected(P.mixed_drag, settings->mixed_drag);
#endif
    ui_toggle_set_checked(P.close_when_empty, settings->close_when_empty);
    int history_count = 0;
    shelf_history_entries(&history_count);
#ifdef LOOK_VARIANT_CARDS
    wchar_t tally[32];
    swprintf(tally, ARRAYSIZE(tally), L"%d / %d", history_count, SHELF_HISTORY_CAPACITY);
    ui_label_set_text(history_tally, UI_LIT(tally));
    if (open_button)
    {
        OpenButton *button = (OpenButton *)open_button;
        StrList keys = {0};
        hotkey_keys(&keys);
        if (!strlist_equal(&keys, &button->keys, false))
        {
            strlist_clear(&button->keys);
            button->keys = keys;
            ui_window_invalidate(M->window);
        }
        else strlist_clear(&keys);
    }
#endif
    if (history_count > 0)
    {
        wchar_t *text = str_history_count(history_count);
        ui_label_set_text(P.history_description, UI_LIT(text));
        free(text);
    }
    else ui_label_set_text(P.history_description, UI_STR(STR_HistoryEmpty));
    ui_set_disabled(P.clear_history, history_count == 0);
    rebuild_watch_list();
    // 没有观察的文件夹时停留时间用不上。
    P.peek_seconds_card->hidden = settings->watched_folders.count == 0;
    refresh_peek_seconds(settings);

    // 快速操作。
    int layout = settings->quick_action_layout;
    ui_segmented_set_selected(P.layout, layout == LAYOUT_FOUR ? 2 : layout == LAYOUT_TWO ? 1 : 0);
    ui_label_set_text(P.layout_description, UI_STR(layout != LAYOUT_OFF ? STR_ActionLayoutDescription : STR_ActionLayoutOffDescription));
    main_repaint();
}

void pages_animate(double time)
{
    if (!M->overview_page) return;
    if (anim_update(&P.label_value, time)) main_request_frame();
    if (anim_update(&P.card_height, time))
    {
        main_request_frame();
        for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->height = floorf((float)P.card_height.value);
        main_relayout();
    }
    else if (M->tiles[0] && M->tiles[0]->height != (float)P.card_height.value)
    {
        for (int index = 0; index < PAGE_COUNT; index++) M->tiles[index]->height = (float)P.card_height.value;
        main_relayout();
    }
}

void pages_close(void)
{
    save_sensitivity();
    unsigned generation = P.sensitivity_generation + 1;
    strlist_clear(&menu_apps);
    strlist_clear(&blocked_names);
    strlist_clear(&watch_names);
    memset(&P, 0, sizeof(P));
#ifdef LOOK_VARIANT_CARDS
    open_button = history_tally = NULL;
#endif
    // 关掉之后还在等的保存计时不再起作用。
    P.sensitivity_generation = generation;
    P.pending_sensitivity = -1;
}

// ───────────── 测试用 ─────────────

UiElement *pages_control(PagesControl control)
{
    switch (control)
    {
        case PAGES_GETTING_STARTED: return P.getting_started;
        case PAGES_LANGUAGE: return P.language;
        case PAGES_PERFORMANCE: return P.performance;
        case PAGES_PERFORMANCE_DESCRIPTION: return P.performance_description;
        case PAGES_TRIGGER: return P.trigger;
        case PAGES_SENSITIVITY_CARD: return P.sensitivity_card;
        case PAGES_SENSITIVITY_SLIDER: return P.sensitivity_slider;
        case PAGES_SENSITIVITY_DESCRIPTION: return P.sensitivity_description;
        case PAGES_EDGE_DWELL_CARD: return P.edge_dwell_card;
        case PAGES_EDGE_DWELL: return P.edge_dwell;
        case PAGES_SHELF_SIZE: return P.shelf_size;
        case PAGES_DRAG_OUT_MODE: return P.drag_out_mode;
        case PAGES_MIXED_DRAG: return P.mixed_drag;
        case PAGES_CLOSE_WHEN_EMPTY: return P.close_when_empty;
        case PAGES_WATCH_LIST: return P.watch_list;
        case PAGES_PEEK_SECONDS: return P.peek_seconds;
        case PAGES_PEEK_SECONDS_CARD: return P.peek_seconds_card;
        case PAGES_LAYOUT: return P.layout;
        case PAGES_LOOK: return P.look;
        case PAGES_THEME_SYSTEM: return P.theme_tiles[THEME_SYSTEM];
        case PAGES_THEME_LIGHT: return P.theme_tiles[THEME_LIGHT];
        case PAGES_THEME_DARK: return P.theme_tiles[THEME_DARK];
        case PAGES_SWATCHES: return P.swatches;
    }
    return NULL;
}

const wchar_t *pages_tile_summary(int page)
{
    return page >= 0 && page < PAGE_COUNT && M->tiles[page] ? ((PageTile *)M->tiles[page])->summary : NULL;
}

bool pages_tile_is_tall(int page)
{
    return page >= 0 && page < PAGE_COUNT && M->tiles[page] && ((PageTile *)M->tiles[page])->tall;
}

float pages_card_height(void)
{
    return (float)P.card_height.value;
}
