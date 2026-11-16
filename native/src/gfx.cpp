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
#include <dxgi1_3.h>
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
/// 另一个设备上下文，专门在画别的东西的同时画阴影的模板（见下面“阴影”一节）；和 context 在同一个设备上，画出的位图两边都能用。
static ID2D1DeviceContext *template_context;
static ID2D1SolidColorBrush *template_brush;
static ID2D1Effect *template_shadow;
static IDWriteFactory *dwrite_factory;
static IDCompositionDevice *composition;

/// 设备每重建一次加一；各窗口的画面发现自己是旧设备上建的，就跟着重建。
static unsigned device_generation;
static GfxSurface *drawing;     // 正在画的画面，gfx_begin 和 gfx_end 之间不为空

/// 界面线程：Direct2D 的工厂是单线程的，设备和它建出来的东西只能在这个线程上用和释放。
static DWORD ui_thread;

// ───────────── 等界面线程释放的位图 ─────────────
// 图片的最后一个引用可能在别的线程上放掉（图片处理线程的缩略图缓存满了换掉旧的）。那时显卡上的位图不能当场释放：
// 界面线程可能正用同一个设备在画。先记在这里，界面线程下次开始画时一起释放。

struct RetiredBitmap
{
    ID2D1Bitmap1 *bitmap;
    RetiredBitmap *next;
};

static SRWLOCK retired_lock = SRWLOCK_INIT;
static RetiredBitmap *retired_bitmaps;
static volatile LONG retired_count;

static void retire_bitmap(ID2D1Bitmap1 *bitmap)
{
    RetiredBitmap *entry = (RetiredBitmap *)malloc(sizeof(RetiredBitmap));
    if (!entry) abort();
    entry->bitmap = bitmap;
    AcquireSRWLockExclusive(&retired_lock);
    entry->next = retired_bitmaps;
    retired_bitmaps = entry;
    InterlockedIncrement(&retired_count);
    ReleaseSRWLockExclusive(&retired_lock);
}

/// 只在界面线程调用。
static void release_retired_bitmaps(void)
{
    if (retired_count == 0) return;
    AcquireSRWLockExclusive(&retired_lock);
    RetiredBitmap *entry = retired_bitmaps;
    retired_bitmaps = nullptr;
    InterlockedExchange(&retired_count, 0);
    ReleaseSRWLockExclusive(&retired_lock);
    while (entry)
    {
        RetiredBitmap *next = entry->next;
        entry->bitmap->Release();
        free(entry);
        entry = next;
    }
}

int gfx_retired_bitmap_count(void)
{
    return (int)retired_count;
}

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

static void release_shadow_templates(void);
static void release_pattern_brushes(void);

static void release_device_resources(void)
{
    release_shadow_templates();
    release_pattern_brushes();
    release(template_shadow);
    release(template_brush);
    release(template_context);
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
    // 画阴影模板用的上下文；建不了时阴影照旧每次现算。模板按像素画，DPI 固定为 96。
    if (SUCCEEDED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &template_context)) &&
        SUCCEEDED(template_context->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &template_brush)) &&
        SUCCEEDED(template_context->CreateEffect(CLSID_D2D1Shadow, &template_shadow)))
    {
        template_context->SetDpi(96, 96);
        template_context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    }
    else
    {
        release(template_shadow);
        release(template_brush);
        release(template_context);
    }
    device_generation++;
    return true;
}

bool gfx_initialize(void)
{
    if (!ui_thread) ui_thread = GetCurrentThreadId();
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

/// 排好的文字留着下次用：同样的文字每一帧都要量、要画，每次现排一遍要做字形分析，一帧上百段文字时很慢。
/// 按文字和样式分组，每组四个，满了换掉最久没用的。
#define LAYOUT_CACHE_SETS 128
#define LAYOUT_CACHE_WAYS 4

struct CachedLayout
{
    UINT32 hash;
    wchar_t *text;
    UINT32 length;
    GfxTextStyle style;
    bool measure;
    float width, height;
    IDWriteTextLayout *layout;
    UINT64 used;
};

static CachedLayout layout_cache[LAYOUT_CACHE_SETS][LAYOUT_CACHE_WAYS];
static UINT64 layout_clock;

static void layout_cache_clear(void)
{
    for (int set = 0; set < LAYOUT_CACHE_SETS; set++)
        for (int way = 0; way < LAYOUT_CACHE_WAYS; way++)
        {
            CachedLayout *entry = &layout_cache[set][way];
            if (entry->layout) entry->layout->Release();
            free(entry->text);
            *entry = {};
        }
}

void gfx_release_text_cache(void)
{
    if (!drawing) layout_cache_clear();
}

/// 变换的栈：gfx_push_transform 叠加，gfx_pop_transform 还原。
#define TRANSFORM_STACK_SIZE 16
static D2D1_MATRIX_3X2_F transform_stack[TRANSFORM_STACK_SIZE];
static int transform_depth;

/// 层的栈：每层是否真的交给了 Direct2D（省掉的层弹出时也不弹）。
#define LAYER_STACK_SIZE 32
static bool layer_pushed[LAYER_STACK_SIZE];
static int layer_depth;

bool gfx_begin(GfxSurface *surface, int width, int height, float dpi)
{
    if (!surface || width <= 0 || height <= 0 || drawing) return false;
    if (!gfx_initialize()) return false;
    release_retired_bitmaps();
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
    layer_depth = 0;
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
    release_retired_bitmaps();
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
    layer_depth = 0;
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

HBITMAP gfx_end_image_bitmap(void)
{
    if (drawing != &image_surface) return nullptr;
    drawing = nullptr;
    HRESULT result = context->EndDraw();
    context->SetTarget(nullptr);

    // 画好的位图在显卡上，先复制到一张处理器能读的位图，再逐行抄进 DIB。
    ID2D1Bitmap1 *readable = nullptr;
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), image_dpi, image_dpi);
    if (SUCCEEDED(result)) result = context->CreateBitmap(D2D1::SizeU((UINT32)image_width, (UINT32)image_height), nullptr, 0, &properties, &readable);
    if (SUCCEEDED(result)) result = readable->CopyFromBitmap(nullptr, image_target, nullptr);
    HBITMAP bitmap = nullptr;
    D2D1_MAPPED_RECT mapped = {};
    if (SUCCEEDED(result)) result = readable->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (SUCCEEDED(result))
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image_width;
        info.bmiHeader.biHeight = -image_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits)
            for (int row = 0; row < image_height; row++)
                memcpy((unsigned char *)bits + (size_t)row * image_width * 4, mapped.bits + (size_t)row * mapped.pitch, (size_t)image_width * 4);
        readable->Unmap();
    }
    release(readable);
    release(image_target);
    if (FAILED(result)) log_error(L"生成拖动图像失败\r\nHRESULT 0x%08lX", (unsigned long)result);
    return bitmap;
}

// ───────────── 图片 ─────────────

struct GfxImage
{
    volatile LONG references;
    int width, height;
    unsigned char *pixels;
    ID2D1Bitmap1 *bitmap;       // 只在界面线程上创建和使用
    unsigned generation;
};

GfxImage *gfx_image_create(const void *pixels, int width, int height, int stride)
{
    if (!pixels || width <= 0 || height <= 0) return nullptr;
    GfxImage *image = (GfxImage *)calloc(1, sizeof(GfxImage));
    if (!image) abort();
    image->references = 1;
    image->width = width;
    image->height = height;
    image->pixels = (unsigned char *)malloc((size_t)width * height * 4);
    if (!image->pixels) abort();
    for (int row = 0; row < height; row++)
        memcpy(image->pixels + (size_t)row * width * 4, (const unsigned char *)pixels + (size_t)row * stride, (size_t)width * 4);
    return image;
}

GfxImage *gfx_image_from_icon(HICON icon)
{
    if (!icon) return nullptr;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *factory = nullptr;
    IWICBitmap *bitmap = nullptr;
    IWICFormatConverter *converter = nullptr;
    GfxImage *image = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void **)&factory);
    if (SUCCEEDED(result)) result = factory->CreateBitmapFromHICON(icon, &bitmap);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result))
        result = converter->Initialize(bitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    UINT width = 0, height = 0;
    if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
    if (SUCCEEDED(result) && width > 0 && height > 0)
    {
        unsigned char *pixels = (unsigned char *)malloc((size_t)width * height * 4);
        if (!pixels) abort();
        if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, width * height * 4, pixels))) image = gfx_image_create(pixels, (int)width, (int)height, (int)width * 4);
        free(pixels);
    }
    release(converter);
    release(bitmap);
    release(factory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return image;
}

GfxImage *gfx_image_retain(GfxImage *image)
{
    if (image) InterlockedIncrement(&image->references);
    return image;
}

void gfx_image_release(GfxImage *image)
{
    if (!image || InterlockedDecrement(&image->references) > 0) return;
    // 显卡上的位图只在界面线程上建过，也只能在界面线程上释放；在别的线程上时交给界面线程下次开始画时释放。
    if (image->bitmap)
    {
        if (GetCurrentThreadId() == ui_thread) release(image->bitmap);
        else retire_bitmap(image->bitmap);
        image->bitmap = nullptr;
    }
    free(image->pixels);
    free(image);
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
    if (!drawing || !image || opacity <= 0) return;
    if (image->bitmap && image->generation != device_generation) release(image->bitmap);
    if (!image->bitmap)
    {
        D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(context->CreateBitmap(D2D1::SizeU((UINT32)image->width, (UINT32)image->height), image->pixels, (UINT32)image->width * 4,
                &properties, &image->bitmap)))
            return;
        image->generation = device_generation;
    }
    D2D1_RECT_F destination = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
    context->DrawBitmap(image->bitmap, &destination, opacity > 1 ? 1 : opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
}

void gfx_trim(void)
{
    if (drawing || !context) return;
    release_retired_bitmaps();
    // Direct2D 自己留着的中间位图、字形缓存等；用到时会重新建。
    d2d_device->ClearResources(0);
    // 再让显卡驱动把替程序留着、准备下次用的内存交回去（Intel 核显的驱动用过的中间位图不主动还）。
    // 调用前要先清掉设备上下文绑定的状态并提交。
    ID3D11DeviceContext *immediate = nullptr;
    d3d_device->GetImmediateContext(&immediate);
    if (immediate)
    {
        immediate->ClearState();
        immediate->Flush();
    }
    release(immediate);
    IDXGIDevice3 *device3 = nullptr;
    if (SUCCEEDED(dxgi_device->QueryInterface(__uuidof(IDXGIDevice3), (void **)&device3))) device3->Trim();
    release(device3);
}

void gfx_shutdown(void)
{
    release_retired_bitmaps();
    layout_cache_clear();
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

GfxMatrix gfx_matrix_rotate(float degrees, float center_x, float center_y)
{
    float radians = degrees * 3.14159265358979f / 180.0f;
    float c = cosf(radians), s = sinf(radians);
    // 绕中心转：先挪到原点，转，再挪回去。y 轴朝下，正的角度是顺时针。
    GfxMatrix matrix = { c, s, -s, c, center_x - c * center_x + s * center_y, center_y - s * center_x - c * center_y };
    return matrix;
}

void gfx_matrix_apply(GfxMatrix matrix, float *x, float *y)
{
    float px = *x, py = *y;
    *x = px * matrix.m11 + py * matrix.m21 + matrix.dx;
    *y = px * matrix.m12 + py * matrix.m22 + matrix.dy;
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

void gfx_stroke_rounded_dashed(GfxRect rect, float radius, Color color, float width, float dash, float gap)
{
    if (!drawing || color.a == 0 || width <= 0) return;
    // 用到的虚线只有两三种，按样式缓存。
    static struct { float dash, gap; ID2D1StrokeStyle *style; } styles[4];
    static int style_count;
    ID2D1StrokeStyle *style = nullptr;
    bool cached = false;
    for (int index = 0; index < style_count && !style; index++)
        if (styles[index].dash == dash && styles[index].gap == gap) style = styles[index].style;
    cached = style != nullptr;
    if (!style)
    {
        float dashes[2] = { dash, gap };
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
            D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
        if (FAILED(d2d_factory->CreateStrokeStyle(properties, dashes, 2, &style))) return;
        if (style_count < 4)
        {
            styles[style_count++] = { dash, gap, style };
            cached = true;
        }
    }
    float inset = width / 2;
    D2D1_RECT_F inner = D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
    float inner_radius = radius > inset ? radius - inset : 0;
    context->DrawRoundedRectangle(D2D1::RoundedRect(inner, inner_radius, inner_radius), use_color(color), width, style);
    if (!cached) style->Release();
}

void gfx_arc(float center_x, float center_y, float radius, float start_degrees, float sweep_degrees, Color color, float width)
{
    if (!drawing || color.a == 0 || width <= 0 || sweep_degrees == 0) return;
    static ID2D1StrokeStyle *round_caps;
    if (!round_caps)
    {
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        if (FAILED(d2d_factory->CreateStrokeStyle(properties, nullptr, 0, &round_caps))) return;
    }
    ID2D1PathGeometry *path = nullptr;
    ID2D1GeometrySink *sink = nullptr;
    if (FAILED(d2d_factory->CreatePathGeometry(&path))) return;
    if (SUCCEEDED(path->Open(&sink)))
    {
        float start = start_degrees * 3.14159265358979f / 180.0f, end = (start_degrees + sweep_degrees) * 3.14159265358979f / 180.0f;
        sink->BeginFigure(D2D1::Point2F(center_x + radius * cosf(start), center_y + radius * sinf(start)), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(center_x + radius * cosf(end), center_y + radius * sinf(end)), D2D1::SizeF(radius, radius), 0,
            sweep_degrees > 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
            fabsf(sweep_degrees) > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        release(sink);
        context->DrawGeometry(path, use_color(color), width, round_caps);
    }
    release(path);
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

// ───────────── 阴影 ─────────────
// 阴影要做一次高斯模糊，每帧每个阴影现算一遍很费显卡，模糊用的中间位图也会被驱动留着不还。
// 圆角矩形的阴影可以按九宫格拼：四个角各是一块固定的图，四条边沿着边的方向处处相同、拉长即可，中间是纯色。
// 所以只要按圆角、模糊半径、颜色和像素比例画一次“模板”（一个刚好够拼的小圆角矩形的阴影），之后每次贴九块图。
// 模板和阴影的大小无关，很小；透明度在贴图时乘上，也不在键里。

struct ShadowTemplate
{
    float radius, blur, scale;  // scale：模板每 DIP 几个像素
    BYTE r, g, b;
    int pad;                    // 矩形外面留给模糊的宽度（像素）
    int corner;                 // 矩形里面受圆角影响的宽度（像素），角上的一块是 pad + corner 见方
    ID2D1Bitmap1 *bitmap;
    unsigned generation;
    UINT64 used;
};

#define SHADOW_TEMPLATES 16
#define SHADOW_MIDDLE 4         // 模板中间留几个像素：边上沿着边的方向处处相同，拉长的就是这一段

static ShadowTemplate shadow_templates[SHADOW_TEMPLATES];
static UINT64 shadow_clock;
static bool shadow_templates_enabled = true;

static void release_shadow_templates(void)
{
    for (int index = 0; index < SHADOW_TEMPLATES; index++)
    {
        release(shadow_templates[index].bitmap);
        shadow_templates[index] = {};
    }
}

void gfx_set_shadow_templates(bool enabled)
{
    shadow_templates_enabled = enabled;
}

int gfx_shadow_template_count(void)
{
    int count = 0;
    for (int index = 0; index < SHADOW_TEMPLATES; index++)
        if (shadow_templates[index].bitmap && shadow_templates[index].generation == device_generation) count++;
    return count;
}

/// 在模板用的上下文里画出这个阴影模板：先把圆角矩形录进命令列表，再交给阴影效果画进一张位图。
static ID2D1Bitmap1 *draw_shadow_template(float radius, float blur, float scale, Color color, int pad, int corner)
{
    int side = 2 * corner + SHADOW_MIDDLE;
    int size = 2 * pad + side;
    ID2D1CommandList *shape = nullptr;
    ID2D1Bitmap1 *bitmap = nullptr;
    HRESULT result = template_context->CreateCommandList(&shape);
    if (SUCCEEDED(result))
    {
        template_context->SetTarget(shape);
        template_context->BeginDraw();
        template_context->SetTransform(D2D1::Matrix3x2F::Identity());
        template_context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, (float)side, (float)side), radius * scale, radius * scale),
            template_brush);
        result = template_context->EndDraw();
        template_context->SetTarget(nullptr);
    }
    if (SUCCEEDED(result)) result = shape->Close();
    if (SUCCEEDED(result))
    {
        D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        result = template_context->CreateBitmap(D2D1::SizeU((UINT32)size, (UINT32)size), nullptr, 0, &properties, &bitmap);
    }
    if (SUCCEEDED(result))
    {
        template_context->SetTarget(bitmap);
        template_context->BeginDraw();
        template_context->SetTransform(D2D1::Matrix3x2F::Identity());
        template_context->Clear(D2D1::ColorF(0, 0, 0, 0));
        template_shadow->SetInput(0, shape);
        // 模糊半径约为标准差的三倍。
        template_shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur * scale / 3.0f);
        template_shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f));
        template_context->DrawImage(template_shadow, D2D1::Point2F((float)pad, (float)pad));
        template_shadow->SetInput(0, nullptr);
        result = template_context->EndDraw();
        template_context->SetTarget(nullptr);
    }
    release(shape);
    if (FAILED(result))
    {
        log_error(L"画阴影模板失败\r\nHRESULT 0x%08lX", (unsigned long)result);
        release(bitmap);
    }
    return bitmap;
}

static ShadowTemplate *shadow_template(float radius, float blur, float scale, Color color)
{
    ShadowTemplate *oldest = &shadow_templates[0];
    for (int index = 0; index < SHADOW_TEMPLATES; index++)
    {
        ShadowTemplate *entry = &shadow_templates[index];
        if (entry->bitmap && entry->generation != device_generation)
        {
            release(entry->bitmap);
            *entry = {};
        }
        if (entry->bitmap && entry->radius == radius && entry->blur == blur && entry->scale == scale &&
            entry->r == color.r && entry->g == color.g && entry->b == color.b)
        {
            entry->used = ++shadow_clock;
            return entry;
        }
        if (!entry->bitmap || (oldest->bitmap && entry->used < oldest->used)) oldest = entry;
    }
    // 模糊在矩形外最多扩出约三倍标准差（即 blur），再多留一个像素；圆角加上模糊的范围以内都受角的影响。
    int pad = (int)ceilf(blur * scale) + 1;
    int corner = (int)ceilf((radius + blur) * scale) + 1;
    ID2D1Bitmap1 *bitmap = draw_shadow_template(radius, blur, scale, color, pad, corner);
    if (!bitmap) return nullptr;
    release(oldest->bitmap);
    *oldest = { radius, blur, scale, color.r, color.g, color.b, pad, corner, bitmap, device_generation, ++shadow_clock };
    return oldest;
}

/// 每次现算阴影：模板用不了时（太小、拼不开或者模板画不出来）。
static void draw_shadow_effect(GfxRect rect, float radius, float blur, float offset_x, float offset_y, Color color)
{
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

void gfx_shadow(GfxRect rect, float radius, float blur, float offset_x, float offset_y, Color color)
{
    if (!drawing || color.a == 0) return;
    if (!shadow_templates_enabled || !template_context || blur <= 0 || radius < 0)
    {
        draw_shadow_effect(rect, radius, blur, offset_x, offset_y, color);
        return;
    }
    // 模板的像素比例：DPI 乘上当前变换的缩放（放大很多时按整数倍画得更细，避免贴图发虚）。
    float dpi_x, dpi_y;
    context->GetDpi(&dpi_x, &dpi_y);
    D2D1_MATRIX_3X2_F transform;
    context->GetTransform(&transform);
    float zoom = sqrtf(fabsf(transform._11 * transform._22 - transform._12 * transform._21));
    float scale = dpi_x / 96.0f * (zoom <= 1.25f ? 1.0f : ceilf(zoom));
    ShadowTemplate *entry = nullptr;
    float width = rect.right - rect.left, height = rect.bottom - rect.top;
    int corner = (int)ceilf((radius + blur) * scale) + 1;
    // 矩形要够大，四个角才拼得开。
    if (width * scale >= 2 * corner && height * scale >= 2 * corner) entry = shadow_template(radius, blur, scale, color);
    if (!entry)
    {
        draw_shadow_effect(rect, radius, blur, offset_x, offset_y, color);
        return;
    }

    // 九块的分界：目标在 DIP（经过当前变换），来源在模板的像素。
    float unit = 1.0f / scale;
    float left = rect.left + offset_x, top = rect.top + offset_y, right = rect.right + offset_x, bottom = rect.bottom + offset_y;
    float xs[4] = { left - entry->pad * unit, left + entry->corner * unit, right - entry->corner * unit, right + entry->pad * unit };
    float ys[4] = { top - entry->pad * unit, top + entry->corner * unit, bottom - entry->corner * unit, bottom + entry->pad * unit };
    float near_edge = (float)(entry->pad + entry->corner), far_edge = near_edge + SHADOW_MIDDLE;
    float size = far_edge + entry->pad + entry->corner;
    float us[4] = { 0, near_edge, far_edge, size };
    // 九块贴在一起：不抗锯齿，相邻两块的接缝就不会因为各自的半透明边缘透出一条细线；最外圈本来就几乎全透明。
    D2D1_ANTIALIAS_MODE antialias = context->GetAntialiasMode();
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    float opacity = color.a / 255.0f;
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 3; column++)
        {
            if (xs[column + 1] <= xs[column] || ys[row + 1] <= ys[row]) continue;
            D2D1_RECT_F destination = D2D1::RectF(xs[column], ys[row], xs[column + 1], ys[row + 1]);
            D2D1_RECT_F source = D2D1::RectF(us[column], us[row], us[column + 1], us[row + 1]);
            context->DrawBitmap(entry->bitmap, &destination, opacity, D2D1_INTERPOLATION_MODE_LINEAR, &source, nullptr);
        }
    context->SetAntialiasMode(antialias);
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
    // 不裁剪、不透明的层画出来和不开层一样，却要一张和窗口一样大的中间位图（显卡驱动用过以后不还给系统），不开。
    bool clips = rect.right > rect.left && rect.bottom > rect.top;
    bool pushed = clips || opacity < 0.999f;
    if (layer_depth < LAYER_STACK_SIZE) layer_pushed[layer_depth] = pushed;
    layer_depth++;
    if (!pushed) return;
    ID2D1Geometry *mask = nullptr;
    D2D1_RECT_F bounds = D2D1::InfiniteRect();
    if (clips)
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
    if (!drawing || layer_depth <= 0) return;
    layer_depth--;
    if (layer_depth < LAYER_STACK_SIZE && !layer_pushed[layer_depth]) return;
    context->PopLayer();
}

void gfx_fill_rounded_replace(GfxRect rect, float radius, GfxMatrix matrix, Color color)
{
    if (!drawing) return;
    D2D1_MATRIX_3X2_F current;
    context->GetTransform(&current);
    D2D1::Matrix3x2F local(matrix.m11, matrix.m12, matrix.m21, matrix.m22, matrix.dx, matrix.dy);
    context->SetTransform(local * (*D2D1::Matrix3x2F::ReinterpretBaseType(&current)));
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    context->FillRoundedRectangle(D2D1::RoundedRect(to_rect(rect), radius, radius), use_color(color));
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    context->SetTransform(current);
}

bool gfx_matrix_invert(GfxMatrix matrix, GfxMatrix *inverse)
{
    float determinant = matrix.m11 * matrix.m22 - matrix.m12 * matrix.m21;
    if (fabsf(determinant) < 1e-9f) return false;
    float a = matrix.m22 / determinant, b = -matrix.m12 / determinant;
    float c = -matrix.m21 / determinant, d = matrix.m11 / determinant;
    *inverse = { a, b, c, d, -(matrix.dx * a + matrix.dy * c), -(matrix.dx * b + matrix.dy * d) };
    return true;
}

// ───────────── 渐变和路径 ─────────────

static ID2D1GradientStopCollection *make_stops(const GfxStop *stops, int count)
{
    if (count <= 0 || count > 8) return nullptr;
    D2D1_GRADIENT_STOP converted[8];
    for (int index = 0; index < count; index++)
    {
        Color color = stops[index].color;
        converted[index] = { stops[index].offset, D2D1::ColorF(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f) };
    }
    ID2D1GradientStopCollection *collection = nullptr;
    context->CreateGradientStopCollection(converted, (UINT32)count, &collection);
    return collection;
}

void gfx_fill_rounded_radial(GfxRect rect, float radius, float center_x, float center_y, float radius_x, float radius_y,
    const GfxStop *stops, int count)
{
    if (!drawing || radius_x <= 0 || radius_y <= 0) return;
    ID2D1GradientStopCollection *collection = make_stops(stops, count);
    ID2D1RadialGradientBrush *gradient = nullptr;
    if (collection && SUCCEEDED(context->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(center_x, center_y), D2D1::Point2F(0, 0), radius_x, radius_y), collection, &gradient)))
    {
        if (radius <= 0) context->FillRectangle(to_rect(rect), gradient);
        else context->FillRoundedRectangle(D2D1::RoundedRect(to_rect(rect), radius, radius), gradient);
    }
    release(gradient);
    release(collection);
}

void gfx_fill_rect_linear(GfxRect rect, float x1, float y1, float x2, float y2, const GfxStop *stops, int count)
{
    if (!drawing) return;
    ID2D1GradientStopCollection *collection = make_stops(stops, count);
    ID2D1LinearGradientBrush *gradient = nullptr;
    if (collection && SUCCEEDED(context->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2)), collection, &gradient)))
        context->FillRectangle(to_rect(rect), gradient);
    release(gradient);
    release(collection);
}

/// 上面两个角半径为 top、下面两个角为 bottom 的圆角矩形路径；open_top 时不画上沿（描边用）。
static ID2D1PathGeometry *corner_path(D2D1_RECT_F rect, float top, float bottom, bool open_top, bool filled)
{
    float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return nullptr;
    float limit = (width < height ? width : height) / 2;
    if (top > limit) top = limit;
    if (bottom > limit) bottom = limit;
    if (top < 0) top = 0;
    if (bottom < 0) bottom = 0;
    ID2D1PathGeometry *path = nullptr;
    ID2D1GeometrySink *sink = nullptr;
    if (FAILED(d2d_factory->CreatePathGeometry(&path))) return nullptr;
    if (FAILED(path->Open(&sink)))
    {
        release(path);
        return nullptr;
    }
    D2D1_FIGURE_BEGIN begin = filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW;
    if (open_top)
    {
        // 从左上沿顺时针反着走：左边往下、底边、右边往上。
        sink->BeginFigure(D2D1::Point2F(rect.left, rect.top + top), begin);
        if (top > 0) sink->AddLine(D2D1::Point2F(rect.left, rect.top + top));
    }
    else
    {
        sink->BeginFigure(D2D1::Point2F(rect.left + top, rect.top), begin);
        sink->AddLine(D2D1::Point2F(rect.right - top, rect.top));
        if (top > 0) sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.right, rect.top + top), D2D1::SizeF(top, top), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - bottom));
        if (bottom > 0) sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.right - bottom, rect.bottom), D2D1::SizeF(bottom, bottom), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(rect.left + bottom, rect.bottom));
        if (bottom > 0) sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.left, rect.bottom - bottom), D2D1::SizeF(bottom, bottom), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(rect.left, rect.top + top));
        if (top > 0) sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.left + top, rect.top), D2D1::SizeF(top, top), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        release(sink);
        return path;
    }
    sink->AddLine(D2D1::Point2F(rect.left, rect.bottom - bottom));
    if (bottom > 0) sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.left + bottom, rect.bottom), D2D1::SizeF(bottom, bottom), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(rect.right - bottom, rect.bottom));
    if (bottom > 0) sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.right, rect.bottom - bottom), D2D1::SizeF(bottom, bottom), 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(rect.right, rect.top + top));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    release(sink);
    return path;
}

void gfx_fill_rounded_corners(GfxRect rect, float top, float bottom, Color color)
{
    if (!drawing || color.a == 0) return;
    ID2D1PathGeometry *path = corner_path(to_rect(rect), top, bottom, false, true);
    if (path) context->FillGeometry(path, use_color(color));
    release(path);
}

void gfx_stroke_rounded_corners(GfxRect rect, float top, float bottom, Color color, float width)
{
    if (!drawing || color.a == 0 || width <= 0) return;
    float inset = width / 2;
    D2D1_RECT_F inner = D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
    ID2D1PathGeometry *path = corner_path(inner, top - inset, bottom - inset, false, false);
    if (path) context->DrawGeometry(path, use_color(color), width);
    release(path);
}

void gfx_stroke_rounded_open_top(GfxRect rect, float top, float bottom, Color color, float width)
{
    if (!drawing || color.a == 0 || width <= 0) return;
    float inset = width / 2;
    D2D1_RECT_F inner = D2D1::RectF(rect.left + inset, rect.top, rect.right - inset, rect.bottom - inset);
    ID2D1PathGeometry *path = corner_path(inner, top - inset, bottom - inset, true, false);
    if (path) context->DrawGeometry(path, use_color(color), width);
    release(path);
}

// ───────────── 图案 ─────────────
// 斜格纹按一格画一张小位图，平铺成画刷；条纹是重复的线性渐变。画刷按图案和缩放比例留几份，设备重建时一起释放。

struct PatternBrush
{
    ThemePattern pattern;
    float dpi;
    ID2D1Brush *brush;
};

#define PATTERN_CACHE_SIZE 6
static PatternBrush pattern_cache[PATTERN_CACHE_SIZE];
static int pattern_next;

static void release_pattern_brushes(void)
{
    for (int index = 0; index < PATTERN_CACHE_SIZE; index++) release(pattern_cache[index].brush);
    memset(pattern_cache, 0, sizeof(pattern_cache));
    pattern_next = 0;
}

static D2D1_COLOR_F color_f(Color color)
{
    return D2D1::ColorF(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
}

/// 一格斜格纹：底色上画两条对角线，平铺后连成菱形格子。在模板的上下文里画，正在画的这一帧不受影响。
static ID2D1Brush *create_lattice(const ThemePattern *pattern, float dpi)
{
    if (!template_context || pattern->size <= 0) return nullptr;
    // 按显示比例取整到像素，再把位图的 DPI 调到正好是 size 个 DIP 宽。
    UINT32 pixels = (UINT32)lroundf(pattern->size * dpi / 96);
    if (pixels < 2) pixels = 2;
    float tile_dpi = pixels * 96.0f / pattern->size;
    ID2D1Bitmap1 *tile = nullptr;
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), tile_dpi, tile_dpi);
    if (FAILED(template_context->CreateBitmap(D2D1::SizeU(pixels, pixels), nullptr, 0, &properties, &tile))) return nullptr;
    float size = pattern->size;
    template_context->SetTarget(tile);
    template_context->SetDpi(tile_dpi, tile_dpi);
    template_context->BeginDraw();
    template_context->SetTransform(D2D1::Matrix3x2F::Identity());
    Color fill = pattern->fill;
    float alpha = fill.a / 255.0f;
    template_context->Clear(D2D1::ColorF(fill.r / 255.0f * alpha, fill.g / 255.0f * alpha, fill.b / 255.0f * alpha, alpha));
    template_brush->SetColor(color_f(pattern->line));
    template_context->DrawLine(D2D1::Point2F(0, 0), D2D1::Point2F(size, size), template_brush, 1);
    template_context->DrawLine(D2D1::Point2F(size, 0), D2D1::Point2F(0, size), template_brush, 1);
    HRESULT result = template_context->EndDraw();
    template_context->SetTarget(nullptr);
    template_context->SetDpi(96, 96);
    ID2D1BitmapBrush1 *tiled = nullptr;
    if (SUCCEEDED(result))
        context->CreateBitmapBrush(tile, D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &tiled);
    release(tile);
    return tiled;
}

/// 45° 的条纹，两种颜色各 2 DIP 宽：重复的线性渐变，从 (0, 0) 到 (2.83, 2.83) 正好一个周期。
static ID2D1Brush *create_stripes(const ThemePattern *pattern)
{
    D2D1_GRADIENT_STOP stops[4] = {
        { 0, color_f(pattern->fill) }, { 0.5f, color_f(pattern->fill) }, { 0.5f, color_f(pattern->line) }, { 1, color_f(pattern->line) }
    };
    ID2D1GradientStopCollection *collection = nullptr;
    ID2D1LinearGradientBrush *gradient = nullptr;
    if (SUCCEEDED(context->CreateGradientStopCollection(stops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_WRAP, &collection)))
        context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(2.83f, 2.83f)), collection, &gradient);
    release(collection);
    return gradient;
}

static bool same_pattern(const ThemePattern *a, const ThemePattern *b)
{
    return a->kind == b->kind && a->size == b->size && color_equal(a->fill, b->fill) && color_equal(a->line, b->line);
}

/// 图案的画刷，从 (x, y) 开始铺；建不了时返回 NULL，调用方改用底色。
static ID2D1Brush *use_pattern(const ThemePattern *pattern, float x, float y)
{
    float dpi_x, dpi_y;
    context->GetDpi(&dpi_x, &dpi_y);
    float dpi = pattern->kind == THEME_PATTERN_LATTICE ? dpi_x : 0;
    ID2D1Brush *found = nullptr;
    for (int index = 0; index < PATTERN_CACHE_SIZE && !found; index++)
        if (pattern_cache[index].brush && pattern_cache[index].dpi == dpi && same_pattern(&pattern_cache[index].pattern, pattern))
            found = pattern_cache[index].brush;
    if (!found)
    {
        found = pattern->kind == THEME_PATTERN_LATTICE ? create_lattice(pattern, dpi) : create_stripes(pattern);
        if (!found) return nullptr;
        PatternBrush *slot = &pattern_cache[pattern_next];
        pattern_next = (pattern_next + 1) % PATTERN_CACHE_SIZE;
        release(slot->brush);
        *slot = { *pattern, dpi, found };
    }
    found->SetTransform(D2D1::Matrix3x2F::Translation(x, y));
    return found;
}

void gfx_fill_rounded_pattern(GfxRect rect, float radius, const ThemePattern *pattern)
{
    if (!drawing || rect.right <= rect.left || rect.bottom <= rect.top) return;
    ID2D1Brush *fill = use_pattern(pattern, rect.left, rect.top);
    if (!fill) fill = use_color(pattern->fill);
    if (radius <= 0) context->FillRectangle(to_rect(rect), fill);
    else context->FillRoundedRectangle(D2D1::RoundedRect(to_rect(rect), radius, radius), fill);
}

void gfx_stroke_rounded_pattern(GfxRect rect, float radius, const ThemePattern *pattern, float width)
{
    if (!drawing || width <= 0 || rect.right <= rect.left || rect.bottom <= rect.top) return;
    ID2D1Brush *stroke = use_pattern(pattern, rect.left, rect.top);
    if (!stroke) stroke = use_color(pattern->fill);
    float inset = width / 2;
    D2D1_RECT_F inner = D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
    if (inner.right < inner.left || inner.bottom < inner.top) return;
    float inner_radius = radius > inset ? radius - inset : 0;
    if (inner_radius <= 0) context->DrawRectangle(inner, stroke, width);
    else context->DrawRoundedRectangle(D2D1::RoundedRect(inner, inner_radius, inner_radius), stroke, width);
}

void gfx_fill_rounded_corners_pattern(GfxRect rect, float top, float bottom, const ThemePattern *pattern)
{
    if (!drawing) return;
    ID2D1PathGeometry *path = corner_path(to_rect(rect), top, bottom, false, true);
    ID2D1Brush *fill = path ? use_pattern(pattern, rect.left, rect.top) : nullptr;
    if (path) context->FillGeometry(path, fill ? fill : use_color(pattern->fill));
    release(path);
}

static ID2D1StrokeStyle *round_stroke(void)
{
    static ID2D1StrokeStyle *style;
    if (!style)
    {
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND);
        d2d_factory->CreateStrokeStyle(properties, nullptr, 0, &style);
    }
    return style;
}

static ID2D1PathGeometry *points_path(const float *points, int count, bool closed)
{
    if (count < 2) return nullptr;
    ID2D1PathGeometry *path = nullptr;
    ID2D1GeometrySink *sink = nullptr;
    if (FAILED(d2d_factory->CreatePathGeometry(&path))) return nullptr;
    if (FAILED(path->Open(&sink)))
    {
        release(path);
        return nullptr;
    }
    sink->BeginFigure(D2D1::Point2F(points[0], points[1]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (int index = 1; index < count; index++) sink->AddLine(D2D1::Point2F(points[index * 2], points[index * 2 + 1]));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    release(sink);
    return path;
}

void gfx_polygon(const float *points, int count, Color fill, Color stroke, float stroke_width)
{
    if (!drawing) return;
    ID2D1PathGeometry *path = points_path(points, count, true);
    if (!path) return;
    if (fill.a) context->FillGeometry(path, use_color(fill));
    if (stroke.a && stroke_width > 0) context->DrawGeometry(path, use_color(stroke), stroke_width, round_stroke());
    release(path);
}

void gfx_polyline(const float *points, int count, Color color, float width)
{
    if (!drawing || color.a == 0 || width <= 0) return;
    ID2D1PathGeometry *path = points_path(points, count, false);
    if (path) context->DrawGeometry(path, use_color(color), width, round_stroke());
    release(path);
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
    if (font == GFX_FONT_NUMBER || font == GFX_FONT_RANK)
    {
        static const wchar_t *numbers;
        if (!numbers) numbers = font_exists(L"Bahnschrift") ? L"Bahnschrift" : L"Segoe UI";
        return numbers;
    }
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
    DWRITE_FONT_STRETCH stretch = font == GFX_FONT_RANK ? DWRITE_FONT_STRETCH_CONDENSED : DWRITE_FONT_STRETCH_NORMAL;
    if (FAILED(dwrite_factory->CreateTextFormat(font_family(font), nullptr, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL,
            stretch, size, locale, &format)))
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

static IDWriteTextLayout *create_layout(const wchar_t *text, UINT32 length, const GfxTextStyle *style, bool measure, float width, float height)
{
    IDWriteTextFormat *format = text_format(style->font, style->size, style->weight > 0 ? style->weight : 400);
    if (!format) return nullptr;
    IDWriteTextLayout *layout = nullptr;
    if (FAILED(dwrite_factory->CreateTextLayout(text, length, format, width, height, &layout))) return nullptr;
    // 和 WPF 一样，一个词比整行还长时在词中间折行。
    layout->SetWordWrapping(style->wrap ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (style->line_height > 0) layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, style->line_height, style->line_height * 0.8f);
    // 量的时候一律靠左上，否则居中的文字会把整个排版范围算进去。
    layout->SetTextAlignment(measure ? DWRITE_TEXT_ALIGNMENT_LEADING : text_alignment(style->align));
    layout->SetParagraphAlignment(measure ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : paragraph_alignment(style->vertical));
    if (style->ellipsis)
    {
        // 省略号的大小跟着字号走，所以按这段文字的格式现做一个。
        IDWriteInlineObject *sign = nullptr;
        dwrite_factory->CreateEllipsisTrimmingSign(format, &sign);
        DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        layout->SetTrimming(&trimming, sign);
        release(sign);
    }
    return layout;
}

static bool same_style(const GfxTextStyle *a, const GfxTextStyle *b)
{
    return a->font == b->font && a->size == b->size && a->weight == b->weight && a->wrap == b->wrap && a->ellipsis == b->ellipsis &&
           a->align == b->align && a->vertical == b->vertical && a->line_height == b->line_height;
}

static UINT32 layout_hash(const wchar_t *text, UINT32 length, const GfxTextStyle *style, bool measure)
{
    UINT32 hash = 2166136261u;
    for (UINT32 index = 0; index < length; index++) hash = (hash ^ text[index]) * 16777619u;
    UINT32 bits;
    memcpy(&bits, &style->size, sizeof(bits));
    UINT32 fields[] = { (UINT32)style->font, bits, (UINT32)style->weight, (UINT32)style->wrap | (UINT32)style->ellipsis << 1 | (UINT32)measure << 2,
        (UINT32)style->align, (UINT32)style->vertical };
    for (UINT32 field : fields) hash = (hash ^ field) * 16777619u;
    return hash;
}

/// 排好一段文字。width、height 是排版的范围，不限制时传很大的数。measure 为真时一律靠左上。
/// 返回的排版归缓存所有，调用的地方不要释放，也不要改它的属性。
static IDWriteTextLayout *text_layout(const wchar_t *text, const GfxTextStyle *style, float width, float height, bool measure)
{
    if (!text || !style || (!dwrite_factory && !gfx_initialize())) return nullptr;
    UINT32 length = (UINT32)wcslen(text);
    UINT32 hash = layout_hash(text, length, style, measure);
    CachedLayout *set = layout_cache[hash % LAYOUT_CACHE_SETS];
    CachedLayout *oldest = &set[0];
    for (int way = 0; way < LAYOUT_CACHE_WAYS; way++)
    {
        CachedLayout *entry = &set[way];
        if (entry->layout && entry->hash == hash && entry->length == length && entry->measure == measure && same_style(&entry->style, style) &&
            memcmp(entry->text, text, length * sizeof(wchar_t)) == 0)
        {
            // 排版范围变了只重新断行，字形分析的结果留着。
            if (entry->width != width) entry->layout->SetMaxWidth(width);
            if (entry->height != height) entry->layout->SetMaxHeight(height);
            entry->width = width;
            entry->height = height;
            entry->used = ++layout_clock;
            return entry->layout;
        }
        if (!entry->layout || (oldest->layout && entry->used < oldest->used)) oldest = entry;
    }

    IDWriteTextLayout *layout = create_layout(text, length, style, measure, width, height);
    if (!layout) return nullptr;
    if (oldest->layout) oldest->layout->Release();
    free(oldest->text);
    wchar_t *copy = (wchar_t *)malloc(((size_t)length + 1) * sizeof(wchar_t));
    if (!copy) abort();
    memcpy(copy, text, ((size_t)length + 1) * sizeof(wchar_t));
    *oldest = { hash, copy, length, *style, measure, width, height, layout, ++layout_clock };
    return layout;
}

GfxSize gfx_text_measure(const wchar_t *text, const GfxTextStyle *style, float max_width)
{
    GfxSize size = { 0, 0 };
    IDWriteTextLayout *layout = text_layout(text, style, max_width > 0 ? max_width : 100000.0f, 100000.0f, true);
    if (!layout) return size;
    DWRITE_TEXT_METRICS metrics;
    if (SUCCEEDED(layout->GetMetrics(&metrics)))
    {
        // 往上取整：按量出来的宽度再排一次时，不会因为小数的误差多折一行。
        size.width = ceilf(metrics.widthIncludingTrailingWhitespace);
        size.height = ceilf(metrics.height);
    }
    return size;
}

void gfx_text_draw(const wchar_t *text, GfxRect rect, const GfxTextStyle *style, Color color)
{
    if (!drawing || color.a == 0 || !text || !*text) return;
    float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    IDWriteTextLayout *layout = text_layout(text, style, width, height, false);
    if (!layout) return;
    context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, use_color(color), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

/// 输入框用的单行排版：一律靠左、不换行、不省略。
static IDWriteTextLayout *line_layout(const wchar_t *text, const GfxTextStyle *style)
{
    GfxTextStyle line = *style;
    line.wrap = false;
    line.ellipsis = false;
    line.align = GFX_ALIGN_START;
    line.vertical = GFX_ALIGN_START;
    return text_layout(text ? text : L"", &line, 100000.0f, 100000.0f, false);
}

float gfx_text_caret_x(const wchar_t *text, const GfxTextStyle *style, int index)
{
    if (!text || index <= 0) return 0;
    IDWriteTextLayout *layout = line_layout(text, style);
    if (!layout) return 0;
    int length = (int)wcslen(text);
    if (index > length) index = length;
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS metrics;
    layout->HitTestTextPosition((UINT32)index, FALSE, &x, &y, &metrics);
    return x;
}

int gfx_text_hit(const wchar_t *text, const GfxTextStyle *style, float x)
{
    if (!text || !*text) return 0;
    IDWriteTextLayout *layout = line_layout(text, style);
    if (!layout) return 0;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics;
    layout->HitTestPoint(x, 1, &trailing, &inside, &metrics);
    int index = (int)metrics.textPosition + (trailing ? (int)metrics.length : 0);
    int length = (int)wcslen(text);
    return index < 0 ? 0 : index > length ? length : index;
}
