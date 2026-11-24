// 系统的打开、保存和选择文件夹对话框：GTK 的 FileChooserNative，在 Wayland 上由桌面环境的
// 文件选择器（xdg-desktop-portal）提供。filter 写法和 Windows 版一样："程序|*.exe;*.lnk|所有文件|*.*"。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "file_dialog.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

typedef struct DialogWait
{
    GMainLoop *loop;
    GFile *file;
} DialogWait;

static void on_response(GtkNativeDialog *dialog, int response, gpointer data)
{
    DialogWait *wait = data;
    if (response == GTK_RESPONSE_ACCEPT)
    {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        if (file) wait->file = file;
    }
    g_main_loop_quit(wait->loop);
}

/// 跑一次对话框，拿到选中的文件；取消时返回 NULL。
static wchar_t *run_dialog(GtkFileChooserNative *dialog)
{
    DialogWait wait = { .loop = g_main_loop_new(NULL, FALSE) };
    g_signal_connect(dialog, "response", G_CALLBACK(on_response), &wait);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
    g_main_loop_run(wait.loop);
    g_main_loop_unref(wait.loop);
    if (!wait.file) return NULL;
    char *path = g_file_get_path(wait.file);
    g_object_unref(wait.file);
    if (!path) return NULL;
    wchar_t *wide = wide_from_utf8(path, strlen(path));
    g_free(path);
    return wide;
}

static void apply_title(GtkNativeDialog *dialog, const wchar_t *title)
{
    if (!title)
    {
        return;
    }
    char *narrow = utf8_from_wide(title, NULL);
    if (narrow)
    {
        gtk_native_dialog_set_title(dialog, narrow);
        free(narrow);
    }
}

static void apply_directory(GtkFileChooserNative *dialog, const wchar_t *directory)
{
    if (wcs_is_blank(directory) || !path_is_directory(directory)) return;
    char *narrow = utf8_from_wide(directory, NULL);
    if (!narrow) return;
    GFile *folder = g_file_new_for_path(narrow);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder, NULL);
    g_object_unref(folder);
    free(narrow);
}

/// 加一组筛选条件；`*.*` 在 GTK 里写成 `*`。
static void apply_filter(GtkFileChooserNative *dialog, const wchar_t *filter)
{
    if (!filter || !*filter) return;
    char *narrow = utf8_from_wide(filter, NULL);
    if (!narrow) return;
    char **parts = g_strsplit(narrow, "|", 0);
    for (int index = 0; parts[index] && parts[index + 1]; index += 2)
    {
        GtkFileFilter *one = gtk_file_filter_new();
        gtk_file_filter_set_name(one, parts[index]);
        char **patterns = g_strsplit(parts[index + 1], ";", 0);
        for (int each = 0; patterns[each]; each++)
        {
            const char *pattern = patterns[each];
            if (*pattern == 0) continue;
            gtk_file_filter_add_pattern(one, strcmp(pattern, "*.*") == 0 ? "*" : pattern);
        }
        g_strfreev(patterns);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), one);
        g_object_unref(one);
    }
    g_strfreev(parts);
    free(narrow);
}

/// 选中的路径没有扩展名时补上默认扩展名（Windows 对话框会自动补）。
static wchar_t *with_extension(wchar_t *path, const wchar_t *extension)
{
    if (!path || wcs_is_blank(extension)) return path;
    const wchar_t *name = path_file_name(path);
    if (name && wcschr(name, L'.')) return path;
    WStr joined = {0};
    wstr_appendf(&joined, L"%ls.%ls", path, extension);
    free(path);
    wchar_t *result = wcs_dup(joined.data);
    wstr_free(&joined);
    return result;
}

wchar_t *file_dialog_open(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *initial_directory)
{
    (void)owner;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new("打开", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "打开", "取消");
    apply_title(GTK_NATIVE_DIALOG(dialog), title);
    apply_filter(dialog, filter);
    apply_directory(dialog, initial_directory);
    wchar_t *result = run_dialog(dialog);
    g_object_unref(dialog);
    return result;
}

wchar_t *file_dialog_folder(HWND owner, const wchar_t *title, const wchar_t *initial_directory)
{
    (void)owner;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new("选择文件夹", NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "选择", "取消");
    apply_title(GTK_NATIVE_DIALOG(dialog), title);
    apply_directory(dialog, initial_directory);
    wchar_t *result = run_dialog(dialog);
    g_object_unref(dialog);
    return result;
}

wchar_t *file_dialog_save(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *file_name,
    const wchar_t *initial_directory, const wchar_t *default_extension)
{
    (void)owner;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new("保存", NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "保存", "取消");
    apply_title(GTK_NATIVE_DIALOG(dialog), title);
    apply_filter(dialog, filter);
    apply_directory(dialog, initial_directory);
    if (!wcs_is_blank(file_name))
    {
        char *narrow = utf8_from_wide(file_name, NULL);
        if (narrow)
        {
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), narrow);
            free(narrow);
        }
    }
    wchar_t *result = run_dialog(dialog);
    g_object_unref(dialog);
    return with_extension(result, default_extension);
}