#define COBJMACROS
#include "quick_action.h"
#include "archive.h"
#include "clipboard.h"
#include "file_ops.h"
#include "folder_watch.h"
#include "image_tools.h"
#include "log.h"
#include "shelf_drag.h"
#include "task.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>

// 图标字形（Segoe Fluent Icons），和 C# 版相同。
#define GLYPH_COMPRESS L"\xF012"
#define GLYPH_COPY_PATH L"\xE8C8"
#define GLYPH_MOVE_TO L"\xE8DE"
#define GLYPH_PROPERTIES L"\xE946"
#define GLYPH_REVEAL L"\xE838"
#define GLYPH_COPY_TO L"\xE8B7"
#define GLYPH_COMPRESS_IMAGE L"\xE8B9"
#define GLYPH_CONVERT_IMAGE L"\xE8AB"
#define GLYPH_OPEN_WITH L"\xE7AC"

// ───────────── 操作本身 ─────────────

void quick_action_clone(QuickAction *target, const QuickAction *source)
{
    *target = (QuickAction){0};
    target->id = wcs_dup(source->id);
    target->type = source->type;
    target->is_custom = source->is_custom;
    if (source->is_custom) custom_action_clone(&target->custom, &source->custom);
}

void quick_action_free(QuickAction *action)
{
    free(action->id);
    if (action->is_custom) custom_action_free(&action->custom);
    *action = (QuickAction){0};
}

/// 按标识查找操作；已删除的自定义操作或未知标识返回 false。
static bool find_action(const wchar_t *id, const AppSettings *settings, QuickAction *action)
{
    if (wcs_is_blank(id)) return false;
    for (int index = 0; index < quick_action_builtin_count(); index++)
    {
        if (wcscmp(quick_action_builtin_id(index), id) != 0) continue;
        *action = (QuickAction){ wcs_dup(id), quick_action_builtin_type(index), false, {0} };
        return true;
    }
    for (int index = 0; index < settings->custom_action_count; index++)
    {
        const CustomAction *custom = &settings->custom_actions[index];
        if (!wcs_equal(custom->id, id)) continue;
        *action = (QuickAction){ wcs_dup(id), QUICK_ACTION_CUSTOM, true, {0} };
        custom_action_clone(&action->custom, custom);
        return true;
    }
    return false;
}

bool quick_action_find(const wchar_t *id, const AppSettings *settings, QuickAction *action)
{
    return find_action(id, settings, action);
}

QuickActionList quick_actions_available(const AppSettings *settings)
{
    QuickActionList list = {0};
    int total = quick_action_builtin_count() + settings->custom_action_count;
    list.items = xcalloc((size_t)(total > 0 ? total : 1), sizeof(QuickAction));
    for (int index = 0; index < quick_action_builtin_count(); index++)
        if (find_action(quick_action_builtin_id(index), settings, &list.items[list.count])) list.count++;
    for (int index = 0; index < settings->custom_action_count; index++)
        if (find_action(settings->custom_actions[index].id, settings, &list.items[list.count])) list.count++;
    return list;
}

QuickActionList quick_actions_visible(const AppSettings *settings)
{
    QuickActionList list = {0};
    int wanted = settings->quick_action_layout;
    if (wanted > settings->quick_action_slots.count) wanted = settings->quick_action_slots.count;
    if (wanted <= 0) return list;
    list.items = xcalloc((size_t)wanted, sizeof(QuickAction));
    for (int index = 0; index < wanted; index++)
        if (find_action(settings->quick_action_slots.items[index], settings, &list.items[list.count])) list.count++;
    return list;
}

void quick_action_list_free(QuickActionList *list)
{
    for (int index = 0; index < list->count; index++) quick_action_free(&list->items[index]);
    free(list->items);
    *list = (QuickActionList){0};
}

bool quick_action_list_same(const QuickActionList *first, const QuickActionList *second)
{
    if (first->count != second->count) return false;
    for (int index = 0; index < first->count; index++)
    {
        const QuickAction *a = &first->items[index], *b = &second->items[index];
        if (!wcs_equal(a->id, b->id) || a->is_custom != b->is_custom) return false;
        if (a->is_custom && !custom_action_same(&a->custom, &b->custom)) return false;
    }
    return true;
}

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

// ───────────── 结果 ─────────────

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
    free(result->message);
    strlist_clear(&result->outputs);
    strlist_clear(&result->move_sources);
    strlist_clear(&result->move_destinations);
    strlist_clear(&result->replaced_sources);
    strlist_clear(&result->replaced_outputs);
    *result = (QuickActionResult){0};
}

// ───────────── 系统窗口和程序 ─────────────

bool shell_open_folder_and_select(const StrList *paths)
{
    StrList folders = {0};
    for (int index = 0; index < paths->count; index++)
    {
        if (!path_exists(paths->items[index])) continue;
        wchar_t *trimmed = path_trim_end_separator(paths->items[index]);
        wchar_t *parent = path_directory(trimmed);
        if (!parent) parent = wcs_dup(trimmed);
        free(trimmed);
        if (strlist_index_of(&folders, parent, true) < 0 && folders.count < 6) strlist_add(&folders, parent);
        free(parent);
    }
    bool opened = false;
    for (int group = 0; group < folders.count; group++)
    {
        PIDLIST_ABSOLUTE folder = NULL;
        if (FAILED(SHParseDisplayName(folders.items[group], NULL, &folder, 0, NULL)) || !folder) continue;
        PIDLIST_ABSOLUTE *children = xcalloc((size_t)paths->count, sizeof(PIDLIST_ABSOLUTE));
        PCUITEMID_CHILD *relative = xcalloc((size_t)paths->count, sizeof(PCUITEMID_CHILD));
        int child_count = 0;
        for (int index = 0; index < paths->count; index++)
        {
            if (!path_exists(paths->items[index])) continue;
            wchar_t *trimmed = path_trim_end_separator(paths->items[index]);
            wchar_t *parent = path_directory(trimmed);
            bool here = wcs_equal_ignore_case(parent ? parent : trimmed, folders.items[group]);
            free(parent);
            free(trimmed);
            PIDLIST_ABSOLUTE child = NULL;
            if (!here || FAILED(SHParseDisplayName(paths->items[index], NULL, &child, 0, NULL)) || !child) continue;
            children[child_count] = child;
            relative[child_count++] = ILFindLastID((PCUIDLIST_RELATIVE)child);
        }
        if (SUCCEEDED(SHOpenFolderAndSelectItems(folder, (UINT)child_count, relative, 0))) opened = true;
        for (int index = 0; index < child_count; index++) CoTaskMemFree(children[index]);
        free(children);
        free(relative);
        CoTaskMemFree(folder);
    }
    strlist_clear(&folders);
    return opened;
}

wchar_t *pick_folder(HWND owner, const wchar_t *title)
{
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(dialog, &options);
    IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (title) IFileOpenDialog_SetTitle(dialog, title);
    wchar_t *folder = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dialog, owner)))
    {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item)) && item)
        {
            PWSTR path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) folder = wcs_dup(path);
            CoTaskMemFree(path);
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dialog);
    return folder;
}

typedef struct ProgramName
{
    wchar_t *path;
    wchar_t *name;
} ProgramName;

static ProgramName *program_names;
static int program_name_count;

static wchar_t *read_file_description(const wchar_t *path)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return NULL;
    void *data = xalloc(size);
    wchar_t *description = NULL;
    struct { WORD language, code_page; } *translations = NULL;
    UINT length = 0;
    if (GetFileVersionInfoW(path, 0, size, data) && VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&translations, &length) &&
        length >= sizeof(*translations))
    {
        wchar_t query[64];
        swprintf(query, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language, translations[0].code_page);
        wchar_t *value = NULL;
        UINT value_length = 0;
        if (VerQueryValueW(data, query, (void **)&value, &value_length) && value && value_length > 0)
        {
            wchar_t *trimmed = wcs_trim_dup(value);
            size_t characters = wcslen(trimmed);
            if (characters > 0 && characters <= 24) description = trimmed;
            else free(trimmed);
        }
    }
    free(data);
    return description;
}

wchar_t *program_display_name(const wchar_t *path)
{
    for (int index = 0; index < program_name_count; index++)
        if (wcs_equal(program_names[index].path, path)) return wcs_dup(program_names[index].name);
    wchar_t *name = path_exists(path) ? read_file_description(path) : NULL;
    if (!name) name = path_file_stem(path);
    program_names = xrealloc(program_names, (size_t)(program_name_count + 1) * sizeof(ProgramName));
    program_names[program_name_count++] = (ProgramName){ wcs_dup(path), wcs_dup(name) };
    return name;
}

static void quote_argument(WStr *text, const wchar_t *path)
{
    wstr_append_char(text, L'"');
    wstr_append(text, path);
    // 以反斜杠结尾的路径（如磁盘根目录）要把结尾的反斜杠写成两倍，否则会转义结尾的引号；Windows 路径里不会有引号。
    for (size_t length = wcslen(path); length > 0 && path[length - 1] == L'\\'; length--) wstr_append_char(text, L'\\');
    wstr_append_char(text, L'"');
}

wchar_t *quick_action_build_arguments(const wchar_t *template_text, const StrList *paths)
{
    WStr files = {0};
    wstr_append(&files, L"");
    for (int index = 0; index < paths->count; index++)
    {
        if (index > 0) wstr_append_char(&files, L' ');
        quote_argument(&files, paths->items[index]);
    }
    if (wcs_is_blank(template_text)) return wstr_take(&files);
    WStr result = {0};
    wstr_append(&result, L"");
    if (wcs_find_ignore_case(template_text, L"{files}"))
    {
        const wchar_t *cursor = template_text, *found;
        while ((found = wcs_find_ignore_case(cursor, L"{files}")) != NULL)
        {
            wstr_append_n(&result, cursor, (size_t)(found - cursor));
            wstr_append(&result, files.data);
            cursor = found + 7;
        }
        wstr_append(&result, cursor);
    }
    else
    {
        wchar_t *trimmed = wcs_trim_dup(template_text);
        wstr_appendf(&result, L"%ls %ls", trimmed, files.data);
        free(trimmed);
    }
    wstr_free(&files);
    return wstr_take(&result);
}

static void show_properties(HWND owner, const StrList *paths, QuickActionResult *result)
{
    StrList available = {0};
    for (int index = 0; index < paths->count; index++)
        if (path_is_available(paths->items[index])) strlist_add(&available, paths->items[index]);
    if (available.count == 0)
    {
        quick_action_result_fail(result, STR_NoFilesForProperties);
        return;
    }
    if (available.count > 1)
    {
        IDataObject *data = shell_data_object_create(&available);
        if (data)
        {
            HRESULT shown = SHMultiFileProperties(data, 0);
            IDataObject_Release(data);
            if (shown == S_OK)
            {
                quick_action_result_set(result, true, str_opened_properties_for(available.count));
                strlist_clear(&available);
                return;
            }
        }
    }
    if (SHObjectProperties(owner, SHOP_FILEPATH, available.items[0], NULL)) quick_action_result_set(result, true, wcs_dup(S(STR_OpenedProperties)));
    else quick_action_result_fail(result, STR_PropertiesUnavailable);
    strlist_clear(&available);
}

static void open_with(const CustomAction *custom, const wchar_t *label, const StrList *paths, QuickActionResult *result)
{
    const wchar_t *program = custom->program_path ? custom->program_path : L"";
    DWORD attributes = io_get_attributes(program);
    if (wcs_is_blank(program) || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        quick_action_result_set(result, false, str_program_missing(label));
        return;
    }
    wchar_t *arguments = quick_action_build_arguments(custom->arguments, paths);
    wchar_t *directory = path_directory(program);
    SHELLEXECUTEINFOW info = { .cbSize = sizeof(info) };
    info.fMask = SEE_MASK_NOASYNC;
    info.lpFile = program;
    info.lpParameters = arguments;
    info.lpDirectory = directory;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) quick_action_result_set(result, true, str_opened_with(paths->count, label));
    else
    {
        log_win32_error(L"用程序打开失败", GetLastError());
        quick_action_result_set(result, false, str_open_with_failed(label));
    }
    free(arguments);
    free(directory);
}

// ───────────── 在后台做的部分 ─────────────

typedef struct Job
{
    QuickActionRequest request;
    QuickActionResult result;
    QuickActionDone done;
    void *context;
    wchar_t *output_directory;  // 临时附件处理后的结果放到下载文件夹
} Job;

static void job_finished(void *data)
{
    Job *job = data;
    // 写出的文件可能就在观察的文件夹里，告诉观察那边这些是自己写的，不要提示。
    StrList written = {0};
    strlist_clone(&written, &job->result.outputs);
    for (int index = 0; index < job->result.move_destinations.count; index++) strlist_add(&written, job->result.move_destinations.items[index]);
    for (int index = 0; index < job->result.replaced_outputs.count; index++) strlist_add(&written, job->result.replaced_outputs.items[index]);
    folder_watch_release(&written);
    strlist_clear(&written);
    job->done(&job->result, job->context);
    quick_action_result_free(&job->result);
    quick_action_free(&job->request.action);
    strlist_clear(&job->request.paths);
    free(job->request.folder);
    free(job->output_directory);
    free(job);
}

static void finish_soon(Job *job)
{
    task_post(job_finished, job);
}

static void move_to_folder(const StrList *paths, const wchar_t *destination, QuickActionResult *result)
{
    // 已经在目标文件夹中的项目不需要移动，否则会被重命名为“(2)”。
    StrList pending = {0};
    int already = 0;
    for (int index = 0; index < paths->count; index++)
    {
        if (path_is_direct_child(paths->items[index], destination)) already++;
        else strlist_add(&pending, paths->items[index]);
    }
    FileOpResults results = file_ops_move(&pending, destination);
    int moved = 0, left = 0;
    for (int index = 0; index < results.count; index++)
    {
        FileOpResult *item = &results.items[index];
        if (!item->succeeded || !item->destination) continue;
        moved++;
        left += item->source_left;
        strlist_add(&result->move_sources, item->source);
        strlist_add(&result->move_destinations, item->destination);
        strlist_add(&result->outputs, item->destination);
    }
    int done = moved + already, failed = results.count - moved;
    wchar_t *folder = path_folder_name(destination);
    wchar_t *message = failed == 0 ? str_moved_to(done, folder) : done == 0 ? wcs_dup(S(STR_MoveFailed)) : str_moved_partially(done, failed);
    if (left > 0)
    {
        // 移过去了，但原位置还有残留：照样算成功，提示用户去原位置看看。
        WStr text = {0};
        wstr_append(&text, message);
        free(message);
        wchar_t *part = str_move_left_behind(left);
        wstr_append(&text, part);
        free(part);
        message = wstr_take(&text);
    }
    quick_action_result_set(result, done > 0, message);
    free(folder);
    file_op_results_free(&results);
    strlist_clear(&pending);
}

static void copy_to_folder(const StrList *paths, const wchar_t *destination, QuickActionResult *result)
{
    FileOpResults results = file_ops_copy(paths, destination);
    int copied = 0;
    for (int index = 0; index < results.count; index++)
    {
        copied += results.items[index].succeeded;
        if (results.items[index].succeeded && results.items[index].destination) strlist_add(&result->outputs, results.items[index].destination);
    }
    int failed = results.count - copied;
    wchar_t *folder = path_folder_name(destination);
    wchar_t *message = failed == 0 ? str_copied_to(copied, folder) : copied == 0 ? wcs_dup(S(STR_CopyFailed)) : str_copied_partially(copied, failed);
    quick_action_result_set(result, copied > 0, message);
    free(folder);
    file_op_results_free(&results);
}

static void move_work(void *data)
{
    Job *job = data;
    move_to_folder(&job->request.paths, job->request.folder, &job->result);
}

static void folder_work(void *data)
{
    Job *job = data;
    const CustomAction *custom = &job->request.action.custom;
    if (custom->folder_mode == TRANSFER_COPY) copy_to_folder(&job->request.paths, custom->folder, &job->result);
    else move_to_folder(&job->request.paths, custom->folder, &job->result);
}

static void compress_work(void *data)
{
    Job *job = data;
    archive_run(&job->request.paths, job->output_directory, &job->result);
    job->result.produced_new_items = job->result.succeeded;
}

static void save_copies_work(void *data)
{
    Job *job = data;
    if (!job->output_directory)
    {
        quick_action_result_fail(&job->result, STR_AttachmentSaveFailed);
        return;
    }
    FileOpResults results = file_ops_copy(&job->request.paths, job->output_directory);
    for (int index = 0; index < results.count; index++)
        if (results.items[index].succeeded && results.items[index].destination) strlist_add(&job->result.outputs, results.items[index].destination);
    if (job->result.outputs.count > 0) quick_action_result_set(&job->result, true, str_saved_to_downloads(job->result.outputs.count));
    else quick_action_result_fail(&job->result, STR_AttachmentSaveFailed);
    file_op_results_free(&results);
}

/// 把路径复制到剪贴板（剪贴板要在界面线程上用）。临时附件先存了一份到下载文件夹，复制的是那份的路径。
static void copy_paths_done(void *data)
{
    Job *job = data;
    const StrList *paths = &job->request.paths;
    if (job->request.has_temporary_files)
    {
        if (!job->result.succeeded || job->result.outputs.count == 0)
        {
            job_finished(job);
            return;
        }
        paths = &job->result.outputs;
    }
    WStr text = {0};
    for (int index = 0; index < paths->count; index++)
    {
        if (index > 0) wstr_append(&text, L"\r\n");
        wstr_append(&text, paths->items[index]);
    }
    int count = paths->count;
    if (clipboard_set_text(text.data ? text.data : L""))
        quick_action_result_set(&job->result, true, count > 1 ? str_copied_paths(count) : wcs_dup(S(STR_CopiedPath)));
    else quick_action_result_fail(&job->result, STR_ClipboardBusyOtherApp);
    wstr_free(&text);
    job_finished(job);
}

static void compress_images_work(void *data)
{
    Job *job = data;
    const CustomAction *custom = &job->request.action.custom;
    long long maximum = (long long)custom->image_size_kb * 1024;
    int compressed = 0, already_small = 0, failed = 0;
    QuickActionResult *result = &job->result;
    for (int index = 0; index < job->request.paths.count; index++)
    {
        wchar_t *output = NULL;
        ImageStatus status = image_compress(job->request.paths.items[index], maximum, job->output_directory, &output);
        if (status == IMAGE_DONE && output)
        {
            compressed++;
            strlist_add(&result->replaced_sources, job->request.paths.items[index]);
            strlist_add(&result->replaced_outputs, output);
            strlist_add(&result->outputs, output);
        }
        else if (status == IMAGE_ALREADY) already_small++;
        else if (status == IMAGE_FAILED) failed++;
        free(output);
    }
    wchar_t *size = str_image_size_text(custom->image_size_kb);
    if (compressed + already_small + failed == 0) quick_action_result_fail(result, STR_NoImagesToCompress);
    else if (compressed == 0)
    {
        if (failed == 0) quick_action_result_set(result, true, str_images_already_small(size));
        else quick_action_result_fail(result, STR_ImageCompressFailed);
    }
    else
    {
        WStr message = {0};
        wchar_t *part = str_images_compressed(compressed, size);
        wstr_append(&message, part);
        free(part);
        if (already_small > 0)
        {
            part = str_images_skipped_small(already_small);
            wstr_append(&message, part);
            free(part);
        }
        if (failed > 0)
        {
            part = str_images_failed(failed);
            wstr_append(&message, part);
            free(part);
        }
        quick_action_result_set(result, true, wstr_take(&message));
    }
    free(size);
}

static void convert_images_work(void *data)
{
    Job *job = data;
    const CustomAction *custom = &job->request.action.custom;
    int converted = 0, same = 0, failed = 0;
    QuickActionResult *result = &job->result;
    for (int index = 0; index < job->request.paths.count; index++)
    {
        wchar_t *output = NULL;
        ImageStatus status = image_convert(job->request.paths.items[index], custom->image_format, job->output_directory, &output);
        if (status == IMAGE_DONE && output)
        {
            converted++;
            strlist_add(&result->replaced_sources, job->request.paths.items[index]);
            strlist_add(&result->replaced_outputs, output);
            strlist_add(&result->outputs, output);
        }
        else if (status == IMAGE_ALREADY) same++;
        else if (status == IMAGE_FAILED) failed++;
        free(output);
    }
    const wchar_t *format = image_format_name(custom->image_format);
    if (converted + same + failed == 0) quick_action_result_fail(result, STR_NoImagesToConvert);
    else if (converted == 0)
    {
        if (failed == 0) quick_action_result_set(result, true, str_images_already_format(format));
        else quick_action_result_fail(result, STR_ImageConvertFailed);
    }
    else
    {
        WStr message = {0};
        wchar_t *part = str_images_converted(converted, format);
        wstr_append(&message, part);
        free(part);
        if (same > 0)
        {
            part = str_images_skipped_same_format(same, format);
            wstr_append(&message, part);
            free(part);
        }
        if (failed > 0)
        {
            part = str_images_failed(failed);
            wstr_append(&message, part);
            free(part);
        }
        quick_action_result_set(result, true, wstr_take(&message));
    }
}

/// 临时附件处理后的结果放到下载文件夹；拿不到时用 failure 说明原因。
static bool prepare_output(Job *job, StringId failure)
{
    if (!job->request.has_temporary_files) return true;
    job->output_directory = persistent_output_directory();
    if (job->output_directory) return true;
    log_error(L"准备输出位置失败");
    quick_action_result_fail(&job->result, failure);
    finish_soon(job);
    return false;
}

static void execute_custom(Job *job)
{
    const CustomAction *custom = &job->request.action.custom;
    switch (custom->kind)
    {
        case CUSTOM_ACTION_FOLDER:
            if (!custom->folder || !path_is_directory(custom->folder))
            {
                wchar_t *name = path_folder_name(custom->folder ? custom->folder : L"");
                quick_action_result_set(&job->result, false, str_folder_missing(name));
                free(name);
                finish_soon(job);
                return;
            }
            task_run(folder_work, job_finished, job);
            return;
        case CUSTOM_ACTION_COMPRESS_IMAGE:
            if (prepare_output(job, STR_ImageCompressFailed)) task_run_long_imaging(compress_images_work, job_finished, job);
            return;
        case CUSTOM_ACTION_CONVERT_IMAGE:
            if (prepare_output(job, STR_ImageConvertFailed)) task_run_long_imaging(convert_images_work, job_finished, job);
            return;
        default:
        {
            wchar_t *label = quick_action_label(&job->request.action);
            open_with(custom, label, &job->request.paths, &job->result);
            free(label);
            job->result.keeps_temporary_files = job->request.has_temporary_files;
            finish_soon(job);
            return;
        }
    }
}

void quick_action_execute(QuickActionRequest *request, QuickActionDone done, void *context)
{
    Job *job = xcalloc(1, sizeof(Job));
    job->request = *request;
    *request = (QuickActionRequest){0};
    folder_watch_hold();
    job->done = done;
    job->context = context;

    StrList available = {0};
    for (int index = 0; index < job->request.paths.count; index++)
        if (path_is_available(job->request.paths.items[index])) strlist_add(&available, job->request.paths.items[index]);
    strlist_clear(&job->request.paths);
    job->request.paths = available;
    if (available.count == 0)
    {
        quick_action_result_fail(&job->result, STR_NoFilesToProcess);
        finish_soon(job);
        return;
    }
    if (job->request.action.is_custom)
    {
        execute_custom(job);
        return;
    }
    switch (job->request.action.type)
    {
        case QUICK_ACTION_COMPRESS_ZIP:
            if (prepare_output(job, STR_OutputFolderUnavailable)) task_run(compress_work, job_finished, job);
            return;
        case QUICK_ACTION_COPY_FULL_PATH:
            if (job->request.has_temporary_files)
            {
                job->output_directory = persistent_output_directory();
                task_run(save_copies_work, copy_paths_done, job);
            }
            else task_post(copy_paths_done, job);
            return;
        case QUICK_ACTION_MOVE_TO:
            if (wcs_is_blank(job->request.folder))
            {
                quick_action_result_fail(&job->result, STR_MoveCancelled);
                job->result.cancelled = true;
                finish_soon(job);
                return;
            }
            task_run(move_work, job_finished, job);
            return;
        case QUICK_ACTION_REVEAL:
            if (shell_open_folder_and_select(&job->request.paths)) quick_action_result_set(&job->result, true, wcs_dup(S(STR_ShownInExplorer)));
            else quick_action_result_fail(&job->result, STR_CannotOpenFolder);
            finish_soon(job);
            return;
        default:
            show_properties(job->request.owner, &job->request.paths, &job->result);
            job->result.keeps_temporary_files = job->request.has_temporary_files;
            finish_soon(job);
            return;
    }
}
