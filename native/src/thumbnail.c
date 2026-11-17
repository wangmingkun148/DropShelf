#define COBJMACROS
#include "thumbnail.h"
#include "task.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_CAPACITY 32

typedef struct CacheEntry
{
    wchar_t *key;
    GfxImage *image;
} CacheEntry;

// 只在图片处理线程上访问；最近用过的排在前面。
static CacheEntry cache[CACHE_CAPACITY];
static int cache_count;

typedef struct ThumbnailJob
{
    wchar_t *path;
    int size;
    volatile LONG *cancelled;
    ThumbnailReady ready;
    void *context;
    GfxImage *image;
} ThumbnailJob;

static GfxImage *from_bitmap(HBITMAP bitmap)
{
    BITMAP info;
    if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0) return NULL;
    int width = info.bmWidth, height = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
    BITMAPINFO header = { .bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB } };
    unsigned char *pixels = xalloc((size_t)width * height * 4);
    HDC screen = GetDC(NULL);
    int rows = GetDIBits(screen, bitmap, 0, (UINT)height, pixels, &header, DIB_RGB_COLORS);
    ReleaseDC(NULL, screen);
    if (rows != height)
    {
        free(pixels);
        return NULL;
    }
    // 照片这类不带透明度的缩略图，透明度通道可能全是 0；按透明度显示会完全看不见，这时当作不透明的图片。
    bool empty_alpha = true;
    size_t count = (size_t)width * height;
    for (size_t index = 0; index < count && empty_alpha; index++) empty_alpha = pixels[index * 4 + 3] == 0;
    if (empty_alpha)
        for (size_t index = 0; index < count; index++) pixels[index * 4 + 3] = 255;
    GfxImage *image = gfx_image_create(pixels, width, height, width * 4);
    free(pixels);
    return image;
}

static GfxImage *load_from_windows(const wchar_t *path, int size)
{
    IShellItemImageFactory *factory = NULL;
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void **)&factory)) || !factory) return NULL;
    HBITMAP bitmap = NULL;
    SIZE wanted = { size, size };
    // 不要更大的图：系统按需要的尺寸缩好，缓存里不留用不到的像素。
    HRESULT result = IShellItemImageFactory_GetImage(factory, wanted, SIIGBF_RESIZETOFIT, &bitmap);
    IShellItemImageFactory_Release(factory);
    if (FAILED(result) || !bitmap) return NULL;
    GfxImage *image = from_bitmap(bitmap);
    DeleteObject(bitmap);
    return image;
}

static void load(void *data)
{
    ThumbnailJob *job = data;
    // 排队期间已经不要了（滚过去了、置物架关了）：外壳取一张缩略图要几毫秒到上百毫秒，
    // 快速滚过很长的列表时排进来的大多是这种，不取的话后面真正要显示的那几行得等它们全部取完。
    if (job->cancelled && *job->cancelled) return;
    WIN32_FILE_ATTRIBUTE_DATA attributes = {0};
    io_get_attributes_ex(job->path, GetFileExInfoStandard, &attributes);
    // 文件被修改后键随之改变，旧的缩略图自然被挤出。
    WStr key = {0};
    wstr_appendf(&key, L"%d|%lu.%lu|%ls", job->size, attributes.ftLastWriteTime.dwHighDateTime, attributes.ftLastWriteTime.dwLowDateTime, job->path);
    for (int index = 0; index < cache_count; index++)
    {
        if (!wcs_equal_ignore_case(cache[index].key, key.data)) continue;
        CacheEntry hit = cache[index];
        memmove(&cache[1], &cache[0], (size_t)index * sizeof(CacheEntry));
        cache[0] = hit;
        job->image = gfx_image_retain(hit.image);
        wstr_free(&key);
        return;
    }
    GfxImage *image = load_from_windows(job->path, job->size);
    if (!image)
    {
        wstr_free(&key);
        return;
    }
    if (cache_count == CACHE_CAPACITY)
    {
        cache_count--;
        free(cache[cache_count].key);
        gfx_image_release(cache[cache_count].image);
    }
    memmove(&cache[1], &cache[0], (size_t)cache_count * sizeof(CacheEntry));
    cache[0] = (CacheEntry){ wstr_take(&key), gfx_image_retain(image) };
    cache_count++;
    job->image = image;
}

static void loaded(void *data)
{
    ThumbnailJob *job = data;
    job->ready(job->image, job->context);
    free(job->path);
    free(job);
}

void thumbnail_load(const wchar_t *path, int size, volatile LONG *cancelled, ThumbnailReady ready, void *context)
{
    ThumbnailJob *job = xcalloc(1, sizeof(ThumbnailJob));
    job->path = wcs_dup(path);
    job->size = size > 0 ? size : 128;
    job->cancelled = cancelled;
    job->ready = ready;
    job->context = context;
    task_run_imaging(load, loaded, job);
}
