using System.IO;
using System.Text;
using DropShelf.Localization;
using DropShelf.Models;

namespace DropShelf.Services;

/// <summary>把置物架上的文本和链接写成文件：文本存成 .txt，网页链接存成 .url 快捷方式。</summary>
public static class ShelfItemFiles
{
    private const int MaximumStemLength = 40;
    private static readonly HashSet<string> ReservedNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    /// <summary>只有网页地址存成快捷方式；其余的（包括不是网页地址的链接）按文本保存，双击时不会交给系统执行。</summary>
    public static bool IsShortcut(ShelfItem item) => item.Kind == ShelfItemKind.Link && DropDataReader.IsSingleUrl(item.Value);

    /// <summary>文件名：文本取第一行开头的一段，链接取标题或网址里的主机名；去掉文件名里不能用的字符。</summary>
    public static string FileName(ShelfItem item)
    {
        var shortcut = IsShortcut(item);
        var source = shortcut
            ? string.IsNullOrWhiteSpace(item.Title) ? new Uri(item.Value).Host : item.Title!
            : ShelfItem.Summarize(item.Value, MaximumStemLength).TrimEnd('…');
        var invalid = Path.GetInvalidFileNameChars();
        var builder = new StringBuilder(source.Length);
        foreach (var character in source)
        {
            var next = Array.IndexOf(invalid, character) >= 0 || char.IsControl(character) ? ' ' : character;
            if (next == ' ' && (builder.Length == 0 || builder[^1] == ' ')) continue;
            builder.Append(next);
            if (builder.Length == MaximumStemLength) break;
        }
        var stem = builder.ToString().Trim(' ', '.');
        if (stem.Length == 0) stem = shortcut ? Strings.LinkFileName : Strings.TextFileName;
        if (ReservedNames.Contains(stem)) stem = "_" + stem;
        return stem + (shortcut ? ".url" : ".txt");
    }

    /// <summary>写到 directory 里，同名时在名称后面加序号，不覆盖已有的文件；返回写好的路径。</summary>
    public static string Write(ShelfItem item, string directory)
    {
        var name = FileName(item);
        var (stem, extension) = (Path.GetFileNameWithoutExtension(name), Path.GetExtension(name));
        var path = Path.Combine(directory, name);
        for (var number = 2; File.Exists(path) || Directory.Exists(path); number++)
            path = Path.Combine(directory, $"{stem} ({number}){extension}");

        if (IsShortcut(item))
            File.WriteAllText(path, $"[InternetShortcut]\r\nURL={new Uri(item.Value).AbsoluteUri}\r\n", Encoding.ASCII);
        else
            // 带上字节顺序标记：旧程序在中文系统上默认按本地编码读，没有标记会乱码。
            File.WriteAllText(path, item.Value, new UTF8Encoding(encoderShouldEmitUTF8Identifier: true));
        return path;
    }
}
