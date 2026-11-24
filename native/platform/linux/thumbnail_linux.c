// thumbnail.h 的 Linux 实现：用 gdk-pixbuf 在图片线程上解码并缩成缩略图。
// 视频缩略图以后可以接 freedesktop 的 thumbnailer；现在只处理 gdk-pixbuf 认得的格式。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "thumbnail.h"
#include "task.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct ThumbnailRequest
{
    wchar_t *path;
    int size;
    ThumbnailReady ready;
    void *context;
    GfxImage *image;
} ThumbnailRequest;

static GfxImage *scaled_image(const wchar_t *path, int size)
{
    size_t length = 0;
    char *utf8 = utf8_from_wide(path, &length);
    if (!utf8) return NULL;
    GError *error = NULL;
    int edge = size > 0 ? size : 128;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(utf8, edge, edge, TRUE, &error);
    free(utf8);
    if (!pixbuf)
    {
        if (error) g_error_free(error);
        return NULL;
    }
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
    bool alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    uint8_t *bgra = xalloc((size_t)width * (size_t)height * 4);
    for (int row = 0; row < height; row++)
    {
        const guchar *source = pixels + (size_t)row * stride;
        uint8_t *target = bgra + (size_t)row * width * 4;
        for (int column = 0; column < width; column++)
        {
            uint8_t red = source[0], green = source[1], blue = source[2];
            uint8_t alpha_value = alpha ? source[3] : 255;
            if (alpha_value != 255)
            {
                // cairo 的 ARGB32 是预乘过的。
                red = (uint8_t)((red * alpha_value + 127) / 255);
                green = (uint8_t)((green * alpha_value + 127) / 255);
                blue = (uint8_t)((blue * alpha_value + 127) / 255);
            }
            target[0] = blue;
            target[1] = green;
            target[2] = red;
            target[3] = alpha_value;
            source += channels;
            target += 4;
        }
    }
    g_object_unref(pixbuf);
    GfxImage *image = gfx_image_create(bgra, width, height, width * 4);
    free(bgra);
    return image;
}

// ───────────── 文件图标 ─────────────
// 图片可以给出真正的缩略图；别的文件没有统一的缩略图协议，用图标主题里按 MIME 类型找到的图标。
// GTK 的图标主题要在主线程上查，这里自己按 freedesktop 的目录规则找文件，后台线程也能跑。

static char *find_icon_in_theme(const char *theme, const char *name, const char *category, int size)
{
    static const int sizes[] = { 256, 128, 96, 64, 48, 32, 24, 16 };
    const char *data_dirs[16];
    int count = 0;
    data_dirs[count++] = g_get_user_data_dir();
    const char *const *system = g_get_system_data_dirs();
    for (int index = 0; system && system[index] && count < 15; index++) data_dirs[count++] = system[index];
    for (int index = 0; index < count; index++)
    {
        char *base = g_build_filename(data_dirs[index], "icons", theme, NULL);
        char *scalable = g_strdup_printf("%s/scalable/%s/%s.svg", base, category, name);
        if (g_file_test(scalable, G_FILE_TEST_IS_REGULAR))
        {
            g_free(base);
            return scalable;
        }
        g_free(scalable);
        char *candidate = NULL;
        bool found = false;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]) && !found; s++)
        {
            if (sizes[s] < size / 2) break;
            candidate = g_strdup_printf("%s/%dx%d/%s/%s.png", base, sizes[s], sizes[s], category, name);
            found = g_file_test(candidate, G_FILE_TEST_IS_REGULAR);
            if (!found)
            {
                g_free(candidate);
                candidate = NULL;
            }
        }
        g_free(base);
        if (candidate) return candidate;
    }
    return NULL;
}

static char *find_icon_file(const char *name, const char *category, int size)
{
    // 先找用户当前的图标主题（能读到 GTK 设置时），再是 Adwaita 和 hicolor。
    const char *themes[4] = { "Adwaita", "hicolor", NULL, NULL };
    char *configured = NULL;
    GSettings *settings = g_settings_new("org.gnome.desktop.interface");
    if (settings)
    {
        configured = g_settings_get_string(settings, "icon-theme");
        if (configured && *configured) themes[2] = configured;
        g_object_unref(settings);
    }
    char *found = NULL;
    for (int index = 0; index < 3 && !found; index++)
    {
        if (!themes[index]) continue;
        found = find_icon_in_theme(themes[index], name, category, size);
    }
    g_free(configured);
    return found;
}

static GfxImage *themed_icon(const wchar_t *path, int size)
{
    size_t length = 0;
    char *utf8 = utf8_from_wide(path, &length);
    if (!utf8) return NULL;
    bool folder = false;
    struct stat status;
    if (stat(utf8, &status) == 0) folder = S_ISDIR(status.st_mode);
    char *content_type = g_content_type_guess(utf8, NULL, 0, NULL);
    GIcon *icon = folder ? g_themed_icon_new("folder") : (content_type ? g_content_type_get_icon(content_type) : NULL);
    g_free(content_type);
    const char *category = folder ? "places" : "mimetypes";
    char *icon_path = NULL;
    if (icon)
    {
        if (G_IS_THEMED_ICON(icon))
        {
            const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
            for (int index = 0; names && names[index] && !icon_path; index++)
                icon_path = find_icon_file(names[index], category, size);
        }
        else if (G_IS_FILE_ICON(icon))
        {
            GFile *file = g_file_icon_get_file(G_FILE_ICON(icon));
            icon_path = g_file_get_path(file);
        }
        g_object_unref(icon);
    }
    if (!icon_path) icon_path = find_icon_file(folder ? "folder" : "text-x-generic", category, size);
    if (!icon_path) icon_path = find_icon_file("text-x-generic", "mimetypes", size);
    free(utf8);
    if (!icon_path) return NULL;
    GError *error = NULL;
    int edge = size > 0 ? size : 128;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(icon_path, edge, edge, TRUE, &error);
    g_free(icon_path);
    if (!pixbuf)
    {
        if (error) g_error_free(error);
        return NULL;
    }
    // 复用和缩略图相同的像素转换：RGBA → 预乘的 BGRA。
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
    bool alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    uint8_t *bgra = xalloc((size_t)width * (size_t)height * 4);
    for (int row = 0; row < height; row++)
    {
        const guchar *source = pixels + (size_t)row * stride;
        uint8_t *target = bgra + (size_t)row * width * 4;
        for (int column = 0; column < width; column++)
        {
            uint8_t red = source[0], green = source[1], blue = source[2];
            uint8_t alpha_value = alpha ? source[3] : 255;
            if (alpha_value != 255)
            {
                red = (uint8_t)((red * alpha_value + 127) / 255);
                green = (uint8_t)((green * alpha_value + 127) / 255);
                blue = (uint8_t)((blue * alpha_value + 127) / 255);
            }
            target[0] = blue;
            target[1] = green;
            target[2] = red;
            target[3] = alpha_value;
            source += channels;
            target += 4;
        }
    }
    g_object_unref(pixbuf);
    GfxImage *image = gfx_image_create(bgra, width, height, width * 4);
    free(bgra);
    return image;
}

static void work(void *data)
{
    ThumbnailRequest *request = data;
    request->image = scaled_image(request->path, request->size);
    if (!request->image) request->image = themed_icon(request->path, request->size);
}

static void finish(void *data)
{
    ThumbnailRequest *request = data;
    if (request->ready) request->ready(request->image, request->context);
    else gfx_image_release(request->image);
    free(request->path);
    free(request);
}

void thumbnail_load(const wchar_t *path, int size, ThumbnailReady ready, void *context)
{
    if (!path || !*path) return;
    ThumbnailRequest *request = xcalloc(1, sizeof(*request));
    request->path = wcs_dup(path);
    request->size = size;
    request->ready = ready;
    request->context = context;
    task_run_imaging(work, finish, request);
}