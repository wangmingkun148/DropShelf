#include "app.h"
#include "anim.h"
#include "gfx.h"
#include "hotkey.h"
#include "main_window.h"
#include "theme.h"
#include "ui.h"
#include "lang.h"
#include "log.h"
#include "single_instance.h"
#include "startup.h"
#include "strings.h"
#include "tray.h"

#include <stdlib.h>
#include <string.h>

#ifndef DROPSHELF_VERSION
#define DROPSHELF_VERSION L"0.0.0"
#endif

#define MESSAGE_WINDOW_CLASS L"DropShelfMessageWindow"
#define MESSAGE_ARGUMENTS (WM_APP + 2)      // lparam：另一个进程转交过来的命令行（StrList *）
#define MESSAGE_SHOW_MAIN_WINDOW (WM_APP + 3)
#define MESSAGE_SNAPSHOT (WM_APP + 4)      // 诊断用：把各窗口的画面存成图片

static AppSettings settings;
static HWND message_window;
static HANDLE show_window_event;
static bool hotkey_registered;
static bool has_shown_tray_hint;
static bool exiting;

const AppSettings *app_settings(void)
{
    return &settings;
}

bool app_is_hotkey_registered(void)
{
    return hotkey_registered;
}

void app_main_window_closed(void)
{
    if (exiting || has_shown_tray_hint) return;
    has_shown_tray_hint = true;
    tray_show_still_running_hint();
}

// ───────────── 还没有移植的窗口 ─────────────
// 置物架窗口在第 4 步移植，在那之前新建置物架只弹一条通知。

static void new_shelf_placeholder(int file_count)
{
    log_info(L"请求打开置物架（%d 个文件）；置物架窗口尚未移植", file_count);
    tray_show_notice(S(STR_NewShelf), tr(L"置物架窗口将在后面的步骤移植。", L"The shelf window comes in a later step."));
}

// ───────────── 设置 ─────────────

static wchar_t *language_pack_directory(void)
{
    wchar_t *data = settings_data_directory();
    wchar_t *directory = path_join(data, LANGUAGE_PACK_FOLDER);
    free(data);
    return directory;
}

/// 设置变化后让各部分立即生效。手势、屏蔽的应用和置物架外观在后面的步骤接进来。
static void apply_settings(const AppSettings *previous, const AppSettings *current)
{
    if (previous->theme != current->theme || !wcs_equal(previous->accent_color, current->accent_color))
        theme_set_mode(current->theme, current->accent_color);
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
    if (!wcs_equal(previous->hotkey, current->hotkey))
        hotkey_registered = hotkey_service_register(settings_hotkey(current));
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

static void change_start_with_windows(AppSettings *target, void *context)
{
    target->start_with_windows = *(bool *)context;
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
    if (!exiting) new_shelf_placeholder(0);
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
    StrList paths;
    single_instance_path_arguments(arguments, &paths);
    new_shelf_placeholder(paths.count);
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
    ui_close_all_windows();
    hotkey_service_stop();
    tray_destroy();
    if (message_window) DestroyWindow(message_window);
    message_window = NULL;
    if (show_window_event) CloseHandle(show_window_event);
    show_window_event = NULL;
    frames_shutdown();
    gfx_shutdown();
    // 关闭主窗口时会再保存一次窗口位置，等它写完再退出。
    settings_flush();
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
            if (!exiting) main_window_show();
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
            // 注销或关机：保存设置。
            if (wparam) app_prepare_for_shutdown();
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
    // 屏蔽的应用在前台时不响应，这个判断在第 3 步（前台窗口监视）接进来。
    app_new_shelf();
}

bool app_start(HINSTANCE instance, bool start_in_background)
{
    settings_load(&settings);
    language_set_mode(settings.language);
    wchar_t *packs = language_pack_directory();
    language_packs_reload(packs);
    free(packs);
    language_set_pack(language_packs_find(settings.language_pack));
    theme_initialize(settings.theme, settings.accent_color);
    low_performance_set_mode(settings.performance_mode);
    log_cleanup_old();
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

    if (!start_in_background) app_show_main_window();
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
