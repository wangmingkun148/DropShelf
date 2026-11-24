// 文件属性窗口。Linux 没有系统提供的属性对话框，这里用程序自绘的元素树显示名称、类型、位置、大小和修改时间。
// 文件夹的总大小在后台线程里统计，算完再更新窗口；窗口先关掉时结果直接丢掉。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "properties_linux.h"
#include "base.h"
#include "file_ops.h"
#include "quick_action.h"
#include "strings.h"
#include "task.h"
#include "theme.h"
#include "ui.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <wctype.h>

#define WINDOW_WIDTH 460.0f
#define CAPTION_HEIGHT 40.0f

static UiWindow *window;
static UiElement *content, *close_button;
static UiElement *size_value, *modified_value;
static int generation;

// ───────────── 显示用的文字 ─────────────

static wchar_t *format_size(long long bytes)
{
    wchar_t text[64];
    if (bytes < 1024) swprintf(text, 64, L"%lld B", bytes);
    else if (bytes < 1024LL * 1024) swprintf(text, 64, L"%lld.%lld KB", bytes / 1024, bytes % 1024 * 10 / 1024);
    else if (bytes < 1024LL * 1024 * 1024) swprintf(text, 64, L"%lld.%lld MB", bytes / (1024 * 1024), bytes % (1024 * 1024) * 10 / (1024 * 1024));
    else swprintf(text, 64, L"%lld.%lld GB", bytes / (1024LL * 1024 * 1024), bytes % (1024LL * 1024 * 1024) * 10 / (1024LL * 1024 * 1024));
    return wcs_dup(text);
}

static wchar_t *format_time(time_t time)
{
    struct tm parts;
    localtime_r(&time, &parts);
    wchar_t text[64];
    swprintf(text, 64, L"%04d-%02d-%02d %02d:%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
    return wcs_dup(text);
}

/// 扩展名转成显示的类型：PDF、PNG；没有扩展名时就是“文件”。
static wchar_t *type_text(const wchar_t *path)
{
    if (path_is_directory(path)) return wcs_dup(S(STR_PropertiesFolder));
    const wchar_t *dot = wcsrchr(path_file_name(path), L'.');
    if (!dot || !dot[1]) return wcs_dup(S(STR_PropertiesFile));
    wchar_t extension[16];
    size_t length = wcslen(dot + 1);
    if (length >= sizeof(extension) / sizeof(extension[0])) length = sizeof(extension) / sizeof(extension[0]) - 1;
    for (size_t index = 0; index < length; index++) extension[index] = (wchar_t)towupper((wint_t)dot[index + 1]);
    extension[length] = 0;
    return str_file_type(extension);
}

/// 所有项目都在同一个文件夹里时返回它，否则返回 NULL（表示“多个位置”）。
static wchar_t *common_folder(const StrList *paths)
{
    if (paths->count == 0) return NULL;
    wchar_t *first = path_directory(paths->items[0]);
    for (int index = 1; first && index < paths->count; index++)
    {
        wchar_t *other = path_directory(paths->items[index]);
        bool same = other && wcscmp(first, other) == 0;
        free(other);
        if (!same)
        {
            free(first);
            return NULL;
        }
    }
    return first;
}

// ───────────── 大小和修改时间 ─────────────

/// 递归累加大小；符号链接只算链接本身，不跟进去。修改时间取其中最新的。
static void sum_path(const wchar_t *path, long long *total, time_t *latest, bool *seen)
{
    char *native = utf8_from_wide(path, NULL);
    if (!native) return;
    struct stat info;
    if (lstat(native, &info) == 0)
    {
        if (!*seen || info.st_mtime > *latest)
        {
            *latest = info.st_mtime;
            *seen = true;
        }
        if (S_ISDIR(info.st_mode))
        {
            GDir *directory = g_dir_open(native, 0, NULL);
            if (directory)
            {
                const char *name;
                while ((name = g_dir_read_name(directory)) != NULL)
                {
                    wchar_t *wide_name = wide_from_utf8(name, strlen(name));
                    if (!wide_name) continue;
                    wchar_t *child = path_join(path, wide_name);
                    sum_path(child, total, latest, seen);
                    free(child);
                    free(wide_name);
                }
                g_dir_close(directory);
            }
        }
        else *total += (long long)info.st_size;
    }
    free(native);
}

typedef struct SizeJob
{
    StrList paths;
    int generation;
    long long total;
    time_t latest;
    bool seen;
} SizeJob;

static void size_work(void *data)
{
    SizeJob *job = data;
    for (int index = 0; index < job->paths.count; index++) sum_path(job->paths.items[index], &job->total, &job->latest, &job->seen);
}

static void size_done(void *data)
{
    SizeJob *job = data;
    if (job->generation == generation && window)
    {
        wchar_t *size = format_size(job->total);
        ui_label_set_text(size_value, UI_LIT(size));
        free(size);
        if (job->seen)
        {
            wchar_t *time = format_time(job->latest);
            ui_label_set_text(modified_value, UI_LIT(time));
            free(time);
        }
    }
    strlist_clear(&job->paths);
    free(job);
}

// ───────────── 界面 ─────────────

static void on_close(void *context)
{
    (void)context;
    if (window) ui_window_close(window);
}

static void on_closed(UiWindow *closed, void *context)
{
    (void)closed;
    (void)context;
    window = NULL;
    content = close_button = size_value = modified_value = NULL;
    generation++;
}

static bool on_key(UiWindow *current, void *context, unsigned key)
{
    (void)current;
    (void)context;
    if (key != VK_ESCAPE) return false;
    on_close(NULL);
    return true;
}

static UiElement *add_row(UiElement *grid, StringId label, const wchar_t *value)
{
    ui_add(grid, ui_label(UI_STR(label), UI_TEXT_CAPTION));
    UiElement *text = ui_label(UI_LIT(value ? value : L""), UI_TEXT_BODY);
    GfxTextStyle *style = ui_label_style(text);
    style->wrap = false;
    style->ellipsis = true;
    ui_add(grid, text);
    return text;
}

static UiElement *build_content(const StrList *paths)
{
    content = ui_stack(false, 0);
    ui_stack_set_padding(content, 24, 44, 24, 20);

    wchar_t *heading = paths->count == 1 ? wcs_dup(path_file_name(paths->items[0])) : str_item_count(paths->count);
    UiElement *title = ui_label(UI_LIT(heading), UI_TEXT_SUBTITLE);
    ui_label_set_size(title, 18);
    GfxTextStyle *title_style = ui_label_style(title);
    title_style->wrap = false;
    title_style->ellipsis = true;
    ui_add(content, title);
    free(heading);

    UiElement *grid = ui_grid(2, 16, 10);
    ui_set_margin(grid, 0, 14, 0, 20);
    if (paths->count == 1)
    {
        wchar_t *type = type_text(paths->items[0]);
        add_row(grid, STR_PropertiesType, type);
        free(type);
    }
    wchar_t *folder = common_folder(paths);
    add_row(grid, STR_PropertiesLocation, folder ? folder : S(STR_PropertiesMultipleLocations));
    free(folder);
    size_value = add_row(grid, STR_PropertiesSize, S(STR_PropertiesCalculating));
    modified_value = add_row(grid, STR_PropertiesModified, S(STR_PropertiesCalculating));
    ui_add(content, grid);

    UiElement *buttons = ui_stack(true, 8);
    buttons->cross = UI_CROSS_CENTER;
    ui_add(buttons, ui_spacer());
    ui_add(buttons, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_CaptionClose), on_close, NULL));
    ui_add(content, buttons);
    return content;
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

static const UiElementType root_type = { .measure = root_measure, .arrange = root_arrange };

// ───────────── 对外接口 ─────────────

bool properties_show(HWND owner, const StrList *paths, QuickActionResult *result)
{
    (void)owner;
    StrList available = {0};
    for (int index = 0; index < paths->count; index++)
        if (path_is_available(paths->items[index])) strlist_add(&available, paths->items[index]);
    if (available.count == 0)
    {
        quick_action_result_fail(result, STR_NoFilesForProperties);
        return false;
    }
    if (window)
    {
        // 已经开着：带到前面就行，不重新统计。
        ui_window_show(window);
        int count = available.count;
        strlist_clear(&available);
        wchar_t *message = count > 1 ? str_opened_properties_for(count) : wcs_dup(S(STR_OpenedProperties));
        quick_action_result_set(result, true, message);
        return true;
    }

    generation++;
    UiWindowOptions options = { 0 };
    options.title = S(STR_ActionProperties);
    options.width = WINDOW_WIDTH;
    options.height = 320;
    options.resizable = false;
    options.backdrop = UI_BACKDROP_MICA;
    options.background = THEME_WINDOW_BACKGROUND;
    options.caption_height = CAPTION_HEIGHT;
    options.icon = NULL;
    options.closed = on_closed;
    options.key = on_key;
    window = ui_window_create(&options);
    if (!window)
    {
        strlist_clear(&available);
        quick_action_result_fail(result, STR_PropertiesUnavailable);
        return false;
    }
    UiElement *root = ui_element_new(&root_type, sizeof(UiElement));
    ui_add(root, build_content(&available));
    close_button = ui_button(UI_BUTTON_CAPTION_CLOSE, UI_LIT(L"\xE711"), on_close, NULL);
    ui_set_tooltip(close_button, UI_STR(STR_CaptionClose));
    ui_add(root, close_button);
    ui_window_set_root(window, root);
    float height = ui_measure(root, WINDOW_WIDTH).height;
    ui_window_place(window, 0, 0, WINDOW_WIDTH, height);
    ui_window_center(window);
    ui_window_show(window);

    SizeJob *job = xcalloc(1, sizeof(SizeJob));
    strlist_clone(&job->paths, &available);
    job->generation = generation;
    task_run(size_work, size_done, job);

    int count = available.count;
    strlist_clear(&available);
    wchar_t *message = count > 1 ? str_opened_properties_for(count) : wcs_dup(S(STR_OpenedProperties));
    quick_action_result_set(result, true, message);
    return true;
}