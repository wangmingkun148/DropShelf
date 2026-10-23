using System.IO;

namespace DropShelf.Services;

/// <summary>
/// 下载来源标记（NTFS 备用数据流 Zone.Identifier）。Windows 据此对来自网络的文件做安全提示；
/// DropShelf 用流另存或解压出的新文件需要自己带上它，否则来源检查会失效。FAT、网络位置等不支持时忽略。
/// </summary>
internal static class ZoneIdentifier
{
    private const string StreamName = ":Zone.Identifier";

    public static string? Read(string path)
    {
        if (!OperatingSystem.IsWindows()) return null;
        try
        {
            return File.ReadAllText(path + StreamName);
        }
        catch
        {
            return null;
        }
    }

    public static void Write(string path, string? zone)
    {
        if (zone is null || !OperatingSystem.IsWindows()) return;
        try
        {
            File.WriteAllText(path + StreamName, zone);
        }
        catch (Exception exception)
        {
            LogService.Error($"无法写入下载来源标记：{path}", exception);
        }
    }

    /// <summary>与资源管理器解压时一样，把压缩包的来源区域标记到解出的每个文件上，并记下来自哪个压缩包。</summary>
    public static void Propagate(string archive, string directory)
    {
        if (Read(archive) is not { } zone || ParseZoneId(zone) is not { } zoneId) return;
        var mark = $"[ZoneTransfer]\r\nZoneId={zoneId}\r\nReferrerUrl={Path.GetFullPath(archive)}\r\n";
        foreach (var file in Directory.EnumerateFiles(directory, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = 0 }))
            Write(file, mark);
    }

    internal static int? ParseZoneId(string zone)
    {
        foreach (var line in zone.Split('\n'))
        {
            var trimmed = line.Trim();
            if (trimmed.StartsWith("ZoneId=", StringComparison.OrdinalIgnoreCase) && int.TryParse(trimmed[7..], out var id))
                return id;
        }
        return null;
    }
}
