#include "main_window.h"
#include "app.h"
#include "power.h"
#include "strings.h"
#include "ui.h"

#include <math.h>
#include <stdlib.h>

#ifndef DROPSHELF_VERSION
#define DROPSHELF_VERSION L"0.0.0"
#endif

#define CAPTION_HEIGHT 48.0f
#define APP_ICON_RESOURCE 1

// 图标字形，Segoe Fluent Icons 与 Segoe MDL2 Assets 共有。
#define GLYPH_MINIMIZE L""
#define GLYPH_MAXIMIZE L""
#define GLYPH_RESTORE L""
#define GLYPH_CLOSE L""
#define GLYPH_LANGUAGE L""
#define GLYPH_POWER L""
#define GLYPH_KEYBOARD L""
#define GLYPH_LIGHTNING L""
#define GLYPH_LIGHT L""

static UiWindow *window;
static UiElement *maximize_button;
static UiElement *language_selector;
static UiElement *startup_toggle;
static UiElement *hotkey_text;
static UiElement *hotkey_description;
static UiElement *performance_selector;
static UiElement *performance_description;
static UiElement *theme_selector;

// ───────────── 修改设置 ─────────────

static void change_language(AppSettings *settings, void *context) { settings->language = *(int *)context; }
static void change_performance(AppSettings *settings, void *context) { settings->performance_mode = *(int *)context; }
static void change_theme(AppSettings *settings, void *context) { settings->theme = *(int *)context; }

static void on_language_selected(int index, void *context)
{
    (void)context;
    app_update_settings(change_language, &index);
}

/// 分段选择的顺序是 自动、关闭、开启，对应 PerformanceMode 的 Auto、Normal、Low，正好一致。
static void on_performance_selected(int index, void *context)
{
    (void)context;
    app_update_settings(change_performance, &index);
}

static void on_theme_selected(int index, void *context)
{
    (void)context;
    app_update_settings(change_theme, &index);
}

static void on_startup_toggled(bool checked, void *context)
{
    (void)context;
    // 写入注册表失败时设置保持原值，main_window_refresh 会把开关拨回去。
    app_set_start_with_windows(checked);
    main_window_refresh();
}

static void on_new_shelf(void *context)
{
    (void)context;
    app_new_shelf();
}

static void on_quit(void *context)
{
    (void)context;
    app_exit();
}

static void on_minimize(void *context)
{
    (void)context;
    ShowWindow(ui_window_handle(window), SW_MINIMIZE);
}

static void on_maximize(void *context)
{
    (void)context;
    HWND handle = ui_window_handle(window);
    ShowWindow(handle, IsZoomed(handle) ? SW_RESTORE : SW_MAXIMIZE);
}

static void on_close(void *context)
{
    (void)context;
    main_window_close();
}

// ───────────── 窗口的回调 ─────────────

/// 把窗口的位置和大小记进设置（DIP），下次打开时还在原处。最大化、最小化时不记。
static void change_bounds(AppSettings *settings, void *context)
{
    const double *bounds = context;
    settings->window_left = bounds[0];
    settings->window_top = bounds[1];
    settings->window_width = bounds[2];
    settings->window_height = bounds[3];
}

static void save_bounds(void)
{
    HWND handle = ui_window_handle(window);
    if (!handle || IsZoomed(handle) || IsIconic(handle)) return;
    RECT outer;
    GetWindowRect(handle, &outer);
    float scale = ui_window_scale(window);
    GfxSize size = ui_window_size(window);
    double bounds[4] = { outer.left / (double)scale, outer.top / (double)scale, size.width, size.height };
    const AppSettings *settings = app_settings();
    if (settings->window_left == bounds[0] && settings->window_top == bounds[1] && settings->window_width == bounds[2] &&
        settings->window_height == bounds[3])
        return;
    app_update_settings(change_bounds, bounds);
}

static void on_moved(UiWindow *moved, void *context)
{
    (void)moved;
    (void)context;
    // 最大化和还原之间按钮的图标不同。
    if (maximize_button) ui_button_set_text(maximize_button, UI_LIT(IsZoomed(ui_window_handle(window)) ? GLYPH_RESTORE : GLYPH_MAXIMIZE));
    save_bounds();
}

static bool on_key(UiWindow *target, void *context, unsigned key)
{
    (void)target;
    (void)context;
    if (key != VK_ESCAPE) return false;
    main_window_close();
    return true;
}

static void on_closed(UiWindow *closed, void *context)
{
    (void)closed;
    (void)context;
    // 窗口显示期间保持正常性能，关掉后交还系统决定。
    power_hold(&window, false);
    window = NULL;
    maximize_button = language_selector = startup_toggle = hotkey_text = hotkey_description = NULL;
    performance_selector = performance_description = theme_selector = NULL;
    app_main_window_closed();
}

// ───────────── 内容 ─────────────

static UiText performance_description_text(int mode)
{
    return UI_STR(mode == PERFORMANCE_NORMAL ? STR_PerformanceDescriptionNormal : mode == PERFORMANCE_LOW ? STR_PerformanceDescriptionLow
                                                                                                      : STR_PerformanceDescriptionAuto);
}

static UiElement *section_header(StringId text)
{
    UiElement *header = ui_label(UI_STR(text), UI_TEXT_BODY_STRONG);
    ui_set_margin(header, 2, 20, 0, 8);
    return header;
}

static UiElement *build_title_bar(void)
{
    UiElement *bar = ui_stack(true, 0);
    bar->height = CAPTION_HEIGHT;
    UiElement *title = ui_label(UI_LIT(L"DropShelf"), UI_TEXT_CAPTION);
    title->cross = UI_CROSS_CENTER;
    ui_set_margin(title, 18, 0, 0, 0);
    ui_add(bar, title);
    ui_add(bar, ui_spacer());

    UiElement *minimize = ui_button(UI_BUTTON_CAPTION, UI_LIT(GLYPH_MINIMIZE), on_minimize, NULL);
    maximize_button = ui_button(UI_BUTTON_CAPTION, UI_LIT(GLYPH_MAXIMIZE), on_maximize, NULL);
    UiElement *close = ui_button(UI_BUTTON_CAPTION_CLOSE, UI_LIT(GLYPH_CLOSE), on_close, NULL);
    // 按钮贴着窗口的右上角，高 32。
    minimize->cross = maximize_button->cross = close->cross = UI_CROSS_START;
    ui_add(bar, minimize);
    ui_add(bar, maximize_button);
    ui_add(bar, close);
    return bar;
}

static UiElement *build_content(void)
{
    const AppSettings *settings = app_settings();
    UiElement *root = ui_stack(false, 0);
    ui_add(root, build_title_bar());

    UiElement *page = ui_stack(false, 0);
    ui_stack_set_padding(page, 32, 0, 32, 24);
    page->grow = 1;
    ui_add(root, page);

    UiElement *heading = ui_label(UI_STR(STR_SettingsSection), UI_TEXT_SUBTITLE);
    ui_add(page, heading);
    UiElement *tagline = ui_label(UI_STR(STR_AboutTagline), UI_TEXT_CAPTION);
    ui_set_margin(tagline, 0, 4, 0, 0);
    ui_add(page, tagline);

    // 常规
    ui_add(page, section_header(STR_General));
    const UiText languages[] = { UI_STR(STR_FollowSystem), UI_LIT(L"中文"), UI_LIT(L"English") };
    language_selector = ui_segmented(languages, 3, settings->language, on_language_selected, NULL);
    ui_add(page, ui_settings_card(GLYPH_LANGUAGE, UI_STR(STR_Language), UI_STR(STR_LanguageDescription), language_selector, NULL));

    startup_toggle = ui_toggle(settings->start_with_windows, on_startup_toggled, NULL);
    ui_add(page, ui_settings_card(GLYPH_POWER, UI_STR(STR_StartWithWindows), UI_STR(STR_StartupDescription), startup_toggle, NULL));

    hotkey_text = ui_label(UI_LIT(L""), UI_TEXT_BODY_STRONG);
    hotkey_text->cross = UI_CROSS_CENTER;
    ui_add(page, ui_settings_card(GLYPH_KEYBOARD, UI_STR(STR_GlobalHotkey), UI_STR(STR_HotkeyRegistered), hotkey_text, &hotkey_description));

    const UiText modes[] = { UI_STR(STR_PerformanceAuto), UI_STR(STR_PerformanceOff), UI_STR(STR_PerformanceOn) };
    performance_selector = ui_segmented(modes, 3, settings->performance_mode, on_performance_selected, NULL);
    ui_add(page, ui_settings_card(GLYPH_LIGHTNING, UI_STR(STR_LowPerformanceMode), performance_description_text(settings->performance_mode),
        performance_selector, &performance_description));

    // 外观
    ui_add(page, section_header(STR_Appearance));
    const UiText themes[] = { UI_STR(STR_FollowSystem), UI_STR(STR_ThemeLight), UI_STR(STR_ThemeDark) };
    theme_selector = ui_segmented(themes, 3, settings->theme, on_theme_selected, NULL);
    ui_add(page, ui_settings_card(GLYPH_LIGHT, UI_STR(STR_Theme), UI_NO_TEXT, theme_selector, NULL));

    // 底部：版本号在左，按钮在右。
    ui_add(page, ui_spacer());
    UiElement *footer = ui_stack(true, 8);
    ui_set_margin(footer, 0, 16, 0, 0);
    UiElement *version = ui_label(UI_LIT(L"DropShelf " DROPSHELF_VERSION), UI_TEXT_CAPTION);
    version->grow = 1;
    version->cross = UI_CROSS_CENTER;
    ui_add(footer, version);
    ui_add(footer, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Quit), on_quit, NULL));
    ui_add(footer, ui_button(UI_BUTTON_ACCENT, UI_STR(STR_NewShelf), on_new_shelf, NULL));
    ui_add(page, footer);
    return root;
}

void main_window_refresh(void)
{
    if (!window) return;
    const AppSettings *settings = app_settings();
    ui_segmented_set_selected(language_selector, settings->language);
    ui_toggle_set_checked(startup_toggle, settings->start_with_windows);
    ui_segmented_set_selected(performance_selector, settings->performance_mode);
    ui_segmented_set_selected(theme_selector, settings->theme);
    ui_label_set_text(performance_description, performance_description_text(settings->performance_mode));

    bool has_hotkey = settings->hotkey && *settings->hotkey;
    ui_label_set_text(hotkey_text, has_hotkey ? UI_LIT(settings->hotkey) : UI_STR(STR_HotkeyNone));
    ui_label_set_text(hotkey_description, UI_STR(!has_hotkey ? STR_HotkeyNotSetDescription
                                                 : app_is_hotkey_registered() ? STR_HotkeyRegistered : STR_HotkeyTaken));
    ui_label_set_color(hotkey_description, has_hotkey && !app_is_hotkey_registered() ? THEME_CRITICAL : THEME_TEXT_SECONDARY);
}

bool main_window_is_open(void)
{
    return window != NULL;
}

void main_window_show(void)
{
    if (window)
    {
        ui_window_show(window);
        return;
    }
    const AppSettings *settings = app_settings();
    UiWindowOptions options = { 0 };
    options.title = L"DropShelf";
    options.width = (float)settings->window_width;
    options.height = (float)settings->window_height;
    options.minimum_width = 640;
    options.minimum_height = 520;
    options.resizable = true;
    options.backdrop = UI_BACKDROP_MICA;
    options.background = THEME_WINDOW_BACKGROUND;
    options.caption_height = CAPTION_HEIGHT;
    options.icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE));
    options.key = on_key;
    options.moved = on_moved;
    options.closed = on_closed;
    window = ui_window_create(&options);
    if (!window) return;

    ui_window_set_root(window, build_content());
    main_window_refresh();

    // 回到上次的位置；没有记录、或那个位置已经不在任何显示器上（换了显示器）时放在屏幕中央。
    bool placed = false;
    if (isfinite(settings->window_left) && isfinite(settings->window_top))
    {
        float scale = ui_window_scale(window);
        POINT corner = { (LONG)(settings->window_left * scale) + 40, (LONG)(settings->window_top * scale) + 20 };
        if (MonitorFromPoint(corner, MONITOR_DEFAULTTONULL))
        {
            ui_window_place(window, (int)(settings->window_left * scale), (int)(settings->window_top * scale), options.width, options.height);
            placed = true;
        }
    }
    if (!placed) ui_window_center(window);
    power_hold(&window, true);
    ui_window_show(window);
}

void main_window_close(void)
{
    if (!window) return;
    save_bounds();
    ui_window_close(window);
}
