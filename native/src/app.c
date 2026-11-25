#include "app.h"
#include "anim.h"
#include "attachment_cache.h"
#include "file_dialog.h"
#include "file_ops.h"
#include "folder_watch.h"
#include "foreground.h"
#include "gesture_service.h"
#include "gfx.h"
#include "hotkey.h"
#include "main_window.h"
#include "onboarding_window.h"
#include "theme.h"
#include "ui.h"
#include "lang.h"
#include "log.h"
#include "look_switch.h"
#include "probe.h"
#include "shortcut_icons.h"
#include "shelf_history.h"
#include "shelf_peek.h"
#include "shelf_window.h"
#include "single_instance.h"
#include "startup.h"
#include "strings.h"
#include "task.h"
#include "tray.h"

#include <ole2.h>
#include <stdlib.h>
#include <string.h>

#ifndef DROPSHELF_VERSION
#define DROPSHELF_VERSION L"0.0.0"
#endif

#define MESSAGE_WINDOW_CLASS L"DropShelfMessageWindow"
#define MESSAGE_ARGUMENTS (WM_APP + 2)      // lparam：另一个进程转交过来的命令行（StrList *）
#define MESSAGE_SHOW_MAIN_WINDOW (WM_APP + 3)
#define MESSAGE_SNAPSHOT (WM_APP + 4)      // 诊断用：把各窗口的画面存成图片
#define MESSAGE_GESTURE (WM_APP + 5)       // 手势识别线程发来的消息，见 gesture_service.h
#define MESSAGE_NUDGE_MOUSE (WM_APP + 6)
#define MESSAGE_SHOW_ONBOARDING (WM_APP + 7)

static AppSettings settings;
static HWND message_window;
static HANDLE show_window_event;
static bool hotkey_registered;
static bool has_shown_tray_hint;
static bool exiting;
static bool ole_initialized;
/// 诊断日志用：识别到手势的时刻，记下探针多久后确认正在拖放。
static LARGE_INTEGER gesture_at;
static ScreenEdge pending_edge;

const AppSettings *app_settings(void)
{
    return &settings;
}

bool app_is_hotkey_registered(void)
{
    return hotkey_registered;
}

bool app_is_gesture_running(void)
{
    return gesture_service_is_running();
}

void app_main_window_closed(void)
{
    if (exiting || has_shown_tray_hint) return;
    has_shown_tray_hint = true;
    tray_show_still_running_hint();
}

// ───────────── 设置 ─────────────

static wchar_t *language_pack_directory(void)
{
    wchar_t *data = settings_data_directory();
    wchar_t *directory = path_join(data, LANGUAGE_PACK_FOLDER);
    free(data);
    return directory;
}

/// 快速操作的布局、四个位置和自定义操作都没变。
static bool quick_actions_settings_equal(const AppSettings *first, const AppSettings *second)
{
    if (first->quick_action_layout != second->quick_action_layout || !strlist_equal(&first->quick_action_slots, &second->quick_action_slots, false) ||
        first->custom_action_count != second->custom_action_count)
        return false;
    for (int index = 0; index < first->custom_action_count; index++)
        if (!custom_action_same(&first->custom_actions[index], &second->custom_actions[index])) return false;
    return true;
}

/// 观察的文件夹里有新文件写完了：在屏幕边缘探出置物架的一角。
static void on_watched_files(const StrList *paths)
{
    if (!exiting) shelf_peek_add(paths);
}

/// 设置变化后让各部分立即生效。
static void apply_settings(const AppSettings *previous, const AppSettings *current)
{
    if (previous->theme != current->theme || !wcs_equal(previous->accent_color, current->accent_color))
        theme_set_mode(current->theme, current->accent_color);
    if (previous->look != current->look) look_switch_to((Look)current->look);
    if (previous->performance_mode != current->performance_mode)
    {
        low_performance_set_mode(current->performance_mode);
        ui_refresh_window_appearance();
    }
    if (previous->language != current->language || !wcs_equal(previous->language_pack, current->language_pack))
    {
        language_set_mode(current->language);
        language_set_pack(language_packs_find(current->language_pack));
        tray_refresh_text();
    }
    gesture_service_set_sensitivity(current->shake_sensitivity);
    gesture_service_set_trigger(current->auto_open_trigger);
    gesture_service_set_edge_dwell(current->edge_dwell);
    if (!wcs_equal(previous->hotkey, current->hotkey))
        hotkey_registered = hotkey_service_register(settings_hotkey(current));
    if (!strlist_equal(&previous->blocked_apps, &current->blocked_apps, true))
        foreground_watcher_set_blocked_apps(&current->blocked_apps);
    if (!strlist_equal(&previous->watched_folders, &current->watched_folders, true))
        folder_watch_set(&current->watched_folders, on_watched_files);
    // 尺寸、拖出方式、快速操作变化时，已打开的置物架需要立即更新。
    if (previous->quick_shelf_size != current->quick_shelf_size || previous->drag_out_mode != current->drag_out_mode ||
        !quick_actions_settings_equal(previous, current))
        shelves_apply_settings();
}

void app_update_settings(void (*change)(AppSettings *settings, void *context), void *context)
{
    AppSettings previous = settings;
    AppSettings next;
    settings_clone(&next, &settings);
    change(&next, context);
    settings_normalize(&next);
    settings = next;
    apply_settings(&previous, &settings);
    settings_save_in_background(&settings);
    settings_free(&previous);
    main_window_refresh();
}

static void change_watch_add(AppSettings *target, void *context)
{
    if (strlist_index_of(&target->watched_folders, context, true) < 0) strlist_add(&target->watched_folders, context);
}

static void change_watch_remove(AppSettings *target, void *context)
{
    int index = strlist_index_of(&target->watched_folders, context, true);
    if (index < 0) return;
    free(target->watched_folders.items[index]);
    memmove(&target->watched_folders.items[index], &target->watched_folders.items[index + 1],
        (size_t)(target->watched_folders.count - index - 1) * sizeof(wchar_t *));
    target->watched_folders.count--;
}

static void change_watch_asked(AppSettings *target, void *context)
{
    (void)context;
    target->has_asked_folder_watch = true;
}

void app_watch_folder(const wchar_t *folder)
{
    if (!wcs_is_blank(folder)) app_update_settings(change_watch_add, (void *)folder);
}

void app_unwatch_folder(const wchar_t *folder)
{
    app_update_settings(change_watch_remove, (void *)folder);
}

bool app_choose_watch_folder(HWND owner)
{
    wchar_t *downloads = persistent_output_directory();
    wchar_t *folder = file_dialog_folder(owner, S(STR_ChooseWatchFolder), downloads);
    free(downloads);
    if (!folder) return false;
    app_watch_folder(folder);
    free(folder);
    return true;
}

static void change_look_asked(AppSettings *target, void *context)
{
    (void)context;
    target->has_asked_look = true;
}

void app_mark_look_asked(void)
{
    if (!settings.has_asked_look) app_update_settings(change_look_asked, NULL);
}

void app_mark_folder_watch_asked(void)
{
    if (!settings.has_asked_folder_watch) app_update_settings(change_watch_asked, NULL);
}

static void change_start_with_windows(AppSettings *target, void *context)
{
    target->start_with_windows = *(bool *)context;
}

static void change_drag_out_mode(AppSettings *target, void *context)
{
    target->drag_out_mode = *(int *)context;
}

void app_update_drag_out_mode(int mode)
{
    app_update_settings(change_drag_out_mode, &mode);
}

bool app_set_start_with_windows(bool enabled)
{
    if (!startup_set_enabled(enabled)) return false;
    app_update_settings(change_start_with_windows, &enabled);
    return true;
}

/// 设置里开着开机启动时，每次启动都重新登记一次：程序换了位置（比如便携版挪了文件夹）后仍然有效。
static void restore_startup_registration(void)
{
    if (!settings.start_with_windows || startup_set_enabled(true)) return;
    settings.start_with_windows = false;
    settings_save(&settings);
}

// ───────────── 托盘和快捷键要做的事 ─────────────

void app_show_main_window(void)
{
    // 放到消息循环里再打开：调用的地方（托盘、快捷键的消息处理）不用等窗口。
    if (!exiting && message_window) PostMessageW(message_window, MESSAGE_SHOW_MAIN_WINDOW, 0, 0);
}

void app_new_shelf(void)
{
    if (!exiting) shelves_open_at_cursor(true);
}

static wchar_t *current_hotkey_text(void)
{
    return wcs_dup(hotkey_registered ? settings.hotkey : L"");
}

void app_handle_arguments(const StrList *arguments)
{
    if (exiting) return;
    if (!single_instance_is_shelf_request(arguments))
    {
        app_show_main_window();
        return;
    }
    ShelfWindow *shelf = shelves_open_at_cursor(true);
    StrList paths;
    single_instance_path_arguments(arguments, &paths);
    if (shelf && paths.count > 0) shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
}

void app_exit(void)
{
    app_prepare_for_shutdown();
    PostQuitMessage(0);
}

void app_prepare_for_shutdown(void)
{
    if (exiting) return;
    exiting = true;
    settings_save(&settings);
    ui_menu_dismiss();
    folder_watch_stop();
    shelf_peek_close();
    foreground_watcher_stop();
    gesture_service_stop();
    probe_destroy();
    // 关闭时仍有文件的置物架记入最近关闭，等记录写完再退出。
    shelves_close_all();
    shelf_history_flush();
    ui_close_all_windows();
    hotkey_service_stop();
    tray_destroy();
    if (message_window) DestroyWindow(message_window);
    message_window = NULL;
    if (show_window_event) CloseHandle(show_window_event);
    show_window_event = NULL;
    frames_shutdown();
    gfx_shutdown();
    task_shutdown();
    if (ole_initialized) OleUninitialize();
    ole_initialized = false;
    // 关闭主窗口时会再保存一次窗口位置，等它写完再退出。
    settings_flush();
}

// ───────────── 手势 ─────────────

/// 发送一次零距离的鼠标移动。光标静止时，正在拖放的程序要等到下一次移动才会发现光标下出现了新窗口；
/// 从屏幕边缘呼出置物架时光标往往停着不动，用它让拖放立即交给新窗口。
static void nudge_mouse(void)
{
    INPUT input = { .type = INPUT_MOUSE };
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

/// 放到消息队列里稍后再做：探针窗口先显示出来。
static void nudge_mouse_later(void)
{
    if (message_window) PostMessageW(message_window, MESSAGE_NUDGE_MOUSE, 0, 0);
}

static double milliseconds_since(LARGE_INTEGER start)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
}

static bool is_blocked_at(POINT point)
{
    if (foreground_watcher_is_blocked_at(point))
    {
        log_trace(L"识别到手势 (%ld, %ld)，但前台或光标下是屏蔽的应用，不弹出置物架", point.x, point.y);
        return true;
    }
    if (settings.pause_in_fullscreen && foreground_is_fullscreen())
    {
        log_trace(L"识别到手势 (%ld, %ld)，但前台是全屏的程序，不弹出置物架", point.x, point.y);
        return true;
    }
    return false;
}

static void on_gesture(GestureEvent event, ScreenEdge edge, POINT point)
{
    if (exiting) return;
    static const wchar_t *const edge_names[] = { L"None", L"Left", L"Top", L"Right" };
    switch (event)
    {
        case GESTURE_SHAKE:
            if (is_blocked_at(point)) return;
            log_trace(L"识别到晃动 (%ld, %ld)，显示探针窗口", point.x, point.y);
            QueryPerformanceCounter(&gesture_at);
            pending_edge = EDGE_NONE;
            probe_show_at(point);
            break;
        case GESTURE_EDGE_DWELL:
            if (is_blocked_at(point)) return;
            log_trace(L"在屏幕边缘停留 (%ld, %ld, %ls)，显示探针窗口", point.x, point.y, edge_names[edge]);
            QueryPerformanceCounter(&gesture_at);
            pending_edge = edge;
            probe_show_at(point);
            nudge_mouse_later();
            break;
        case GESTURE_RELEASED:
            log_trace(L"晃动后松开鼠标");
            probe_hide();
            shelves_drag_released();
            break;
    }
}

static void on_probe_detected(void)
{
    log_trace(L"探针确认正在拖放，距识别到手势 %.0fms", milliseconds_since(gesture_at));
    POINT cursor = {0};
    GetCursorPos(&cursor);
    shelves_open_at(cursor, true, false, pending_edge);
    if (pending_edge != EDGE_NONE) nudge_mouse_later();
}

static void on_blocked_changed(bool blocked)
{
    gesture_service_set_paused(blocked);
    hotkey_service_set_paused(blocked);
}

// ───────────── 消息窗口 ─────────────
// 隐藏的顶层窗口：接收托盘图标回调、全局快捷键、资源管理器重启通知和另一个进程转交的命令行。

static LRESULT CALLBACK message_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_COPYDATA:
        {
            StrList *arguments = xcalloc(1, sizeof(StrList));
            if (!single_instance_read_arguments((const COPYDATASTRUCT *)lparam, arguments))
            {
                free(arguments);
                break;
            }
            // 先回应发送方，再在消息循环里处理：对方在等这条消息返回。
            if (!PostMessageW(window, MESSAGE_ARGUMENTS, 0, (LPARAM)arguments))
            {
                strlist_clear(arguments);
                free(arguments);
            }
            return 1;
        }
        case MESSAGE_ARGUMENTS:
        {
            StrList *arguments = (StrList *)lparam;
            app_handle_arguments(arguments);
            strlist_clear(arguments);
            free(arguments);
            return 0;
        }
        case MESSAGE_SHOW_MAIN_WINDOW:
            if (exiting) return 0;
            main_window_show();
            // 升级来的老用户没在引导里见过选风格和观察文件夹，打开主窗口时单独问一次；两样都没问过时先问风格，下次再问文件夹。
            if (settings.has_completed_first_run && !onboarding_window_is_open())
            {
                if (!settings.has_asked_look) onboarding_window_show_look();
                else if (!settings.has_asked_folder_watch) onboarding_window_show_watch();
            }
            return 0;
        case MESSAGE_SHOW_ONBOARDING:
            if (!exiting) onboarding_window_show();
            return 0;
        case MESSAGE_GESTURE:
            on_gesture(gesture_message_event(wparam), gesture_message_edge(wparam), gesture_message_point(lparam));
            return 0;
        case MESSAGE_NUDGE_MOUSE:
            nudge_mouse();
            return 0;
        case MESSAGE_SNAPSHOT:
        {
            // 只有设置了环境变量 DROPSHELF_SNAPSHOT_DIRECTORY 才起作用，用来在没有屏幕的环境里检查界面。
            wchar_t *directory = env_get(L"DROPSHELF_SNAPSHOT_DIRECTORY");
            if (directory) ui_snapshot_windows(directory);
            free(directory);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            // Windows 的深浅色或强调色可能变了；没变时这里什么都不做。
            theme_refresh();
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            // 注销、关机，或者安装程序要替换正在运行的程序（重启管理器发来的，lparam 带 ENDSESSION_CLOSEAPP）：
            // 保存设置、收拾好以后退出。注销和关机时系统随后也会结束进程；安装程序却要等进程真的退出才能替换文件，
            // 只收拾不退出的话，进程连托盘图标都没了却还在，安装时替换不了 DropShelf.exe。
            if (wparam)
            {
                app_prepare_for_shutdown();
                PostQuitMessage(0);
            }
            return 0;
    }
    if (hotkey_service_handle_message(message, wparam)) return 0;
    if (tray_handle_message(message, wparam, lparam)) return 0;
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND create_message_window(HINSTANCE instance)
{
    WNDCLASSEXW class = { .cbSize = sizeof(class) };
    class.lpfnWndProc = message_window_procedure;
    class.hInstance = instance;
    class.lpszClassName = MESSAGE_WINDOW_CLASS;
    if (!RegisterClassExW(&class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    wchar_t *title = single_instance_window_title();
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, MESSAGE_WINDOW_CLASS, title, WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    free(title);
    return window;
}

static void on_hotkey_pressed(void)
{
    // 屏蔽的应用在前台时不响应（快捷键本该已经注销，这里再确认一次）；全屏的程序在前台时也不响应。
    if (foreground_watcher_check() || (settings.pause_in_fullscreen && foreground_is_fullscreen())) return;
    app_new_shelf();
}

bool app_start(HINSTANCE instance, bool start_in_background)
{
    // 探针窗口接收系统拖放要用 OLE。
    task_initialize();
    HRESULT ole = OleInitialize(NULL);
    ole_initialized = SUCCEEDED(ole);
    if (!ole_initialized) log_info(L"OLE 初始化失败，错误码 0x%08lX", (unsigned long)ole);
    settings_load(&settings);
    language_set_mode(settings.language);
    wchar_t *packs = language_pack_directory();
    language_packs_reload(packs);
    free(packs);
    language_set_pack(language_packs_find(settings.language_pack));
    theme_initialize(settings.theme, settings.accent_color, (Look)settings.look);
    // 重装或升级后安装程序把快捷方式的图标恢复成了第一个：按现在的风格再对一遍。
    shortcut_icons_sync((Look)settings.look);
    low_performance_set_mode(settings.performance_mode);
    log_cleanup_old();
    attachment_cache_cleanup_stale();
    shelf_history_load(NULL);
    restore_startup_registration();

    message_window = create_message_window(instance);
    if (!message_window)
    {
        log_win32_error(L"无法创建消息窗口", GetLastError());
        return false;
    }

    // 再次启动的进程用这个事件唤醒主窗口。
    wchar_t *event_name = single_instance_show_event_name();
    show_window_event = CreateEventW(NULL, FALSE, FALSE, event_name);
    if (!show_window_event) log_win32_error(L"重复启动唤醒功能初始化失败", GetLastError());
    free(event_name);

    TrayActions actions = { app_show_main_window, app_new_shelf, app_exit, current_hotkey_text };
    hotkey_service_start(message_window, on_hotkey_pressed);
    hotkey_registered = hotkey_service_register(settings_hotkey(&settings));
    tray_create(message_window, &actions);

    if (ole_initialized) probe_create(instance, on_probe_detected);
    gesture_service_start(message_window, MESSAGE_GESTURE, settings.shake_sensitivity);
    gesture_service_set_trigger(settings.auto_open_trigger);
    gesture_service_set_edge_dwell(settings.edge_dwell);
    foreground_watcher_set_listener(on_blocked_changed);
    foreground_watcher_set_blocked_apps(&settings.blocked_apps);
    folder_watch_set(&settings.watched_folders, on_watched_files);

    shelves_prewarm();
    if (!start_in_background)
    {
        app_show_main_window();
        // 第一次运行：主窗口出来之后再打开引导，放在主窗口中间。
        if (!settings.has_completed_first_run) PostMessageW(message_window, MESSAGE_SHOW_ONBOARDING, 0, 0);
    }
    return true;
}

int app_run(void)
{
    MSG message;
    for (;;)
    {
        // 一边等消息，一边等“再次启动”的唤醒事件。
        DWORD count = show_window_event ? 1 : 0;
        DWORD result = MsgWaitForMultipleObjectsEx(count, &show_window_event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (count == 1 && result == WAIT_OBJECT_0)
        {
            app_show_main_window();
            continue;
        }
        if (result == WAIT_FAILED)
        {
            // 事件句柄不可用时退回到只等消息。
            if (show_window_event) CloseHandle(show_window_event);
            show_window_event = NULL;
        }
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) return (int)message.wParam;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}
