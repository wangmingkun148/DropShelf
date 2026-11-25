// app.h 在 Linux 上的最小实现：设置的生命周期和置物架需要的几个操作。
// 主窗口、托盘、快捷键等以后再接；现在启动命令直接操作置物架。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "app.h"
#include "anim.h"
#include "attachment_cache.h"
#include "folder_watch.h"
#include "foreground.h"
#include "foreground_linux.h"
#include "gesture_service.h"
#include "hotkey.h"
#include "hyprland_linux.h"
#include "lang.h"
#include "log.h"
#include "main_window.h"
#include "onboarding_window.h"
#include "probe.h"
#include "quick_action.h"
#include "shelf_history.h"
#include "shelf_peek.h"
#include "shelf_window.h"
#include "startup.h"
#include "theme.h"
#include "tray.h"
#include "ui.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

static AppSettings current_settings;
static bool settings_ready;
static bool hotkey_registered;

static void on_watched_files(const StrList *paths);

// 手势识别的消息窗口等状态，见文件后面的 app_start。
static UiWindow *message_window;
static guint pause_source;
static ScreenEdge pending_edge;

const AppSettings *app_settings(void)
{
    if (!settings_ready)
    {
        settings_init(&current_settings);
        settings_load(&current_settings);
        settings_ready = true;
    }
    return &current_settings;
}

void app_update_settings(void (*change)(AppSettings *settings, void *context), void *context)
{
    app_settings();
    AppSettings updated;
    settings_init(&updated);
    settings_clone(&updated, &current_settings);
    change(&updated, context);
    settings_normalize(&updated);
    settings_free(&current_settings);
    current_settings = updated;
    settings_save_in_background(&current_settings);
    theme_set_mode(current_settings.theme, current_settings.accent_color);
    low_performance_set_mode(current_settings.performance_mode);
    language_set_mode(current_settings.language);
    shelves_apply_settings();
    folder_watch_set(&current_settings.watched_folders, on_watched_files);
    tray_refresh_text();
    ui_refresh_window_appearance();
}

static void set_drag_out_mode(AppSettings *settings, void *context)
{
    settings->drag_out_mode = *(int *)context;
}

void app_update_drag_out_mode(int mode)
{
    app_update_settings(set_drag_out_mode, &mode);
}

bool app_is_hotkey_registered(void)
{
    return hotkey_registered;
}

bool app_is_gesture_running(void)
{
    return gesture_service_is_running();
}

void app_watch_folder(const wchar_t *folder)
{
    if (!folder || !*folder) return;
    app_settings();
    for (int index = 0; index < current_settings.watched_folders.count; index++)
    {
        if (wcs_equal_ignore_case(current_settings.watched_folders.items[index], folder)) return;
    }
    StrList updated = {0};
    for (int index = 0; index < current_settings.watched_folders.count; index++)
        strlist_add(&updated, current_settings.watched_folders.items[index]);
    strlist_add(&updated, folder);
    AppSettings copy;
    settings_init(&copy);
    settings_clone(&copy, &current_settings);
    strlist_clear(&copy.watched_folders);
    copy.watched_folders = updated;
    settings_normalize(&copy);
    settings_free(&current_settings);
    current_settings = copy;
    settings_save_in_background(&current_settings);
    folder_watch_set(&current_settings.watched_folders, on_watched_files);
}

void app_unwatch_folder(const wchar_t *folder)
{
    if (!folder) return;
    app_settings();
    AppSettings copy;
    settings_init(&copy);
    settings_clone(&copy, &current_settings);
    StrList kept = {0};
    for (int index = 0; index < copy.watched_folders.count; index++)
    {
        if (wcs_equal_ignore_case(copy.watched_folders.items[index], folder)) continue;
        strlist_add(&kept, copy.watched_folders.items[index]);
    }
    strlist_clear(&copy.watched_folders);
    copy.watched_folders = kept;
    settings_free(&current_settings);
    current_settings = copy;
    settings_save_in_background(&current_settings);
    folder_watch_set(&current_settings.watched_folders, on_watched_files);
}

bool app_choose_watch_folder(HWND owner)
{
    wchar_t *folder = pick_folder(owner, L"选择要观察的文件夹");
    if (!folder) return false;
    app_watch_folder(folder);
    free(folder);
    return true;
}

void app_mark_folder_watch_asked(void)
{
    app_settings();
    if (current_settings.has_asked_folder_watch) return;
    AppSettings copy;
    settings_init(&copy);
    settings_clone(&copy, &current_settings);
    copy.has_asked_folder_watch = true;
    settings_free(&current_settings);
    current_settings = copy;
    settings_save_in_background(&current_settings);
}

void app_mark_look_asked(void)
{
    app_settings();
    if (current_settings.has_asked_look) return;
    AppSettings copy;
    settings_init(&copy);
    settings_clone(&copy, &current_settings);
    copy.has_asked_look = true;
    settings_free(&current_settings);
    current_settings = copy;
    settings_save_in_background(&current_settings);
}

static void set_start_with_windows(AppSettings *settings, void *context)
{
    settings->start_with_windows = *(bool *)context;
}

bool app_set_start_with_windows(bool enabled)
{
    if (!startup_set_enabled(enabled)) return false;
    app_update_settings(set_start_with_windows, &enabled);
    return true;
}

void app_show_main_window(void)
{
    main_window_show();
}

void app_main_window_closed(void)
{
}

void app_new_shelf(void)
{
    shelves_open_at_cursor(true);
}

void app_exit(void)
{
    GApplication *application = g_application_get_default();
    if (application) g_application_quit(application);
}

void app_prepare_for_shutdown(void)
{
    if (pause_source)
    {
        g_source_remove(pause_source);
        pause_source = 0;
    }
    gesture_service_stop();
    hotkey_service_stop();
    tray_destroy();
    foreground_watcher_stop();
    folder_watch_stop();
    shelf_peek_close();
    probe_destroy();
    if (message_window)
    {
        ui_window_close(message_window);
        message_window = NULL;
    }
    if (settings_ready)
    {
        settings_save(&current_settings);
        settings_flush();
    }
}

void app_handle_arguments(const StrList *arguments)
{
    ShelfWindow *shelf = shelves_open_at_cursor(true);
    if (!shelf) return;
    if (arguments && arguments->count > 0) shelf_window_add_paths(shelf, arguments);
}

// ───────────── 手势、前台与探针 ─────────────
// 和 Windows 版一样：识别到晃动或边缘停留先放一个探针窗口，系统拖放落到它上面才弹出置物架，
// 全屏的游戏、演示和屏蔽的应用在前台时一律不弹。

#define MESSAGE_GESTURE (WM_APP + 5)

/// 托盘菜单里“新建置物架”右边显示的快捷键。
static wchar_t *tray_hotkey_text(void)
{
    return hotkey_to_text(settings_hotkey(app_settings()));
}

/// 按下全局快捷键：和 Windows 版一样，前台是屏蔽的应用时不弹。
static void on_hotkey_pressed(void)
{
    if (foreground_watcher_check()) return;
    shelves_open_at_cursor(true);
}

static bool gestures_blocked_at(POINT point)
{
    if (foreground_watcher_is_blocked_at(point))
    {
        log_info(L"识别到手势 (%ld, %ld)，但前台或光标下是屏蔽的应用，不弹出置物架", point.x, point.y);
        return true;
    }
    if (foreground_is_fullscreen())
    {
        log_info(L"识别到手势 (%ld, %ld)，但前台全屏，不弹出置物架", point.x, point.y);
        return true;
    }
    return false;
}

/// 观察的文件夹里出现新文件：屏幕边缘探出一角。
static void on_watched_files(const StrList *paths)
{
    shelf_peek_add(paths);
}

static void on_probe_detected(void)
{
    POINT cursor = {0};
    GetCursorPos(&cursor);
    shelves_open_at(cursor, true, false, pending_edge);
}

static void on_gesture(GestureEvent event, ScreenEdge edge, POINT point)
{
    switch (event)
    {
        case GESTURE_SHAKE:
            if (gestures_blocked_at(point)) return;
            log_info(L"识别到晃动 (%ld, %ld)，显示探针窗口", point.x, point.y);
            pending_edge = EDGE_NONE;
            probe_show_at(point);
            break;
        case GESTURE_EDGE_DWELL:
            if (gestures_blocked_at(point)) return;
            log_info(L"在屏幕边缘停留 (%ld, %ld)，显示探针窗口", point.x, point.y);
            pending_edge = edge;
            probe_show_at(point);
            break;
        case GESTURE_RELEASED:
            probe_hide();
            shelves_drag_released();
            break;
    }
}

static bool on_message(UiWindow *window, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    (void)window;
    (void)context;
    if (message != MESSAGE_GESTURE) return false;
    on_gesture(gesture_message_event(wparam), gesture_message_edge(wparam), gesture_message_point(lparam));
    if (result) *result = 0;
    return true;
}

static bool gestures_paused;

static void pause_if_blocked(void)
{
    // 全屏或屏蔽的应用在前台时，手势和快捷键都让给前台。
    bool blocked = foreground_watcher_is_blocked() || foreground_is_fullscreen();
    gesture_service_set_paused(blocked);
    hotkey_service_set_paused(blocked);
    if (blocked == gestures_paused) return;
    gestures_paused = blocked;
    log_info(blocked ? L"前台是全屏或屏蔽的应用，手势暂停" : L"手势恢复");
}

static void on_blocked_changed(bool blocked)
{
    (void)blocked;
    pause_if_blocked();
}

static gboolean refresh_pause(gpointer data)
{
    (void)data;
    foreground_watcher_check();
    pause_if_blocked();
    return G_SOURCE_CONTINUE;
}

bool app_start(HINSTANCE instance, bool start_in_background)
{
    (void)instance;
    (void)start_in_background;
    // 上次没正常退出时留下的临时附件，在后台清理掉。
    attachment_cache_cleanup_stale();
    const AppSettings *settings = app_settings();
    // 设置里开着开机启动时，每次启动都重新写一次：程序换了位置（比如挪了文件夹）后仍然有效。
    low_performance_set_mode(settings->performance_mode);
    // Hyprland 默认平铺普通窗口；主窗口和引导是固定尺寸的 Windows 式窗口，启动时加一条浮动规则。
    GApplication *own_application = g_application_get_default();
    if (own_application)
    {
        const char *app_id = g_application_get_application_id(own_application);
        if (app_id) hyprland_float_windows(app_id);
    }
    if (settings->start_with_windows && !startup_set_enabled(true))
    {
        bool off = false;
        app_update_settings(set_start_with_windows, &off);
        settings = app_settings();
    }
    // 不显示的消息窗口：只收手势识别线程投递过来的消息。
    UiWindowOptions options = {0};
    options.title = L"DropShelf";
    options.width = 1;
    options.height = 1;
    options.popup = true;
    options.no_activate = true;
    options.message = on_message;
    message_window = ui_window_create(&options);
    if (!message_window) log_info(L"消息窗口创建失败");
    if (!probe_create(NULL, on_probe_detected)) log_info(L"探针窗口创建失败");

    TrayActions tray_actions = { app_show_main_window, app_new_shelf, app_exit, tray_hotkey_text };
    tray_create(message_window ? ui_window_handle(message_window) : NULL, &tray_actions);

    hotkey_service_start(message_window ? ui_window_handle(message_window) : NULL, on_hotkey_pressed);
    hotkey_registered = hotkey_service_register(settings_hotkey(settings));
    if (!hotkey_registered && !hotkey_is_empty(settings_hotkey(settings)))
        log_info(L"全局快捷键没有被系统接受，可以在 Hyprland 配置里加 bind = ..., exec, dropshelf --shelf");

    foreground_watcher_set_listener(on_blocked_changed);
    foreground_watcher_set_blocked_apps(&settings->blocked_apps);
    folder_watch_set(&settings->watched_folders, on_watched_files);
    if (gesture_service_start(message_window ? ui_window_handle(message_window) : NULL, MESSAGE_GESTURE,
            settings->shake_sensitivity))
    {
        gesture_service_set_trigger(settings->auto_open_trigger);
        gesture_service_set_edge_dwell(settings->edge_dwell);
    }
    // 前台是全屏的游戏或演示时，每秒看一眼并暂停识别。
    pause_source = g_timeout_add_seconds(1, refresh_pause, NULL);
    refresh_pause(NULL);
    // 第一次运行：介绍怎么用；老用户没在引导里见过观察文件夹的话，这里先不打扰。
    if (!settings->has_completed_first_run) onboarding_window_show();
    return true;
}