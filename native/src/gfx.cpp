// 图形层的实现。Direct2D、DirectWrite、DirectComposition 的头文件只能在 C++ 里用，
// 所以这一个文件是 C++；写法上仍是 C 的风格：不用 STL、异常和类，对外只有 gfx.h 里的 C 函数。
#include "gfx.h"

extern "C" {
#include "log.h"
}

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

template <typename T> static void release(T *&pointer)
{
    if (pointer) pointer->Release();
    pointer = nullptr;
}

// ───────────── 设备 ─────────────

static ID3D11Device *d3d_device;
static IDXGIDevice *dxgi_device;
static ID2D1Factory1 *d2d_factory;
static ID2D1Device *d2d_device;
static ID2D1DeviceContext *context;
static ID2D1SolidColorBrush *brush;
static ID2D1Effect *shadow_effect;
static IDWriteFactory *dwrite_factory;
static IDCompositionDevice *composition;

/// 设备每重建一次加一；各窗口的画面发现自己是旧设备上建的，就跟着重建。
static unsigned device_generation;
static GfxSurface *drawing;     // 正在画的画面，gfx_begin 和 gfx_end 之间不为空

struct GfxSurface
{
    HWND window;
    IDXGISwapChain1 *swap_chain;
    ID2D1Bitmap1 *target;
    IDCompositionTarget *composition_target;
    IDCompositionVisual *visual;
    int width, height;
    float dpi;
    unsigned generation;
};

static void release_device_resources(void)
{
    release(shadow_effect);
    release(brush);
    release(context);
    release(d2d_device);
    release(composition);
    release(dxgi_device);
    release(d3d_device);
}

static bool create_device_resources(void)
{
    if (context) return true;
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    // 先用显卡；没有可用的显卡（虚拟机、远程会话）时用系统的软件渲染。
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &d3d_device, nullptr, nullptr);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &d3d_device, nullptr, nullptr);
    if (SUCCEEDED(result)) result = d3d_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device);
    if (SUCCEEDED(result)) result = d2d_factory->CreateDevice(dxgi_device, &d2d_device);
    if (SUCCEEDED(result)) result = d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
    if (SUCCEEDED(result)) result = context->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
    if (SUCCEEDED(result)) result = DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice), (void **)&composition);
    if (FAILED(result))
    {
        log_error(L"创建显卡设备失败\r\nHRESULT 0x%08lX", (unsigned long)result);
        release_device_resources();
        return false;
    }
    // 画面是带透明度的，子像素（ClearType）抗锯齿在透明背景上会出彩边，用灰度抗锯齿。
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    device_generation++;
    return true;
}

bool gfx_initialize(void)
{
    if (!d2d_factory)
    {
        D2D1_FACTORY_OPTIONS options = {};
        HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, (void **)&d2d_factory);
        if (SUCCEEDED(result))
            result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dwrite_factory);
        if (FAILED(result))
        {
            log_error(L"创建 Direct2D 或 DirectWrite 失败\r\nHRESULT 0x%08lX", (unsigned long)result);
            release(d2d_factory);
            release(dwrite_factory);
            return false;
        }
    }
    return create_device_resources();
}

// ───────────── 窗口的画面 ─────────────

static void release_surface_resources(GfxSurface *surface)
{
    release(surface->target);
    release(surface->visual);
    release(surface->composition_target);
    release(surface->swap_chain);
    surface->width = surface->height = 0;
}

GfxSurface *gfx_surface_create(HWND window)
{
    GfxSurface *surface = (GfxSurface *)calloc(1, sizeof(GfxSurface));
    if (!surface) abort();
    surface->window = window;
    return surface;
}

void gfx_surface_destroy(GfxSurface *surface)
{
    if (!surface) return;
    if (drawing == surface) drawing = nullptr;
    release_surface_resources(surface);
    free(surface);
}

static HRESULT create_swap_chain(GfxSurface *surface, int width, int height)
{
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    HRESULT result = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(result)) result = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
    if (SUCCEEDED(result))
    {
        DXGI_SWAP_CHAIN_DESC1 description = {};
        description.Width = (UINT)width;
        description.Height = (UINT)height;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        result = factory->CreateSwapChainForComposition(dxgi_device, &description, nullptr, &surface->swap_chain);
    }
    if (SUCCEEDED(result)) result = composition->CreateTargetForHwnd(surface->window, TRUE, &surface->composition_target);
    if (SUCCEEDED(result)) result = composition->CreateVisual(&surface->visual);
    if (SUCCEEDED(result)) result = surface->visual->SetContent(surface->swap_chain);
    if (SUCCEEDED(result)) result = surface->composition_target->SetRoot(surface->visual);
    if (SUCCEEDED(result)) result = composition->Commit();
    release(factory);
    release(adapter);
    return result;
}

/// 让画面的大小与窗口一致，并准备好可以画的位图。
static bool prepare_surface(GfxSurface *surface, int width, int height, float dpi)
{
    if (surface->generation != device_generation)
    {
        release_surface_resources(surface);
        surface->generation = device_generation;
    }
    HRESULT result = S_OK;
    if (!surface->swap_chain)
    {
        result = create_swap_chain(surface, width, height);
    }
    else if (surface->width != width || surface->height != height)
    {
        release(surface->target);
        context->SetTarget(nullptr);
        result = surface->swap_chain->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    }
    if (SUCCEEDED(result) && surface->target && surface->dpi != dpi) release(surface->target);
    if (SUCCEEDED(result) && !surface->target)
    {
        IDXGISurface *buffer = nullptr;
        result = surface->swap_chain->GetBuffer(0, __uuidof(IDXGISurface), (void **)&buffer);
        if (SUCCEEDED(result))
        {
            D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
            result = context->CreateBitmapFromDxgiSurface(buffer, &properties, &surface->target);
        }
        release(buffer);
    }
    if (FAILED(result))
    {
        log_error(L"准备窗口画面失败\r\nHRESULT 0x%08lX", (unsigned long)result);
        release_surface_resources(surface);
        return false;
    }
    surface->width = width;
    surface->height = height;
    surface->dpi = dpi;
    return true;
}

#define FORMAT_CACHE_SIZE 24

struct CachedFormat
{
    GfxFont font;
    float size;
    int weight;
    IDWriteTextFormat *format;
};

static CachedFormat format_cache[FORMAT_CACHE_SIZE];
static int format_count;

/// 变换的栈：gfx_push_transform 叠加，gfx_pop_transform 还原。
#define TRANSFORM_STACK_SIZE 16
static D2D1_MATRIX_3X2_F transform_stack[TRANSFORM_STACK_SIZE];
static int transform_depth;

bool gfx_begin(GfxSurface *surface, int width, int height, float dpi)
{
    if (!surface || width <= 0 || height <= 0 || drawing) return false;
    if (!gfx_initialize()) return false;
    if (!prepare_surface(surface, width, height, dpi))
    {
        // 可能是设备丢失：重建后再试一次。
        release_device_resources();
        if (!create_device_resources() || !prepare_surface(surface, width, height, dpi)) return false;
    }
    context->SetTarget(surface->target);
    context->SetDpi(dpi, dpi);
    context->BeginDraw();
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    transform_depth = 0;
    drawing = surface;
    return true;
}

bool gfx_end(GfxSurface *surface)
{
    if (!surface || drawing != surface) return false;
    drawing = nullptr;
    HRESULT result = context->EndDraw();
    context->SetTarget(nullptr);
    if (SUCCEEDED(result)) result = surface->swap_chain->Present(0, 0);
    if (SUCCEEDED(result)) return true;

    // 显卡设备丢失（驱动更新、远程桌面切换、显卡被拔掉）：全部重建，由调用方重画。
    log_error(L"显示画面失败，重建显卡设备\r\nHRESULT 0x%08lX", (unsigned long)result);
    release_surface_resources(surface);
    release_device_resources();
    create_device_resources();
    return false;
}

// ───────────── 画进图片（诊断和测试用） ─────────────

static GfxSurface image_surface;    // 只用来标记“正在画图片”
static ID2D1Bitmap1 *image_target;
static int image_width, image_height;
static float image_dpi;

bool gfx_begin_image(int width, int height, float dpi)
{
    if (width <= 0 || height <= 0 || drawing || !gfx_initialize()) return false;
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    if (FAILED(context->CreateBitmap(D2D1::SizeU((UINT32)width, (UINT32)height), nullptr, 0, &properties, &image_target))) return false;
    image_width = width;
    image_height = height;
    image_dpi = dpi;
    context->SetTarget(image_target);
    context->SetDpi(dpi, dpi);
    context->BeginDraw();
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    transform_depth = 0;
    drawing = &image_surface;
    return true;
}

bool gfx_end_image(const wchar_t *path)
{
    if (drawing != &image_surface) return false;
    drawing = nullptr;
    HRESULT result = context->EndDraw();
    context->SetTarget(nullptr);

    // 用系统的图像组件（WIC）把这张位图编码成 PNG。
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory2 *factory = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    IWICImageEncoder *image_encoder = nullptr;
    if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory2), (void **)&factory);
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = factory->CreateImageEncoder(d2d_device, &image_encoder);
    // 不说明的话图片会按 DIP 的大小存，缩放比例不是 100% 时只存下左上角的一块。
    WICImageParameters parameters = {};
    parameters.PixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    parameters.DpiX = parameters.DpiY = image_dpi;
    parameters.PixelWidth = (UINT32)image_width;
    parameters.PixelHeight = (UINT32)image_height;
    if (SUCCEEDED(result)) result = image_encoder->WriteFrame(image_target, frame, &parameters);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    release(image_encoder);
    release(frame);
    release(encoder);
    release(stream);
    release(factory);
    release(image_target);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(result)) log_error(L"保存画面到图片失败\r\nHRESULT 0x%08lX", (unsigned long)result);
    return SUCCEEDED(result);
}

void gfx_shutdown(void)
{
    for (int index = 0; index < format_count; index++) release(format_cache[index].format);
    format_count = 0;
    release_device_resources();
    release(dwrite_factory);
    release(d2d_factory);
}

// ───────────── 变换 ─────────────

GfxMatrix gfx_matrix_identity(void)
{
    GfxMatrix matrix = { 1, 0, 0, 1, 0, 0 };
    return matrix;
}

GfxMatrix gfx_matrix_translate(float x, float y)
{
    GfxMatrix matrix = { 1, 0, 0, 1, x, y };
    return matrix;
}

GfxMatrix gfx_matrix_scale(float scale_x, float scale_y, float center_x, float center_y)
{
    GfxMatrix matrix = { scale_x, 0, 0, scale_y, center_x - scale_x * center_x, center_y - scale_y * center_y };
    return matrix;
}

GfxMatrix gfx_matrix_multiply(GfxMatrix a, GfxMatrix b)
{
    GfxMatrix matrix = {
        a.m11 * b.m11 + a.m12 * b.m21, a.m11 * b.m12 + a.m12 * b.m22,
        a.m21 * b.m11 + a.m22 * b.m21, a.m21 * b.m12 + a.m22 * b.m22,
        a.dx * b.m11 + a.dy * b.m21 + b.dx, a.dx * b.m12 + a.dy * b.m22 + b.dy
    };
    return matrix;
}

void gfx_push_transform(GfxMatrix matrix)
{
    if (!drawing || transform_depth >= TRANSFORM_STACK_SIZE) return;
    D2D1_MATRIX_3X2_F current;
    context->GetTransform(&current);
    transform_stack[transform_depth++] = current;
    D2D1::Matrix3x2F local(matrix.m11, matrix.m12, matrix.m21, matrix.m22, matrix.dx, matrix.dy);
    context->SetTransform(local * (*D2D1::Matrix3x2F::ReinterpretBaseType(&current)));
}

void gfx_pop_transform(void)
{
    if (!drawing || transform_depth <= 0) return;
    context->SetTransform(transform_stack[--transform_depth]);
}

// ───────────── 图形 ─────────────

static ID2D1SolidColorBrush *use_color(Color color)
{
    brush->SetColor(D2D1::ColorF(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f));
    return brush;
}

static D2D1_RECT_F to_rect(GfxRect rect)
{
    return D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
}

void gfx_clear(Color color)
{
    if (!drawing) return;
    float alpha = color.a / 255.0f;
    // 清屏的颜色要预乘透明度。
    context->Clear(D2D1::ColorF(color.r / 255.0f * alpha, color.g / 255.0f * alpha, color.b / 255.0f * alpha, alpha));
}

void gfx_fill_rect(GfxRect rect, Color color)
{
    if (!drawing || color.a == 0) return;
    context->FillRectangle(to_rect(rect), use_color(color));
}

void gfx_fill_rounded(GfxRect rect, float radius, Color color)
{
    if (!drawing || color.a == 0) return;
    if (radius <= 0) context->FillRectangle(to_rect(rect), use_color(color));
    else context->FillRoundedRectangle(D2D1::RoundedRect(to_rect(rect), radius, radius), use_color(color));
}

void gfx_stroke_rounded(GfxRect rect, float radius, Color color, float width)
{
    if (!drawing || color.a == 0 || width <= 0) return;
    // 线画在路径两侧各一半，往里收半个线宽，边框就正好落在矩形以内。
    float inset = width / 2;
    D2D1_RECT_F inner = D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
    float inner_radius = radius > inset ? radius - inset : 0;
    if (inner_radius <= 0) context->DrawRectangle(inner, use_color(color), width);
    else context->DrawRoundedRectangle(D2D1::RoundedRect(inner, inner_radius, inner_radius), use_color(color), width);
}

void gfx_fill_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color)
{
    if (!drawing || color.a == 0) return;
    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), radius_x, radius_y), use_color(color));
}

void gfx_stroke_ellipse(float center_x, float center_y, float radius_x, float radius_y, Color color, float width)
{
    if (!drawing || color.a == 0 || width <= 0) return;
    context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center_x, center_y), radius_x - width / 2, radius_y - width / 2), use_color(color), width);
}

void gfx_line(float x1, float y1, float x2, float y2, Color color, float width)
{
    if (!drawing || color.a == 0) return;
    context->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), use_color(color), width);
}

void gfx_shadow(GfxRect rect, float radius, float blur, float offset_x, float offset_y, Color color)
{
    if (!drawing || color.a == 0) return;
    if (!shadow_effect && FAILED(context->CreateEffect(CLSID_D2D1Shadow, &shadow_effect))) return;

    // 先把圆角矩形画进一份“命令列表”，再让阴影效果按它的形状算出模糊的影子。
    ID2D1CommandList *shape = nullptr;
    if (FAILED(context->CreateCommandList(&shape))) return;
    ID2D1Image *previous = nullptr;
    D2D1_MATRIX_3X2_F transform;
    context->GetTarget(&previous);
    context->GetTransform(&transform);
    context->SetTarget(shape);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    Color opaque = { 255, 0, 0, 0 };
    context->FillRoundedRectangle(D2D1::RoundedRect(to_rect(rect), radius, radius), use_color(opaque));
    context->SetTarget(previous);
    context->SetTransform(transform);
    release(previous);

    if (SUCCEEDED(shape->Close()))
    {
        shadow_effect->SetInput(0, shape);
        // 模糊半径约为标准差的三倍。
        shadow_effect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur / 3.0f);
        shadow_effect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f));
        context->DrawImage(shadow_effect, D2D1::Point2F(offset_x, offset_y));
        shadow_effect->SetInput(0, nullptr);
    }
    release(shape);
}

void gfx_push_clip(GfxRect rect)
{
    if (drawing) context->PushAxisAlignedClip(to_rect(rect), D2D1_ANTIALIAS_MODE_ALIASED);
}

void gfx_pop_clip(void)
{
    if (drawing) context->PopAxisAlignedClip();
}

void gfx_push_layer(GfxRect rect, float radius, float opacity)
{
    if (!drawing) return;
    ID2D1Geometry *mask = nullptr;
    D2D1_RECT_F bounds = D2D1::InfiniteRect();
    if (rect.right > rect.left && rect.bottom > rect.top)
    {
        bounds = to_rect(rect);
        ID2D1RoundedRectangleGeometry *rounded = nullptr;
        if (radius > 0 && SUCCEEDED(d2d_factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(bounds, radius, radius), &rounded))) mask = rounded;
    }
    context->PushLayer(D2D1::LayerParameters1(bounds, mask, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity), nullptr);
    release(mask);
}

void gfx_push_opacity(float opacity)
{
    GfxRect none = { 0, 0, 0, 0 };
    gfx_push_layer(none, 0, opacity);
}

void gfx_pop_layer(void)
{
    if (drawing) context->PopLayer();
}

// ───────────── 文字 ─────────────

static bool font_exists(const wchar_t *family)
{
    IDWriteFontCollection *fonts = nullptr;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(dwrite_factory->GetSystemFontCollection(&fonts, FALSE))) fonts->FindFamilyName(family, &index, &exists);
    release(fonts);
    return exists != FALSE;
}

static const wchar_t *font_family(GfxFont font)
{
    if (font != GFX_FONT_ICON) return L"Segoe UI";
    // Segoe Fluent Icons 是 Windows 11 才有的，Windows 10 上用图形相同的 Segoe MDL2 Assets。
    static const wchar_t *icons;
    if (!icons) icons = font_exists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return icons;
}

static IDWriteTextFormat *text_format(GfxFont font, float size, int weight)
{
    for (int index = 0; index < format_count; index++)
        if (format_cache[index].font == font && format_cache[index].size == size && format_cache[index].weight == weight)
            return format_cache[index].format;

    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
    if (!GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) wcscpy_s(locale, L"en-us");
    IDWriteTextFormat *format = nullptr;
    if (FAILED(dwrite_factory->CreateTextFormat(font_family(font), nullptr, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, locale, &format)))
        return nullptr;
    // 缓存满了就从头覆盖：用到的字号和粗细组合很少，正常情况下到不了这里。
    int slot = format_count < FORMAT_CACHE_SIZE ? format_count++ : 0;
    if (format_cache[slot].format) format_cache[slot].format->Release();
    format_cache[slot] = { font, size, weight, format };
    return format;
}

static DWRITE_TEXT_ALIGNMENT text_alignment(GfxAlign align)
{
    return align == GFX_ALIGN_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : align == GFX_ALIGN_END ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
}

static DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment(GfxAlign align)
{
    return align == GFX_ALIGN_CENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : align == GFX_ALIGN_END ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
}

/// 排好一段文字。width、height 是排版的范围，不限制时传很大的数。
static IDWriteTextLayout *text_layout(const wchar_t *text, const GfxTextStyle *style, float width, float height)
{
    if (!text || !style || (!dwrite_factory && !gfx_initialize())) return nullptr;
    IDWriteTextFormat *format = text_format(style->font, style->size, style->weight > 0 ? style->weight : 400);
    if (!format) return nullptr;
    IDWriteTextLayout *layout = nullptr;
    if (FAILED(dwrite_factory->CreateTextLayout(text, (UINT32)wcslen(text), format, width, height, &layout))) return nullptr;
    layout->SetWordWrapping(style->wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(text_alignment(style->align));
    layout->SetParagraphAlignment(paragraph_alignment(style->vertical));
    if (style->ellipsis)
    {
        // 省略号的大小跟着字号走，所以每次按这段文字的格式现做一个。
        IDWriteInlineObject *sign = nullptr;
        dwrite_factory->CreateEllipsisTrimmingSign(format, &sign);
        DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        layout->SetTrimming(&trimming, sign);
        release(sign);
    }
    return layout;
}

GfxSize gfx_text_measure(const wchar_t *text, const GfxTextStyle *style, float max_width)
{
    GfxSize size = { 0, 0 };
    IDWriteTextLayout *layout = text_layout(text, style, max_width > 0 ? max_width : 100000.0f, 100000.0f);
    if (!layout) return size;
    // 量的时候一律靠左上，否则居中的文字会把整个排版范围算进去。
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS metrics;
    if (SUCCEEDED(layout->GetMetrics(&metrics)))
    {
        // 往上取整：按量出来的宽度再排一次时，不会因为小数的误差多折一行。
        size.width = ceilf(metrics.widthIncludingTrailingWhitespace);
        size.height = ceilf(metrics.height);
    }
    layout->Release();
    return size;
}

void gfx_text_draw(const wchar_t *text, GfxRect rect, const GfxTextStyle *style, Color color)
{
    if (!drawing || color.a == 0 || !text || !*text) return;
    float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    IDWriteTextLayout *layout = text_layout(text, style, width, height);
    if (!layout) return;
    context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, use_color(color), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    layout->Release();
}
