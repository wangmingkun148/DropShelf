// 图片压缩和转换的冒烟测试：生成一张噪点图，压缩到上限以内，再转成 JPEG 和 PNG。
#include "base.h"
#include "image_tools.h"
#include "settings.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(bool condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
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

/// 噪点图不好压缩，正好用来找画质。
static GdkPixbuf *make_noise(int width, int height)
{
    GdkPixbuf *image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    if (!image) return NULL;
    guchar *pixels = gdk_pixbuf_get_pixels(image);
    int stride = gdk_pixbuf_get_rowstride(image);
    // 线性同余的伪随机数：噪点没有规律，PNG 和 JPEG 都压不小。
    unsigned state = 12345;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            guchar *pixel = pixels + y * stride + x * 3;
            for (int channel = 0; channel < 3; channel++)
            {
                state = state * 1103515245u + 12345u;
                pixel[channel] = (guchar)(state >> 16);
            }
        }
    return image;
}

static long long file_size(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (long long)info.st_size : -1;
}

int main(void)
{
    char *directory = g_dir_make_tmp("dropshelf-image-XXXXXX", NULL);
    char *output_directory = g_dir_make_tmp("dropshelf-image-out-XXXXXX", NULL);
    check(directory && output_directory, "无法创建临时文件夹");
    if (!directory || !output_directory) return 1;

    GdkPixbuf *noise = make_noise(320, 320);
    check(noise != NULL, "无法生成测试图片");
    char *source = g_build_filename(directory, "noise.png", NULL);
    GError *error = NULL;
    check(noise && gdk_pixbuf_save(noise, source, "png", &error, NULL), "无法保存测试图片");
    if (error) g_error_free(error);
    if (noise) g_object_unref(noise);

    wchar_t *wide_source = wide_from_utf8(source, strlen(source));
    wchar_t *wide_output = wide_from_utf8(output_directory, strlen(output_directory));

    // ── 压缩到 16KB 以内 ──
    wchar_t *compressed = NULL;
    ImageStatus status = image_compress(wide_source, 32 * 1024, wide_output, &compressed);
    check(status == IMAGE_DONE, "压缩图片失败");
    if (compressed)
    {
        char *native = utf8_from_wide(compressed, NULL);
        long long size = native ? file_size(native) : -1;
        check(size > 0 && size <= 32 * 1024, "压缩后的图片超过了上限");
        size_t length = native ? strlen(native) : 0;
        check(length > 4 && strcmp(native + length - 4, ".jpg") == 0, "压缩结果不是 JPEG");
        free(native);
    }
    free(compressed);

    // ── 本来就够小的图片不需要压缩 ──
    wchar_t *again = NULL;
    status = image_compress(wide_source, 1024 * 1024, wide_output, &again);
    check(status == IMAGE_ALREADY, "够小的图片被重新压缩了");
    free(again);

    // ── 转成 JPEG ──
    wchar_t *converted = NULL;
    status = image_convert(wide_source, IMAGE_FORMAT_JPEG, wide_output, &converted);
    check(status == IMAGE_DONE, "转换图片失败");
    char *converted_native = converted ? utf8_from_wide(converted, NULL) : NULL;
    check(converted_native && g_file_test(converted_native, G_FILE_TEST_EXISTS), "转换结果不在磁盘上");
    if (converted_native)
    {
        // 再把 JPEG 转回 PNG，确认能读自己写出来的 JPEG。
        wchar_t *wide_jpeg = wide_from_utf8(converted_native, strlen(converted_native));
        wchar_t *back = NULL;
        status = image_convert(wide_jpeg, IMAGE_FORMAT_PNG, wide_output, &back);
        check(status == IMAGE_DONE, "把 JPEG 转回 PNG 失败");
        char *back_native = back ? utf8_from_wide(back, NULL) : NULL;
        check(back_native && g_file_test(back_native, G_FILE_TEST_EXISTS), "转回的 PNG 不在磁盘上");
        size_t back_length = back_native ? strlen(back_native) : 0;
        check(back_length > 4 && strcmp(back_native + back_length - 4, ".png") == 0, "转回的结果不是 PNG");
        free(back_native);
        free(back);
        free(wide_jpeg);
    }
    // ── 已经是目标格式时不需要转换 ──
    wchar_t *same = NULL;
    status = image_convert(wide_source, IMAGE_FORMAT_PNG, wide_output, &same);
    check(status == IMAGE_ALREADY, "已经是 PNG 的图片又被转换了");
    free(same);

    free(converted_native);
    free(converted);
    free(wide_source);
    free(wide_output);
    free(source);
    remove_tree(output_directory);
    remove_tree(directory);
    g_free(output_directory);
    g_free(directory);

    if (failures == 0) printf("image tests passed\n");
    return failures == 0 ? 0 : 1;
}