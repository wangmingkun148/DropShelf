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
    private const string DibV5Format = "Format17";
    private static readonly string[] UrlFormats = ["UniformResourceLocatorW", "UniformResourceLocator", "text/x-moz-url"];
    private static readonly string[] QqRichEditFormats = ["QQ_Unicode_RichEdit_Format", "QQ_RichEdit_Format"];

    /// <summary>以压缩数据提供的图片格式及保存时使用的扩展名，按优先顺序排列。</summary>
    private static readonly (string Format, string Extension)[] EncodedImageFormats =
    [
        ("PNG", ".png"), ("image/png", ".png"), ("JFIF", ".jpg"), ("image/jpeg", ".jpg"), ("GIF", ".gif"), ("image/gif", ".gif")
    ];

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
            if (ReadSingleFileName(data) is not null) return DropContentKind.Files;
            var hasUrl = UrlFormats.Any(format => data.GetDataPresent(format, false));
            if (VirtualFileDropService.CanImport(data))
            {
                if (hasUrl && VirtualFileDropService.GetDescriptorNames(data) is { Count: > 0 } names &&
                    names.All(name => name.EndsWith(".url", StringComparison.OrdinalIgnoreCase)))
                    return DropContentKind.Link;
                return DropContentKind.VirtualFiles;
            }
            // 聊天软件拖出图片时常同时提供图片地址，有图片数据时优先当作图片。
            if (HasImageData(data)) return DropContentKind.Image;
            if (ReadLocalImagePaths(data).Count > 0) return DropContentKind.Files;
            if (hasUrl) return DropContentKind.Link;
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
                return await ReadFileDropAsync(data);

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

    /// <summary>
    /// 读取文件路径。来源程序放在临时文件夹中的文件（聊天软件拖出的图片和附件）会在拖放结束后被删除，
    /// 必须在 Drop 回调返回前打开它们，再在后台复制到 DropShelf 自己的缓存中。
    /// </summary>
    private static async Task<DropReadResult> ReadFileDropAsync(WpfDataObject data)
    {
        var paths = data.GetData(WpfDataFormats.FileDrop, false) is string[] { Length: > 0 } dropped
            ? dropped
            : ReadSingleFileName(data) is { } single ? [single] : ReadLocalImagePaths(data).ToArray();
        paths = paths.Where(path => !string.IsNullOrWhiteSpace(path)).ToArray();
        if (paths.Length == 0) return DropReadResult.Failed(Strings.NoFilesRead);

        var entries = paths
            .Select(path => new DroppedEntry(Directory.Exists(path) ? ShelfItemKind.Folder : ShelfItemKind.File, path))
            .ToArray();
        var snapshots = OpenVolatileFiles(entries);
        if (snapshots.Count == 0) return new DropReadResult(entries, [], 0, null);

        var cacheDirectory = VirtualFileDropService.CreateCacheDirectory();
        await Task.Run(() =>
        {
            var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var (index, stream) in snapshots)
            {
                var source = entries[index].Value;
                var destination = Path.Combine(cacheDirectory, VirtualFileDropService.CreateUniqueSafeName(Path.GetFileName(source), usedNames));
                try
                {
                    using (stream)
                    using (var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, FileOptions.SequentialScan))
                        stream.CopyTo(output);
                    try { File.SetLastWriteTimeUtc(destination, File.GetLastWriteTimeUtc(source)); }
                    catch { }
                    entries[index] = new DroppedEntry(ShelfItemKind.File, destination, IsTemporary: true);
                }
                catch (Exception exception)
                {
                    // 复制失败时保留原路径，文件只要还在就仍然可用。
                    LogService.Error($"保存来源程序的临时文件失败：{source}", exception);
                    try { if (File.Exists(destination)) File.Delete(destination); } catch { }
                }
            }
        });
        return new DropReadResult(entries, [cacheDirectory], 0, null);
    }

    private static List<(int Index, FileStream Stream)> OpenVolatileFiles(IReadOnlyList<DroppedEntry> entries)
    {
        var opened = new List<(int, FileStream)>();
        for (var index = 0; index < entries.Count; index++)
        {
            var path = entries[index].Value;
            if (entries[index].Kind != ShelfItemKind.File || !ChatAppCompatibility.IsVolatilePath(path)) continue;
            try
            {
                if (new FileInfo(path).Length > ChatAppCompatibility.MaximumSnapshotBytes) continue;
                opened.Add((index, new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 81920,
                    FileOptions.SequentialScan)));
            }
            catch
            {
                // 打不开的文件按原路径处理。
            }
        }
        return opened;
    }

    /// <summary>只提供单个文件名（FileNameW / FileName）的来源程序。</summary>
    private static string? ReadSingleFileName(WpfDataObject data)
    {
        try
        {
            var path = data.GetDataPresent("FileNameW", false) ? ReadString(data.GetData("FileNameW", false), unicode: true)
                : data.GetDataPresent("FileName", false) ? ReadString(data.GetData("FileName", false), unicode: false)
                : null;
            return !string.IsNullOrWhiteSpace(path) && (File.Exists(path) || Directory.Exists(path)) ? path : null;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>旧版 QQ 的富文本或只含图片的网页片段中，仍然存在的本地图片。</summary>
    private static IReadOnlyList<string> ReadLocalImagePaths(WpfDataObject data)
    {
        try
        {
            foreach (var format in QqRichEditFormats)
            {
                if (!data.GetDataPresent(format, false)) continue;
                var xml = data.GetData(format, false) is string text ? text : ChatAppCompatibility.DecodeRichEdit(ReadBytes(data.GetData(format, false)));
                var images = ChatAppCompatibility.ParseQqRichEdit(xml).Where(File.Exists).ToArray();
                if (images.Length > 0) return images;
            }
            if (data.GetDataPresent(WpfDataFormats.Html, false) && data.GetData(WpfDataFormats.Html, false) is string html)
                return ChatAppCompatibility.ParseHtmlLocalImages(html).Where(File.Exists).ToArray();
        }
        catch
        {
            // 富文本格式不规范时按其他内容处理。
        }
        return [];
    }

    private static bool HasImageData(WpfDataObject data) =>
        EncodedImageFormats.Any(format => data.GetDataPresent(format.Format, false)) ||
        data.GetDataPresent(WpfDataFormats.Bitmap, false) ||
        data.GetDataPresent(WpfDataFormats.Dib, false) ||
        data.GetDataPresent(DibV5Format, false);

    private static async Task<DropReadResult> ReadImageAsync(WpfDataObject data)
    {
        byte[]? encoded = null;
        var extension = ".png";
        BitmapSource? bitmap = null;
        try
        {
            foreach (var (format, formatExtension) in EncodedImageFormats)
            {
                if (!data.GetDataPresent(format, false)) continue;
                encoded = ReadBytes(data.GetData(format, false));
                if (encoded is not { Length: > 0 }) continue;
                extension = formatExtension;
                break;
            }
            if (encoded is null && data.GetData(WpfDataFormats.Bitmap, true) is BitmapSource source) bitmap = source;
            bitmap ??= DecodeDib(data, DibV5Format) ?? DecodeDib(data, WpfDataFormats.Dib);
            if (bitmap is { CanFreeze: true }) bitmap.Freeze();
        }
        catch (Exception exception)
        {
            LogService.Error("读取拖入的图片失败", exception);
        }

        if (encoded is null && bitmap is null) return DropReadResult.Failed(Strings.NoImageRead);

        var cacheDirectory = VirtualFileDropService.CreateCacheDirectory();
        var path = Path.Combine(cacheDirectory, $"{Strings.DroppedImageName} {DateTime.Now:yyyy-MM-dd HH.mm.ss}{extension}");
        try
        {
            await Task.Run(() =>
            {
                if (encoded is not null)
                {
                    File.WriteAllBytes(path, encoded);
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

    /// <summary>
    /// 把 DIB 数据（位图信息头 + 像素）补上 BMP 文件头后解码。部分程序只提供 CF_DIBV5，WPF 不会自动转换。
    /// </summary>
    private static BitmapSource? DecodeDib(WpfDataObject data, string format)
    {
        try
        {
            if (!data.GetDataPresent(format, false) || ReadBytes(data.GetData(format, false)) is not { Length: > 40 } dib) return null;
            var headerSize = BitConverter.ToInt32(dib, 0);
            var bitCount = BitConverter.ToInt16(dib, 14);
            var compression = BitConverter.ToInt32(dib, 16);
            var colorsUsed = BitConverter.ToInt32(dib, 32);
            var paletteEntries = colorsUsed > 0 ? colorsUsed : bitCount <= 8 ? 1 << bitCount : 0;
            // BI_BITFIELDS 且只有基本信息头时，三个颜色掩码紧跟在信息头后面。
            var masks = headerSize == 40 && compression == 3 ? 12 : 0;
            var offset = 14 + headerSize + masks + paletteEntries * 4;

            var file = new byte[14 + dib.Length];
            file[0] = (byte)'B';
            file[1] = (byte)'M';
            BitConverter.GetBytes(file.Length).CopyTo(file, 2);
            BitConverter.GetBytes(offset).CopyTo(file, 10);
            dib.CopyTo(file, 14);
            var decoder = new BmpBitmapDecoder(new MemoryStream(file), BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            return decoder.Frames[0];
        }
        catch
        {
            return null;
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
