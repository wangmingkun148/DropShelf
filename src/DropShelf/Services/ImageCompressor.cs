using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DropShelf.Services;

public enum ImageCompressionStatus
{
    Compressed,
    AlreadySmall,
    NotImage,
    Failed
}

public sealed record ImageCompressionResult(string Source, string? Output, ImageCompressionStatus Status);

/// <summary>
/// 把图片压缩为不超过指定大小的 JPG，常用于网页表单和聊天软件的上传限制。
/// 先在保持尺寸的前提下降低画质，仍然过大时再按比例缩小；原文件不会改变。
/// </summary>
public static class ImageCompressor
{
    private const int MaximumQuality = 92;
    private const int MinimumQuality = 40;
    private const int MinimumEdge = 32;
    private const long MaximumWorkingPixels = 40_000_000;

    private static readonly HashSet<string> ImageExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".jpg", ".jpeg", ".jfif", ".png", ".bmp", ".gif", ".tif", ".tiff", ".webp", ".heic", ".heif", ".avif"
    };

    /// <summary>网页和聊天软件普遍接受的格式；已经足够小时不需要转换。</summary>
    private static readonly HashSet<string> WidelySupported = new(StringComparer.OrdinalIgnoreCase) { ".jpg", ".jpeg", ".png" };

    public static bool IsImagePath(string path) => ImageExtensions.Contains(Path.GetExtension(path));

    public static IReadOnlyList<ImageCompressionResult> CompressAll(IReadOnlyList<string> paths, long maximumBytes, string? outputDirectory) =>
        paths.Select(path => Compress(path, maximumBytes, outputDirectory)).ToArray();

    public static ImageCompressionResult Compress(string source, long maximumBytes, string? outputDirectory = null)
    {
        if (!File.Exists(source) || !IsImagePath(source)) return new ImageCompressionResult(source, null, ImageCompressionStatus.NotImage);
        string? temporaryPath = null;
        try
        {
            if (new FileInfo(source).Length <= maximumBytes && WidelySupported.Contains(Path.GetExtension(source)))
                return new ImageCompressionResult(source, null, ImageCompressionStatus.AlreadySmall);

            var bytes = EncodeWithinLimit(Load(source), maximumBytes);
            // 极窄或极小的图片缩到最小尺寸仍然过大时，不保存一个名不副实的文件。
            if (bytes.Length > maximumBytes) return new ImageCompressionResult(source, null, ImageCompressionStatus.Failed);
            var directory = string.IsNullOrWhiteSpace(outputDirectory)
                ? Path.GetDirectoryName(source) ?? throw new IOException("无法确定输出文件保存位置")
                : outputDirectory;
            Directory.CreateDirectory(directory);
            var output = FileOperationService.GetAvailablePath(directory, $"{Path.GetFileNameWithoutExtension(source)}-{FormatLimit(maximumBytes)}", ".jpg");
            temporaryPath = Path.Combine(directory, $".dropshelf-{Guid.NewGuid():N}.tmp");
            File.WriteAllBytes(temporaryPath, bytes);
            File.Move(temporaryPath, output);
            temporaryPath = null;
            return new ImageCompressionResult(source, output, ImageCompressionStatus.Compressed);
        }
        catch (Exception exception)
        {
            LogService.Error($"压缩图片失败：{source}", exception);
            return new ImageCompressionResult(source, null, ImageCompressionStatus.Failed);
        }
        finally
        {
            if (temporaryPath is not null)
            {
                try { File.Delete(temporaryPath); }
                catch { }
            }
        }
    }

    /// <summary>
    /// 找出不超过上限的最高画质；最低画质仍然过大时按比例缩小后重试。
    /// 搜索要编码很多次：两个缓冲区轮流使用，一个存目前最好的结果，另一个写新的候选，只在最后复制一次。
    /// </summary>
    public static byte[] EncodeWithinLimit(BitmapSource image, long maximumBytes)
    {
        var best = new MemoryStream();
        var candidate = new MemoryStream();
        var current = image;
        for (var round = 0; round < 10; round++)
        {
            Encode(current, MaximumQuality, best);
            if (best.Length <= maximumBytes) return best.ToArray();

            Encode(current, MinimumQuality, best);
            if (best.Length <= maximumBytes)
            {
                var low = MinimumQuality;
                var high = MaximumQuality - 1;
                while (low < high)
                {
                    var middle = (low + high + 1) / 2;
                    Encode(current, middle, candidate);
                    if (candidate.Length <= maximumBytes)
                    {
                        (best, candidate) = (candidate, best);
                        low = middle;
                    }
                    else
                    {
                        high = middle - 1;
                    }
                }
                return best.ToArray();
            }

            // 文件大小与像素数大致成正比，按面积比例缩小并多留一点余量。
            var ratio = Math.Clamp(Math.Sqrt((double)maximumBytes / best.Length) * 0.92, 0.2, 0.92);
            // 两边都已经很小时停止；细长的图片允许短边继续缩小。
            if (current.PixelWidth * ratio < MinimumEdge && current.PixelHeight * ratio < MinimumEdge) return best.ToArray();
            if (current.PixelWidth * ratio < 1 || current.PixelHeight * ratio < 1) return best.ToArray();
            current = Resize(current, ratio);
        }
        Encode(current, MinimumQuality, best);
        return best.ToArray();
    }

    /// <summary>
    /// 读取图片：按拍摄方向转正，透明部分铺白色背景（JPG 不支持透明）。
    /// 先只读尺寸和方向，超大的图片在解码时就缩小到处理上限以内，不会先完整解码再缩小。
    /// </summary>
    public static BitmapSource Load(string path)
    {
        int width, height, orientation;
        using (var stream = OpenRead(path))
        {
            var frame = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat | BitmapCreateOptions.DelayCreation,
                BitmapCacheOption.None).Frames[0];
            (width, height, orientation) = (frame.PixelWidth, frame.PixelHeight, ReadOrientation(frame));
        }

        using var content = OpenRead(path);
        var image = new BitmapImage();
        image.BeginInit();
        image.StreamSource = content;
        image.CacheOption = BitmapCacheOption.OnLoad;
        image.CreateOptions = BitmapCreateOptions.PreservePixelFormat;
        var pixels = (long)width * height;
        if (pixels > MaximumWorkingPixels)
            image.DecodePixelWidth = Math.Max(1, (int)(width * Math.Sqrt((double)MaximumWorkingPixels / pixels)));
        image.EndInit();
        image.Freeze();
        return Flatten(ApplyOrientation(image, orientation));
    }

    private static FileStream OpenRead(string path) =>
        new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);

    /// <summary>编码到 output 中，覆盖原有内容，已分配的容量留着下次用。</summary>
    private static void Encode(BitmapSource bitmap, int quality, MemoryStream output)
    {
        var encoder = new JpegBitmapEncoder { QualityLevel = quality };
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        output.SetLength(0);
        encoder.Save(output);
    }

    private static BitmapSource Resize(BitmapSource bitmap, double ratio)
    {
        var scaled = new TransformedBitmap(bitmap, new ScaleTransform(ratio, ratio));
        scaled.Freeze();
        return Materialize(scaled);
    }

    private static int ReadOrientation(BitmapFrame frame)
    {
        try
        {
            if (frame.Metadata is BitmapMetadata metadata && metadata.GetQuery("System.Photo.Orientation") is { } value)
                return Convert.ToInt32(value);
        }
        catch
        {
            // 部分格式没有方向信息或不支持查询。
        }
        return 1;
    }

    /// <summary>按 EXIF 方向值转正：2/4 为镜像，3 为旋转 180°，5–8 为旋转 90° 的组合。</summary>
    private static BitmapSource ApplyOrientation(BitmapSource bitmap, int orientation)
    {
        Transform? transform = orientation switch
        {
            2 => new ScaleTransform(-1, 1),
            3 => new RotateTransform(180),
            4 => new ScaleTransform(1, -1),
            5 => new TransformGroup { Children = { new RotateTransform(90), new ScaleTransform(-1, 1) } },
            6 => new RotateTransform(90),
            7 => new TransformGroup { Children = { new RotateTransform(270), new ScaleTransform(-1, 1) } },
            8 => new RotateTransform(270),
            _ => null
        };
        if (transform is null) return bitmap;
        var oriented = new TransformedBitmap(bitmap, transform);
        oriented.Freeze();
        return oriented;
    }

    /// <summary>按条带逐段转换，只需要几十行像素的临时缓冲区，不会同时占用整张图的两份像素数组。</summary>
    private static BitmapSource Flatten(BitmapSource bitmap)
    {
        var source = new FormatConvertedBitmap(bitmap, PixelFormats.Bgra32, null, 0);
        var width = source.PixelWidth;
        var height = source.PixelHeight;
        var stride = (width * 3 + 3) & ~3;
        var rows = Math.Clamp(4_000_000 / (width * 4), 1, height);
        var input = new byte[width * 4 * rows];
        var output = new byte[stride * rows];
        var flattened = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgr24, null);

        for (var top = 0; top < height; top += rows)
        {
            var count = Math.Min(rows, height - top);
            source.CopyPixels(new Int32Rect(0, top, width, count), input, width * 4, 0);
            for (var y = 0; y < count; y++)
            {
                var from = y * width * 4;
                var to = y * stride;
                for (var x = 0; x < width; x++, from += 4, to += 3)
                {
                    var alpha = input[from + 3];
                    var inverse = 255 - alpha;
                    output[to] = (byte)((input[from] * alpha + 255 * inverse) / 255);
                    output[to + 1] = (byte)((input[from + 1] * alpha + 255 * inverse) / 255);
                    output[to + 2] = (byte)((input[from + 2] * alpha + 255 * inverse) / 255);
                }
            }
            flattened.WritePixels(new Int32Rect(0, top, width, count), output, stride, 0);
        }

        flattened.Freeze();
        return flattened;
    }

    /// <summary>把缩放结果固定下来，之后多次编码不必每次重新缩放；与 Flatten 一样按条带复制，不建整张图大小的临时数组。</summary>
    internal static BitmapSource Materialize(BitmapSource bitmap)
    {
        var width = bitmap.PixelWidth;
        var height = bitmap.PixelHeight;
        var stride = (width * bitmap.Format.BitsPerPixel + 31) / 32 * 4;
        var rows = Math.Clamp(4_000_000 / stride, 1, height);
        var buffer = new byte[stride * rows];
        var copy = new WriteableBitmap(width, height, 96, 96, bitmap.Format, bitmap.Palette);
        for (var top = 0; top < height; top += rows)
        {
            var strip = new Int32Rect(0, top, width, Math.Min(rows, height - top));
            bitmap.CopyPixels(strip, buffer, stride, 0);
            copy.WritePixels(strip, buffer, stride, 0);
        }
        copy.Freeze();
        return copy;
    }

    /// <summary>输出文件名中的大小，例如 500KB、2MB。</summary>
    public static string FormatLimit(long maximumBytes)
    {
        var kilobytes = maximumBytes / 1024;
        return kilobytes >= 1024 && kilobytes % 1024 == 0 ? $"{kilobytes / 1024}MB" : $"{kilobytes}KB";
    }
}
