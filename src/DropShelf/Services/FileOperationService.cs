using System.Diagnostics;
using System.IO;
using DropShelf.Localization;

namespace DropShelf.Services;

public sealed record FileOperationResult(string Source, string? Destination, bool Succeeded, string? ErrorMessage = null);

public sealed class FileOperationService
{
    private const string FolderIntoItselfMessage = "不能把文件夹复制或移动到它自己的内部。";

    public void Open(string path)
    {
        EnsureExists(path);
        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
    }

    public void Reveal(string path)
    {
        EnsureExists(path);
        var arguments = File.Exists(path) ? $"/select,\"{path}\"" : $"\"{path}\"";
        Process.Start(new ProcessStartInfo("explorer.exe", arguments) { UseShellExecute = true });
    }

    public async Task<IReadOnlyList<FileOperationResult>> CopyAsync(
        IEnumerable<string> sources,
        string destination,
        CancellationToken cancellationToken = default)
    {
        ValidateDestination(destination);
        Directory.CreateDirectory(destination);

        var results = new List<FileOperationResult>();
        foreach (var source in sources)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string? target = null;
            try
            {
                EnsureExists(source);
                EnsureDestinationIsNotInsideSource(source, destination);
                target = GetAvailablePath(destination, GetSourceName(source), Directory.Exists(source));

                if (File.Exists(source))
                    await CopyFileAsync(source, target, cancellationToken);
                else
                    await CopyDirectoryAsync(source, target, cancellationToken);

                results.Add(new FileOperationResult(source, target, true));
                LogService.Info($"复制完成：{source} -> {target}");
            }
            catch (Exception exception) when (exception is not OperationCanceledException)
            {
                results.Add(new FileOperationResult(source, target, false, ToUserMessage(exception)));
                LogService.Error($"复制失败：{source}", exception);
            }
        }
        return results;
    }

    public async Task<IReadOnlyList<FileOperationResult>> MoveAsync(
        IEnumerable<string> sources,
        string destination,
        CancellationToken cancellationToken = default)
    {
        ValidateDestination(destination);
        Directory.CreateDirectory(destination);

        var results = new List<FileOperationResult>();
        foreach (var source in sources)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string? target = null;
            try
            {
                EnsureExists(source);
                EnsureDestinationIsNotInsideSource(source, destination);
                target = GetAvailablePath(destination, GetSourceName(source), Directory.Exists(source));
                await MoveOneAsync(source, target, cancellationToken);
                results.Add(new FileOperationResult(source, target, true));
                LogService.Info($"移动完成：{source} -> {target}");
            }
            catch (Exception exception) when (exception is not OperationCanceledException)
            {
                results.Add(new FileOperationResult(source, target, false, ToUserMessage(exception)));
                LogService.Error($"移动失败：{source}", exception);
            }
        }
        return results;
    }

    private static async Task MoveOneAsync(string source, string target, CancellationToken cancellationToken)
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
                await CopyFileAsync(source, target, cancellationToken);
                File.Delete(source);
            }
            else
            {
                await CopyDirectoryAsync(source, target, cancellationToken);
                Directory.Delete(source, true);
            }
        }
    }

    private static async Task CopyFileAsync(string source, string destination, CancellationToken cancellationToken)
    {
        const int bufferSize = 1024 * 128;
        await using var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, bufferSize, true);
        await using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, bufferSize, true);
        await input.CopyToAsync(output, cancellationToken);
    }

    private static async Task CopyDirectoryAsync(string source, string destination, CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(destination);
        var pending = new Stack<(string Source, string Destination)>();
        pending.Push((source, destination));

        while (pending.Count > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var current = pending.Pop();

            foreach (var file in Directory.EnumerateFiles(current.Source))
                await CopyFileAsync(file, Path.Combine(current.Destination, Path.GetFileName(file)), cancellationToken);

            foreach (var directory in Directory.EnumerateDirectories(current.Source))
            {
                var info = new DirectoryInfo(directory);
                if ((info.Attributes & FileAttributes.ReparsePoint) != 0)
                    continue;

                var childDestination = Path.Combine(current.Destination, info.Name);
                Directory.CreateDirectory(childDestination);
                pending.Push((directory, childDestination));
            }
        }
    }

    private static string GetAvailablePath(string destination, string name, bool isDirectory)
    {
        var candidate = Path.Combine(destination, name);
        if (!File.Exists(candidate) && !Directory.Exists(candidate)) return candidate;

        var baseName = isDirectory ? name : Path.GetFileNameWithoutExtension(name);
        var extension = isDirectory ? string.Empty : Path.GetExtension(name);
        for (var index = 2; index < 10_000; index++)
        {
            candidate = Path.Combine(destination, $"{baseName} ({index}){extension}");
            if (!File.Exists(candidate) && !Directory.Exists(candidate)) return candidate;
        }
        throw new IOException("目标文件夹中重名项目过多，无法生成新名称。");
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
        var sourcePath = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source));
        var destinationPath = Path.TrimEndingDirectorySeparator(Path.GetFullPath(destination));
        // 目标就是源文件夹本身也不行：复制时会把新建的子文件夹一起遍历进去，无限嵌套。
        if (string.Equals(destinationPath, sourcePath, StringComparison.OrdinalIgnoreCase) ||
            destinationPath.StartsWith(sourcePath + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException(FolderIntoItselfMessage);
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
        IOException => Strings.ItemBusy,
        _ => Strings.OperationIncomplete
    };
}
