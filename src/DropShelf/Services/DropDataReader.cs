using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Media.Imaging;
using DropShelf.Localization;
using DropShelf.Models;
using WpfDataFormats = System.Windows.DataFormats;
using WpfDataObject = System.Windows.IDataObject;

namespace DropShelf.Services;

public enum DropContentKind
{
    None,
    Files,
    VirtualFiles,
    Link,
    Image,
    Text
}

public sealed record DroppedEntry(ShelfItemKind Kind, string Value, string? Title = null, bool IsTemporary = false);

public sealed record DropReadResult(
    IReadOnlyList<DroppedEntry> Entries,
    IReadOnlyList<string> CacheDirectories,
    int FailedCount,
    string? ErrorMessage)
{
    public static DropReadResult Failed(string message) => new([], [], 1, message);

    public IReadOnlyList<string> FilePaths =>
        Entries.Where(entry => entry.Kind is ShelfItemKind.File or ShelfItemKind.Folder).Select(entry => entry.Value).ToArray();

    public bool HasTemporaryFiles => Entries.Any(entry => entry.IsTemporary);
}

/// <summary>把拖放或剪贴板数据转换成置物架项目：文件、未落盘附件、网页链接、图片或文本。</summary>
public static class DropDataReader
{
    public const string ShelfIdFormat = "DropShelf.ShelfId";
    private static readonly string[] UrlFormats = ["UniformResourceLocatorW", "UniformResourceLocator", "text/x-moz-url"];

    public static string? GetSourceShelfId(WpfDataObject data)
    {
        try
        {
            return data.GetDataPresent(ShelfIdFormat, false) ? ReadString(data.GetData(ShelfIdFormat, false), unicode: true) : null;
        }
        catch
        {
            return null;
        }
    }

    public static DropContentKind Classify(WpfDataObject data)
    {
        try
        {
            if (data.GetDataPresent(WpfDataFormats.FileDrop, false)) return DropContentKind.Files;
            var hasUrl = UrlFormats.Any(format => data.GetDataPresent(format, false));
            if (VirtualFileDropService.CanImport(data))
            {
                if (hasUrl && VirtualFileDropService.GetDescriptorNames(data) is { Count: > 0 } names &&
                    names.All(name => name.EndsWith(".url", StringComparison.OrdinalIgnoreCase)))
                    return DropContentKind.Link;
                return DropContentKind.VirtualFiles;
            }
            if (hasUrl) return DropContentKind.Link;
            if (data.GetDataPresent("PNG", false) || data.GetDataPresent(WpfDataFormats.Bitmap, false) ||
                data.GetDataPresent(WpfDataFormats.Dib, false))
                return DropContentKind.Image;
            if (data.GetDataPresent(WpfDataFormats.UnicodeText, false) || data.GetDataPresent(WpfDataFormats.Text, false))
                return DropContentKind.Text;
        }
        catch
        {
            // 少数来源程序在枚举格式时出错，按不支持处理。
        }
        return DropContentKind.None;
    }

    /// <summary>是否能转换为磁盘文件，供快速操作使用。</summary>
    public static bool CanProvideFiles(DropContentKind kind) =>
        kind is DropContentKind.Files or DropContentKind.VirtualFiles or DropContentKind.Image;

    /// <summary>
    /// 读取拖放内容。必须在 Drop 回调中调用：需要同步取得来源程序交出的数据，之后的写盘在后台完成。
    /// </summary>
    public static async Task<DropReadResult> ReadAsync(WpfDataObject data, bool filesOnly = false)
    {
        var kind = Classify(data);
        if (filesOnly && kind == DropContentKind.Link && VirtualFileDropService.CanImport(data))
            kind = DropContentKind.VirtualFiles;

        switch (kind)
        {
            case DropContentKind.Files:
                return ReadFileDrop(data);

            case DropContentKind.VirtualFiles:
                var imported = await VirtualFileDropService.ImportAsync(data);
                return new DropReadResult(
                    imported.Paths.Select(path => new DroppedEntry(ShelfItemKind.File, path, IsTemporary: true)).ToArray(),
                    imported.CacheDirectory is { Length: > 0 } cache ? [cache] : [],
                    imported.FailedCount,
                    imported.ErrorMessage);

            case DropContentKind.Image:
                return await ReadImageAsync(data);

            case DropContentKind.Link when !filesOnly:
                return ReadLink(data) is { } link
                    ? new DropReadResult([link], [], 0, null)
                    : DropReadResult.Failed(Strings.NoLinkRead);

            case DropContentKind.Text when !filesOnly:
                var text = ReadText(data);
                if (string.IsNullOrWhiteSpace(text)) return DropReadResult.Failed(Strings.NoTextToAdd);
                var trimmed = text.Trim();
                return IsSingleUrl(trimmed)
                    ? new DropReadResult([new DroppedEntry(ShelfItemKind.Link, trimmed)], [], 0, null)
                    : new DropReadResult([new DroppedEntry(ShelfItemKind.Text, trimmed)], [], 0, null);

            case DropContentKind.Link or DropContentKind.Text:
                return DropReadResult.Failed(Strings.FilesAndFoldersOnly);

            default:
                return DropReadResult.Failed(Strings.SourceProvidedNothing);
        }
    }

    public static bool IsSingleUrl(string text) =>
        !text.Contains('\n') && !text.Contains(' ') &&
        Uri.TryCreate(text, UriKind.Absolute, out var uri) &&
        (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps);

    private static DropReadResult ReadFileDrop(WpfDataObject data)
    {
        if (data.GetData(WpfDataFormats.FileDrop, false) is not string[] paths || paths.Length == 0)
            return DropReadResult.Failed(Strings.NoFilesRead);
        var entries = paths
            .Where(path => !string.IsNullOrWhiteSpace(path))
            .Select(path => new DroppedEntry(Directory.Exists(path) ? ShelfItemKind.Folder : ShelfItemKind.File, path))
            .ToArray();
        return new DropReadResult(entries, [], 0, null);
    }

    private static async Task<DropReadResult> ReadImageAsync(WpfDataObject data)
    {
        byte[]? pngBytes = null;
        BitmapSource? bitmap = null;
        try
        {
            if (data.GetDataPresent("PNG", false))
                pngBytes = ReadBytes(data.GetData("PNG", false));
            if (pngBytes is null && data.GetData(WpfDataFormats.Bitmap, true) is BitmapSource source)
            {
                bitmap = source;
                if (bitmap.CanFreeze) bitmap.Freeze();
            }
        }
        catch (Exception exception)
        {
            LogService.Error("读取拖入的图片失败", exception);
        }

        if (pngBytes is null && bitmap is null) return DropReadResult.Failed(Strings.NoImageRead);

        var cacheDirectory = VirtualFileDropService.CreateCacheDirectory();
        var path = Path.Combine(cacheDirectory, $"{Strings.DroppedImageName} {DateTime.Now:yyyy-MM-dd HH.mm.ss}.png");
        try
        {
            await Task.Run(() =>
            {
                if (pngBytes is not null)
                {
                    File.WriteAllBytes(path, pngBytes);
                    return;
                }
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap!));
                using var stream = File.Create(path);
                encoder.Save(stream);
            });
            return new DropReadResult([new DroppedEntry(ShelfItemKind.File, path, IsTemporary: true)], [cacheDirectory], 0, null);
        }
        catch (Exception exception)
        {
            LogService.Error("保存拖入的图片失败", exception);
            await VirtualFileDropService.DeleteCacheAsync(cacheDirectory);
            return DropReadResult.Failed(Strings.ImageSaveFailed);
        }
    }

    private static DroppedEntry? ReadLink(WpfDataObject data)
    {
        string? url = null;
        string? title = null;
        try
        {
            if (data.GetDataPresent("text/x-moz-url", false) &&
                ReadString(data.GetData("text/x-moz-url", false), unicode: true) is { } moz)
            {
                var lines = moz.Split('\n', 2, StringSplitOptions.TrimEntries);
                url = lines[0];
                if (lines.Length > 1 && !string.IsNullOrWhiteSpace(lines[1])) title = lines[1];
            }
            url ??= data.GetDataPresent("UniformResourceLocatorW", false)
                ? ReadString(data.GetData("UniformResourceLocatorW", false), unicode: true)
                : null;
            url ??= data.GetDataPresent("UniformResourceLocator", false)
                ? ReadString(data.GetData("UniformResourceLocator", false), unicode: false)
                : null;
            url ??= ReadText(data)?.Trim();

            if (title is null && VirtualFileDropService.GetDescriptorNames(data) is { Count: 1 } names)
                title = Path.GetFileNameWithoutExtension(names[0]);
        }
        catch
        {
        }

        url = url?.Trim();
        return string.IsNullOrWhiteSpace(url) ? null : new DroppedEntry(ShelfItemKind.Link, url, title);
    }

    private static string? ReadText(WpfDataObject data)
    {
        try
        {
            if (data.GetData(WpfDataFormats.UnicodeText, false) is string unicode) return unicode;
            if (data.GetData(WpfDataFormats.Text, true) is string text) return text;
        }
        catch
        {
        }
        return null;
    }

    private static byte[]? ReadBytes(object? value) => value switch
    {
        byte[] bytes => bytes,
        MemoryStream memory => memory.ToArray(),
        Stream stream => CopyStream(stream),
        _ => null
    };

    private static byte[] CopyStream(Stream stream)
    {
        using var copy = new MemoryStream();
        if (stream.CanSeek) stream.Position = 0;
        stream.CopyTo(copy);
        return copy.ToArray();
    }

    private static string? ReadString(object? value, bool unicode)
    {
        if (value is string text) return text.TrimEnd('\0');
        var bytes = ReadBytes(value);
        if (bytes is null) return null;
        var decoded = unicode ? Encoding.Unicode.GetString(bytes) : Encoding.Latin1.GetString(bytes);
        var end = decoded.IndexOf('\0');
        return end >= 0 ? decoded[..end] : decoded;
    }
}
