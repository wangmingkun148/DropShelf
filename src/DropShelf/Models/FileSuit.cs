using System.IO;

namespace DropShelf.Models;

/// <summary>牌面角标上表示文件类型的花色：▲ 图片、■ 文档、● 文件夹、◆ 其他。</summary>
public enum FileSuit
{
    Image,
    Document,
    Folder,
    Other
}

public static class FileSuits
{
    private static readonly HashSet<string> Images = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".jfif", ".gif", ".bmp", ".webp", ".tif", ".tiff", ".heic", ".heif", ".avif", ".svg", ".ico",
        ".psd", ".raw", ".cr2", ".cr3", ".nef", ".arw", ".dng"
    };

    private static readonly HashSet<string> Documents = new(StringComparer.OrdinalIgnoreCase)
    {
        ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp", ".rtf", ".txt", ".md", ".csv",
        ".json", ".xml", ".html", ".htm", ".epub", ".pages", ".numbers", ".key", ".wps", ".et", ".dps"
    };

    /// <summary>按扩展名判断；没有扩展名的按文件夹算，不用访问磁盘。</summary>
    public static FileSuit Of(string path)
    {
        var extension = Path.GetExtension(path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (string.IsNullOrEmpty(extension)) return FileSuit.Folder;
        if (Images.Contains(extension)) return FileSuit.Image;
        return Documents.Contains(extension) ? FileSuit.Document : FileSuit.Other;
    }

    public static FileSuit Of(ShelfItem item) => item.Kind switch
    {
        ShelfItemKind.Folder => FileSuit.Folder,
        ShelfItemKind.Text => FileSuit.Document,
        ShelfItemKind.Link => FileSuit.Other,
        _ => Of(item.Value)
    };

    /// <summary>数量最多的一种；一样多时取最后放入的那一种。没有项目时为其他。</summary>
    public static FileSuit Dominant(IEnumerable<FileSuit> suits)
    {
        var counts = new int[Enum.GetValues<FileSuit>().Length];
        var last = new int[counts.Length];
        var index = 0;
        foreach (var suit in suits)
        {
            counts[(int)suit]++;
            last[(int)suit] = ++index;
        }
        var best = FileSuit.Other;
        for (var suit = 0; suit < counts.Length; suit++)
            if (counts[suit] > counts[(int)best] || counts[suit] == counts[(int)best] && counts[suit] > 0 && last[suit] > last[(int)best])
                best = (FileSuit)suit;
        return best;
    }
}
