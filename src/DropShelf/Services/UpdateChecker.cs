using System.Net.Http;
using System.Text.Json;

namespace DropShelf.Services;

/// <summary>比现在新的版本，以及它的下载页。</summary>
public sealed record UpdateInfo(Version Version, string Url);

/// <summary>
/// 检查更新。DropShelf 平时不联网，只有在“关于”里点了“检查更新”才访问一次 GitHub 的发布列表，
/// 请求里只有程序名称和版本号；不自动下载，也不自动安装。
/// </summary>
public static class UpdateChecker
{
    /// <summary>发布标签的前缀：这一版是 style 开头的标签（如 style-v2.8.0），主线的 v2.8.0 是另一种外观，不混在一起比较。</summary>
    public const string TagPrefix = "style";
    public const string ReleasesApi = "https://api.github.com/repos/wangmingkun148/dropshelf/releases?per_page=30";
    public const string ReleasesPage = "https://github.com/wangmingkun148/dropshelf/releases";
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    public static Version CurrentVersion => Normalize(typeof(UpdateChecker).Assembly.GetName().Version ?? new Version(0, 0, 0));

    /// <summary>
    /// 访问发布列表，返回比当前版本新的最新版本；已经是最新时返回 null。连不上、超时或返回的内容看不懂时抛出异常。
    /// </summary>
    public static async Task<UpdateInfo?> CheckAsync(CancellationToken cancellationToken = default)
    {
        using var client = new HttpClient { Timeout = Timeout };
        client.DefaultRequestHeaders.UserAgent.ParseAdd($"DropShelf/{CurrentVersion}");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        using var response = await client.GetAsync(ReleasesApi, cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        var json = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        return FindNewer(json, CurrentVersion);
    }

    /// <summary>
    /// 从发布列表里找比 current 新的最新正式版本：只看标签以 <see cref="TagPrefix"/> 开头的，草稿和预发布不算。
    /// 没有更新的返回 null；内容不是发布列表时抛出异常。
    /// </summary>
    public static UpdateInfo? FindNewer(string json, Version current)
    {
        using var document = JsonDocument.Parse(json);
        if (document.RootElement.ValueKind != JsonValueKind.Array) throw new FormatException("发布列表的格式不对。");
        UpdateInfo? newest = null;
        foreach (var release in document.RootElement.EnumerateArray())
        {
            if (release.ValueKind != JsonValueKind.Object || IsTrue(release, "draft") || IsTrue(release, "prerelease")) continue;
            if (!release.TryGetProperty("tag_name", out var tag) || ParseTag(tag.GetString()) is not { } version) continue;
            if (version <= current || newest is not null && version <= newest.Version) continue;
            var url = release.TryGetProperty("html_url", out var page) && page.GetString() is { } link && IsReleasePage(link) ? link : ReleasesPage;
            newest = new UpdateInfo(version, url);
        }
        return newest;
    }

    /// <summary>style-v2.8.5、style2.8.5 → 2.8.5；前缀不对或不是版本号时返回 null。</summary>
    public static Version? ParseTag(string? tag)
    {
        if (tag is null || !tag.StartsWith(TagPrefix, StringComparison.OrdinalIgnoreCase)) return null;
        var text = tag[TagPrefix.Length..];
        if (text.StartsWith('-')) text = text[1..];
        if (text.StartsWith('v') || text.StartsWith('V')) text = text[1..];
        // 前缀后面必须直接是数字：别的前缀（比如同样以 v 开头的单词）不算。
        if (text.Length == 0 || !char.IsAsciiDigit(text[0])) return null;
        return Version.TryParse(text, out var version) ? Normalize(version) : null;
    }

    /// <summary>只比较主、次、修订三位。</summary>
    private static Version Normalize(Version version) => new(version.Major, version.Minor, Math.Max(version.Build, 0));

    private static bool IsTrue(JsonElement release, string name) =>
        release.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.True;

    /// <summary>只打开本项目在 GitHub 上的页面：返回的内容里即使混进了别的地址，也不会被交给浏览器。</summary>
    private static bool IsReleasePage(string url) => url.StartsWith(ReleasesPage + "/", StringComparison.OrdinalIgnoreCase);
}
