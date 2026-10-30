using System.IO;
using DropShelf.Localization;

namespace DropShelf.Services;

public sealed record FileOperationResult(string Source, string? Destination, bool Succeeded, string? ErrorMessage = null);

public sealed class FileOperationService
{
    private const string FolderIntoItselfMessage = "不能把文件夹复制或移动到它自己的内部。";
    private const string FolderContainsLinksMessage = "文件夹中包含目录链接，无法完整复制。";
    private static readonly EnumerationOptions ScanOptions = new() { AttributesToSkip = 0, IgnoreInaccessible = false };

    /// <summary>整个过程在后台线程中进行，目录扫描和复制不会占住界面线程。</summary>
    public Task<IReadOnlyList<FileOperationResult>> CopyAsync(
        IEnumerable<string> sources,
        string destination,
        CancellationToken cancellationToken = default) =>
        RunEachAsync(sources, destination, "复制", (source, target) =>
        {
            if (File.Exists(source)) CopyFile(source, target);
            else CopyDirectory(source, target, cancellationToken);
        }, cancellationToken);

    public Task<IReadOnlyList<FileOperationResult>> MoveAsync(
        IEnumerable<string> sources,
        string destination,
        CancellationToken cancellationToken = default) =>
        RunEachAsync(sources, destination, "移动", (source, target) => MoveOne(source, target, cancellationToken), cancellationToken);

    private static Task<IReadOnlyList<FileOperationResult>> RunEachAsync(
        IEnumerable<string> sources,
        string destination,
        string verb,
        Action<string, string> operation,
        CancellationToken cancellationToken)
    {
        var list = sources.ToArray();
        return Task.Run<IReadOnlyList<FileOperationResult>>(() =>
        {
            ValidateDestination(destination);
            Directory.CreateDirectory(destination);
            var results = new List<FileOperationResult>();
            foreach (var source in list)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string? target = null;
                try
                {
                    EnsureExists(source);
                    EnsureDestinationIsNotInsideSource(source, destination);
                    var name = GetSourceName(source);
                    target = Directory.Exists(source)
                        ? GetAvailablePath(destination, name)
                        : GetAvailablePath(destination, Path.GetFileNameWithoutExtension(name), Path.GetExtension(name));
                    operation(source, target);
                    results.Add(new FileOperationResult(source, target, true));
                    LogService.Info($"{verb}完成：{source} -> {target}");
                }
                catch (Exception exception) when (exception is not OperationCanceledException)
                {
                    results.Add(new FileOperationResult(source, target, false, ToUserMessage(exception)));
                    LogService.Error($"{verb}失败：{source}", exception);
                }
            }
            return results;
        }, cancellationToken);
    }

    private static void MoveOne(string source, string target, CancellationToken cancellationToken)
    {
        try
        {
            if (File.Exists(source)) File.Move(source, target);
            else Directory.Move(source, target);
        }
        catch (IOException)
        {
            if (File.Exists(source))
            {
                CopyFile(source, target);
                File.Delete(source);
            }
            else
            {
                CopyDirectory(source, target, cancellationToken);
                Directory.Delete(source, true);
            }
        }
    }

    /// <summary>由系统复制，连同下载来源标记（Zone.Identifier 等备用数据流）、文件属性和修改时间一起保留。</summary>
    private static void CopyFile(string source, string destination) => File.Copy(source, destination, overwrite: false);

    /// <summary>
    /// 联接点和目录符号链接无法原样复制，跳过又会让移动后删掉源文件夹时丢失它们；
    /// 扫描时一并检查，发现链接就在复制之前报错，源文件夹保持不变。整棵目录只遍历一次。
    /// </summary>
    private static void CopyDirectory(string source, string destination, CancellationToken cancellationToken)
    {
        var directories = new List<string>();
        var files = new List<string>();
        var pending = new Stack<string>();
        pending.Push(source);
        while (pending.Count > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var current = pending.Pop();
            foreach (var entry in new DirectoryInfo(current).EnumerateFileSystemInfos("*", ScanOptions))
            {
                if (entry is FileInfo) files.Add(entry.FullName);
                else if ((entry.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException(FolderContainsLinksMessage);
                else
                {
                    directories.Add(entry.FullName);
                    pending.Push(entry.FullName);
                }
            }
        }

        var sourceRoot = new DirectoryInfo(source).FullName;
        string Target(string path) => Path.Combine(destination, Path.GetRelativePath(sourceRoot, path));
        Directory.CreateDirectory(destination);
        foreach (var directory in directories) Directory.CreateDirectory(Target(directory));
        foreach (var file in files)
        {
            cancellationToken.ThrowIfCancellationRequested();
            CopyFile(file, Target(file));
        }
    }

    /// <summary>路径非空且文件或文件夹存在。</summary>
    internal static bool IsAvailablePath(string? path) =>
        !string.IsNullOrWhiteSpace(path) && (File.Exists(path) || Directory.Exists(path));

    /// <summary>目标文件夹里还没被占用的路径：重名时依次加上 (2)、(3)…；压缩包、解压和压缩图片的输出也用它。</summary>
    internal static string GetAvailablePath(string directory, string baseName, string extension = "")
    {
        var candidate = Path.Combine(directory, baseName + extension);
        for (var index = 2; File.Exists(candidate) || Directory.Exists(candidate); index++)
        {
            if (index >= 10_000) throw new IOException("目标文件夹中重名项目过多，无法生成新名称。");
            candidate = Path.Combine(directory, $"{baseName} ({index}){extension}");
        }
        return candidate;
    }

    private static void ValidateDestination(string destination)
    {
        if (string.IsNullOrWhiteSpace(destination))
            throw new ArgumentException("没有选择目标文件夹。", nameof(destination));
        _ = Path.GetFullPath(destination);
    }

    private static void EnsureExists(string path)
    {
        if (!File.Exists(path) && !Directory.Exists(path))
            throw new FileNotFoundException("项目已被移动、删除或暂时无法访问。", path);
    }

    private static void EnsureDestinationIsNotInsideSource(string source, string destination)
    {
        if (!Directory.Exists(source)) return;
        // 目标就是源文件夹本身也不行：复制时会把新建的子文件夹一起遍历进去，无限嵌套。
        // 路径本身或它的上级可能是指向源文件夹的联接点、符号链接，按实际位置再比较一次。
        if (IsSameOrInside(destination, source) || IsSameOrInside(ResolveLinks(destination), ResolveLinks(source)))
            throw new IOException(FolderIntoItselfMessage);
    }

    private static bool IsSameOrInside(string path, string folder)
    {
        var folderPath = Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder));
        var candidate = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        return string.Equals(candidate, folderPath, StringComparison.OrdinalIgnoreCase) ||
               candidate.StartsWith(folderPath + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>从根目录起逐级把联接点和目录符号链接换成它实际指向的位置。</summary>
    internal static string ResolveLinks(string path, int depth = 0)
    {
        if (depth > 32) throw new IOException("目录链接层级过多。");
        var full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        var root = Path.GetPathRoot(full) ?? string.Empty;
        var current = root;
        foreach (var part in full[root.Length..].Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, part);
            var info = new DirectoryInfo(current);
            if (!info.Exists || (info.Attributes & FileAttributes.ReparsePoint) == 0) continue;
            if (info.ResolveLinkTarget(returnFinalTarget: true) is { } target)
                current = ResolveLinks(target.FullName, depth + 1);
        }
        return current;
    }

    private static string GetSourceName(string source)
    {
        var fullPath = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source));
        return Path.GetFileName(fullPath);
    }

    private static string ToUserMessage(Exception exception) => exception switch
    {
        UnauthorizedAccessException => Strings.AccessDenied,
        FileNotFoundException => Strings.ItemNotFound,
        DirectoryNotFoundException => Strings.FolderNotFound,
        PathTooLongException => Strings.PathTooLong,
        IOException when exception.Message == FolderIntoItselfMessage => Strings.FolderIntoItself,
        IOException when exception.Message == FolderContainsLinksMessage => Strings.FolderContainsLinks,
        IOException => Strings.ItemBusy,
        _ => Strings.OperationIncomplete
    };
}
