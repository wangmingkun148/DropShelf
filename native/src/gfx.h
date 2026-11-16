// 图形层：把 Direct2D、DirectWrite、DirectComposition 包成 C 函数。
// 这三套接口的头文件只能在 C++ 里用，所以实现放在 gfx.cpp（整个项目唯一的 C++ 文件，不用 STL 和异常）。
// 坐标都是与设备无关的像素（DIP，96 分之一英寸），由图形层按窗口的 DPI 换算。
#pragma once

#include "theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GfxRect
{
    float left, top, right, bottom;
} GfxRect;

typedef struct GfxSize
{
    float width, height;
} GfxSize;

/// 2D 仿射变换：x' = x*m11 + y*m21 + dx，y' = x*m12 + y*m22 + dy。
typedef struct GfxMatrix
{
    float m11, m12, m21, m22, dx, dy;
} GfxMatrix;

GfxMatrix gfx_matrix_identity(void);
GfxMatrix gfx_matrix_translate(float x, float y);

/// 以 (center_x, center_y) 为中心缩放。
GfxMatrix gfx_matrix_scale(float scale_x, float scale_y, float center_x, float center_y);

/// 以 (center_x, center_y) 为中心顺时针转 degrees 度（和 WPF 的 RotateTransform 相同）。
GfxMatrix gfx_matrix_rotate(float degrees, float center_x, float center_y);
GfxMatrix gfx_matrix_multiply(GfxMatrix first, GfxMatrix second);   // 先做 first 再做 second

/// 点 (x, y) 经过 matrix 之后的位置。
void gfx_matrix_apply(GfxMatrix matrix, float *x, float *y);

/// 创建显卡设备和各个工厂；失败时返回 false（已记入日志），界面无法显示。可以重复调用。
bool gfx_initialize(void);
void gfx_shutdown(void);

/// 空闲时调用（没有窗口在显示）：让 Direct2D 放掉留着下次用的中间位图和字形缓存，再让显卡驱动把替程序留着的内存交回去。
/// 之后第一次画时这些会重新建，所以只在一段时间不画之后调用。正在画时什么也不做。
void gfx_trim(void);

// ───────────── 窗口的画面 ─────────────
// 每个窗口一份：一个带透明度的交换链，经 DirectComposition 显示在窗口上，没画到的地方透出窗口背后的系统材质或桌面。
// 窗口需要带 WS_EX_NOREDIRECTIONBITMAP。

typedef struct GfxSurface GfxSurface;

GfxSurface *gfx_surface_create(HWND window);
void gfx_surface_destroy(GfxSurface *surface);

/// 开始画一帧：width、height 是窗口客户区的大小（屏幕像素），dpi 是窗口的 DPI。
/// 返回 false 时这一帧画不了（窗口大小为 0，或显卡设备暂时不可用），不要调用绘制函数和 gfx_end。
bool gfx_begin(GfxSurface *surface, int width, int height, float dpi);

/// 结束并显示这一帧。显卡设备丢失（驱动更新、远程桌面切换）时返回 false，已经重建设备，调用方应当重画一次。
bool gfx_end(GfxSurface *surface);

/// 诊断和测试用：不画到窗口上，而是画进一张图片。用法与 gfx_begin、gfx_end 相同，结束时存成 PNG 文件。
bool gfx_begin_image(int width, int height, float dpi);
bool gfx_end_image(const wchar_t *path);

/// 同上，但结束时交出一张自上而下、32 位、预乘透明度的 DIB（拖动图像用）。失败时返回 NULL。调用方负责 DeleteObject。
HBITMAP gfx_end_image_bitmap(void);

// ───────────── 图片 ─────────────
// 像素放在内存里，任何线程都可以创建；画的时候才在显卡上建对应的位图，显卡设备重建后自动重建。

typedef struct GfxImage GfxImage;

/// pixels 是自上而下、32 位、预乘透明度的 BGRA，会被复制。
GfxImage *gfx_image_create(const void *pixels, int width, int height, int stride);

/// 把图标（LoadImage 按需要的大小取出的）转成图片；失败时返回 NULL。只在界面线程调用。
GfxImage *gfx_image_from_icon(HICON icon);
GfxImage *gfx_image_retain(GfxImage *image);
/// 可以在任何线程调用；可以是 NULL。在界面线程以外放掉最后一个引用时，显卡上的位图留到界面线程下次开始画时释放。
void gfx_image_release(GfxImage *image);

/// 诊断和测试用：在别的线程上放掉、还等着界面线程释放的显卡位图有几张。
int gfx_retired_bitmap_count(void);
int gfx_image_width(const GfxImage *image);
int gfx_image_height(const GfxImage *image);

/// 按高质量缩放画进 rect，整体乘以 opacity。
void gfx_draw_image(GfxImage *image, GfxRect rect, float opacity);

// ───────────── 绘制（在 gfx_begin 和 gfx_end 之间调用） ─────────────

/// 整个画面清成一种颜色；全透明就是什么都不画，露出系统材质。
void gfx_clear(Color color);

/// 放掉留着下次用的文字排版；正在画时什么也不做。
void gfx_release_text_cache(void);

void gfx_fill_rect(GfxRect rect, Color color);
void gfx_fill_rounded(GfxRect rect, float radius, Color color);

/// 画在矩形边缘以内的边框（与 WPF 的 Border 一样，边框不超出矩形）。
void gfx_stroke_rounded(GfxRect rect, float radius, Color color, float width);

/// 虚线边框，和 WPF 的 Rectangle 一样画在矩形以内；dash、gap 以线宽为单位（WPF 的 StrokeDashArray）。
void gfx_stroke_rounded_dashed(GfxRect rect, float radius, Color color, float width, float dash, float gap);

/// 一段圆弧，两端是圆头：从 start_degrees（0 是正右方，顺时针为正）转过 sweep_degrees。
void gfx_arc(float center_x, float center_y, float radius, float start_degrees, float sweep_degrees, Color color, float width);
void gfx_fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color);
void gfx_stroke_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color, float width);
void gfx_line(float x1, float y1, float x2, float y2, Color color, float width);

/// 圆角矩形投下的阴影：blur 是模糊半径，color 的透明度是阴影的浓度。
/// 第一次遇到某种圆角、模糊和颜色时画一个模板，之后按九宫格贴图，不再每次做模糊；矩形太小拼不开时每次现算。
void gfx_shadow(GfxRect rect, float radius, float blur, float offset_x, float offset_y, Color color);

/// 诊断和测试用：关掉时阴影都每次现算（和以前一样），用来对比；现有的模板有几个。
void gfx_set_shadow_templates(bool enabled);
int gfx_shadow_template_count(void);

/// 之后的内容只画在 rect 以内，直到 gfx_pop_clip。
void gfx_push_clip(GfxRect rect);
void gfx_pop_clip(void);

/// 之后的内容裁成圆角矩形并整体乘以 opacity，直到 gfx_pop_layer。radius 为 0 且 rect 为空时只调整透明度；
/// 这时 opacity 为 1 的层什么也不改变，不会真的开。
void gfx_push_layer(GfxRect rect, float radius, float opacity);
void gfx_push_opacity(float opacity);
void gfx_pop_layer(void);

/// 圆角矩形 rect（经过 matrix 变换）以内已经画上的内容直接换成 color，不和底下的颜色混合；边缘按覆盖的比例过渡。
/// 翻牌时用它在旧内容里挖出卡片的形状：底下是一种纯色时，先照常画、再用底色盖回去，和开一层挖空的效果一样，
/// 却不用另开一层（每层都要一张和范围一样大的中间位图，显卡驱动用过以后不还给系统）。
void gfx_fill_rounded_replace(GfxRect rect, float radius, GfxMatrix matrix, Color color);

/// 之后的内容先做 matrix 变换（叠加在已有的变换上），直到 gfx_pop_transform。
void gfx_push_transform(GfxMatrix matrix);
void gfx_pop_transform(void);

/// 矩阵的逆变换；没有逆（缩放为 0）时返回 false。
bool gfx_matrix_invert(GfxMatrix matrix, GfxMatrix *inverse);

// ───────────── 渐变和路径 ─────────────

/// 渐变上的一点：offset 在 0–1 之间。
typedef struct GfxStop
{
    float offset;
    Color color;
} GfxStop;

/// 用径向渐变填充圆角矩形：渐变的中心 (center_x, center_y)，横向半径 radius_x、纵向半径 radius_y（都是 DIP）。和 WPF 的 RadialGradientBrush 一样，GradientOrigin 与中心相同。
void gfx_fill_rounded_radial(GfxRect rect, float radius, float center_x, float center_y, float radius_x, float radius_y,
    const GfxStop *stops, int count);

/// 用线性渐变填充矩形：从 (x1, y1) 渐变到 (x2, y2)。
void gfx_fill_rect_linear(GfxRect rect, float x1, float y1, float x2, float y2, const GfxStop *stops, int count);

/// 上面两个角的半径是 top、下面两个角是 bottom 的圆角矩形（牌堆露出的边）。描边画在矩形以内。
void gfx_fill_rounded_corners(GfxRect rect, float top, float bottom, Color color);
void gfx_stroke_rounded_corners(GfxRect rect, float top, float bottom, Color color, float width);

/// 只描左、右、下三条边（上沿被前一张牌盖住），圆角同上。
void gfx_stroke_rounded_open_top(GfxRect rect, float top, float bottom, Color color, float width);

/// 由 count 个点 (x, y) 围成的多边形：先填充 fill，再沿边描 stroke（圆角连接）。不要的传全透明。
void gfx_polygon(const float *points, int count, Color fill, Color stroke, float stroke_width);

/// 一串相连的线段，圆头、圆角连接。
void gfx_polyline(const float *points, int count, Color color, float width);

// ───────────── 文字 ─────────────

typedef enum GfxFont
{
    GFX_FONT_TEXT,      // Segoe UI，中文等由系统回退到相应字体（微软雅黑 UI）
    GFX_FONT_ICON       // Segoe Fluent Icons，没有时用 Segoe MDL2 Assets
} GfxFont;

typedef enum GfxAlign
{
    GFX_ALIGN_START,
    GFX_ALIGN_CENTER,
    GFX_ALIGN_END
} GfxAlign;

typedef struct GfxTextStyle
{
    GfxFont font;
    float size;             // DIP
    int weight;             // 400 常规，600 半粗
    bool wrap;              // 超出宽度时换行，否则只有一行
    bool ellipsis;          // 放不下时末尾显示省略号
    GfxAlign align;         // 水平方向
    GfxAlign vertical;      // 垂直方向
    float line_height;      // 固定的行高（DIP），0 表示按字体自己的行高
} GfxTextStyle;

/// 文字占多大；max_width 是可用的宽度（换行或省略时用），不限制时传 0。
GfxSize gfx_text_measure(const wchar_t *text, const GfxTextStyle *style, float max_width);
void gfx_text_draw(const wchar_t *text, GfxRect rect, const GfxTextStyle *style, Color color);

/// 单行文字里第 index 个字符前面的横坐标（从文字的左边算起）。输入框画光标和选中范围用。
float gfx_text_caret_x(const wchar_t *text, const GfxTextStyle *style, int index);

/// 单行文字里横坐标 x 处离得最近的字符边界（0 到文字长度）。
int gfx_text_hit(const wchar_t *text, const GfxTextStyle *style, float x);

#ifdef __cplusplus
}
#endif
