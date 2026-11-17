#define COBJMACROS
#include "image_tools.h"
#include "file_ops.h"
#include "image_quality.h"
#include "log.h"
#include "settings.h"
#include "shelf_item.h"

#include <math.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

#define MAXIMUM_QUALITY 92
#define MINIMUM_QUALITY 40
#define MINIMUM_EDGE 32
#define COMPRESS_MAXIMUM_PIXELS 40000000ll
#define CONVERT_JPEG_QUALITY 92
/// 转换不压缩尺寸，处理上限比压缩图片时宽：一亿像素以内原样转换，再大的在解码时缩小。
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

/// 网页和聊天软件普遍接受的格式；已经足够小时不需要转换。
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

static int read_orientation(IWICBitmapFrameDecode *frame)
{
    IWICMetadataQueryReader *reader = NULL;
    int orientation = 1;
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &reader)) && reader)
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(reader, L"System.Photo.Orientation", &value)))
        {
            switch (value.vt)
            {
                case VT_UI1: orientation = value.bVal; break;
                case VT_UI2: orientation = value.uiVal; break;
                case VT_UI4: orientation = (int)value.ulVal; break;
                case VT_I2: orientation = value.iVal; break;
                case VT_I4: orientation = value.lVal; break;
                default: break;
            }
        }
        PropVariantClear(&value);
        IWICMetadataQueryReader_Release(reader);
    }
    return orientation;
}

/// 在 source 后面接一步旋转或翻转；成功时换掉 source。
static HRESULT apply_transform(IWICImagingFactory *factory, IWICBitmapSource **source, WICBitmapTransformOptions options)
{
    IWICBitmapFlipRotator *rotator = NULL;
    HRESULT result = IWICImagingFactory_CreateBitmapFlipRotator(factory, &rotator);
    if (SUCCEEDED(result)) result = IWICBitmapFlipRotator_Initialize(rotator, *source, options);
    if (FAILED(result))
    {
        if (rotator) IWICBitmapFlipRotator_Release(rotator);
        return result;
    }
    IWICBitmapSource_Release(*source);
    *source = (IWICBitmapSource *)rotator;
    return S_OK;
}

/// 按 EXIF 方向值转正：2/4 为镜像，3 为旋转 180°，5–8 为旋转 90° 的组合（先转再左右翻转）。
static HRESULT apply_orientation(IWICImagingFactory *factory, IWICBitmapSource **source, int orientation)
{
    switch (orientation)
    {
        case 2: return apply_transform(factory, source, WICBitmapTransformFlipHorizontal);
        case 3: return apply_transform(factory, source, WICBitmapTransformRotate180);
        case 4: return apply_transform(factory, source, WICBitmapTransformFlipVertical);
        case 5:
        {
            HRESULT result = apply_transform(factory, source, WICBitmapTransformRotate90);
            return SUCCEEDED(result) ? apply_transform(factory, source, WICBitmapTransformFlipHorizontal) : result;
        }
        case 6: return apply_transform(factory, source, WICBitmapTransformRotate90);
        case 7:
        {
            HRESULT result = apply_transform(factory, source, WICBitmapTransformRotate270);
            return SUCCEEDED(result) ? apply_transform(factory, source, WICBitmapTransformFlipHorizontal) : result;
        }
        case 8: return apply_transform(factory, source, WICBitmapTransformRotate270);
        default: return S_OK;
    }
}

static HRESULT scale_to(IWICImagingFactory *factory, IWICBitmapSource **source, UINT width, UINT height)
{
    IWICBitmapScaler *scaler = NULL;
    HRESULT result = IWICImagingFactory_CreateBitmapScaler(factory, &scaler);
    if (SUCCEEDED(result)) result = IWICBitmapScaler_Initialize(scaler, *source, width, height, WICBitmapInterpolationModeFant);
    if (FAILED(result))
    {
        if (scaler) IWICBitmapScaler_Release(scaler);
        return result;
    }
    IWICBitmapSource_Release(*source);
    *source = (IWICBitmapSource *)scaler;
    return S_OK;
}

/// 透明部分铺白色（JPG 不支持透明）。按条带逐段转换，只需要几十行像素的临时缓冲区。
static HRESULT flatten(IWICImagingFactory *factory, IWICBitmapSource **source)
{
    IWICFormatConverter *converter = NULL;
    HRESULT result = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(result))
        result = IWICFormatConverter_Initialize(converter, *source, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
            WICBitmapPaletteTypeCustom);
    UINT width = 0, height = 0;
    if (SUCCEEDED(result)) result = IWICFormatConverter_GetSize(converter, &width, &height);
    IWICBitmap *flattened = NULL;
    if (SUCCEEDED(result))
        result = IWICImagingFactory_CreateBitmap(factory, width, height, &GUID_WICPixelFormat24bppBGR, WICBitmapCacheOnLoad, &flattened);
    IWICBitmapLock *lock = NULL;
    WICRect whole = { 0, 0, (INT)width, (INT)height };
    if (SUCCEEDED(result)) result = IWICBitmap_Lock(flattened, &whole, WICBitmapLockWrite, &lock);
    UINT stride = 0, size = 0;
    BYTE *pixels = NULL;
    if (SUCCEEDED(result)) result = IWICBitmapLock_GetStride(lock, &stride);
    if (SUCCEEDED(result)) result = IWICBitmapLock_GetDataPointer(lock, &size, &pixels);
    if (SUCCEEDED(result))
    {
        UINT rows = 4000000 / (width * 4);
        if (rows < 1) rows = 1;
        if (rows > height) rows = height;
        BYTE *input = xalloc((size_t)width * 4 * rows);
        for (UINT top = 0; top < height && SUCCEEDED(result); top += rows)
        {
            UINT count = rows < height - top ? rows : height - top;
            WICRect strip = { 0, (INT)top, (INT)width, (INT)count };
            result = IWICFormatConverter_CopyPixels(converter, &strip, width * 4, width * 4 * count, input);
            for (UINT y = 0; y < count && SUCCEEDED(result); y++)
            {
                const BYTE *from = input + (size_t)y * width * 4;
                BYTE *to = pixels + (size_t)(top + y) * stride;
                for (UINT x = 0; x < width; x++, from += 4, to += 3)
                {
                    unsigned alpha = from[3], inverse = 255 - alpha;
                    to[0] = (BYTE)((from[0] * alpha + 255 * inverse) / 255);
                    to[1] = (BYTE)((from[1] * alpha + 255 * inverse) / 255);
                    to[2] = (BYTE)((from[2] * alpha + 255 * inverse) / 255);
                }
            }
        }
        free(input);
    }
    if (lock) IWICBitmapLock_Release(lock);
    if (converter) IWICFormatConverter_Release(converter);
    if (FAILED(result))
    {
        if (flattened) IWICBitmap_Release(flattened);
        return result;
    }
    IWICBitmapSource_Release(*source);
    *source = (IWICBitmapSource *)flattened;
    return S_OK;
}

/// 读取图片：按拍摄方向转正；flatten 时透明部分铺白色，否则保留透明。超大的图片在解码时就缩小到处理上限以内。
static HRESULT load(IWICImagingFactory *factory, const wchar_t *path, bool flatten_alpha, long long maximum_pixels, IWICBitmapSource **image)
{
    *image = NULL;
    IStream *stream = NULL;
    wchar_t *target = path_for_io(path);
    HRESULT result = SHCreateStreamOnFileEx(target, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, NULL, &stream);
    free(target);
    IWICBitmapDecoder *decoder = NULL;
    if (SUCCEEDED(result)) result = IWICImagingFactory_CreateDecoderFromStream(factory, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
    IWICBitmapFrameDecode *frame = NULL;
    if (SUCCEEDED(result)) result = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(result)) result = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    if (SUCCEEDED(result) && (width == 0 || height == 0)) result = E_FAIL;
    IWICBitmapSource *source = NULL;
    if (SUCCEEDED(result))
    {
        int orientation = read_orientation(frame);
        source = (IWICBitmapSource *)frame;
        IWICBitmapSource_AddRef(source);
        long long pixels = (long long)width * height;
        if (pixels > maximum_pixels)
        {
            UINT scaled_width = (UINT)(width * sqrt((double)maximum_pixels / (double)pixels));
            if (scaled_width < 1) scaled_width = 1;
            UINT scaled_height = (UINT)llround((double)height * scaled_width / width);
            if (scaled_height < 1) scaled_height = 1;
            result = scale_to(factory, &source, scaled_width, scaled_height);
        }
        if (SUCCEEDED(result)) result = apply_orientation(factory, &source, orientation);
        if (SUCCEEDED(result) && flatten_alpha) result = flatten(factory, &source);
    }
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IStream_Release(stream);
    if (FAILED(result))
    {
        if (source) IWICBitmapSource_Release(source);
        return result;
    }
    *image = source;
    return S_OK;
}

// ───────────── 编码 ─────────────

static HRESULT reset_stream(IStream *stream)
{
    ULARGE_INTEGER zero = {0};
    LARGE_INTEGER start = {0};
    HRESULT result = IStream_SetSize(stream, zero);
    if (SUCCEEDED(result)) result = IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
    return result;
}

static unsigned long long stream_size(IStream *stream)
{
    STATSTG stat;
    return SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) ? stat.cbSize.QuadPart : 0;
}

/// 编码到 output 中，覆盖原有内容。quality 小于 0 时是 PNG。
static HRESULT encode(IWICImagingFactory *factory, IWICBitmapSource *image, int quality, IStream *output)
{
    HRESULT result = reset_stream(output);
    IWICBitmapEncoder *encoder = NULL;
    if (SUCCEEDED(result))
        result = IWICImagingFactory_CreateEncoder(factory, quality >= 0 ? &GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(result)) result = IWICBitmapEncoder_Initialize(encoder, output, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    if (SUCCEEDED(result)) result = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &options);
    if (SUCCEEDED(result) && quality >= 0 && options)
    {
        PROPBAG2 option = {0};
        option.pstrName = L"ImageQuality";
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = quality / 100.0f;
        result = IPropertyBag2_Write(options, 1, &option, &value);
    }
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_Initialize(frame, options);
    UINT width = 0, height = 0;
    if (SUCCEEDED(result)) result = IWICBitmapSource_GetSize(image, &width, &height);
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_SetSize(frame, width, height);
    // 编码器不接受原来的像素格式时转换成它要的。
    WICPixelFormatGUID source_format = GUID_WICPixelFormatDontCare, accepted;
    IWICBitmapSource *converted = NULL;
    if (SUCCEEDED(result)) result = IWICBitmapSource_GetPixelFormat(image, &source_format);
    accepted = source_format;
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_SetPixelFormat(frame, &accepted);
    if (SUCCEEDED(result) && !IsEqualGUID(&accepted, &source_format)) result = WICConvertBitmapSource(&accepted, image, &converted);
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_WriteSource(frame, converted ? converted : image, NULL);
    if (SUCCEEDED(result)) result = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(result)) result = IWICBitmapEncoder_Commit(encoder);
    if (converted) IWICBitmapSource_Release(converted);
    if (options) IPropertyBag2_Release(options);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    return result;
}

/// 把缩放结果固定下来，之后多次编码不必每次重新缩放。
static HRESULT resize(IWICImagingFactory *factory, IWICBitmapSource **image, double ratio)
{
    UINT width = 0, height = 0;
    HRESULT result = IWICBitmapSource_GetSize(*image, &width, &height);
    if (FAILED(result)) return result;
    UINT scaled_width = (UINT)lround(width * ratio), scaled_height = (UINT)lround(height * ratio);
    if (scaled_width < 1) scaled_width = 1;
    if (scaled_height < 1) scaled_height = 1;
    IWICBitmapSource *scaled = *image;
    IWICBitmapSource_AddRef(scaled);
    result = scale_to(factory, &scaled, scaled_width, scaled_height);
    IWICBitmap *bitmap = NULL;
    if (SUCCEEDED(result)) result = IWICImagingFactory_CreateBitmapFromSource(factory, scaled, WICBitmapCacheOnLoad, &bitmap);
    IWICBitmapSource_Release(scaled);
    if (FAILED(result)) return result;
    IWICBitmapSource_Release(*image);
    *image = (IWICBitmapSource *)bitmap;
    return S_OK;
}

typedef struct QualityProbe
{
    IWICImagingFactory *factory;
    IWICBitmapSource *image;
    long long maximum;
    IStream *best, *candidate;      // 放得下的留在 best 里，新的一次写到 candidate
    HRESULT result;
} QualityProbe;

static long long probe_quality(void *context, int quality)
{
    QualityProbe *probe = context;
    probe->result = encode(probe->factory, probe->image, quality, probe->candidate);
    if (FAILED(probe->result)) return -1;
    long long size = (long long)stream_size(probe->candidate);
    if (size <= probe->maximum)
    {
        IStream *swap = probe->best;
        probe->best = probe->candidate;
        probe->candidate = swap;
    }
    return size;
}

/// 找出不超过上限的最高画质；最低画质仍然过大时按比例缩小后重试。两个缓冲区轮流使用，一个存目前最好的结果，另一个写新的候选。
/// 返回装着结果的那个（另一个调用方照常释放）。
/// 每编码一次都要处理整张图片，所以能省就省：画质按估计的位置去试（见 image_quality_search）；
/// 缩小以后先试最低画质——尺寸是按“最低画质刚好放得下”算的，最高画质几乎不可能放得下，不值得先花一次去试，
/// 它留在搜索范围里，真放得下时照样会选到。
static IStream *encode_within_limit(IWICImagingFactory *factory, IWICBitmapSource *image, long long maximum_bytes, IStream *best, IStream *candidate,
    HRESULT *result)
{
    IWICBitmapSource *current = image;
    IWICBitmapSource_AddRef(current);
    IStream *found = NULL;
    // 原尺寸下最高画质比最低画质大多少倍；缩小以后不再编码最高画质，按这个倍数估计。
    double steepness = 0;
    for (int round = 0; round < 10 && SUCCEEDED(*result) && !found; round++)
    {
        long long top_size = 0;
        if (round == 0)
        {
            *result = encode(factory, current, MAXIMUM_QUALITY, best);
            if (FAILED(*result)) break;
            top_size = (long long)stream_size(best);
            if (top_size <= maximum_bytes)
            {
                found = best;
                break;
            }
        }
        *result = encode(factory, current, MINIMUM_QUALITY, best);
        if (FAILED(*result)) break;
        long long low_size = (long long)stream_size(best);
        if (low_size <= maximum_bytes)
        {
            // 第一轮已经知道最高画质放不下；之后没有试过，把它也算进范围，大小用估计的。
            int high = round == 0 ? MAXIMUM_QUALITY : MAXIMUM_QUALITY + 1;
            long long high_size = round == 0 ? top_size : (long long)((double)low_size * steepness);
            QualityProbe probe = { factory, current, maximum_bytes, best, candidate, S_OK };
            if (image_quality_search(MINIMUM_QUALITY, low_size, high, high_size, maximum_bytes, probe_quality, &probe) < 0)
                *result = FAILED(probe.result) ? probe.result : E_FAIL;
            best = probe.best;
            candidate = probe.candidate;
            found = best;
            break;
        }
        if (round == 0 && low_size > 0) steepness = (double)top_size / (double)low_size;
        // 文件大小与像素数大致成正比，按面积比例缩小并多留一点余量。
        double ratio = sqrt((double)maximum_bytes / (double)low_size) * 0.92;
        if (ratio < 0.2) ratio = 0.2;
        if (ratio > 0.92) ratio = 0.92;
        UINT width = 0, height = 0;
        IWICBitmapSource_GetSize(current, &width, &height);
        // 两边都已经很小时停止；细长的图片允许短边继续缩小。
        if ((width * ratio < MINIMUM_EDGE && height * ratio < MINIMUM_EDGE) || width * ratio < 1 || height * ratio < 1)
        {
            found = best;
            break;
        }
        *result = resize(factory, &current, ratio);
    }
    if (!found && SUCCEEDED(*result))
    {
        *result = encode(factory, current, MINIMUM_QUALITY, best);
        found = best;
    }
    IWICBitmapSource_Release(current);
    return SUCCEEDED(*result) ? found : NULL;
}

// ───────────── 保存 ─────────────

/// 先写到临时文件，写完再改名，不会留下写了一半的文件。
static bool save_stream(IStream *stream, const wchar_t *directory, const wchar_t *output)
{
    HGLOBAL memory = NULL;
    if (FAILED(GetHGlobalFromStream(stream, &memory))) return false;
    unsigned long long size = stream_size(stream);
    const BYTE *bytes = GlobalLock(memory);
    if (!bytes) return false;
    wchar_t *guid = new_guid_text();
    WStr name = {0};
    wstr_appendf(&name, L".dropshelf-%ls.tmp", guid);
    wchar_t *temporary = path_join(directory, name.data);
    wstr_free(&name);
    free(guid);
    bool ok = false;
    HANDLE file = io_create_file(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ok = size < 0xFFFFFFFFull && WriteFile(file, bytes, (DWORD)size, &written, NULL) && written == size;
        CloseHandle(file);
        if (ok) ok = io_move_file(temporary, output, 0) != 0;
        if (!ok) io_delete_file(temporary);
    }
    GlobalUnlock(memory);
    if (!ok) log_win32_error(L"保存图片失败", GetLastError());
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

static IWICImagingFactory *create_factory(void)
{
    IWICImagingFactory *factory = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory))) return NULL;
    return factory;
}

static bool is_file(const wchar_t *path)
{
    DWORD attributes = io_get_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

ImageStatus image_compress(const wchar_t *source, long long maximum_bytes, const wchar_t *output_directory, wchar_t **output)
{
    *output = NULL;
    if (!is_file(source) || !image_is_path(source)) return IMAGE_NOT_IMAGE;
    long long size = 0;
    if (file_size_of(source, &size) && size <= maximum_bytes && widely_supported(source)) return IMAGE_ALREADY;

    IWICImagingFactory *factory = create_factory();
    IWICBitmapSource *image = NULL;
    HRESULT result = factory ? load(factory, source, true, COMPRESS_MAXIMUM_PIXELS, &image) : E_FAIL;
    IStream *best = NULL, *candidate = NULL;
    if (SUCCEEDED(result)) result = CreateStreamOnHGlobal(NULL, TRUE, &best);
    if (SUCCEEDED(result)) result = CreateStreamOnHGlobal(NULL, TRUE, &candidate);
    IStream *encoded = SUCCEEDED(result) ? encode_within_limit(factory, image, maximum_bytes, best, candidate, &result) : NULL;
    ImageStatus status = IMAGE_FAILED;
    // 极窄或极小的图片缩到最小尺寸仍然过大时，不保存一个名不副实的文件。
    if (encoded && (long long)stream_size(encoded) <= maximum_bytes)
    {
        wchar_t *directory = output_directory_for(source, output_directory);
        wchar_t *stem = path_file_stem(source), *limit = image_format_limit(maximum_bytes);
        WStr base = {0};
        wstr_appendf(&base, L"%ls-%ls", stem, limit);
        wchar_t *target = directory ? path_available(directory, base.data, L".jpg") : NULL;
        if (target && save_stream(encoded, directory, target))
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
    if (status == IMAGE_FAILED) log_error(L"压缩图片失败：%ls\r\nHRESULT 0x%08lX", source, (unsigned long)result);
    if (best) IStream_Release(best);
    if (candidate) IStream_Release(candidate);
    if (image) IWICBitmapSource_Release(image);
    if (factory) IWICImagingFactory_Release(factory);
    return status;
}

ImageStatus image_convert(const wchar_t *source, int format, const wchar_t *output_directory, wchar_t **output)
{
    *output = NULL;
    if (!is_file(source) || !image_is_path(source)) return IMAGE_NOT_IMAGE;
    if (is_format(source, format)) return IMAGE_ALREADY;
    bool png = format == IMAGE_FORMAT_PNG;
    IWICImagingFactory *factory = create_factory();
    IWICBitmapSource *image = NULL;
    HRESULT result = factory ? load(factory, source, !png, CONVERT_MAXIMUM_PIXELS, &image) : E_FAIL;
    IStream *stream = NULL;
    if (SUCCEEDED(result)) result = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (SUCCEEDED(result)) result = encode(factory, image, png ? -1 : CONVERT_JPEG_QUALITY, stream);
    ImageStatus status = IMAGE_FAILED;
    if (SUCCEEDED(result))
    {
        wchar_t *directory = output_directory_for(source, output_directory);
        wchar_t *stem = path_file_stem(source);
        wchar_t *target = directory ? path_available(directory, stem, png ? L".png" : L".jpg") : NULL;
        if (target && save_stream(stream, directory, target))
        {
            *output = target;
            target = NULL;
            status = IMAGE_DONE;
        }
        free(target);
        free(stem);
        free(directory);
    }
    if (status == IMAGE_FAILED) log_error(L"转换图片失败：%ls\r\nHRESULT 0x%08lX", source, (unsigned long)result);
    if (stream) IStream_Release(stream);
    if (image) IWICBitmapSource_Release(image);
    if (factory) IWICImagingFactory_Release(factory);
    return status;
}
