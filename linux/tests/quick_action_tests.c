// 快速操作的文件测试：移动到文件夹、复制到文件夹，以及目标重名时换名字。
#include "base.h"
#include "file_ops.h"
#include "quick_action.h"
#include "task.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;

static void check(bool condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(text, 1, strlen(text), file);
    fclose(file);
}

static bool same_text(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char text[64];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    text[length] = 0;
    fclose(file);
    return strcmp(text, expected) == 0;
}

static void remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (S_ISDIR(info.st_mode))
    {
        GDir *directory = g_dir_open(path, 0, NULL);
        if (directory)
        {
            const char *name;
            while ((name = g_dir_read_name(directory)) != NULL)
            {
                char *child = g_build_filename(path, name, NULL);
                remove_tree(child);
                g_free(child);
            }
            g_dir_close(directory);
        }
        rmdir(path);
        return;
    }
    unlink(path);
}

typedef struct Captured
{
    bool succeeded;
    int outputs;
    int moves;
    int replaced;
    wchar_t *message;
} Captured;

static GMainLoop *running_loop;

static void on_done(QuickActionResult *result, void *context)
{
    Captured *captured = context;
    captured->succeeded = result->succeeded;
    captured->outputs = result->outputs.count;
    captured->moves = result->move_sources.count;
    captured->replaced = result->replaced_sources.count;
    captured->message = wcs_dup(result->message);
    if (running_loop) g_main_loop_quit(running_loop);
}

static wchar_t *wide(const char *text)
{
    return wide_from_utf8(text, strlen(text));
}

int main(void)
{
    char *source = g_dir_make_tmp("dropshelf-qa-src-XXXXXX", NULL);
    char *target = g_dir_make_tmp("dropshelf-qa-dst-XXXXXX", NULL);
    char *other_source = g_dir_make_tmp("dropshelf-qa-src2-XXXXXX", NULL);
    char *other_target = g_dir_make_tmp("dropshelf-qa-dst2-XXXXXX", NULL);
    check(source && target && other_source && other_target, "无法创建临时文件夹");
    if (!source || !target || !other_source || !other_target) return 1;

    // ── 移动到已有同名文件的文件夹 ──
    char *file = g_build_filename(source, "a.txt", NULL);
    char *existing = g_build_filename(target, "a.txt", NULL);
    write_file(file, "new");
    write_file(existing, "old");
    wchar_t *wide_file = wide(file);
    wchar_t *wide_target = wide(target);
    StrList paths = {0};
    strlist_add(&paths, wide_file);
    QuickActionRequest request = {0};
    request.action.type = QUICK_ACTION_MOVE_TO;
    request.paths = paths;
    request.folder = wide_target;
    Captured moved = {0};
    quick_action_execute(&request, on_done, &moved);
    paths = (StrList){0};   // 列表已经交给 quick_action_execute
    check(moved.succeeded, "移动失败");
    check(moved.outputs == 1 && moved.moves == 1, "移动结果的数量不对");
    check(!g_file_test(file, G_FILE_TEST_EXISTS), "移动后源文件还在");
    check(same_text(existing, "old"), "移动把已有的同名文件覆盖了");
    char *renamed = g_build_filename(target, "a (2).txt", NULL);
    check(same_text(renamed, "new"), "移动后的文件不在新名字下");
    free(moved.message);

    // ── 已经在目标文件夹里的项目不再移动 ──
    StrList already = {0};
    wchar_t *wide_renamed = wide(renamed);
    strlist_add(&already, wide_renamed);
    QuickActionRequest again = {0};
    again.action.type = QUICK_ACTION_MOVE_TO;
    again.paths = already;
    again.folder = wide(target);   // 上一个请求已经释放了自己的副本
    Captured skipped = {0};
    quick_action_execute(&again, on_done, &skipped);
    already = (StrList){0};
    check(skipped.succeeded && skipped.outputs == 0 && skipped.moves == 0, "已在文件夹里的项目被重复移动了");
    free(skipped.message);

    // ── 自定义操作：复制到文件夹 ──
    char *copy_source = g_build_filename(other_source, "b.txt", NULL);
    write_file(copy_source, "copy");
    wchar_t *wide_copy_source = wide(copy_source);
    wchar_t *wide_other_target = wide(other_target);
    StrList copy_paths = {0};
    strlist_add(&copy_paths, wide_copy_source);
    QuickActionRequest copy = {0};
    copy.action.is_custom = true;
    copy.action.custom.kind = CUSTOM_ACTION_FOLDER;
    copy.action.custom.folder_mode = TRANSFER_COPY;
    copy.action.custom.folder = wcs_dup(wide_other_target);
    copy.paths = copy_paths;
    Captured copied = {0};
    quick_action_execute(&copy, on_done, &copied);
    copy_paths = (StrList){0};
    check(copied.succeeded, "复制失败");
    check(g_file_test(copy_source, G_FILE_TEST_EXISTS), "复制后源文件不见了");
    char *copied_file = g_build_filename(other_target, "b.txt", NULL);
    check(same_text(copied_file, "copy"), "复制出来的文件内容不对");
    free(copied.message);

    // ── 自定义操作：压缩图片 ──
    char *photo = g_build_filename(other_source, "photo.png", NULL);
    GdkPixbuf *noise = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 200, 200);
    check(noise != NULL, "无法生成测试图片");
    if (noise)
    {
        guchar *pixels = gdk_pixbuf_get_pixels(noise);
        int stride = gdk_pixbuf_get_rowstride(noise);
        unsigned state = 54321;
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < 200; x++)
            {
                guchar *pixel = pixels + y * stride + x * 3;
                for (int channel = 0; channel < 3; channel++)
                {
                    state = state * 1103515245u + 12345u;
                    pixel[channel] = (guchar)(state >> 16);
                }
            }
        GError *save_error = NULL;
        check(gdk_pixbuf_save(noise, photo, "png", &save_error, NULL), "无法保存测试图片");
        if (save_error) g_error_free(save_error);
        g_object_unref(noise);
    }
    wchar_t *wide_photo = wide(photo);
    StrList photo_paths = {0};
    strlist_add(&photo_paths, wide_photo);
    QuickActionRequest compress_image = {0};
    compress_image.action.is_custom = true;
    compress_image.action.custom.kind = CUSTOM_ACTION_COMPRESS_IMAGE;
    compress_image.action.custom.image_size_kb = 32;
    compress_image.paths = photo_paths;
    Captured compressed = {0};
    quick_action_execute(&compress_image, on_done, &compressed);
    photo_paths = (StrList){0};
    check(compressed.succeeded, "压缩图片的快速操作失败");
    check(compressed.outputs == 1 && compressed.replaced == 1, "压缩图片没有生成替换项目");
    free(compressed.message);
    free(wide_photo);
    free(photo);

    // ── 有后台线程池时，结果回到主循环里 ──
    task_initialize();
    char *async_file = g_build_filename(other_source, "async.txt", NULL);
    write_file(async_file, "async");
    wchar_t *wide_async = wide(async_file);
    StrList async_paths = {0};
    strlist_add(&async_paths, wide_async);
    QuickActionRequest async_copy = {0};
    async_copy.action.is_custom = true;
    async_copy.action.custom.kind = CUSTOM_ACTION_FOLDER;
    async_copy.action.custom.folder_mode = TRANSFER_COPY;
    async_copy.action.custom.folder = wide(other_target);
    async_copy.paths = async_paths;
    Captured async = {0};
    running_loop = g_main_loop_new(NULL, FALSE);
    quick_action_execute(&async_copy, on_done, &async);
    async_paths = (StrList){0};
    g_main_loop_run(running_loop);
    g_main_loop_unref(running_loop);
    running_loop = NULL;
    task_shutdown();
    check(async.succeeded && async.outputs == 1, "后台执行的结果没有回到主循环");
    char *async_copy_path = g_build_filename(other_target, "async.txt", NULL);
    check(same_text(async_copy_path, "async"), "后台复制出来的文件不对");
    free(async.message);
    free(async_copy_path);
    free(wide_async);
    free(async_file);

    free(renamed);
    free(existing);
    free(file);
    free(wide_file);
    // wide_target 已经交给 quick_action_execute，由它释放。
    free(wide_copy_source);
    free(wide_other_target);
    free(copy_source);
    free(copied_file);
    free(wide_renamed);
    strlist_clear(&paths);
    strlist_clear(&already);
    strlist_clear(&copy_paths);
    remove_tree(other_target);
    remove_tree(other_source);
    remove_tree(target);
    remove_tree(source);
    g_free(source);
    g_free(target);
    g_free(other_source);
    g_free(other_target);

    if (failures == 0) printf("quick action tests passed\n");
    return failures == 0 ? 0 : 1;
}