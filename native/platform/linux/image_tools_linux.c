// image_tools.h 的 Linux 实现：用 GdkPixbuf 读图，JPEG 画质搜索用共享的 image_quality.c。
// 和 Windows 版一样同步执行，从后台线程调用；原图不动，结果另存为新文件。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "image_tools.h"
#include "file_ops.h"
#include "image_quality.h"
#include "log.h"
#include "settings.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXIMUM_QUALITY 92
#define MINIMUM_QUALITY 40
#define MINIMUM_EDGE 32
#define COMPRESS_MAXIMUM_PIXELS 40000000ll
#define CONVERT_JPEG_QUALITY 92
#define CONVERT_MAXIMUM_PIXELS 100000000ll

static const wchar_t *const image_extensions[] = {
    L".jpg", L".jpeg", L".jfif", L".png", L".bmp", L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif", L".avif"
};

static const wchar_t *extension_of(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path_file_name(path), L'.');
    return dot && dot[1] ? dot : L"";
}

bool image_is_path(const wchar_t *path)
{
    const wchar_t *extension = extension_of(path);
    for (size_t index = 0; index < sizeof(image_extensions) / sizeof(image_extensions[0]); index++)
        if (_wcsicmp(extension, image_extensions[index]) == 0) return true;
    return false;
}

static bool widely_supported(const wchar_t *path)
{
    const wchar_t *extension = extension_of(path);
    return _wcsicmp(extension, L".jpg") == 0 || _wcsicmp(extension, L".jpeg") == 0 || _wcsicmp(extension, L".png") == 0;
}

static bool is_format(const wchar_t *path, int format)
{
    const wchar_t *extension = extension_of(path);
    if (format == IMAGE_FORMAT_PNG) return _wcsicmp(extension, L".png") == 0;
    return _wcsicmp(extension, L".jpg") == 0 || _wcsicmp(extension, L".jpeg") == 0 || _wcsicmp(extension, L".jfif") == 0;
}

wchar_t *image_format_limit(long long maximum_bytes)
{
    long long kilobytes = maximum_bytes / 1024;
    wchar_t text[32];
    if (kilobytes >= 1024 && kilobytes % 1024 == 0) swprintf(text, 32, L"%lldMB", kilobytes / 1024);
    else swprintf(text, 32, L"%lldKB", kilobytes);
    return wcs_dup(text);
}

// ───────────── 读取 ─────────────

static bool is_file(const wchar_t *path)
{
    char *native = utf8_from_wide(path, NULL);
    if (!native) return false;
    GStatBuf info;
    bool ok = g_stat(native, &info) == 0 && S_ISREG(info.st_mode);
    free(native);
    return ok;
}

static bool file_size_of(const wchar_t *path, long long *size)
{
    char *native = utf8_from_wide(path, NULL);
    if (!native) return false;
    GStatBuf info;
    bool ok = g_stat(native, &info) == 0;
    if (ok) *size = (long long)info.st_size;
    free(native);
    return ok;
}

/// 读入图片，按 EXIF 方向转正；像素太多时在解码时缩小。
static GdkPixbuf *load_image(const wchar_t *source, long long maximum_pixels)
{
    char *native = utf8_from_wide(source, NULL);
    if (!native) return NULL;
    GError *error = NULL;
    GdkPixbuf *image = NULL;
    int width = 0, height = 0;
    if (gdk_pixbuf_get_file_info(native, &width, &height) && width > 0 && height > 0 &&
        (long long)width * height > maximum_pixels)
    {
        double ratio = sqrt((double)maximum_pixels / ((double)width * (double)height));
        int scaled_width = (int)(width * ratio), scaled_height = (int)(height * ratio);
        if (scaled_width < 1) scaled_width = 1;
        if (scaled_height < 1) scaled_height = 1;
        image = gdk_pixbuf_new_from_file_at_scale(native, scaled_width, scaled_height, TRUE, &error);
    }
    else image = gdk_pixbuf_new_from_file(native, &error);
    free(native);
    if (!image)
    {
        if (error) g_error_free(error);
        return NULL;
    }
    GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(image);
    if (upright)
    {
        g_object_unref(image);
        image = upright;
    }
    return image;
}

/// JPEG 不支持透明：先铺一层白底。
static GdkPixbuf *flatten_for_jpeg(GdkPixbuf *image)
{
    if (!gdk_pixbuf_get_has_alpha(image)) return g_object_ref(image);
    int width = gdk_pixbuf_get_width(image), height = gdk_pixbuf_get_height(image);
    GdkPixbuf *flat = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    if (!flat) return NULL;
    gdk_pixbuf_fill(flat, 0xFFFFFFFFu);
    gdk_pixbuf_composite(image, flat, 0, 0, width, height, 0, 0, 1, 1, GDK_INTERP_NEAREST, 255);
    return flat;
}

// ───────────── 编码 ─────────────

/// 编码成 JPEG（quality 1–100）或 PNG（quality < 0）。返回 g_malloc 的字节，调用方 g_free。
static guchar *encode_image(GdkPixbuf *image, int quality, gsize *size)
{
    *size = 0;
    guchar *buffer = NULL;
    GError *error = NULL;
    gboolean ok;
    if (quality < 0) ok = gdk_pixbuf_save_to_bufferv(image, (gchar **)&buffer, size, "png", NULL, NULL, &error);
    else
    {
        GdkPixbuf *flat = flatten_for_jpeg(image);
        if (!flat) return NULL;
        char quality_text[16];
        snprintf(quality_text, sizeof(quality_text), "%d", quality);
        char *keys[] = { "quality", NULL };
        char *values[] = { quality_text, NULL };
        ok = gdk_pixbuf_save_to_bufferv(flat, (gchar **)&buffer, size, "jpeg", keys, values, &error);
        g_object_unref(flat);
    }
    if (!ok)
    {
        if (error) g_error_free(error);
        g_free(buffer);
        return NULL;
    }
    return buffer;
}

typedef struct QualityProbe
{
    GdkPixbuf *image;
    long long maximum;
    guchar *best;           // 放得下的留在 best 里
    gsize best_size;
    guchar *candidate;      // 新的一次写到 candidate
    gsize candidate_size;
} QualityProbe;

static long long probe_quality(void *context, int quality)
{
    QualityProbe *probe = context;
    g_free(probe->candidate);
    probe->candidate = encode_image(probe->image, quality, &probe->candidate_size);
    if (!probe->candidate) return -1;
    if ((long long)probe->candidate_size <= probe->maximum)
    {
        guchar *swap = probe->best;
        gsize swap_size = probe->best_size;
        probe->best = probe->candidate;
        probe->best_size = probe->candidate_size;
        probe->candidate = swap;
        probe->candidate_size = swap_size;
    }
    return (long long)probe->candidate_size;
}

/// 找出不超过上限的最高画质；最低画质仍然过大时按比例缩小后重试。和 Windows 版同样的策略。
static guchar *encode_within_limit(GdkPixbuf *image, long long maximum_bytes, gsize *result_size)
{
    GdkPixbuf *current = g_object_ref(image);
    guchar *found = NULL;
    double steepness = 0;
    for (int round = 0; round < 10 && !found; round++)
    {
        long long top_size = 0;
        guchar *top = NULL;
        gsize top_length = 0;
        if (round == 0)
        {
            top = encode_image(current, MAXIMUM_QUALITY, &top_length);
            if (!top) break;
            top_size = (long long)top_length;
            if (top_size <= maximum_bytes)
            {
                found = top;
                *result_size = top_length;
                break;
            }
            g_free(top);
        }
        gsize low_length = 0;
        guchar *low = encode_image(current, MINIMUM_QUALITY, &low_length);
        if (!low) break;
        if ((long long)low_length <= maximum_bytes)
        {
            int high = round == 0 ? MAXIMUM_QUALITY : MAXIMUM_QUALITY + 1;
            long long high_size = round == 0 ? top_size : (long long)((double)low_length * steepness);
            QualityProbe probe = { current, maximum_bytes, low, low_length, NULL, 0 };
            if (image_quality_search(MINIMUM_QUALITY, (long long)low_length, high, high_size, maximum_bytes, probe_quality, &probe) < 0)
            {
                g_free(probe.best);
                g_free(probe.candidate);
                break;
            }
            found = probe.best;
            *result_size = probe.best_size;
            g_free(probe.candidate);
            break;
        }
        if (round == 0 && low_length > 0) steepness = (double)top_size / (double)low_length;
        g_free(low);
        double ratio = sqrt((double)maximum_bytes / (double)low_length) * 0.92;
        if (ratio < 0.2) ratio = 0.2;
        if (ratio > 0.92) ratio = 0.92;
        int width = gdk_pixbuf_get_width(current), height = gdk_pixbuf_get_height(current);
        if ((width * ratio < MINIMUM_EDGE && height * ratio < MINIMUM_EDGE) || width * ratio < 1 || height * ratio < 1) break;
        int scaled_width = (int)lround(width * ratio), scaled_height = (int)lround(height * ratio);
        if (scaled_width < 1) scaled_width = 1;
        if (scaled_height < 1) scaled_height = 1;
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(current, scaled_width, scaled_height, GDK_INTERP_BILINEAR);
        if (!scaled) break;
        g_object_unref(current);
        current = scaled;
    }
    if (!found)
    {
        gsize length = 0;
        found = encode_image(current, MINIMUM_QUALITY, &length);
        if (found) *result_size = length;
    }
    g_object_unref(current);
    return found;
}

// ───────────── 保存 ─────────────

/// 先写临时文件再改名，不会留下写了一半的文件。
static bool save_bytes(const unsigned char *bytes, size_t size, const wchar_t *directory, const wchar_t *output)
{
    wchar_t *temporary = path_available(directory, L".dropshelf-image", L".tmp");
    if (!temporary) return false;
    char *temporary_native = utf8_from_wide(temporary, NULL);
    char *output_native = utf8_from_wide(output, NULL);
    GError *error = NULL;
    bool ok = temporary_native && output_native && g_file_set_contents(temporary_native, (const char *)bytes, (gssize)size, &error) &&
        rename(temporary_native, output_native) == 0;
    if (!ok)
    {
        if (error) g_error_free(error);
        log_error(L"保存图片失败：%ls", output);
        if (temporary_native) unlink(temporary_native);
    }
    free(temporary_native);
    free(output_native);
    free(temporary);
    return ok;
}

static wchar_t *output_directory_for(const wchar_t *source, const wchar_t *requested)
{
    wchar_t *directory = !wcs_is_blank(requested) ? wcs_dup(requested) : path_directory(source);
    if (directory && !directory_create(directory))
    {
        free(directory);
        return NULL;
    }
    return directory;
}

// ───────────── 对外接口 ─────────────

ImageStatus image_compress(const wchar_t *source, long long maximum_bytes, const wchar_t *output_directory, wchar_t **output)
{
    *output = NULL;
    if (!is_file(source) || !image_is_path(source)) return IMAGE_NOT_IMAGE;
    long long size = 0;
    if (file_size_of(source, &size) && size <= maximum_bytes && widely_supported(source)) return IMAGE_ALREADY;
    GdkPixbuf *image = load_image(source, COMPRESS_MAXIMUM_PIXELS);
    gsize encoded_size = 0;
    guchar *encoded = image ? encode_within_limit(image, maximum_bytes, &encoded_size) : NULL;
    ImageStatus status = IMAGE_FAILED;
    if (encoded && (long long)encoded_size <= maximum_bytes)
    {
        wchar_t *directory = output_directory_for(source, output_directory);
        wchar_t *stem = path_file_stem(source), *limit = image_format_limit(maximum_bytes);
        WStr base = {0};
        wstr_appendf(&base, L"%ls-%ls", stem, limit);
        wchar_t *target = directory ? path_available(directory, base.data, L".jpg") : NULL;
        if (target && save_bytes(encoded, encoded_size, directory, target))
        {
            *output = target;
            target = NULL;
            status = IMAGE_DONE;
        }
        free(target);
        wstr_free(&base);
        free(stem);
        free(limit);
        free(directory);
    }
    if (status == IMAGE_FAILED) log_error(L"压缩图片失败：%ls", source);
    g_free(encoded);
    if (image) g_object_unref(image);
    return status;
}

ImageStatus image_convert(const wchar_t *source, int format, const wchar_t *output_directory, wchar_t **output)
{
    *output = NULL;
    if (!is_file(source) || !image_is_path(source)) return IMAGE_NOT_IMAGE;
    if (is_format(source, format)) return IMAGE_ALREADY;
    bool png = format == IMAGE_FORMAT_PNG;
    GdkPixbuf *image = load_image(source, CONVERT_MAXIMUM_PIXELS);
    gsize encoded_size = 0;
    guchar *encoded = image ? encode_image(image, png ? -1 : CONVERT_JPEG_QUALITY, &encoded_size) : NULL;
    ImageStatus status = IMAGE_FAILED;
    if (encoded)
    {
        wchar_t *directory = output_directory_for(source, output_directory);
        wchar_t *stem = path_file_stem(source);
        wchar_t *target = directory ? path_available(directory, stem, png ? L".png" : L".jpg") : NULL;
        if (target && save_bytes(encoded, encoded_size, directory, target))
        {
            *output = target;
            target = NULL;
            status = IMAGE_DONE;
        }
        free(target);
        free(stem);
        free(directory);
    }
    if (status == IMAGE_FAILED) log_error(L"转换图片失败：%ls", source);
    g_free(encoded);
    if (image) g_object_unref(image);
    return status;
}