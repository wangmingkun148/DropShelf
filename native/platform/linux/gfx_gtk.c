// gfx.h 的 GTK4 后端：Cairo 负责几何和图片，Pango 负责文字。
//
// Windows 版用 Direct2D/DirectWrite；这里用 GTK 绘制回调里的 Cairo。绘制函数、坐标（DIP）和元素树都不用改。
// 显卡加速由 GTK 的渲染节点（cairo node）交给 GSK；如果某台机器上 GPU 路径有问题，GTK 自己会退回软件渲染。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gfx_gtk.h"
#include "base.h"
#include "file_ops.h"
#include "log.h"

#include <fontconfig/fontconfig.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct GfxSurface
{
    cairo_t *cairo;
    bool active;
    int width, height;
    float dpi;
};

struct GfxImage
{
    cairo_surface_t *surface;
    int width, height;
    int references;
};

static GfxSurface offscreen_surface;
static cairo_surface_t *offscreen_image;
static cairo_t *current_cairo;
static PangoContext *text_context;
static cairo_user_data_key_t image_data_key;

static cairo_t *current(void);

// ───────────── 初始化 ─────────────

// 图标字体：WinSymbols3 的改版，和 Segoe Fluent Icons 使用同样的码位，MIT 许可（见 fonts/）。
// 字体文件跟着程序走，运行时交给 fontconfig，不需要用户另外安装。
static bool icon_font_loaded;

static bool add_icon_font(const wchar_t *path)
{
    if (!path_is_available(path)) return false;
    char *native = utf8_from_wide(path, NULL);
    if (!native) return false;
    FcBool added = FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)native);
    free(native);
    return added == FcTrue;
}

static void load_icon_font(void)
{
    if (icon_font_loaded) return;
    icon_font_loaded = true;
    wchar_t *directory = executable_directory();
    if (!directory) return;
    // 构建目录里字体就在程序旁边；安装和便携版在 ../share/dropshelf/fonts。
    const wchar_t *places[] = { L"DropShelfIcons.ttf", L"fonts/DropShelfIcons.ttf", L"../share/dropshelf/fonts/DropShelfIcons.ttf" };
    bool loaded = false;
    for (size_t index = 0; !loaded && index < sizeof(places) / sizeof(places[0]); index++)
    {
        wchar_t *path = path_join(directory, places[index]);
        loaded = add_icon_font(path);
        free(path);
    }
    free(directory);
    if (!loaded) log_error(L"找不到图标字体，界面图标会显示不出来");
}

bool gfx_initialize(void)
{
    load_icon_font();
    return true;
}

void gfx_shutdown(void)
{
    gfx_release_text_cache();
}

void gfx_trim(void)
{
}

void gfx_release_text_cache(void)
{
    if (text_context)
    {
        g_object_unref(text_context);
        text_context = NULL;
    }
}

// ───────────── 矩阵 ─────────────

GfxMatrix gfx_matrix_identity(void)
{
    return (GfxMatrix){ 1, 0, 0, 1, 0, 0 };
}

GfxMatrix gfx_matrix_translate(float x, float y)
{
    return (GfxMatrix){ 1, 0, 0, 1, x, y };
}

GfxMatrix gfx_matrix_scale(float scale_x, float scale_y, float center_x, float center_y)
{
    return (GfxMatrix){ scale_x, 0, 0, scale_y, center_x - center_x * scale_x, center_y - center_y * scale_y };
}

GfxMatrix gfx_matrix_rotate(float degrees, float center_x, float center_y)
{
    double radians = degrees * M_PI / 180.0;
    float cosine = (float)cos(radians), sine = (float)sin(radians);
    // x' = cx + (x-cx)cos - (y-cy)sin；与 WPF 的 RotateTransform 方向一致。
    return (GfxMatrix){ cosine, sine, -sine, cosine, center_x - center_x * cosine + center_y * sine,
        center_y - center_x * sine - center_y * cosine };
}

GfxMatrix gfx_matrix_multiply(GfxMatrix first, GfxMatrix second)
{
    // 先做 first 再做 second。
    GfxMatrix result;
    result.m11 = first.m11 * second.m11 + first.m12 * second.m21;
    result.m12 = first.m11 * second.m12 + first.m12 * second.m22;
    result.m21 = first.m21 * second.m11 + first.m22 * second.m21;
    result.m22 = first.m21 * second.m12 + first.m22 * second.m22;
    result.dx = first.dx * second.m11 + first.dy * second.m21 + second.dx;
    result.dy = first.dx * second.m12 + first.dy * second.m22 + second.dy;
    return result;
}

void gfx_matrix_apply(GfxMatrix matrix, float *x, float *y)
{
    float source_x = *x, source_y = *y;
    *x = source_x * matrix.m11 + source_y * matrix.m21 + matrix.dx;
    *y = source_x * matrix.m12 + source_y * matrix.m22 + matrix.dy;
}

bool gfx_matrix_invert(GfxMatrix matrix, GfxMatrix *inverse)
{
    float determinant = matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21;
    if (fabsf(determinant) < 1e-9f) return false;
    float scale = 1.0f / determinant;
    GfxMatrix result;
    result.m11 = matrix.m22 * scale;
    result.m12 = -matrix.m12 * scale;
    result.m21 = -matrix.m21 * scale;
    result.m22 = matrix.m11 * scale;
    result.dx = (matrix.m21 * matrix.dy - matrix.m22 * matrix.dx) * scale;
    result.dy = (matrix.m12 * matrix.dx - matrix.m11 * matrix.dy) * scale;
    if (inverse) *inverse = result;
    return true;
}

static void apply_matrix(cairo_t *cairo, GfxMatrix matrix)
{
    cairo_matrix_t transform;
    cairo_matrix_init(&transform, matrix.m11, matrix.m12, matrix.m21, matrix.m22, matrix.dx, matrix.dy);
    cairo_transform(cairo, &transform);
}

// ───────────── 表面 ─────────────

GfxSurface *gfx_surface_create(HWND window)
{
    (void)window;
    GfxSurface *surface = xcalloc(1, sizeof(*surface));
    return surface;
}

void gfx_surface_destroy(GfxSurface *surface)
{
    free(surface);
}

void gfx_surface_set_cairo(GfxSurface *surface, cairo_t *cairo)
{
    if (surface) surface->cairo = cairo;
}

bool gfx_begin(GfxSurface *surface, int width, int height, float dpi)
{
    if (!surface || !surface->cairo || width <= 0 || height <= 0 || dpi <= 0) return false;
    surface->width = width;
    surface->height = height;
    surface->dpi = dpi;
    surface->active = true;
    current_cairo = surface->cairo;
    cairo_save(surface->cairo);
    float scale = dpi / 96.0f;
    if (scale != 1.0f) cairo_scale(surface->cairo, scale, scale);
    // 绘制范围之外的东西不要漏出去。
    cairo_rectangle(surface->cairo, 0, 0, width / scale, height / scale);
    cairo_clip(surface->cairo);
    return true;
}

bool gfx_end(GfxSurface *surface)
{
    if (!surface || !surface->active) return false;
    cairo_restore(surface->cairo);
    surface->active = false;
    current_cairo = NULL;
    return true;
}

bool gfx_begin_image(int width, int height, float dpi)
{
    if (width <= 0 || height <= 0) return false;
    offscreen_image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(offscreen_image) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(offscreen_image);
        offscreen_image = NULL;
        return false;
    }
    cairo_t *cairo = cairo_create(offscreen_image);
    offscreen_surface.cairo = cairo;
    if (!gfx_begin(&offscreen_surface, width, height, dpi))
    {
        cairo_destroy(cairo);
        cairo_surface_destroy(offscreen_image);
        offscreen_image = NULL;
        return false;
    }
    return true;
}

static void finish_image(void)
{
    gfx_end(&offscreen_surface);
    cairo_destroy(offscreen_surface.cairo);
    offscreen_surface.cairo = NULL;
    cairo_surface_flush(offscreen_image);
}

bool gfx_end_image(const wchar_t *path)
{
    if (!offscreen_image) return false;
    finish_image();
    char *bytes = utf8_from_wide(path, NULL);
    cairo_status_t status = cairo_surface_write_to_png(offscreen_image, bytes);
    free(bytes);
    cairo_surface_destroy(offscreen_image);
    offscreen_image = NULL;
    return status == CAIRO_STATUS_SUCCESS;
}

GfxImage *gfx_end_image_capture(void)
{
    if (!offscreen_image) return NULL;
    finish_image();
    GfxImage *image = xcalloc(1, sizeof(*image));
    image->surface = offscreen_image;
    image->width = cairo_image_surface_get_width(offscreen_image);
    image->height = cairo_image_surface_get_height(offscreen_image);
    image->references = 1;
    offscreen_image = NULL;
    return image;
}

HBITMAP gfx_end_image_bitmap(void)
{
    // Wayland 没有跟手的拖动图像；这个接口留给以后的自绘拖放。
    if (offscreen_image)
    {
        finish_image();
        cairo_surface_destroy(offscreen_image);
        offscreen_image = NULL;
    }
    return NULL;
}

// ───────────── 图片 ─────────────

GfxImage *gfx_image_create(const void *pixels, int width, int height, int stride)
{
    if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return NULL;
    size_t length = (size_t)width * (size_t)height * 4;
    uint8_t *data = xalloc(length);
    for (int row = 0; row < height; row++)
        memcpy(data + (size_t)row * width * 4, (const uint8_t *)pixels + (size_t)row * stride, (size_t)width * 4);
    cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height, width * 4);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(surface);
        free(data);
        return NULL;
    }
    // 数据跟着 surface 一起释放。
    cairo_surface_set_user_data(surface, &image_data_key, data, free);
    GfxImage *image = xcalloc(1, sizeof(*image));
    image->surface = surface;
    image->width = width;
    image->height = height;
    image->references = 1;
    return image;
}

GfxImage *gfx_image_from_icon(HICON icon)
{
    (void)icon;
    return NULL;
}

GfxImage *gfx_image_retain(GfxImage *image)
{
    if (image) __atomic_add_fetch(&image->references, 1, __ATOMIC_RELAXED);
    return image;
}

void gfx_image_release(GfxImage *image)
{
    if (!image) return;
    if (__atomic_sub_fetch(&image->references, 1, __ATOMIC_ACQ_REL) != 0) return;
    cairo_surface_destroy(image->surface);
    free(image);
}

int gfx_retired_bitmap_count(void)
{
    return 0;
}

int gfx_image_width(const GfxImage *image)
{
    return image ? image->width : 0;
}

int gfx_image_height(const GfxImage *image)
{
    return image ? image->height : 0;
}

void gfx_draw_image(GfxImage *image, GfxRect rect, float opacity)
{
    if (!image || opacity <= 0 || rect.right <= rect.left || rect.bottom <= rect.top) return;
    cairo_t *cairo = current();
    if (!cairo) return;
    cairo_save(cairo);
    cairo_rectangle(cairo, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    cairo_clip(cairo);
    cairo_translate(cairo, rect.left, rect.top);
    cairo_scale(cairo, (rect.right - rect.left) / image->width, (rect.bottom - rect.top) / image->height);
    cairo_set_source_surface(cairo, image->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(cairo, opacity);
    cairo_restore(cairo);
}

// ───────────── 基本图形 ─────────────

static cairo_t *current(void)
{
    return current_cairo;
}

static void set_color(cairo_t *cairo, Color color)
{
    cairo_set_source_rgba(cairo, color.r / 255.0, color.g / 255.0, color.b / 255.0, color.a / 255.0);
}

static void rounded_path(cairo_t *cairo, GfxRect rect, float top, float bottom)
{
    float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    float maximum = (width < height ? width : height) / 2;
    if (top > maximum) top = maximum;
    if (bottom > maximum) bottom = maximum;
    if (top < 0) top = 0;
    if (bottom < 0) bottom = 0;
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, rect.left + top, rect.top + top, top, M_PI, 1.5 * M_PI);
    cairo_arc(cairo, rect.right - top, rect.top + top, top, 1.5 * M_PI, 2 * M_PI);
    cairo_arc(cairo, rect.right - bottom, rect.bottom - bottom, bottom, 0, 0.5 * M_PI);
    cairo_arc(cairo, rect.left + bottom, rect.bottom - bottom, bottom, 0.5 * M_PI, M_PI);
    cairo_close_path(cairo);
}

static void rounded_path_uniform(cairo_t *cairo, GfxRect rect, float radius)
{
    rounded_path(cairo, rect, radius, radius);
}

static GfxRect inset(GfxRect rect, float amount)
{
    return (GfxRect){ rect.left + amount, rect.top + amount, rect.right - amount, rect.bottom - amount };
}

void gfx_clear(Color color)
{
    cairo_t *cairo = current();
    if (!cairo) return;
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    set_color(cairo, color);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

void gfx_fill_rect(GfxRect rect, Color color)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0) return;
    set_color(cairo, color);
    cairo_rectangle(cairo, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    cairo_fill(cairo);
}

void gfx_fill_rounded(GfxRect rect, float radius, Color color)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0) return;
    rounded_path_uniform(cairo, rect, radius);
    set_color(cairo, color);
    cairo_fill(cairo);
}

void gfx_stroke_rounded(GfxRect rect, float radius, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    GfxRect inner = inset(rect, width / 2);
    rounded_path_uniform(cairo, inner, radius > width / 2 ? radius - width / 2 : 0);
    set_color(cairo, color);
    cairo_set_line_width(cairo, width);
    cairo_stroke(cairo);
}

void gfx_stroke_rounded_dashed(GfxRect rect, float radius, Color color, float width, float dash, float gap)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    GfxRect inner = inset(rect, width / 2);
    rounded_path_uniform(cairo, inner, radius > width / 2 ? radius - width / 2 : 0);
    set_color(cairo, color);
    cairo_set_line_width(cairo, width);
    double dashes[2] = { dash * width, gap * width };
    cairo_set_dash(cairo, dashes, 2, 0);
    cairo_stroke(cairo);
    cairo_set_dash(cairo, NULL, 0, 0);
}

void gfx_arc(float center_x, float center_y, float radius, float start_degrees, float sweep_degrees, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    double start = start_degrees * M_PI / 180.0;
    double end = (start_degrees + sweep_degrees) * M_PI / 180.0;
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cairo, width);
    set_color(cairo, color);
    cairo_arc(cairo, center_x, center_y, radius, start, end);
    cairo_stroke(cairo);
}

void gfx_fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0) return;
    cairo_save(cairo);
    cairo_translate(cairo, center_x, center_y);
    cairo_scale(cairo, radius_x, radius_y);
    cairo_arc(cairo, 0, 0, 1, 0, 2 * M_PI);
    set_color(cairo, color);
    cairo_fill(cairo);
    cairo_restore(cairo);
}

void gfx_stroke_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    cairo_save(cairo);
    cairo_translate(cairo, center_x, center_y);
    cairo_scale(cairo, radius_x, radius_y);
    cairo_arc(cairo, 0, 0, 1, 0, 2 * M_PI);
    set_color(cairo, color);
    cairo_set_line_width(cairo, width / ((radius_x + radius_y) / 2));
    cairo_stroke(cairo);
    cairo_restore(cairo);
}

void gfx_line(float x1, float y1, float x2, float y2, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    set_color(cairo, color);
    cairo_set_line_width(cairo, width);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cairo, x1, y1);
    cairo_line_to(cairo, x2, y2);
    cairo_stroke(cairo);
}

void gfx_shadow(GfxRect rect, float radius, float blur, float offset_x, float offset_y, Color color)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || blur <= 0) return;
    // Cairo 没有模糊；用几层逐渐扩大、逐渐变淡的圆角矩形近似。阴影的浓度均匀时效果足够。
    const int steps = 8;
    for (int step = steps - 1; step >= 0; step--)
    {
        float spread = blur * (step + 1) / steps;
        float alpha = (float)color.a / 255.0f * 0.22f * (1.0f - (float)step / steps);
        GfxRect layer = { rect.left + offset_x - spread, rect.top + offset_y - spread, rect.right + offset_x + spread,
            rect.bottom + offset_y + spread };
        gfx_fill_rounded(layer, radius + spread, color_with_alpha(color, (uint8_t)(alpha * 255)));
    }
}

void gfx_set_shadow_templates(bool enabled)
{
    (void)enabled;
}

int gfx_shadow_template_count(void)
{
    return 0;
}

void gfx_push_clip(GfxRect rect)
{
    cairo_t *cairo = current();
    if (!cairo) return;
    cairo_save(cairo);
    cairo_rectangle(cairo, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    cairo_clip(cairo);
}

void gfx_pop_clip(void)
{
    cairo_t *cairo = current();
    if (cairo) cairo_restore(cairo);
}

/// gfx_push_layer 和 gfx_push_opacity 都开一层：都先 cairo_save，再 push_group；
/// gfx_pop_layer 统一把这一层按记下的透明度贴回去再 restore。
static float layer_opacities[64];
static int layer_depth;

void gfx_push_layer(GfxRect rect, float radius, float opacity)
{
    cairo_t *cairo = current();
    if (!cairo || layer_depth >= 64) return;
    cairo_save(cairo);
    if (rect.right > rect.left && rect.bottom > rect.top)
    {
        rounded_path_uniform(cairo, rect, radius);
        cairo_clip(cairo);
    }
    cairo_push_group(cairo);
    layer_opacities[layer_depth++] = opacity;
}

void gfx_push_opacity(float opacity)
{
    cairo_t *cairo = current();
    if (!cairo || layer_depth >= 64) return;
    cairo_save(cairo);
    cairo_push_group(cairo);
    layer_opacities[layer_depth++] = opacity;
}

void gfx_pop_layer(void)
{
    cairo_t *cairo = current();
    if (!cairo || layer_depth <= 0) return;
    float opacity = layer_opacities[--layer_depth];
    cairo_pop_group_to_source(cairo);
    cairo_paint_with_alpha(cairo, opacity);
    cairo_restore(cairo);
}

// ───────────── 渐变 ─────────────

// ───────────── 图案（纸牌风格的牌背、牌桌） ─────────────

/// 图案的画刷源，从 (x, y) 开始平铺；建不了时返回 NULL，调用方改用底色。坐标是 DIP。
static cairo_pattern_t *pattern_source(const ThemePattern *pattern, float x, float y)
{
    cairo_pattern_t *source = NULL;
    if (pattern->kind == THEME_PATTERN_LATTICE && pattern->size > 0)
    {
        // 斜格纹：一格画一个叉，平铺后连成菱形格子；线宽 1。
        int pixels = (int)lroundf(pattern->size);
        if (pixels < 2) pixels = 2;
        cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixels, pixels);
        if (cairo_surface_status(tile) != CAIRO_STATUS_SUCCESS)
        {
            cairo_surface_destroy(tile);
            return NULL;
        }
        cairo_t *tile_cairo = cairo_create(tile);
        set_color(tile_cairo, pattern->fill);
        cairo_paint(tile_cairo);
        set_color(tile_cairo, pattern->line);
        cairo_set_line_width(tile_cairo, 1);
        cairo_move_to(tile_cairo, 0, 0);
        cairo_line_to(tile_cairo, pixels, pixels);
        cairo_stroke(tile_cairo);
        cairo_move_to(tile_cairo, pixels, 0);
        cairo_line_to(tile_cairo, 0, pixels);
        cairo_stroke(tile_cairo);
        cairo_destroy(tile_cairo);
        source = cairo_pattern_create_for_surface(tile);
        cairo_surface_destroy(tile);
        if (cairo_pattern_status(source) != CAIRO_STATUS_SUCCESS)
        {
            cairo_pattern_destroy(source);
            return NULL;
        }
        cairo_pattern_set_extend(source, CAIRO_EXTEND_REPEAT);
    }
    else
    {
        // 45° 的条纹，两种颜色各 2 DIP 宽：从 (0, 0) 到 (2.83, 2.83) 正好一个周期，重复平铺。
        source = cairo_pattern_create_linear(0, 0, 2.83, 2.83);
        cairo_pattern_add_color_stop_rgba(source, 0, pattern->fill.r / 255.0, pattern->fill.g / 255.0, pattern->fill.b / 255.0,
            pattern->fill.a / 255.0);
        cairo_pattern_add_color_stop_rgba(source, 0.5, pattern->fill.r / 255.0, pattern->fill.g / 255.0, pattern->fill.b / 255.0,
            pattern->fill.a / 255.0);
        cairo_pattern_add_color_stop_rgba(source, 0.5, pattern->line.r / 255.0, pattern->line.g / 255.0, pattern->line.b / 255.0,
            pattern->line.a / 255.0);
        cairo_pattern_add_color_stop_rgba(source, 1, pattern->line.r / 255.0, pattern->line.g / 255.0, pattern->line.b / 255.0,
            pattern->line.a / 255.0);
        cairo_pattern_set_extend(source, CAIRO_EXTEND_REPEAT);
    }
    cairo_matrix_t matrix;
    cairo_matrix_init_translate(&matrix, -x, -y);
    cairo_pattern_set_matrix(source, &matrix);
    return source;
}

void gfx_fill_rounded_pattern(GfxRect rect, float radius, const ThemePattern *pattern)
{
    cairo_t *cairo = current();
    if (!cairo || !pattern || rect.right <= rect.left || rect.bottom <= rect.top) return;
    cairo_pattern_t *source = pattern_source(pattern, rect.left, rect.top);
    if (!source) return;
    rounded_path_uniform(cairo, rect, radius);
    cairo_set_source(cairo, source);
    cairo_fill(cairo);
    cairo_pattern_destroy(source);
}

void gfx_stroke_rounded_pattern(GfxRect rect, float radius, const ThemePattern *pattern, float width)
{
    cairo_t *cairo = current();
    if (!cairo || !pattern || width <= 0 || rect.right <= rect.left || rect.bottom <= rect.top) return;
    GfxRect inner = inset(rect, width / 2);
    if (inner.right < inner.left || inner.bottom < inner.top) return;
    cairo_pattern_t *source = pattern_source(pattern, rect.left, rect.top);
    if (!source) return;
    rounded_path_uniform(cairo, inner, radius > width / 2 ? radius - width / 2 : 0);
    cairo_set_source(cairo, source);
    cairo_set_line_width(cairo, width);
    cairo_stroke(cairo);
    cairo_pattern_destroy(source);
}

void gfx_fill_rounded_corners_pattern(GfxRect rect, float top, float bottom, const ThemePattern *pattern)
{
    cairo_t *cairo = current();
    if (!cairo || !pattern) return;
    cairo_pattern_t *source = pattern_source(pattern, rect.left, rect.top);
    if (!source) return;
    rounded_path(cairo, rect, top, bottom);
    cairo_set_source(cairo, source);
    cairo_fill(cairo);
    cairo_pattern_destroy(source);
}

static void set_gradient_stops(cairo_pattern_t *pattern, const GfxStop *stops, int count)
{
    for (int index = 0; index < count; index++)
        cairo_pattern_add_color_stop_rgba(pattern, stops[index].offset, stops[index].color.r / 255.0,
            stops[index].color.g / 255.0, stops[index].color.b / 255.0, stops[index].color.a / 255.0);
}

void gfx_fill_rounded_radial(GfxRect rect, float radius, float center_x, float center_y, float radius_x, float radius_y,
    const GfxStop *stops, int count)
{
    cairo_t *cairo = current();
    if (!cairo || !stops || count <= 0 || rect.right <= rect.left) return;
    cairo_save(cairo);
    cairo_translate(cairo, center_x, center_y);
    cairo_scale(cairo, radius_x > 0 ? radius_x : 1, radius_y > 0 ? radius_y : 1);
    cairo_pattern_t *pattern = cairo_pattern_create_radial(0, 0, 0, 0, 0, 1);
    set_gradient_stops(pattern, stops, count);
    cairo_set_source(cairo, pattern);
    cairo_translate(cairo, -center_x, -center_y);
    rounded_path_uniform(cairo, rect, radius);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
    cairo_restore(cairo);
}

void gfx_fill_rect_linear(GfxRect rect, float x1, float y1, float x2, float y2, const GfxStop *stops, int count)
{
    cairo_t *cairo = current();
    if (!cairo || !stops || count <= 0) return;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(x1, y1, x2, y2);
    set_gradient_stops(pattern, stops, count);
    cairo_set_source(cairo, pattern);
    cairo_rectangle(cairo, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
}

void gfx_fill_rounded_corners(GfxRect rect, float top, float bottom, Color color)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0) return;
    rounded_path(cairo, rect, top, bottom);
    set_color(cairo, color);
    cairo_fill(cairo);
}

void gfx_stroke_rounded_corners(GfxRect rect, float top, float bottom, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    GfxRect inner = inset(rect, width / 2);
    rounded_path(cairo, inner, top > width / 2 ? top - width / 2 : 0, bottom > width / 2 ? bottom - width / 2 : 0);
    set_color(cairo, color);
    cairo_set_line_width(cairo, width);
    cairo_stroke(cairo);
}

void gfx_stroke_rounded_open_top(GfxRect rect, float top, float bottom, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0 || width <= 0) return;
    GfxRect inner = inset(rect, width / 2);
    float top_radius = top > width / 2 ? top - width / 2 : 0;
    float bottom_radius = bottom > width / 2 ? bottom - width / 2 : 0;
    cairo_new_sub_path(cairo);
    cairo_move_to(cairo, inner.left, inner.top + top_radius);
    cairo_line_to(cairo, inner.left, inner.bottom - bottom_radius);
    cairo_arc(cairo, inner.left + bottom_radius, inner.bottom - bottom_radius, bottom_radius, M_PI, 0.5 * M_PI);
    cairo_line_to(cairo, inner.right - bottom_radius, inner.bottom);
    cairo_arc(cairo, inner.right - bottom_radius, inner.bottom - bottom_radius, bottom_radius, 0.5 * M_PI, 0);
    cairo_line_to(cairo, inner.right, inner.top + top_radius);
    set_color(cairo, color);
    cairo_set_line_width(cairo, width);
    cairo_stroke(cairo);
}

void gfx_polygon(const float *points, int count, Color fill, Color stroke, float stroke_width)
{
    cairo_t *cairo = current();
    if (!cairo || !points || count < 2) return;
    cairo_new_path(cairo);
    cairo_move_to(cairo, points[0], points[1]);
    for (int index = 1; index < count; index++) cairo_line_to(cairo, points[index * 2], points[index * 2 + 1]);
    cairo_close_path(cairo);
    if (fill.a > 0)
    {
        set_color(cairo, fill);
        cairo_fill_preserve(cairo);
    }
    if (stroke.a > 0 && stroke_width > 0)
    {
        set_color(cairo, stroke);
        cairo_set_line_width(cairo, stroke_width);
        cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cairo);
    }
    else
    {
        cairo_new_path(cairo);
    }
}

void gfx_polyline(const float *points, int count, Color color, float width)
{
    cairo_t *cairo = current();
    if (!cairo || !points || count < 2 || color.a == 0 || width <= 0) return;
    cairo_new_path(cairo);
    cairo_move_to(cairo, points[0], points[1]);
    for (int index = 1; index < count; index++) cairo_line_to(cairo, points[index * 2], points[index * 2 + 1]);
    set_color(cairo, color);
    cairo_set_line_width(cairo, width);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cairo, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cairo);
}

void gfx_push_transform(GfxMatrix matrix)
{
    cairo_t *cairo = current();
    if (!cairo) return;
    cairo_save(cairo);
    apply_matrix(cairo, matrix);
}

void gfx_pop_transform(void)
{
    cairo_t *cairo = current();
    if (cairo) cairo_restore(cairo);
}

void gfx_fill_rounded_replace(GfxRect rect, float radius, GfxMatrix matrix, Color color)
{
    cairo_t *cairo = current();
    if (!cairo) return;
    cairo_save(cairo);
    rounded_path_uniform(cairo, rect, radius);
    cairo_clip(cairo);
    apply_matrix(cairo, matrix);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    set_color(cairo, color);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

// ───────────── 文字 ─────────────

static PangoContext *context(void)
{
    if (!text_context)
    {
        PangoFontMap *font_map = pango_cairo_font_map_get_default();
        text_context = pango_font_map_create_context(font_map);
    }
    return text_context;
}

static const char *font_family(GfxFont font)
{
    if (font == GFX_FONT_ICON) return "DropShelf Icons";
    // fontconfig 会把缺的字（中文等）回退到 Noto Sans CJK。
    return "Noto Sans";
}

static PangoLayout *make_layout(const wchar_t *text, const GfxTextStyle *style, float width)
{
    PangoContext *pango = context();
    PangoLayout *layout = pango_layout_new(pango);
    PangoFontDescription *description = pango_font_description_new();
    pango_font_description_set_family(description, font_family(style->font));
    pango_font_description_set_absolute_size(description, style->size * PANGO_SCALE);
    pango_font_description_set_weight(description, style->weight >= 600 ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, description);

    if (style->line_height > 0)
    {
        PangoFontMetrics *metrics = pango_context_get_metrics(pango, description, pango_language_get_default());
        double natural = pango_font_metrics_get_height(metrics) / (double)PANGO_SCALE;
        pango_font_metrics_unref(metrics);
        int spacing = (int)lround((style->line_height - natural) * PANGO_SCALE);
        pango_layout_set_spacing(layout, spacing > 0 ? spacing : 0);
    }
    pango_font_description_free(description);

    char *bytes = utf8_from_wide(text ? text : L"", NULL);
    pango_layout_set_text(layout, bytes, -1);
    free(bytes);

    pango_layout_set_alignment(layout, style->align == GFX_ALIGN_CENTER ? PANGO_ALIGN_CENTER
        : style->align == GFX_ALIGN_END ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    if ((style->wrap || style->ellipsis) && width > 0)
    {
        pango_layout_set_width(layout, (int)lround(width * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }
    else
    {
        pango_layout_set_width(layout, -1);
        pango_layout_set_wrap(layout, PANGO_WRAP_CHAR);
    }
    pango_layout_set_ellipsize(layout, style->ellipsis ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
    return layout;
}

GfxSize gfx_text_measure(const wchar_t *text, const GfxTextStyle *style, float max_width)
{
    PangoLayout *layout = make_layout(text, style, max_width);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    return (GfxSize){ (float)width, (float)height };
}

void gfx_text_draw(const wchar_t *text, GfxRect rect, const GfxTextStyle *style, Color color)
{
    cairo_t *cairo = current();
    if (!cairo || color.a == 0) return;
    float width = rect.right - rect.left;
    PangoLayout *layout = make_layout(text, style, width);
    int text_width = 0, text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);

    float x = rect.left;
    if (!style->wrap && !style->ellipsis)
    {
        if (style->align == GFX_ALIGN_CENTER) x = rect.left + (width - text_width) / 2;
        else if (style->align == GFX_ALIGN_END) x = rect.right - text_width;
    }
    float y = rect.top;
    if (style->vertical == GFX_ALIGN_CENTER) y = rect.top + ((rect.bottom - rect.top) - text_height) / 2;
    else if (style->vertical == GFX_ALIGN_END) y = rect.bottom - text_height;

    cairo_save(cairo);
    cairo_move_to(cairo, x, y);
    set_color(cairo, color);
    pango_cairo_show_layout(cairo, layout);
    // 路径不在 save/restore 保存的状态里：清掉 move_to，不然下一个圆弧会从文字起点连一条线过去。
    cairo_new_path(cairo);
    cairo_restore(cairo);
    g_object_unref(layout);
}

/// 宽字符串里第 wide_index 个字符在 UTF-8 里是第几个字节。
static int byte_index_for_wide(const wchar_t *text, int wide_index)
{
    size_t bytes = 0;
    for (int index = 0; index < wide_index && text[index]; index++)
    {
        uint32_t code = (uint32_t)text[index];
        if (code < 0x80) bytes += 1;
        else if (code < 0x800) bytes += 2;
        else if (code < 0x10000) bytes += 3;
        else bytes += 4;
    }
    return (int)bytes;
}

static int wide_index_for_byte(const wchar_t *text, int byte_index)
{
    size_t bytes = 0;
    int index = 0;
    while (text[index] && (int)bytes < byte_index)
    {
        uint32_t code = (uint32_t)text[index];
        if (code < 0x80) bytes += 1;
        else if (code < 0x800) bytes += 2;
        else if (code < 0x10000) bytes += 3;
        else bytes += 4;
        index++;
    }
    return index;
}

float gfx_text_caret_x(const wchar_t *text, const GfxTextStyle *style, int index)
{
    if (!text) return 0;
    int length = (int)wcslen(text);
    if (index < 0) index = 0;
    if (index > length) index = length;
    PangoLayout *layout = make_layout(text, style, 0);
    PangoRectangle position;
    pango_layout_index_to_pos(layout, byte_index_for_wide(text, index), &position);
    float x = position.x / (float)PANGO_SCALE;
    g_object_unref(layout);
    return x;
}

int gfx_text_hit(const wchar_t *text, const GfxTextStyle *style, float x)
{
    if (!text) return 0;
    PangoLayout *layout = make_layout(text, style, 0);
    int byte_index = 0, trailing = 0;
    pango_layout_xy_to_index(layout, (int)lround(x * PANGO_SCALE), 0, &byte_index, &trailing);
    int index = wide_index_for_byte(text, byte_index);
    g_object_unref(layout);
    if (trailing) index++;
    int length = (int)wcslen(text);
    return index < 0 ? 0 : index > length ? length : index;
}