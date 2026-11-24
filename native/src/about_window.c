// “关于”窗口：程序和数据的位置、语言包、检查更新、退出与卸载。和 C# 版的 AboutWindow 对应。
#include "about_window.h"
#include "app.h"
#include "file_dialog.h"
#include "lang.h"
#include "log.h"
#include "quick_action.h"
#include "settings.h"
#include "strings.h"
#include "ui.h"
#include "update_check.h"

#ifdef _WIN32
#include <shellapi.h>
#endif
#include <stdlib.h>
#include <string.h>

#ifndef DROPSHELF_VERSION
#define DROPSHELF_VERSION L"0.0.0"
#endif

#define APP_ICON_RESOURCE 1
#define WINDOW_WIDTH 580.0f
#define CAPTION_HEIGHT 40.0f

typedef enum UpdateState { STATE_IDLE, STATE_CHECKING, STATE_LATEST, STATE_AVAILABLE, STATE_FAILED } UpdateState;

static UiWindow *window;
static HWND owner_window;
static GfxImage *icon;
static UiElement *content, *close_button;
static UiElement *version_line;
static UiElement *update_description, *update_button, *uninstall_description, *uninstall_button;
static UpdateState update_state;
static wchar_t *update_url;
static int update_major, update_minor, update_patch;
static bool listening;

bool about_window_is_open(void)
{
    return window != NULL;
}

// ───────────── 打开文件夹 ─────────────

static void open_folder(const wchar_t *path)
{
    if (wcs_is_blank(path)) return;
    directory_create(path);
#ifdef _WIN32
    WStr arguments = {0};
    wstr_appendf(&arguments, L"\"%ls\"", path);
    if ((INT_PTR)ShellExecuteW(NULL, L"open", L"explorer.exe", arguments.data, NULL, SW_SHOWNORMAL) <= 32) log_error(L"打开文件夹失败\r\n%ls", path);
    wstr_free(&arguments);
#else
    if (!platform_open_external(path)) log_error(L"打开文件夹失败\r\n%ls", path);
#endif
}

static wchar_t *language_folder(void)
{
    wchar_t *data = settings_data_directory();
    wchar_t *folder = path_join(data, LANGUAGE_PACK_FOLDER);
    free(data);
    return folder;
}

static void on_open_program(void *context)
{
    (void)context;
    wchar_t *path = executable_directory();
    open_folder(path);
    free(path);
}

static void on_open_settings(void *context)
{
    (void)context;
    wchar_t *path = settings_path();
    wchar_t *folder = path_directory(path);
    open_folder(folder);
    free(folder);
    free(path);
}

static void on_open_logs(void *context)
{
    (void)context;
    wchar_t *path = log_directory();
    open_folder(path);
    free(path);
}

static void on_open_languages(void *context)
{
    (void)context;
    wchar_t *path = language_folder();
    open_folder(path);
    free(path);
}

/// 选个位置导出翻译模板，默认放在语言包文件夹里；写好后在资源管理器里选中它。
static void on_export_template(void *context)
{
    (void)context;
    wchar_t *folder = language_folder();
    directory_create(folder);
    WStr title = {0};
    wstr_append(&title, S(STR_ExportLanguageTemplate));
    // 去掉标题末尾的省略号。
    while (title.length > 0 && (title.data[title.length - 1] == L'…' || title.data[title.length - 1] == L'.')) title.data[--title.length] = 0;
    wchar_t *path = file_dialog_save(ui_window_handle(window), title.data, S(STR_LanguageTemplateFilter), L"template.json", folder, L"json");
    wstr_free(&title);
    free(folder);
    if (!path) return;
    // 对照中文还是英文来翻译，按现在的界面语言定。
    LanguagePack *pack = language_current_pack();
    wchar_t *json = language_template_build(pack ? language_pack_from_chinese(pack) : language_is_chinese());
    size_t length = 0;
    char *bytes = utf8_from_wide(json, &length);
    bool written = file_write_atomic(path, bytes, length);
    free(bytes);
    free(json);
    if (written)
    {
        StrList paths = {0};
        strlist_add(&paths, path);
        shell_open_folder_and_select(&paths);
        strlist_clear(&paths);
    }
    else
    {
        log_win32_error(L"导出翻译模板失败", GetLastError());
        MessageBoxW(ui_window_handle(window), S(STR_LanguageTemplateFailed), L"DropShelf", MB_OK | MB_ICONWARNING);
    }
    free(path);
}

// ───────────── 检查更新 ─────────────

static void refresh_update(void)
{
    if (!update_description) return;
    wchar_t *text = NULL;
    switch (update_state)
    {
        case STATE_CHECKING: text = wcs_dup(S(STR_UpdateChecking)); break;
        case STATE_LATEST: text = str_update_latest(DROPSHELF_VERSION); break;
        case STATE_AVAILABLE:
        {
            wchar_t version[32];
            swprintf(version, 32, L"%d.%d.%d", update_major, update_minor, update_patch);
            text = str_update_available(version);
            break;
        }
        case STATE_FAILED: text = wcs_dup(S(STR_UpdateFailed)); break;
        default: text = wcs_dup(S(STR_UpdateIdle)); break;
    }
    ui_label_set_text(update_description, UI_LIT(text));
    free(text);
    // 有新版本时按钮改为打开下载页。
    ui_button_set_text(update_button, UI_STR(update_state == STATE_AVAILABLE ? STR_UpdateOpenPage : STR_CheckForUpdates));
    ui_set_disabled(update_button, update_state == STATE_CHECKING);
}

static void on_update_checked(const UpdateInfo *info, void *context)
{
    (void)context;
    update_state = info->outcome == UPDATE_NEWER ? STATE_AVAILABLE : info->outcome == UPDATE_LATEST ? STATE_LATEST : STATE_FAILED;
    free(update_url);
    update_url = wcs_dup(info->url);
    update_major = info->major;
    update_minor = info->minor;
    update_patch = info->patch;
    if (window) refresh_update();
}

/// 点“检查更新”：访问一次 GitHub 上的发布列表，告诉用户有没有新版本；有的话再点一次打开下载页。
static void on_check_update(void *context)
{
    (void)context;
    if (update_state == STATE_AVAILABLE && update_url)
    {
        #ifdef _WIN32
        if ((INT_PTR)ShellExecuteW(NULL, L"open", update_url, NULL, NULL, SW_SHOWNORMAL) <= 32) log_error(L"打开下载页失败");
#else
        if (!platform_open_external(update_url)) log_error(L"打开下载页失败");
#endif
        return;
    }
    update_state = STATE_CHECKING;
    refresh_update();
    update_check_start(on_update_checked, NULL);
}

// ───────────── 退出和卸载 ─────────────

/// 安装版的卸载程序与主程序在同一文件夹；便携版和开发版没有。
static wchar_t *uninstaller_path(void)
{
    wchar_t *directory = executable_directory();
    wchar_t *path = path_join(directory, L"unins000.exe");
    free(directory);
    return path;
}

static void on_quit(void *context)
{
    (void)context;
    app_exit();
}

/// 启动卸载程序后退出，卸载时文件不会被占用。
static void on_uninstall(void *context)
{
    (void)context;
    wchar_t *path = uninstaller_path();
#ifdef _WIN32
    wchar_t *directory = executable_directory();
    bool started = (INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, directory, SW_SHOWNORMAL) > 32;
    free(directory);
#else
    // Linux 用发行版的包管理器卸载；按钮在没有卸载程序时本来就是灰的。
    bool started = false;
    (void)path;
#endif
    free(path);
    if (started)
    {
        app_exit();
        return;
    }
    log_win32_error(L"启动卸载程序失败", GetLastError());
    MessageBoxW(ui_window_handle(window), S(STR_UninstallFailed), L"DropShelf", MB_OK | MB_ICONWARNING);
}

static void refresh_uninstall(void)
{
    wchar_t *path = uninstaller_path();
    bool can = path_exists(path);
    free(path);
    ui_label_set_text(uninstall_description, UI_STR(can ? STR_UninstallDescription : STR_UninstallPortable));
    ui_set_disabled(uninstall_button, !can);
}

// ───────────── 内容 ─────────────

static void on_close(void *context)
{
    (void)context;
    about_window_close();
}

static GfxSize root_measure(UiElement *element, float available)
{
    (void)element;
    ui_measure(content, available);
    ui_measure(close_button, 46);
    return (GfxSize){ available, content->desired.height };
}

static void root_arrange(UiElement *element)
{
    GfxRect bounds = element->bounds;
    ui_arrange(content, (GfxRect){ bounds.left, bounds.top, bounds.right, bounds.top + content->desired.height });
    ui_arrange(close_button, (GfxRect){ bounds.right - 46, 0, bounds.right, CAPTION_HEIGHT });
}

static void root_paint(UiElement *element, double time)
{
    (void)element;
    (void)time;
    // 程序图标和名称。
    if (icon)
    {
        GfxRect header = version_line->parent->parent->bounds;
        gfx_draw_image(icon, (GfxRect){ header.left, header.top, header.left + 48, header.top + 48 }, 1);
    }
}

static const UiElementType root_type = { root_measure, root_arrange, root_paint, NULL, NULL, false };

static UiElement *button(StringId text, UiClickHandler clicked, float minimum_width)
{
    UiElement *element = ui_button(UI_BUTTON_STANDARD, UI_STR(text), clicked, NULL);
    element->minimum_width = minimum_width;
    return element;
}

static UiElement *section(UiElement *parent, StringId text)
{
    UiElement *header = ui_label(UI_STR(text), UI_TEXT_BODY_STRONG);
    ui_set_margin(header, 2, 24, 0, 8);
    ui_add(parent, header);
    return header;
}

static void refresh_version_line(void)
{
    wchar_t *version = str_version(DROPSHELF_VERSION);
    WStr line = {0};
    wstr_appendf(&line, L"%ls · %ls", version, S(STR_AboutTagline));
    ui_label_set_text(version_line, UI_LIT(line.data));
    wstr_free(&line);
    free(version);
}

static UiElement *build_content(void)
{
    content = ui_stack(false, 0);
    ui_stack_set_padding(content, 28, 44, 28, 28);

    UiElement *header = ui_stack(false, 0);
    ui_set_margin(header, 0, 0, 0, 0);
    header->minimum_height = 48;
    UiElement *names = ui_stack(false, 0);
    ui_set_margin(names, 64, 4, 0, 0);
    UiElement *title = ui_label(UI_LIT(L"DropShelf for Windows"), UI_TEXT_SUBTITLE);
    ui_label_set_size(title, 18);
    ui_add(names, title);
    version_line = ui_label(UI_NO_TEXT, UI_TEXT_CAPTION);
    ui_label_set_size(version_line, 13);
    ui_set_margin(version_line, 0, 3, 0, 0);
    ui_add(names, version_line);
    ui_add(header, names);
    ui_add(content, header);
    // version_line 的上一级是 names；图标画在 header 的左上角。
    refresh_version_line();

    section(content, STR_DataLocations);
    wchar_t *program = executable_directory();
    ui_add(content, ui_settings_card(L"\xE838", UI_STR(STR_ProgramLocation), UI_LIT(program), button(STR_OpenFolder, on_open_program, 0), NULL));
    free(program);
    wchar_t *settings_file = settings_path();
    ui_add(content, ui_settings_card(L"\xE713", UI_STR(STR_SettingsFile), UI_LIT(settings_file), button(STR_OpenFolder, on_open_settings, 0), NULL));
    free(settings_file);
    wchar_t *logs = log_directory();
    ui_add(content, ui_settings_card(L"\xE8A5", UI_STR(STR_Logs), UI_LIT(logs), button(STR_OpenFolder, on_open_logs, 0), NULL));
    free(logs);
    // 语言包放在这个文件夹里；导出的模板是一个空白的语言包，填上译文再放回来。
    UiElement *pack_buttons = ui_stack(true, 8);
    pack_buttons->cross = UI_CROSS_CENTER;
    ui_add(pack_buttons, button(STR_ExportLanguageTemplate, on_export_template, 0));
    ui_add(pack_buttons, button(STR_OpenFolder, on_open_languages, 0));
    wchar_t *languages = language_folder();
    ui_add(content, ui_settings_card(L"\xE775", UI_STR(STR_LanguagePackHeader), UI_LIT(languages), pack_buttons, NULL));
    free(languages);

    section(content, STR_General);
    // 平时不联网：点了这个按钮才访问一次 GitHub 上的发布列表。
    update_button = button(STR_CheckForUpdates, on_check_update, 96);
    ui_add(content, ui_settings_card(L"\xE896", UI_STR(STR_CheckForUpdates), UI_STR(STR_UpdateIdle), update_button, &update_description));
    ui_add(content, ui_settings_card(L"\xE7E8", UI_STR(STR_QuitApp), UI_STR(STR_QuitDescription), button(STR_Quit, on_quit, 96), NULL));
    uninstall_button = button(STR_Uninstall, on_uninstall, 96);
    ui_add(content, ui_settings_card(L"\xE74D", UI_STR(STR_UninstallHeader), UI_STR(STR_UninstallDescription), uninstall_button, &uninstall_description));
    return content;
}

static void on_closed(UiWindow *closed, void *context)
{
    (void)closed;
    (void)context;
    window = NULL;
    content = close_button = version_line = NULL;
    update_description = update_button = uninstall_description = uninstall_button = NULL;
    // 关掉之后主窗口又能操作了。
    if (owner_window && IsWindow(owner_window))
    {
        EnableWindow(owner_window, TRUE);
        SetForegroundWindow(owner_window);
    }
    owner_window = NULL;
}

static void on_language_changed(void *context)
{
    (void)context;
    if (!window) return;
    refresh_version_line();
    refresh_update();
    refresh_uninstall();
}

void about_window_show(HWND owner)
{
    if (window)
    {
        ui_window_show(window);
        return;
    }
    if (!listening)
    {
        listening = true;
        language_add_listener(on_language_changed, NULL);
    }
    UiWindowOptions options = { 0 };
    options.title = S(STR_AboutTooltip);
    options.width = WINDOW_WIDTH;
    options.height = 400;
    options.resizable = false;
    options.backdrop = UI_BACKDROP_MICA;
    options.background = THEME_WINDOW_BACKGROUND;
    options.caption_height = CAPTION_HEIGHT;
    #ifdef _WIN32
    options.icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE));
#else
    options.icon = NULL;
#endif
    options.closed = on_closed;
    window = ui_window_create(&options);
    if (!window) return;
#ifdef _WIN32
    // 不在任务栏里另占一格，跟着主窗口。
    HWND handle = ui_window_handle(window);
    if (owner) SetWindowLongPtrW(handle, GWLP_HWNDPARENT, (LONG_PTR)owner);
    if (!icon)
    {
        HICON large = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(APP_ICON_RESOURCE), IMAGE_ICON, 96, 96, 0);
        icon = gfx_image_from_icon(large);
        if (large) DestroyIcon(large);
    }
#else
    // Linux 的图标跟着 .desktop 的 app id 走，窗口里不另外画。
    (void)owner;
#endif
    UiElement *root = ui_element_new(&root_type, sizeof(UiElement));
    ui_add(root, build_content());
    close_button = ui_button(UI_BUTTON_CAPTION_CLOSE, UI_LIT(L"\xE711"), on_close, NULL);
    ui_set_tooltip(close_button, UI_STR(STR_CaptionClose));
    ui_add(root, close_button);
    ui_window_set_root(window, root);
    update_state = update_state == STATE_CHECKING ? STATE_CHECKING : STATE_IDLE;
    refresh_update();
    refresh_uninstall();

    // 高度随内容（和 WPF 的 SizeToContent 一样），放在主窗口中央。
    float height = ui_measure(root, WINDOW_WIDTH).height;
    RECT anchor;
    float scale = ui_window_scale(window);
    if (owner && GetWindowRect(owner, &anchor))
    {
        int width_pixels = (int)(WINDOW_WIDTH * scale), height_pixels = (int)(height * scale);
        int x = anchor.left + (anchor.right - anchor.left - width_pixels) / 2, y = anchor.top + (anchor.bottom - anchor.top - height_pixels) / 2;
        ui_window_place(window, x, y, WINDOW_WIDTH, height);
    }
    else
    {
        ui_window_place(window, 0, 0, WINDOW_WIDTH, height);
        ui_window_center(window);
    }
    // 模态：关掉之前主窗口不能操作。
    owner_window = owner;
    if (owner) EnableWindow(owner, FALSE);
    ui_window_show(window);
}

void about_window_close(void)
{
    if (window) ui_window_close(window);
}
