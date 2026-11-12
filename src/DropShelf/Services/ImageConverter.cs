using System.IO;
using System.Windows.Media.Imaging;
using DropShelf.Models;

namespace DropShelf.Services;

public enum ImageConversionStatus
{
    Converted,
    AlreadyFormat,
    NotImage,
    Failed
}

public sealed record ImageConversionResult(string Source, string? Output, ImageConversionStatus Status);

/// <summary>
/// 把图片另存为 JPG 或 PNG：HEIC、WebP、AVIF 等格式很多网页和程序不认，转一下就能用。
/// 按拍摄方向转正；转成 PNG 时保留透明，转成 JPG 时透明部分铺白色。原文件不会改变。
/// </summary>
public static class ImageConverter
{
    private const int JpegQuality = 92;
    /// <summary>转换不压缩尺寸，处理上限比压缩图片时宽：一亿像素以内原样转换，再大的在解码时缩小。</summary>
    private const long MaximumPixels = 100_000_000;

    private static readonly HashSet<string> JpegExtensions = new(StringComparer.OrdinalIgnoreCase) { ".jpg", ".jpeg", ".jfif" };

    public static string Extension(ImageFormat format) => format == ImageFormat.Png ? ".png" : ".jpg";

    /// <summary>已经是目标格式的不用转。</summary>
    public static bool IsFormat(string path, ImageFormat format)
    {
        var extension = Path.GetExtension(path);
        return format == ImageFormat.Png ? extension.Equals(".png", StringComparison.OrdinalIgnoreCase) : JpegExtensions.Contains(extension);
    }

    public static IReadOnlyList<ImageConversionResult> ConvertAll(IReadOnlyList<string> paths, ImageFormat format, string? outputDirectory) =>
        paths.Select(path => Convert(path, format, outputDirectory)).ToArray();

    public static ImageConversionResult Convert(string source, ImageFormat format, string? outputDirectory = null)
    {
        if (!File.Exists(source) || !ImageCompressor.IsImagePath(source)) return new ImageConversionResult(source, null, ImageConversionStatus.NotImage);
        if (IsFormat(source, format)) return new ImageConversionResult(source, null, ImageConversionStatus.AlreadyFormat);
        string? temporaryPath = null;
        try
        {
            var image = ImageCompressor.Load(source, flatten: format == ImageFormat.Jpeg, MaximumPixels);
            BitmapEncoder encoder = format == ImageFormat.Png ? new PngBitmapEncoder() : new JpegBitmapEncoder { QualityLevel = JpegQuality };
            encoder.Frames.Add(BitmapFrame.Create(image));

            var directory = string.IsNullOrWhiteSpace(outputDirectory)
                ? Path.GetDirectoryName(source) ?? throw new IOException("无法确定输出文件保存位置")
                : outputDirectory;
            Directory.CreateDirectory(directory);
            var output = FileOperationService.GetAvailablePath(directory, Path.GetFileNameWithoutExtension(source), Extension(format));
            temporaryPath = Path.Combine(directory, $".dropshelf-{Guid.NewGuid():N}.tmp");
            using (var stream = new FileStream(temporaryPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                encoder.Save(stream);
            File.Move(temporaryPath, output);
            temporaryPath = null;
            return new ImageConversionResult(source, output, ImageConversionStatus.Converted);
        }
        catch (Exception exception)
        {
            LogService.Error($"转换图片失败：{source}", exception);
            return new ImageConversionResult(source, null, ImageConversionStatus.Failed);
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
}
