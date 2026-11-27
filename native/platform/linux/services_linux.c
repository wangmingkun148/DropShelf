// 共享代码依赖的系统服务在 Linux 上的实现：快速操作、文件夹选择、消息框、电源抑制、外壳操作等。
// 平台无关的逻辑仍在 native/src，这里只接系统 API。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "app.h"
#include "archive.h"
#include "base.h"
#include "clipboard.h"
#include "drop_data.h"
#include "file_ops.h"
#include "folder_watch.h"
#include "foreground_linux.h"
#include "image_tools.h"
#include "log.h"
#include "power.h"
#include "probe.h"
#include "properties_linux.h"
#include "quick_action.h"
#include "shelf_drag.h"
#include "shelf_window_internal.h"
#include "strings.h"
#include "task.h"
#include "ui.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// 主窗口本身在 main_window.c 里，这里不需要再提供 main_window_ui。

// ───────────── 通知状态和图标 ─────────────

/// 前台全屏（游戏、演示）时算“忙”，其余的勿扰状态交给通知服务自己判断。
HRESULT SHQueryUserNotificationState(QUERY_USER_NOTIFICATION_STATE *state)
{
    if (!state) return E_POINTER;
    *state = foreground_is_fullscreen() ? QUNS_RUNNING_D3D_FULL_SCREEN : QUNS_ACCEPTS_NOTIFICATIONS;
    return S_OK;
}

/// Linux 上没有嵌在可执行文件里的图标资源。
HMODULE GetModuleHandleW(LPCWSTR name)
{
    (void)name;
    return NULL;
}

HICON LoadIconW(HMODULE instance, LPCWSTR name)
{
    (void)instance;
    (void)name;
    return NULL;
}

HICON LoadImageW(HMODULE instance, LPCWSTR name, UINT type, int width, int height, UINT flags)
{
    (void)instance;
    (void)name;
    (void)type;
    (void)width;
    (void)height;
    (void)flags;
    return NULL;
}

/// 消息框：用 GTK 的提示对话框，不阻塞调用方。
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
{
    (void)owner;
    (void)type;
    (void)caption;
    char *message = text ? utf8_from_wide(text, NULL) : NULL;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message ? message : "");
    gtk_alert_dialog_set_modal(dialog, TRUE);
    char *ok = utf8_from_wide(S(STR_Ok), NULL);
    const char *buttons[] = { ok ? ok : "OK", NULL };
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_show(dialog, NULL);
    g_object_unref(dialog);
    free(ok);
    free(message);
    return 1;
}

// ───────────── 电源 ─────────────

// Windows 上这里是请系统别给后台进程降频；Linux 没有对应的按进程节能，只记个数供诊断日志用。
// 不阻止空闲：置物架可以一直开着，阻止了就会一直不锁屏、不休眠。
static GMutex power_lock;
static int power_holders;

void power_hold(const void *holder, bool active)
{
    (void)holder;
    g_mutex_lock(&power_lock);
    if (active) power_holders++;
    else if (power_holders > 0) power_holders--;
    g_mutex_unlock(&power_lock);
}

bool power_is_high(void)
{
    g_mutex_lock(&power_lock);
    bool high = power_holders > 0;
    g_mutex_unlock(&power_lock);
    return high;
}

void power_background_thread(void)
{
}

void power_follow_process(void)
{
}

// ───────────── 探针 ─────────────

// Wayland 上不需要探针窗口：抓手势时 evdev 已经知道按住了鼠标，光标位置由 Hyprland IPC 给出；
// 这个函数只在别的窗口打开前算一个落在工作区里的位置，保留原来的算法。
POINT probe_place(POINT center, RECT work, int width, int height)
{
    double maximum_left = fmax(work.left, (double)work.right - width);
    double maximum_top = fmax(work.top, (double)work.bottom - height);
    double left = fmin(fmax(center.x - width / 2.0, work.left), maximum_left);
    double top = fmin(fmax(center.y - height / 2.0, work.top), maximum_top);
    return (POINT){ (LONG)rint(left), (LONG)rint(top) };
}

// ───────────── 快速操作 ─────────────

// 操作的查找和列表：内置操作来自 settings.c，自定义操作跟设置一起读。
// 真正执行它们（压缩、移动、属性…）要到 M4。
bool quick_action_find(const wchar_t *id, const AppSettings *settings, QuickAction *action)
{
    *action = (QuickAction){0};
    if (wcs_is_blank(id)) return false;
    for (int index = 0; index < quick_action_builtin_count(); index++)
    {
        if (wcscmp(quick_action_builtin_id(index), id) != 0) continue;
        action->id = wcs_dup(id);
        action->type = quick_action_builtin_type(index);
        return true;
    }
    for (int index = 0; index < settings->custom_action_count; index++)
    {
        const CustomAction *custom = &settings->custom_actions[index];
        if (!wcs_equal(custom->id, id)) continue;
        action->id = wcs_dup(id);
        action->type = QUICK_ACTION_CUSTOM;
        action->is_custom = true;
        custom_action_clone(&action->custom, custom);
        return true;
    }
    return false;
}

QuickActionList quick_actions_visible(const AppSettings *settings)
{
    QuickActionList list = {0};
    int wanted = settings->quick_action_layout;
    if (wanted > settings->quick_action_slots.count) wanted = settings->quick_action_slots.count;
    if (wanted <= 0) return list;
    list.items = xcalloc((size_t)wanted, sizeof(QuickAction));
    for (int index = 0; index < wanted; index++)
        if (quick_action_find(settings->quick_action_slots.items[index], settings, &list.items[list.count])) list.count++;
    return list;
}

QuickActionList quick_actions_available(const AppSettings *settings)
{
    QuickActionList list = {0};
    int total = quick_action_builtin_count() + settings->custom_action_count;
    list.items = xcalloc((size_t)(total > 0 ? total : 1), sizeof(QuickAction));
    for (int index = 0; index < quick_action_builtin_count(); index++)
        if (quick_action_find(quick_action_builtin_id(index), settings, &list.items[list.count])) list.count++;
    for (int index = 0; index < settings->custom_action_count; index++)
        if (quick_action_find(settings->custom_actions[index].id, settings, &list.items[list.count])) list.count++;
    return list;
}

void quick_action_list_free(QuickActionList *list)
{
    if (!list) return;
    for (int index = 0; index < list->count; index++) quick_action_free(&list->items[index]);
    free(list->items);
    *list = (QuickActionList){0};
}

bool quick_action_list_same(const QuickActionList *first, const QuickActionList *second)
{
    if (!first || !second || first->count != second->count) return false;
    for (int index = 0; index < first->count; index++)
    {
        const wchar_t *left = first->items[index].id;
        const wchar_t *right = second->items[index].id;
        if (!left || !right)
        {
            if (left != right) return false;
            continue;
        }
        if (wcscmp(left, right) != 0) return false;
    }
    return true;
}

void quick_action_clone(QuickAction *target, const QuickAction *source)
{
    if (!target) return;
    *target = (QuickAction){0};
    if (!source) return;
    target->id = wcs_dup(source->id);
    target->type = source->type;
    target->is_custom = source->is_custom;
    if (source->is_custom) custom_action_clone(&target->custom, &source->custom);
}

void quick_action_free(QuickAction *action)
{
    if (!action) return;
    free(action->id);
    if (action->is_custom) custom_action_free(&action->custom);
    *action = (QuickAction){0};
}

// 图标字形和 Windows 版相同（内置图标字体里重映射了这些码位）。
#define GLYPH_COMPRESS L"\xF012"
#define GLYPH_COPY_PATH L"\xE8C8"
#define GLYPH_MOVE_TO L"\xE8DE"
#define GLYPH_PROPERTIES L"\xE946"
#define GLYPH_REVEAL L"\xE838"
#define GLYPH_COPY_TO L"\xE8B7"
#define GLYPH_COMPRESS_IMAGE L"\xE8B9"
#define GLYPH_CONVERT_IMAGE L"\xE8AB"
#define GLYPH_OPEN_WITH L"\xE7AC"

const wchar_t *quick_action_glyph(const QuickAction *action)
{
    if (action->is_custom)
    {
        switch (action->custom.kind)
        {
            case CUSTOM_ACTION_FOLDER: return action->custom.folder_mode == TRANSFER_COPY ? GLYPH_COPY_TO : GLYPH_MOVE_TO;
            case CUSTOM_ACTION_COMPRESS_IMAGE: return GLYPH_COMPRESS_IMAGE;
            case CUSTOM_ACTION_CONVERT_IMAGE: return GLYPH_CONVERT_IMAGE;
            default: return GLYPH_OPEN_WITH;
        }
    }
    switch (action->type)
    {
        case QUICK_ACTION_COMPRESS_ZIP: return GLYPH_COMPRESS;
        case QUICK_ACTION_COPY_FULL_PATH: return GLYPH_COPY_PATH;
        case QUICK_ACTION_MOVE_TO: return GLYPH_MOVE_TO;
        case QUICK_ACTION_REVEAL: return GLYPH_REVEAL;
        default: return GLYPH_PROPERTIES;
    }
}

wchar_t *program_display_name(const wchar_t *path)
{
    // Linux 上没有可执行文件的“说明”资源；用文件名（去掉扩展名）。
    return path_file_stem(path);
}

static const wchar_t *image_format_name(int format)
{
    return format == IMAGE_FORMAT_PNG ? L"PNG" : L"JPG";
}

/// 自定义操作按内容生成的简短名称。
static wchar_t *custom_auto_name(const CustomAction *custom)
{
    switch (custom->kind)
    {
        case CUSTOM_ACTION_FOLDER:
            return wcs_is_blank(custom->folder) ? wcs_dup(S(STR_KindFolder)) : path_folder_name(custom->folder);
        case CUSTOM_ACTION_COMPRESS_IMAGE:
        {
            wchar_t *size = str_image_size_text(custom->image_size_kb);
            WStr name = {0};
            wstr_appendf(&name, L"≤ %ls", size);
            free(size);
            return wstr_take(&name);
        }
        case CUSTOM_ACTION_CONVERT_IMAGE:
        {
            WStr name = {0};
            wstr_appendf(&name, L"→ %ls", image_format_name(custom->image_format));
            return wstr_take(&name);
        }
        default:
            return wcs_is_blank(custom->program_path) ? wcs_dup(S(STR_KindOpenWith)) : program_display_name(custom->program_path);
    }
}

wchar_t *quick_action_label(const QuickAction *action)
{
    if (action->is_custom) return !wcs_is_blank(action->custom.name) ? wcs_dup(action->custom.name) : custom_auto_name(&action->custom);
    switch (action->type)
    {
        case QUICK_ACTION_COMPRESS_ZIP: return wcs_dup(S(STR_ActionCompress));
        case QUICK_ACTION_COPY_FULL_PATH: return wcs_dup(S(STR_ActionCopyPath));
        case QUICK_ACTION_MOVE_TO: return wcs_dup(S(STR_ActionMoveTo));
        case QUICK_ACTION_REVEAL: return wcs_dup(S(STR_ActionReveal));
        default: return wcs_dup(S(STR_ActionProperties));
    }
}

wchar_t *quick_action_title(const QuickAction *action)
{
    if (!action->is_custom)
    {
        if (action->type == QUICK_ACTION_COPY_FULL_PATH) return wcs_dup(S(STR_MenuCopyPath));
        if (action->type == QUICK_ACTION_REVEAL) return wcs_dup(S(STR_MenuReveal));
        return quick_action_label(action);
    }
    const CustomAction *custom = &action->custom;
    if (!wcs_is_blank(custom->name)) return wcs_dup(custom->name);
    wchar_t *label = custom_auto_name(custom), *title = NULL;
    switch (custom->kind)
    {
        case CUSTOM_ACTION_FOLDER:
            title = custom->folder_mode == TRANSFER_COPY ? str_copy_to_folder_title(label) : str_move_to_folder_title(label);
            break;
        case CUSTOM_ACTION_COMPRESS_IMAGE:
        {
            wchar_t *size = str_image_size_text(custom->image_size_kb);
            title = str_compress_images_title(size);
            free(size);
            break;
        }
        case CUSTOM_ACTION_CONVERT_IMAGE:
            title = str_convert_images_title(image_format_name(custom->image_format));
            break;
        default:
            title = str_open_with_title(label);
            break;
    }
    free(label);
    return title;
}

wchar_t *quick_action_description(const QuickAction *action)
{
    if (!action->is_custom)
    {
        switch (action->type)
        {
            case QUICK_ACTION_COMPRESS_ZIP: return wcs_dup(S(STR_QuickActionDescriptionCompress));
            case QUICK_ACTION_COPY_FULL_PATH: return wcs_dup(S(STR_QuickActionDescriptionCopyPath));
            case QUICK_ACTION_MOVE_TO: return wcs_dup(S(STR_QuickActionDescriptionMoveTo));
            case QUICK_ACTION_REVEAL: return wcs_dup(S(STR_QuickActionDescriptionReveal));
            default: return wcs_dup(S(STR_QuickActionDescriptionProperties));
        }
    }
    const CustomAction *custom = &action->custom;
    const wchar_t *folder = custom->folder ? custom->folder : L"";
    switch (custom->kind)
    {
        case CUSTOM_ACTION_FOLDER:
            return custom->folder_mode == TRANSFER_COPY ? str_copy_to_folder_description(folder) : str_move_to_folder_description(folder);
        case CUSTOM_ACTION_COMPRESS_IMAGE:
        {
            wchar_t *size = str_image_size_text(custom->image_size_kb);
            wchar_t *description = str_compress_images_description(size);
            free(size);
            return description;
        }
        case CUSTOM_ACTION_CONVERT_IMAGE:
            return str_convert_images_description(image_format_name(custom->image_format));
        default:
            return str_open_with_description(custom->program_path ? custom->program_path : L"");
    }
}

wchar_t *quick_action_busy_text(const QuickAction *action)
{
    if (action->is_custom)
    {
        switch (action->custom.kind)
        {
            case CUSTOM_ACTION_FOLDER: return wcs_dup(S(action->custom.folder_mode == TRANSFER_COPY ? STR_BusyCopying : STR_BusyMoving));
            case CUSTOM_ACTION_COMPRESS_IMAGE: return wcs_dup(S(STR_BusyCompressingImages));
            case CUSTOM_ACTION_CONVERT_IMAGE: return wcs_dup(S(STR_BusyConvertingImages));
            default: return wcs_dup(S(STR_BusyOpening));
        }
    }
    switch (action->type)
    {
        case QUICK_ACTION_COMPRESS_ZIP: return wcs_dup(S(STR_BusyCompressing));
        case QUICK_ACTION_COPY_FULL_PATH: return wcs_dup(S(STR_BusyCopyingPaths));
        case QUICK_ACTION_MOVE_TO: return wcs_dup(S(STR_BusyMoving));
        case QUICK_ACTION_REVEAL: return wcs_dup(S(STR_BusyOpeningFolder));
        default: return wcs_dup(S(STR_BusyOpeningProperties));
    }
}

wchar_t *quick_action_tooltip(const QuickAction *action)
{
    wchar_t *title = quick_action_title(action), *description = quick_action_description(action);
    wchar_t *tooltip = str_quick_action_tooltip(title, description);
    free(title);
    free(description);
    return tooltip;
}

bool quick_action_shows_progress(const QuickAction *action)
{
    if (action->is_custom) return action->custom.kind != CUSTOM_ACTION_OPEN_WITH;
    return action->type == QUICK_ACTION_COMPRESS_ZIP || action->type == QUICK_ACTION_MOVE_TO;
}

bool quick_action_takes_text(const QuickAction *action)
{
    if (action->is_custom) return action->custom.kind == CUSTOM_ACTION_FOLDER || action->custom.kind == CUSTOM_ACTION_OPEN_WITH;
    return action->type == QUICK_ACTION_COMPRESS_ZIP || action->type == QUICK_ACTION_MOVE_TO;
}

bool quick_action_asks_for_folder(const QuickAction *action)
{
    return !action->is_custom && action->type == QUICK_ACTION_MOVE_TO;
}

void quick_action_result_set(QuickActionResult *result, bool succeeded, wchar_t *message)
{
    result->succeeded = succeeded;
    free(result->message);
    result->message = message;
}

void quick_action_result_fail(QuickActionResult *result, StringId message)
{
    quick_action_result_set(result, false, wcs_dup(S(message)));
}

void quick_action_result_free(QuickActionResult *result)
{
    if (!result) return;
    free(result->message);
    strlist_clear(&result->outputs);
    strlist_clear(&result->move_sources);
    strlist_clear(&result->move_destinations);
    strlist_clear(&result->replaced_sources);
    strlist_clear(&result->replaced_outputs);
    *result = (QuickActionResult){0};
}

/// 把每个文件作为一个参数加进 argv；token 不为空时是含 {files} 的参数，按每个文件替换一份。
static void add_paths(GPtrArray *argv, const StrList *paths, const char *token)
{
    for (int index = 0; paths && index < paths->count; index++)
    {
        char *path = utf8_from_wide(paths->items[index], NULL);
        if (!path) continue;
        if (!token)
        {
            g_ptr_array_add(argv, g_strdup(path));
        }
        else
        {
            char **parts = g_regex_split_simple("\\{files\\}", token, G_REGEX_CASELESS, 0);
            g_ptr_array_add(argv, g_strjoinv(path, parts));
            g_strfreev(parts);
        }
        free(path);
    }
}

/// 直接构造 argv 启动程序：只解析用户配置的参数模板，文件路径原样作为独立参数传入，避免特殊字符注入额外参数。
static bool launch_program(const wchar_t *program, const wchar_t *arguments_template, const StrList *paths)
{
    if (wcs_is_blank(program)) return false;
    char *program_utf8 = utf8_from_wide(program, NULL);
    if (!program_utf8) return false;

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(program_utf8));

    bool ok = true;
    bool expanded_files = false;
    GError *error = NULL;

    if (!wcs_is_blank(arguments_template))
    {
        char *template_utf8 = utf8_from_wide(arguments_template, NULL);
        char **tokens = NULL;
        int token_count = 0;
        if (!template_utf8 || !g_shell_parse_argv(template_utf8, &token_count, &tokens, &error))
        {
            ok = false;
        }
        else
        {
            for (int index = 0; index < token_count; index++)
            {
                const char *token = tokens[index];
                if (!g_regex_match_simple("\\{files\\}", token, G_REGEX_CASELESS, 0))
                {
                    g_ptr_array_add(argv, g_strdup(token));
                    continue;
                }
                // 单独的 {files} 展开成每个文件一个参数；--x={files} 这样的按每个文件各写一份。
                add_paths(argv, paths, g_ascii_strcasecmp(token, "{files}") == 0 ? NULL : token);
                expanded_files = true;
            }
            g_strfreev(tokens);
        }
        free(template_utf8);
    }

    if (ok && !expanded_files) add_paths(argv, paths, NULL);

    g_ptr_array_add(argv, NULL);
    if (ok) ok = g_spawn_async(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
    g_ptr_array_free(argv, TRUE);
    if (!ok && error)
    {
        log_error(L"启动程序失败：%ls", program);
        g_error_free(error);
    }
    free(program_utf8);
    return ok;
}

static bool direct_child_of(const wchar_t *path, const wchar_t *folder)
{
    wchar_t *trimmed = path_trim_end_separator(path);
    wchar_t *parent = path_directory(trimmed);
    wchar_t *trimmed_folder = path_trim_end_separator(folder);
    bool same = parent && wcscmp(parent, trimmed_folder) == 0;
    free(parent);
    free(trimmed);
    free(trimmed_folder);
    return same;
}

static void split_file_name(const wchar_t *name, wchar_t **stem, wchar_t **extension)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot || dot == name)
    {
        *stem = wcs_dup(name);
        *extension = wcs_dup(L"");
        return;
    }
    *stem = wcs_dup_n(name, (size_t)(dot - name));
    *extension = wcs_dup(dot);
}

/// 复制或移动一项到 destination；目标重名时换一个没占用的名字。成功时 *target 是新位置，由调用方 free。
static bool transfer_one(const wchar_t *source, const wchar_t *destination, bool move, wchar_t **target)
{
    *target = NULL;
    const wchar_t *name = path_file_name(source);
    wchar_t *stem = NULL, *extension = NULL;
    // 文件夹名里的点不是扩展名，重名时整个名字后面再加 (2)。
    if (path_is_directory(source))
    {
        stem = wcs_dup(name);
        extension = wcs_dup(L"");
    }
    else split_file_name(name, &stem, &extension);
    wchar_t *candidate = path_available(destination, stem, extension);
    free(stem);
    free(extension);
    if (!candidate) return false;
    char *source_utf8 = utf8_from_wide(source, NULL);
    char *candidate_utf8 = utf8_from_wide(candidate, NULL);
    bool ok = false;
    if (source_utf8 && candidate_utf8)
    {
        GFile *from = g_file_new_for_path(source_utf8);
        GFile *to = g_file_new_for_path(candidate_utf8);
        GError *error = NULL;
        // Linux 上跨文件系统的移动由 GIO 自己退回“复制再删除”，和 Windows 版行为一致。
        ok = move ? g_file_move(from, to, 0, NULL, NULL, NULL, &error)
                  : g_file_copy(from, to, 0, NULL, NULL, NULL, &error);
        if (!ok)
        {
            log_error(L"复制或移动失败：%ls", source);
            if (error) g_error_free(error);
        }
        g_object_unref(from);
        g_object_unref(to);
    }
    free(source_utf8);
    free(candidate_utf8);
    if (!ok)
    {
        free(candidate);
        return false;
    }
    *target = candidate;
    return true;
}

static void transfer_to_folder(const StrList *paths, const wchar_t *destination, bool move, QuickActionResult *result)
{
    if (wcs_is_blank(destination) || !path_is_directory(destination))
    {
        quick_action_result_fail(result, move ? STR_MoveFailed : STR_CopyFailed);
        return;
    }
    int done = 0, failed = 0, already = 0;
    for (int index = 0; index < paths->count; index++)
    {
        if (move && direct_child_of(paths->items[index], destination))
        {
            already++;
            continue;
        }
        wchar_t *target = NULL;
        if (!transfer_one(paths->items[index], destination, move, &target))
        {
            failed++;
            continue;
        }
        done++;
        if (move)
        {
            strlist_add(&result->move_sources, paths->items[index]);
            strlist_add(&result->move_destinations, target);
        }
        strlist_add(&result->outputs, target);
        free(target);
    }
    int total = done + already;
    wchar_t *folder = path_folder_name(destination);
    wchar_t *message;
    if (failed == 0) message = move ? str_moved_to(total, folder) : str_copied_to(total, folder);
    else if (total == 0) message = wcs_dup(S(move ? STR_MoveFailed : STR_CopyFailed));
    else message = move ? str_moved_partially(total, failed) : str_copied_partially(total, failed);
    free(folder);
    quick_action_result_set(result, total > 0, message);
}

typedef struct ExecuteFast
{
    QuickActionRequest request;
    QuickActionDone done;
    void *context;
} ExecuteFast;

static void finish_request(QuickActionRequest *request, QuickActionResult *result, QuickActionDone done, void *context)
{
    // 写出的文件可能就在观察的文件夹里，告诉观察那边这些是自己写的，不要提示。
    StrList written = {0};
    strlist_clone(&written, &result->outputs);
    for (int index = 0; index < result->move_destinations.count; index++) strlist_add(&written, result->move_destinations.items[index]);
    for (int index = 0; index < result->replaced_outputs.count; index++) strlist_add(&written, result->replaced_outputs.items[index]);
    folder_watch_release(&written);
    strlist_clear(&written);
    quick_action_free(&request->action);
    strlist_clear(&request->paths);
    free(request->folder);
    *request = (QuickActionRequest){0};
    if (done) done(result, context);
    quick_action_result_free(result);
}

typedef struct WorkJob
{
    QuickActionRequest request;
    QuickActionDone done;
    void *context;
    QuickActionResult result;
    wchar_t *directory;     // 压缩的输出位置，或移动/复制到的文件夹
    bool move;
    bool compress;
} WorkJob;

/// 压缩和移动/复制可能很慢，放到后台线程；做完回到界面线程收尾。
static void work_run(void *data)
{
    WorkJob *job = data;
    if (job->compress)
    {
        archive_run(&job->request.paths, job->directory, &job->result);
        job->result.produced_new_items = job->result.succeeded;
    }
    else transfer_to_folder(&job->request.paths, job->directory, job->move, &job->result);
}

static void work_done(void *data)
{
    WorkJob *job = data;
    finish_request(&job->request, &job->result, job->done, job->context);
    free(job->directory);
    free(job);
}

static void start_work(ExecuteFast *job, const wchar_t *directory, bool move, bool compress)
{
    WorkJob *work = xcalloc(1, sizeof(WorkJob));
    work->request = job->request;
    job->request = (QuickActionRequest){0};
    work->done = job->done;
    work->context = job->context;
    work->directory = directory ? wcs_dup(directory) : NULL;
    work->move = move;
    work->compress = compress;
    free(job);
    task_run(work_run, work_done, work);
}

typedef struct ImageJob
{
    QuickActionRequest request;
    QuickActionDone done;
    void *context;
    QuickActionResult result;
    wchar_t *output_directory;
} ImageJob;

/// 压缩或转换图片：在单线程的图片队列里做，做完回到界面线程。
static void image_work(void *data)
{
    ImageJob *job = data;
    const CustomAction *custom = &job->request.action.custom;
    bool compress = custom->kind == CUSTOM_ACTION_COMPRESS_IMAGE;
    QuickActionResult *result = &job->result;
    long long maximum = (long long)custom->image_size_kb * 1024;
    int done = 0, skipped = 0, failed = 0;
    for (int index = 0; index < job->request.paths.count; index++)
    {
        wchar_t *output = NULL;
        ImageStatus status = compress
            ? image_compress(job->request.paths.items[index], maximum, job->output_directory, &output)
            : image_convert(job->request.paths.items[index], custom->image_format, job->output_directory, &output);
        if (status == IMAGE_DONE && output)
        {
            done++;
            strlist_add(&result->replaced_sources, job->request.paths.items[index]);
            strlist_add(&result->replaced_outputs, output);
            strlist_add(&result->outputs, output);
        }
        else if (status == IMAGE_ALREADY) skipped++;
        else if (status == IMAGE_FAILED) failed++;
        free(output);
    }
    if (done + skipped + failed == 0)
    {
        quick_action_result_fail(result, compress ? STR_NoImagesToCompress : STR_NoImagesToConvert);
        return;
    }
    if (done == 0)
    {
        if (failed > 0) quick_action_result_fail(result, compress ? STR_ImageCompressFailed : STR_ImageConvertFailed);
        else if (compress)
        {
            wchar_t *size = str_image_size_text(custom->image_size_kb);
            quick_action_result_set(result, true, str_images_already_small(size));
            free(size);
        }
        else quick_action_result_set(result, true, str_images_already_format(image_format_name(custom->image_format)));
        return;
    }
    WStr message = {0};
    wchar_t *part;
    if (compress)
    {
        wchar_t *size = str_image_size_text(custom->image_size_kb);
        part = str_images_compressed(done, size);
        wstr_append(&message, part);
        free(part);
        if (skipped > 0)
        {
            part = str_images_skipped_small(skipped);
            wstr_append(&message, part);
            free(part);
        }
        free(size);
    }
    else
    {
        const wchar_t *format = image_format_name(custom->image_format);
        part = str_images_converted(done, format);
        wstr_append(&message, part);
        free(part);
        if (skipped > 0)
        {
            part = str_images_skipped_same_format(skipped, format);
            wstr_append(&message, part);
            free(part);
        }
    }
    if (failed > 0)
    {
        part = str_images_failed(failed);
        wstr_append(&message, part);
        free(part);
    }
    quick_action_result_set(result, true, wstr_take(&message));
}

static void image_done(void *data)
{
    ImageJob *job = data;
    free(job->output_directory);
    finish_request(&job->request, &job->result, job->done, job->context);
    free(job);
}

/// 开始图片操作；临时附件要放到下载文件夹，拿不到时直接报告失败。
static void start_image_job(ExecuteFast *job)
{
    ImageJob *image = xcalloc(1, sizeof(ImageJob));
    bool temporary = job->request.has_temporary_files;
    image->request = job->request;
    job->request = (QuickActionRequest){0};
    image->done = job->done;
    image->context = job->context;
    free(job);
    if (temporary)
    {
        image->output_directory = persistent_output_directory();
        if (!image->output_directory)
        {
            log_error(L"准备输出位置失败");
            quick_action_result_fail(&image->result,
                image->request.action.custom.kind == CUSTOM_ACTION_COMPRESS_IMAGE ? STR_ImageCompressFailed : STR_ImageConvertFailed);
            image_done(image);
            return;
        }
    }
    task_run_long_imaging(image_work, image_done, image);
}

/// 快操作（复制路径、在文件管理器中显示、用程序打开）：做完立刻回调。
static void execute_fast(ExecuteFast *job)
{
    QuickActionRequest *request = &job->request;
    QuickActionResult result = {0};
    if (!request->action.is_custom && request->action.type == QUICK_ACTION_COPY_FULL_PATH)
    {
        // 临时附件在缓存里，别的程序读不到：先存一份到下载文件夹，复制的是那份的路径。
        StrList copies = {0};
        const StrList *paths = &request->paths;
        if (request->has_temporary_files)
        {
            wchar_t *directory = persistent_output_directory();
            if (directory)
            {
                for (int index = 0; index < request->paths.count; index++)
                {
                    wchar_t *target = NULL;
                    if (transfer_one(request->paths.items[index], directory, false, &target))
                    {
                        strlist_add_owned(&copies, target);
                        strlist_add(&result.outputs, target);
                    }
                }
                free(directory);
            }
            paths = &copies;
        }
        WStr text = {0};
        for (int index = 0; index < paths->count; index++)
        {
            if (index > 0) wstr_append(&text, L"\r\n");
            wstr_append(&text, paths->items[index]);
        }
        int count = paths->count;
        if (count == 0) quick_action_result_fail(&result, request->has_temporary_files ? STR_AttachmentSaveFailed : STR_NoFilesToProcess);
        else if (clipboard_set_text(text.data ? text.data : L""))
            quick_action_result_set(&result, true, count > 1 ? str_copied_paths(count) : wcs_dup(S(STR_CopiedPath)));
        else quick_action_result_fail(&result, STR_ClipboardBusyOtherApp);
        wstr_free(&text);
        strlist_clear(&copies);
    }
    else if (!request->action.is_custom && request->action.type == QUICK_ACTION_COMPRESS_ZIP)
    {
        wchar_t *directory = request->has_temporary_files ? persistent_output_directory() : NULL;
        if (request->has_temporary_files && !directory) quick_action_result_fail(&result, STR_OutputFolderUnavailable);
        else
        {
            start_work(job, directory, false, true);
            free(directory);
            return;
        }
        free(directory);
    }
    else if (!request->action.is_custom && request->action.type == QUICK_ACTION_MOVE_TO)
    {
        if (wcs_is_blank(request->folder))
        {
            quick_action_result_fail(&result, STR_MoveCancelled);
            result.cancelled = true;
        }
        else
        {
            start_work(job, request->folder, true, false);
            return;
        }
    }
    else if (request->action.is_custom && request->action.custom.kind == CUSTOM_ACTION_FOLDER)
    {
        const wchar_t *folder = request->action.custom.folder;
        if (!folder || !path_is_directory(folder))
        {
            wchar_t *name = path_folder_name(folder ? folder : L"");
            quick_action_result_set(&result, false, str_folder_missing(name));
            free(name);
        }
        else
        {
            bool move = request->action.custom.folder_mode != TRANSFER_COPY;
            start_work(job, folder, move, false);
            return;
        }
    }
    else if (!request->action.is_custom && request->action.type == QUICK_ACTION_PROPERTIES)
    {
        properties_show(request->owner, &request->paths, &result);
        result.keeps_temporary_files = request->has_temporary_files;
    }
    else if (!request->action.is_custom && request->action.type == QUICK_ACTION_REVEAL)
    {
        if (shell_open_folder_and_select(&request->paths)) quick_action_result_set(&result, true, wcs_dup(S(STR_ShownInExplorer)));
        else quick_action_result_fail(&result, STR_CannotOpenFolder);
    }
    else if (request->action.is_custom && (request->action.custom.kind == CUSTOM_ACTION_COMPRESS_IMAGE ||
        request->action.custom.kind == CUSTOM_ACTION_CONVERT_IMAGE))
    {
        start_image_job(job);
        return;
    }
    else if (request->action.is_custom && request->action.custom.kind == CUSTOM_ACTION_OPEN_WITH)
    {
        bool started = launch_program(request->action.custom.program_path, request->action.custom.arguments, &request->paths);
        if (started)
        {
            wchar_t *name = program_display_name(request->action.custom.program_path);
            quick_action_result_set(&result, true, str_opened_with(request->paths.count, name));
            free(name);
        }
        else quick_action_result_fail(&result, STR_CannotOpenItem);
        result.keeps_temporary_files = request->has_temporary_files;
    }
    else
    {
        quick_action_result_set(&result, false, wcs_dup(S(STR_ActionUnsupported)));
    }
    finish_request(request, &result, job->done, job->context);
    free(job);
}

void quick_action_execute(QuickActionRequest *request, QuickActionDone done, void *context)
{
    ExecuteFast *job = xcalloc(1, sizeof(ExecuteFast));
    job->request = *request;
    *request = (QuickActionRequest){0};
    job->done = done;
    job->context = context;
    folder_watch_hold();
    // 和 Windows 版一样，先把不存在（已移动、已删除）的项目去掉。
    StrList available = {0};
    for (int index = 0; index < job->request.paths.count; index++)
        if (path_is_available(job->request.paths.items[index])) strlist_add(&available, job->request.paths.items[index]);
    strlist_clear(&job->request.paths);
    job->request.paths = available;
    if (available.count == 0)
    {
        QuickActionResult result = {0};
        quick_action_result_fail(&result, STR_NoFilesToProcess);
        finish_request(&job->request, &result, job->done, job->context);
        free(job);
        return;
    }
    execute_fast(job);
}

bool shell_open_folder_and_select(const StrList *paths)
{
    if (!paths || paths->count == 0) return false;
    // 桌面有 org.freedesktop.FileManager1 时让它打开文件夹并选中这些文件。
    char **uris = g_new0(char *, (gsize)paths->count + 1);
    int count = 0;
    for (int index = 0; index < paths->count; index++)
    {
        char *native = utf8_from_wide(paths->items[index], NULL);
        if (!native) continue;
        char *uri = g_filename_to_uri(native, NULL, NULL);
        free(native);
        if (uri) uris[count++] = uri;
    }
    bool shown = false;
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (connection && count > 0)
    {
        GVariantBuilder list;
        g_variant_builder_init(&list, G_VARIANT_TYPE_STRING_ARRAY);
        for (int index = 0; index < count; index++) g_variant_builder_add(&list, "s", uris[index]);
        GVariant *reply = g_dbus_connection_call_sync(connection, "org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
            g_variant_new("(@ass)", g_variant_builder_end(&list), ""), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
        if (reply)
        {
            shown = true;
            g_variant_unref(reply);
        }
    }
    if (error) g_error_free(error);
    if (connection) g_object_unref(connection);
    for (int index = 0; index < count; index++) g_free(uris[index]);
    g_free(uris);
    if (shown) return true;
    // 没有 FileManager1：退回到打开所在的文件夹。
    wchar_t *parent = path_directory(paths->items[0]);
    if (!parent) return false;
    bool opened = platform_open_external(parent);
    free(parent);
    return opened;
}

typedef struct FolderPicker
{
    GMainLoop *loop;
    GFile *folder;
} FolderPicker;

static void folder_picked(GtkNativeDialog *dialog, int response, gpointer data)
{
    FolderPicker *picker = data;
    if (response == GTK_RESPONSE_ACCEPT)
    {
        GFile *folder = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (folder) picker->folder = folder;
    }
    g_main_loop_quit(picker->loop);
}

/// 选一个文件夹：用 GTK 的文件对话框（在 Wayland 上由桌面环境提供）。返回值由调用方 free。
wchar_t *pick_folder(HWND owner, const wchar_t *title)
{
    (void)owner;
    char *title_utf8 = title && *title ? utf8_from_wide(title, NULL) : utf8_from_wide(S(STR_ChooseTargetFolder), NULL);
    char *select_utf8 = utf8_from_wide(S(STR_Select), NULL);
    char *cancel_utf8 = utf8_from_wide(S(STR_Cancel), NULL);
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(title_utf8 ? title_utf8 : "", NULL,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, select_utf8 ? select_utf8 : "Select", cancel_utf8 ? cancel_utf8 : "Cancel");
    free(title_utf8);
    free(select_utf8);
    free(cancel_utf8);
    FolderPicker picker = { .loop = g_main_loop_new(NULL, FALSE) };
    g_signal_connect(dialog, "response", G_CALLBACK(folder_picked), &picker);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
    g_main_loop_run(picker.loop);
    g_main_loop_unref(picker.loop);
    g_object_unref(dialog);
    if (!picker.folder) return NULL;
    char *path = g_file_get_path(picker.folder);
    g_object_unref(picker.folder);
    if (!path) return NULL;
    wchar_t *wide = wide_from_utf8(path, strlen(path));
    g_free(path);
    return wide;
}

// ───────────── 拖入时的系统反馈 ─────────────

// Wayland 的 GdkDrop 自己显示拖动图像，没有对应的“让来源程序继续画”的接口。
DropTargetHelper *drop_helper_create(void)
{
    return NULL;
}

void drop_helper_destroy(DropTargetHelper *helper)
{
    (void)helper;
}

void drop_helper_enter(DropTargetHelper *helper, HWND window, IDataObject *data, DWORD effect)
{
    (void)helper;
    (void)window;
    (void)data;
    (void)effect;
}

void drop_helper_over(DropTargetHelper *helper, DWORD effect)
{
    (void)helper;
    (void)effect;
}

void drop_helper_leave(DropTargetHelper *helper)
{
    (void)helper;
}

void drop_helper_drop(DropTargetHelper *helper, IDataObject *data, DWORD effect)
{
    (void)helper;
    (void)data;
    (void)effect;
}

void drop_set_description(IDataObject *data, DropImageType type, const wchar_t *message)
{
    (void)data;
    (void)type;
    (void)message;
}

void shell_paths_prefetch(const StrList *paths)
{
    (void)paths;
}

// 移动模式下同时允许复制（聊天软件和浏览器只接受复制）；复制模式下不允许移动。临时附件不提供链接。
DWORD drag_allowed_effects(bool move_mode, bool has_temporary_files)
{
    DWORD effects = DROPEFFECT_COPY;
    if (move_mode) effects |= DROPEFFECT_MOVE;
    if (!has_temporary_files) effects |= DROPEFFECT_LINK;
    return effects;
}
