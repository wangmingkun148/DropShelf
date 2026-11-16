#include "tray.h"
#include "history_menu.h"
#include "log.h"
#include "shelf_window.h"
#include "strings.h"
#include "ui.h"

#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#define TRAY_ICON_ID 1
#define TRAY_CALLBACK_MESSAGE (WM_APP + 1)
#define APP_ICON_RESOURCE 1

enum
{
    MENU_NEW_SHELF = 1,
    MENU_RECENT,
    MENU_SETTINGS,
    MENU_QUIT,
    MENU_CLEAR_HISTORY,
    MENU_HISTORY_FIRST = 100
};

static HistoryMenu history_menu;

static HWND tray_window;
static TrayActions tray_actions;
static UINT taskbar_created_message;
static HICON tray_icon;
static HICON balloon_icon;      // 通知里用的大图标，第一次显示通知时取
static bool tray_added;
static bool tray_version_4;     // 系统接受了新版的通知方式（NOTIFYICON_VERSION_4）

static void copy_text(wchar_t *target, size_t capacity, const wchar_t *text)
{
    wcsncpy(target, text ? text : L"", capacity - 1);
    target[capacity - 1] = 0;
}

static NOTIFYICONDATAW create_data(UINT flags)
{
    NOTIFYICONDATAW data = { .cbSize = sizeof(data) };
    data.hWnd = tray_window;
    data.uID = TRAY_ICON_ID;
    data.uFlags = flags;
    data.uCallbackMessage = TRAY_CALLBACK_MESSAGE;
    data.hIcon = tray_icon;
    copy_text(data.szTip, ARRAYSIZE(data.szTip), S(STR_TrayTip));
    return data;
}

static void add_icon(void)
{
    NOTIFYICONDATAW data = create_data(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    tray_added = Shell_NotifyIconW(NIM_ADD, &data) != 0;
    if (!tray_added)
    {
        // 资源管理器还没启动好时会失败；它启动后会广播 TaskbarCreated，到时再添加。
        log_info(L"托盘图标暂时没有添加上，等资源管理器就绪后重试");
        return;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    tray_version_4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != 0;
}

void tray_create(HWND window, const TrayActions *actions)
{
    tray_window = window;
    tray_actions = *actions;
    taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;
    tray_icon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    add_icon();
}

void tray_refresh_text(void)
{
    if (!tray_added) return;
    NOTIFYICONDATAW data = create_data(NIF_TIP | NIF_SHOWTIP);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void tray_show_notice(const wchar_t *title, const wchar_t *body)
{
    if (!tray_added) return;
    NOTIFYICONDATAW data = create_data(NIF_INFO);
    copy_text(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), title);
    copy_text(data.szInfo, ARRAYSIZE(data.szInfo), body);
    // 通知里放程序自己的大图标（内嵌的图标按系统大图标的尺寸取），不用系统的信息图标。
    if (!balloon_icon)
    {
        int size = GetSystemMetrics(SM_CXICON);
        if (size <= 0) size = 32;
        balloon_icon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    }
    data.hBalloonIcon = balloon_icon;
    data.dwInfoFlags = (balloon_icon ? NIIF_USER | NIIF_LARGE_ICON : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void tray_show_still_running_hint(void)
{
    tray_show_notice(S(STR_StillRunningTitle), S(STR_StillRunningBody));
}

void tray_destroy(void)
{
    if (tray_added)
    {
        NOTIFYICONDATAW data = create_data(0);
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added = false;
    }
    if (tray_icon) DestroyIcon(tray_icon);
    if (balloon_icon) DestroyIcon(balloon_icon);
    tray_icon = balloon_icon = NULL;
    tray_window = NULL;
}

static void on_menu_chosen(int id, void *context)
{
    (void)context;
    switch (id)
    {
        case MENU_NEW_SHELF: if (tray_actions.new_shelf) tray_actions.new_shelf(); break;
        case MENU_SETTINGS: if (tray_actions.show_window) tray_actions.show_window(); break;
        case MENU_QUIT: if (tray_actions.quit) tray_actions.quit(); break;
        case MENU_CLEAR_HISTORY: shelf_history_clear(); break;
        default:
        {
            ShelfHistoryEntry *entry = history_menu_entry(&history_menu, id);
            if (entry) shelves_restore(entry, NULL);
            break;
        }
    }
}

/// 没有最近关闭的置物架时“最近关闭的置物架”是灰的。
static void show_menu(int x, int y)
{
    if (ui_menu_is_open()) return;
    wchar_t *hotkey = tray_actions.hotkey_text ? tray_actions.hotkey_text() : NULL;
    history_menu_build(&history_menu, MENU_HISTORY_FIRST, MENU_CLEAR_HISTORY);
    UiMenuItem recent = history_menu_root(&history_menu);
    recent.id = MENU_RECENT;
    UiMenuItem items[] = {
        { .id = MENU_NEW_SHELF, .text = S(STR_NewShelf), .glyph = L"\uE710", .gesture = hotkey },
        recent,
        { .separator = true },
        { .id = MENU_SETTINGS, .text = S(STR_OpenSettings), .glyph = L"\uE713" },
        { .id = MENU_QUIT, .text = S(STR_QuitApp), .glyph = L"\uE7E8" },
    };
    ui_menu_show(items, ARRAYSIZE(items), x, y, on_menu_chosen, NULL);
    history_menu_free_texts(&history_menu);
    free(hotkey);
}

bool tray_handle_message(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (taskbar_created_message != 0 && message == taskbar_created_message)
    {
        // 资源管理器重启后需要重新添加托盘图标。
        tray_added = false;
        add_icon();
        return false;
    }
    if (message != TRAY_CALLBACK_MESSAGE || !tray_window) return false;

    // 新版通知方式下，点击会同时送来 WM_LBUTTONUP 和 NIN_SELECT，只认后者，免得打开两次。
    UINT event = LOWORD(lparam);
    bool select = tray_version_4 ? (event == NIN_SELECT || event == NIN_KEYSELECT) : event == WM_LBUTTONUP;
    bool context = tray_version_4 ? event == WM_CONTEXTMENU : event == WM_RBUTTONUP;
    if (select)
    {
        if (tray_actions.show_window) tray_actions.show_window();
        return true;
    }
    if (context)
    {
        POINT cursor;
        if (tray_version_4)
        {
            cursor.x = (short)LOWORD(wparam);
            cursor.y = (short)HIWORD(wparam);
        }
        else
        {
            GetCursorPos(&cursor);
        }
        show_menu(cursor.x, cursor.y);
        return true;
    }
    return false;
}
