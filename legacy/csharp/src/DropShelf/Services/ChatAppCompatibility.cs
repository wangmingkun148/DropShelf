using System.IO;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;

namespace DropShelf.Services;

/// <summary>
/// 与聊天软件等来源程序配合的细节：
/// 有些程序拖出图片或文件时先写到临时文件夹，拖放结束后马上删除，需要在放下时立即保存一份；
/// 旧版 QQ 和部分程序拖出图片时只提供带本地图片路径的富文本或网页片段。
/// </summary>
public static partial class ChatAppCompatibility
{
    /// <summary>超过这个大小的临时文件不复制，直接使用原路径，避免放下时长时间等待。</summary>
    public const long MaximumSnapshotBytes = 512L * 1024 * 1024;

    /// <summary>聊天软件自己的数据目录；其中名为 temp 的子文件夹也会很快被清理。</summary>
    private static readonly string[] ChatAppFolders =
    [
        @"\wechat files\", @"\xwechat_files\", @"\wxwork\", @"\tencent files\", @"\tencent\", @"\dingtalk", @"\feishu", @"\lark"
    ];

    private static readonly string[] TemporaryFolders = [@"\temp\", @"\tmp\", @"\rwtemp\", @"\cache\"];

    /// <summary>文件是否位于来源程序随时可能删除的临时位置。DropShelf 自己的缓存不算。</summary>
    public static bool IsVolatilePath(string path)
    {
        try
        {
            var full = Path.GetFullPath(path);
            if (IsUnder(full, VirtualFileDropService.CacheRoot)) return false;
            if (IsUnder(full, Path.GetTempPath())) return true;
            var lower = full.ToLowerInvariant();
            return ChatAppFolders.Any(lower.Contains) && TemporaryFolders.Any(lower.Contains);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// 旧版 QQ 拖出的富文本：只包含图片时返回图片的本地路径，含有文字时返回空列表（按文本处理）。
    /// </summary>
    public static IReadOnlyList<string> ParseQqRichEdit(string? xml)
    {
        if (string.IsNullOrWhiteSpace(xml) || !xml.Contains("<QQRichEditFormat", StringComparison.OrdinalIgnoreCase)) return [];
        var hasText = QqTextElement().Matches(xml).Any(match => !string.IsNullOrWhiteSpace(match.Groups[1].Value));
        if (hasText) return [];
        return QqImageElement().Matches(xml)
            .Select(match => WebUtility.HtmlDecode(match.Groups[1].Value))
            .Where(IsLocalPath)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    /// <summary>
    /// 网页片段（HTML Format）只包含本地图片时返回图片路径；含有文字时返回空列表，仍按文本或链接处理。
    /// </summary>
    public static IReadOnlyList<string> ParseHtmlLocalImages(string? html)
    {
        if (string.IsNullOrWhiteSpace(html)) return [];
        var start = html.IndexOf("<!--StartFragment-->", StringComparison.OrdinalIgnoreCase);
        var end = html.IndexOf("<!--EndFragment-->", StringComparison.OrdinalIgnoreCase);
        var fragment = start >= 0 && end > start ? html[(start + 20)..end] : html;

        var text = WebUtility.HtmlDecode(HtmlTag().Replace(fragment, " "));
        if (!string.IsNullOrWhiteSpace(text)) return [];

        var paths = new List<string>();
        foreach (Match match in HtmlImageSource().Matches(fragment))
        {
            var source = WebUtility.HtmlDecode(match.Groups[2].Value);
            if (!Uri.TryCreate(source, UriKind.Absolute, out var uri) || !uri.IsFile) continue;
            var path = uri.LocalPath;
            if (!IsLocalPath(path)) continue;
            if (!paths.Contains(path, StringComparer.OrdinalIgnoreCase)) paths.Add(path);
        }
        return paths;
    }

    /// <summary>读取数据对象中以字节形式提供的文本：先按 UTF-8 判断，含有 QQ 标记时使用，否则按 UTF-16。</summary>
    public static string? DecodeRichEdit(byte[]? bytes)
    {
        if (bytes is null || bytes.Length == 0) return null;
        var utf8 = Encoding.UTF8.GetString(bytes).TrimEnd('\0');
        if (utf8.Contains("<QQRichEditFormat", StringComparison.OrdinalIgnoreCase)) return utf8;
        return Encoding.Unicode.GetString(bytes).TrimEnd('\0');
    }

    /// <summary>
    /// 富文本中间接引用的图片只接受本机磁盘上的路径。检查是否存在时会访问对应位置，
    /// 网络共享或映射的网络驱动器会发起网络连接，可能卡住界面，甚至向对方发送登录凭据。
    /// </summary>
    internal static bool IsLocalPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path) || path.StartsWith(@"\\", StringComparison.Ordinal) ||
            path.StartsWith("//", StringComparison.Ordinal)) return false;
        try
        {
            return new DriveInfo(Path.GetPathRoot(path)!).DriveType is not (DriveType.Network or DriveType.NoRootDirectory);
        }
        catch
        {
            return false;
        }
    }

    private static bool IsUnder(string path, string folder)
    {
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder)) + Path.DirectorySeparatorChar;
        return path.StartsWith(root, StringComparison.OrdinalIgnoreCase);
    }

    [GeneratedRegex(""""<EditElement[^>]*\btype="1"[^>]*\bfilepath="([^"]+)"""", RegexOptions.IgnoreCase)]
    private static partial Regex QqImageElement();

    [GeneratedRegex("""<EditElement[^>]*\btype="0"[^>]*>(?:<!\[CDATA\[)?(.*?)(?:\]\]>)?</EditElement>""", RegexOptions.IgnoreCase | RegexOptions.Singleline)]
    private static partial Regex QqTextElement();

    [GeneratedRegex("<[^>]+>", RegexOptions.Singleline)]
    private static partial Regex HtmlTag();

    [GeneratedRegex("""<img\b[^>]*\bsrc\s*=\s*(["'])(.*?)\1""", RegexOptions.IgnoreCase | RegexOptions.Singleline)]
    private static partial Regex HtmlImageSource();
}
