// clipboard.h 的 Linux 实现：用 GdkClipboard 写文字和文件列表。
// 读剪贴板走 GdkDrop（粘贴）而不是 COM 数据对象，所以 clipboard_get_data 在 Linux 上不用。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "clipboard.h"

#include <gdk/gdk.h>
#include <string.h>

static GdkClipboard *system_clipboard(void)
{
    GdkDisplay *display = gdk_display_get_default();
    return display ? gdk_display_get_clipboard(display) : NULL;
}

bool clipboard_set_text(const wchar_t *text)
{
    GdkClipboard *clipboard = system_clipboard();
    if (!clipboard) return false;
    size_t length = 0;
    char *utf8 = utf8_from_wide(text ? text : L"", &length);
    if (!utf8) return false;
    gdk_clipboard_set_text(clipboard, utf8);
    free(utf8);
    return true;
}

typedef struct TextRead
{
    GMainLoop *loop;
    char *text;
} TextRead;

static void text_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    TextRead *read = data;
    read->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, NULL);
    g_main_loop_quit(read->loop);
}

wchar_t *clipboard_get_text(void)
{
    GdkClipboard *clipboard = system_clipboard();
    if (!clipboard) return NULL;
    TextRead read = { .loop = g_main_loop_new(NULL, FALSE) };
    gdk_clipboard_read_text_async(clipboard, NULL, text_ready, &read);
    g_main_loop_run(read.loop);
    g_main_loop_unref(read.loop);
    if (!read.text) return NULL;
    wchar_t *wide = wide_from_utf8(read.text, strlen(read.text));
    g_free(read.text);
    return wide;
}

bool clipboard_set_files(const StrList *paths)
{
    GdkClipboard *clipboard = system_clipboard();
    if (!clipboard || !paths || paths->count == 0) return false;
    GFile **files = g_new0(GFile *, (gsize)paths->count);
    char *text = NULL;
    size_t text_length = 0;
    WStr joined = {0};
    for (int index = 0; index < paths->count; index++)
    {
        size_t length = 0;
        char *utf8 = utf8_from_wide(paths->items[index], &length);
        if (!utf8) continue;
        files[index] = g_file_new_for_path(utf8);
        free(utf8);
        if (index > 0) wstr_append(&joined, L"\r\n");
        wstr_append(&joined, paths->items[index]);
    }
    text = utf8_from_wide(joined.data ? joined.data : L"", &text_length);
    wstr_free(&joined);
    GdkFileList *list = gdk_file_list_new_from_array(files, (gsize)paths->count);
    for (int index = 0; index < paths->count; index++)
        if (files[index]) g_object_unref(files[index]);
    g_free(files);
    if (!list)
    {
        free(text);
        return false;
    }
    GdkContentProvider *providers[2];
    int count = 0;
    providers[count++] = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list);
    GdkContentProvider *text_provider = NULL;
    if (text && text_length > 0) text_provider = gdk_content_provider_new_typed(G_TYPE_STRING, text);
    if (text_provider) providers[count++] = text_provider;
    GdkContentProvider *provider = count > 1 ? gdk_content_provider_new_union(providers, (gsize)count) : providers[0];
    if (count > 1)
    {
        for (int index = 0; index < count; index++) g_object_unref(providers[index]);
    }
    gdk_clipboard_set_content(clipboard, provider);
    g_object_unref(provider);
    free(text);
    return true;
}

IDataObject *clipboard_get_data(void)
{
    return NULL;
}