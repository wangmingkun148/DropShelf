using System.IO;
using System.IO.Compression;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using DropShelf.Interop;
using DropShelf.Models;

namespace DropShelf.Services;

public sealed record QuickActionResult(
    bool Succeeded,
    string Message,
    IReadOnlyList<string>? OutputPaths = null)
{
    public string? OutputPath => OutputPaths?.FirstOrDefault();

    /// <summary>用户在选择文件夹时取消，不需要提示失败。</summary>
    public bool Cancelled { get; init; }

    /// <summary>移动操作的源路径与新路径，用于更新置物架中的引用。</summary>
    public IReadOnlyList<(string Source, string Destination)>? Moves { get; init; }

    /// <summary>操作生成了新文件（如压缩包），适合放回置物架方便继续拖走。</summary>
    public bool ProducedNewItems { get; init; }
}

public sealed record QuickActionRequest(
    QuickActionType Type,
    IReadOnlyList<string> Paths,
    bool HasTemporaryFiles,
    IntPtr Owner,
    Func<string?>? PickFolder = null);

public static class QuickActionService
{
    private const uint ShopFilePath = 2;
    private static readonly Guid DownloadsFolderId = new("374DE290-123F-4565-9164-39C4925E467B");

    public static async Task<QuickActionResult> ExecuteAsync(QuickActionRequest request)
    {
        var paths = request.Paths.Where(IsAvailablePath).ToArray();
        if (paths.Length == 0) return new QuickActionResult(false, "没有可操作的文件");

        switch (request.Type)
        {
            case QuickActionType.CompressZip:
                try
                {
                    var outputDirectory = request.HasTemporaryFiles ? GetPersistentOutputDirectory() : null;
                    var archived = await ArchiveAsync(paths, outputDirectory);
                    return archived with { ProducedNewItems = archived.Succeeded };
                }
                catch (Exception exception)
                {
                    LogService.Error("准备压缩或解压输出位置失败", exception);
                    return new QuickActionResult(false, "无法使用输出文件夹，原文件没有改变");
                }

            case QuickActionType.CopyFullPath:
                IReadOnlyList<string> pathsToCopy = paths;
                if (request.HasTemporaryFiles)
                {
                    var saved = await SaveCopiesAsync(paths);
                    if (!saved.Succeeded || saved.OutputPaths is null) return saved;
                    pathsToCopy = saved.OutputPaths;
                }
                return ClipboardService.TrySetText(string.Join(Environment.NewLine, pathsToCopy))
                    ? new QuickActionResult(true, pathsToCopy.Count > 1 ? $"已复制 {pathsToCopy.Count} 个路径" : "已复制路径")
                    : new QuickActionResult(false, "剪贴板正被其他程序占用，请重试");

            case QuickActionType.MoveTo:
                var destination = request.PickFolder?.Invoke();
                if (string.IsNullOrWhiteSpace(destination))
                    return new QuickActionResult(false, "已取消移动") { Cancelled = true };
                var results = await new FileOperationService().MoveAsync(paths, destination);
                var moved = results.Where(result => result.Succeeded && result.Destination is not null)
                    .Select(result => (result.Source, result.Destination!))
                    .ToArray();
                var failed = results.Count - moved.Length;
                var message = failed == 0
                    ? $"已移动 {moved.Length} 项到 {Path.GetFileName(Path.TrimEndingDirectorySeparator(destination))}"
                    : moved.Length == 0 ? "移动失败，原项目保持不变" : $"已移动 {moved.Length} 项，{failed} 项未能移动";
                return new QuickActionResult(moved.Length > 0, message, moved.Select(item => item.Item2).ToArray()) { Moves = moved };

            case QuickActionType.Reveal:
                return ShellInterop.OpenFolderAndSelect(paths)
                    ? new QuickActionResult(true, "已在文件资源管理器中显示")
                    : new QuickActionResult(false, "无法打开所在文件夹");

            default:
                return ShowProperties(request.Owner, paths);
        }
    }

    public static QuickActionResult ShowProperties(IntPtr owner, IReadOnlyList<string> paths)
    {
        var available = paths.Where(IsAvailablePath).ToArray();
        if (available.Length == 0) return new QuickActionResult(false, "没有可查看属性的文件");

        try
        {
            if (available.Length > 1 && ShellInterop.CreateShellDataObject(available) is { } data)
            {
                try
                {
                    if (NativeMethods.SHMultiFileProperties(data, 0) == 0)
                        return new QuickActionResult(true, $"已打开 {available.Length} 个项目的属性");
                }
                finally
                {
                    Marshal.ReleaseComObject(data);
                }
            }

            return NativeMethods.SHObjectProperties(owner, ShopFilePath, available[0], null)
                ? new QuickActionResult(true, "已打开属性")
                : new QuickActionResult(false, "Windows 未能打开属性窗口");
        }
        catch (Exception exception)
        {
            LogService.Error("打开文件属性失败", exception);
            return new QuickActionResult(false, "未能打开属性窗口，原文件没有改变");
        }
    }

    public static Task<QuickActionResult> ArchiveAsync(IReadOnlyList<string> paths, string? outputDirectory = null) =>
        Task.Run(() => Archive(paths, outputDirectory));

    public static QuickActionResult Archive(IReadOnlyList<string> paths, string? outputDirectory = null)
    {
        var sources = NormalizeSources(paths);
        if (sources.Length == 0) return new QuickActionResult(false, "没有可压缩或解压的文件");
        if (sources.Length == 1 && File.Exists(sources[0]))
        {
            var extension = Path.GetExtension(sources[0]);
            if (string.Equals(extension, ".zip", StringComparison.OrdinalIgnoreCase))
                return ExtractZip(sources[0], outputDirectory);
            if (string.Equals(extension, ".7z", StringComparison.OrdinalIgnoreCase))
                return Extract7Zip(sources[0], outputDirectory);
        }
        return Compress(sources, outputDirectory);
    }

    public static QuickActionResult Compress(IReadOnlyList<string> paths, string? outputDirectory = null)
    {
        var sources = NormalizeSources(paths);
        if (sources.Length == 0) return new QuickActionResult(false, "没有可压缩的文件");

        string? temporaryPath = null;
        try
        {
            var destinationDirectory = ResolveDestinationDirectory(sources[0], outputDirectory);
            var archiveName = sources.Length == 1 ? ResolveSourceName(sources[0]) : "DropShelf 压缩文件";
            var outputPath = GetAvailableFilePath(destinationDirectory, archiveName, ".zip");
            temporaryPath = Path.Combine(destinationDirectory, $".dropshelf-{Guid.NewGuid():N}.tmp");

            using (var stream = new FileStream(temporaryPath, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None))
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Create, leaveOpen: false))
            {
                var usedEntryNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var source in sources)
                {
                    if (File.Exists(source))
                    {
                        var entryName = GetAvailableEntryName(Path.GetFileName(source), usedEntryNames);
                        archive.CreateEntryFromFile(source, entryName, CompressionLevel.Optimal);
                    }
                    else
                    {
                        AddDirectory(archive, source, usedEntryNames);
                    }
                }
            }

            File.Move(temporaryPath, outputPath);
            temporaryPath = null;
            return new QuickActionResult(true, $"已创建 {Path.GetFileName(outputPath)}", [outputPath]);
        }
        catch (Exception exception)
        {
            LogService.Error("快速压缩失败", exception);
            return new QuickActionResult(false, "压缩失败，原文件没有改变");
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

    public static async Task<QuickActionResult> SaveCopiesAsync(IReadOnlyList<string> paths)
    {
        try
        {
            var results = await new FileOperationService().CopyAsync(paths, GetPersistentOutputDirectory());
            var outputs = results.Where(result => result.Succeeded && result.Destination is not null)
                .Select(result => result.Destination!)
                .ToArray();
            return outputs.Length > 0
                ? new QuickActionResult(true, $"已保存 {outputs.Length} 项到下载文件夹", outputs)
                : new QuickActionResult(false, "附件保存失败，无法复制可用路径");
        }
        catch (Exception exception)
        {
            LogService.Error("保存附件副本失败", exception);
            return new QuickActionResult(false, "附件保存失败，无法复制可用路径");
        }
    }

    public static string GetPersistentOutputDirectory()
    {
        var directory = TryGetDownloadsDirectory();
        if (string.IsNullOrWhiteSpace(directory))
        {
            var userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            directory = string.IsNullOrWhiteSpace(userProfile)
                ? Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)
                : Path.Combine(userProfile, "Downloads");
        }
        Directory.CreateDirectory(directory);
        return directory;
    }

    private static string? TryGetDownloadsDirectory() => ShellInterop.GetKnownFolderPath(DownloadsFolderId);

    public static bool IsNative7ZipAvailable() => File.Exists(GetWindowsTarPath());

    private static QuickActionResult ExtractZip(string source, string? outputDirectory)
    {
        string? temporaryDirectory = null;
        try
        {
            var parent = ResolveDestinationDirectory(source, outputDirectory);
            var destination = GetAvailableDirectoryPath(parent, Path.GetFileNameWithoutExtension(source));
            temporaryDirectory = Path.Combine(parent, $".dropshelf-{Guid.NewGuid():N}");
            Directory.CreateDirectory(temporaryDirectory);
            ExtractZipSafely(source, temporaryDirectory);
            Directory.Move(temporaryDirectory, destination);
            temporaryDirectory = null;
            return new QuickActionResult(true, $"已解压到 {Path.GetFileName(destination)}", [destination]);
        }
        catch (Exception exception)
        {
            LogService.Error("快速解压失败", exception);
            return new QuickActionResult(false, "解压失败，压缩包没有改变");
        }
        finally
        {
            if (temporaryDirectory is not null)
            {
                try { Directory.Delete(temporaryDirectory, true); }
                catch { }
            }
        }
    }

    private static QuickActionResult Extract7Zip(string source, string? outputDirectory)
    {
        string? temporaryDirectory = null;
        try
        {
            var tarPath = GetWindowsTarPath();
            if (!File.Exists(tarPath))
                return new QuickActionResult(false, "当前 Windows 版本不支持内置 7z 解压");

            var parent = ResolveDestinationDirectory(source, outputDirectory);
            var destination = GetAvailableDirectoryPath(parent, Path.GetFileNameWithoutExtension(source));
            temporaryDirectory = Path.Combine(parent, $".dropshelf-{Guid.NewGuid():N}");
            Directory.CreateDirectory(temporaryDirectory);

            var listing = Validate7ZipListing(tarPath, source, temporaryDirectory);
            if (listing.ExitCode != 0)
                throw new InvalidDataException(string.IsNullOrWhiteSpace(listing.Error)
                    ? "Windows 无法读取此 7z 文件"
                    : listing.Error.Trim());

            var extraction = RunWindowsTar(tarPath, ["-xf", source, "-C", temporaryDirectory]);
            if (extraction.ExitCode != 0)
                throw new InvalidDataException(string.IsNullOrWhiteSpace(extraction.Error)
                    ? "Windows 无法解压此 7z 文件"
                    : extraction.Error.Trim());
            RejectExtractedLinks(temporaryDirectory);

            Directory.Move(temporaryDirectory, destination);
            temporaryDirectory = null;
            return new QuickActionResult(true, $"已解压到 {Path.GetFileName(destination)}", [destination]);
        }
        catch (Exception exception)
        {
            LogService.Error("Windows 内置 7z 解压失败", exception);
            return new QuickActionResult(false, "7z 解压失败，压缩包没有改变");
        }
        finally
        {
            if (temporaryDirectory is not null)
            {
                try { Directory.Delete(temporaryDirectory, true); }
                catch { }
            }
        }
    }

    private static void ExtractZipSafely(string source, string destination)
    {
        var destinationBase = Path.GetFullPath(destination);
        var destinationRoot = destinationBase + Path.DirectorySeparatorChar;
        using var archive = ZipFile.OpenRead(source);
        foreach (var entry in archive.Entries)
        {
            var relativePath = entry.FullName.Replace('/', Path.DirectorySeparatorChar)
                .Replace('\\', Path.DirectorySeparatorChar);
            var targetPath = Path.GetFullPath(Path.Combine(destination, relativePath));
            if (!string.Equals(targetPath, destinationBase, StringComparison.OrdinalIgnoreCase) &&
                !targetPath.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("压缩包包含不安全路径");

            if (string.IsNullOrEmpty(entry.Name))
            {
                Directory.CreateDirectory(targetPath);
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);
            using var input = entry.Open();
            using var output = new FileStream(targetPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            input.CopyTo(output);
        }
    }

    private static (int ExitCode, string Error) Validate7ZipListing(
        string tarPath,
        string source,
        string destination)
    {
        var startInfo = CreateTarStartInfo(tarPath, ["-tf", source]);
        using var process = Process.Start(startInfo) ?? throw new IOException("Windows 解压程序无法启动");
        var errorTask = process.StandardError.ReadToEndAsync();
        var destinationBase = Path.GetFullPath(destination);
        var destinationRoot = destinationBase + Path.DirectorySeparatorChar;
        try
        {
            while (process.StandardOutput.ReadLine() is { } entry)
                ValidateArchiveEntryPath(entry, destination, destinationBase, destinationRoot);
            process.WaitForExit();
            return (process.ExitCode, errorTask.GetAwaiter().GetResult());
        }
        catch
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch { }
            throw;
        }
    }

    private static void ValidateArchiveEntryPath(
        string entry,
        string destination,
        string destinationBase,
        string destinationRoot)
    {
        if (string.IsNullOrWhiteSpace(entry)) return;
        var relativePath = entry.Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);
        var targetPath = Path.GetFullPath(Path.Combine(destination, relativePath));
        if (!string.Equals(targetPath, destinationBase, StringComparison.OrdinalIgnoreCase) &&
            !targetPath.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("7z 文件包含不安全路径");
    }

    private static void RejectExtractedLinks(string destination)
    {
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true,
            IgnoreInaccessible = false,
            AttributesToSkip = 0,
            ReturnSpecialDirectories = false
        };
        foreach (var path in Directory.EnumerateFileSystemEntries(destination, "*", options))
        {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("7z 文件包含不支持的链接项目");
        }
    }

    private static (int ExitCode, string Error) RunWindowsTar(string tarPath, IReadOnlyList<string> arguments)
    {
        var startInfo = CreateTarStartInfo(tarPath, arguments);
        using var process = Process.Start(startInfo) ?? throw new IOException("Windows 解压程序无法启动");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        _ = outputTask.GetAwaiter().GetResult();
        return (process.ExitCode, errorTask.GetAwaiter().GetResult());
    }

    private static ProcessStartInfo CreateTarStartInfo(string tarPath, IReadOnlyList<string> arguments)
    {
        var startInfo = new ProcessStartInfo(tarPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var argument in arguments) startInfo.ArgumentList.Add(argument);
        return startInfo;
    }

    private static string GetWindowsTarPath()
    {
        var systemDirectory = Environment.GetFolderPath(Environment.SpecialFolder.System);
        return string.IsNullOrWhiteSpace(systemDirectory) ? string.Empty : Path.Combine(systemDirectory, "tar.exe");
    }

    private static void AddDirectory(ZipArchive archive, string directoryPath, HashSet<string> usedEntryNames)
    {
        var rootName = GetAvailableEntryName(new DirectoryInfo(directoryPath).Name, usedEntryNames);
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true,
            IgnoreInaccessible = false,
            AttributesToSkip = FileAttributes.ReparsePoint,
            ReturnSpecialDirectories = false
        };

        var hasEntries = false;
        foreach (var directory in Directory.EnumerateDirectories(directoryPath, "*", options))
        {
            if (Directory.EnumerateFileSystemEntries(directory).Any()) continue;
            var relative = Path.GetRelativePath(directoryPath, directory).Replace('\\', '/');
            archive.CreateEntry($"{rootName}/{relative}/");
            hasEntries = true;
        }

        foreach (var file in Directory.EnumerateFiles(directoryPath, "*", options))
        {
            var relative = Path.GetRelativePath(directoryPath, file).Replace('\\', '/');
            archive.CreateEntryFromFile(file, $"{rootName}/{relative}", CompressionLevel.Optimal);
            hasEntries = true;
        }

        if (!hasEntries) archive.CreateEntry(rootName + "/");
    }

    private static string[] NormalizeSources(IEnumerable<string> paths) => paths
        .Where(IsAvailablePath)
        .Select(Path.GetFullPath)
        .Distinct(StringComparer.OrdinalIgnoreCase)
        .ToArray();

    private static bool IsAvailablePath(string? path) =>
        !string.IsNullOrWhiteSpace(path) && (File.Exists(path) || Directory.Exists(path));

    private static string ResolveDestinationDirectory(string source, string? requestedDirectory)
    {
        if (!string.IsNullOrWhiteSpace(requestedDirectory))
        {
            Directory.CreateDirectory(requestedDirectory);
            return requestedDirectory;
        }
        return File.Exists(source)
            ? Path.GetDirectoryName(source) ?? throw new IOException("无法确定输出文件保存位置")
            : Directory.GetParent(source)?.FullName ?? throw new IOException("无法在磁盘根目录旁创建输出文件");
    }

    private static string ResolveSourceName(string source) => File.Exists(source)
        ? Path.GetFileNameWithoutExtension(source)
        : new DirectoryInfo(source).Name;

    private static string GetAvailableFilePath(string directory, string baseName, string extension)
    {
        var candidate = Path.Combine(directory, baseName + extension);
        for (var suffix = 2; File.Exists(candidate) || Directory.Exists(candidate); suffix++)
            candidate = Path.Combine(directory, $"{baseName} ({suffix}){extension}");
        return candidate;
    }

    private static string GetAvailableDirectoryPath(string directory, string baseName)
    {
        var candidate = Path.Combine(directory, baseName);
        for (var suffix = 2; File.Exists(candidate) || Directory.Exists(candidate); suffix++)
            candidate = Path.Combine(directory, $"{baseName} ({suffix})");
        return candidate;
    }

    private static string GetAvailableEntryName(string requestedName, HashSet<string> usedNames)
    {
        var candidate = requestedName;
        var name = Path.GetFileNameWithoutExtension(requestedName);
        var extension = Path.GetExtension(requestedName);
        for (var suffix = 2; !usedNames.Add(candidate); suffix++)
            candidate = $"{name} ({suffix}){extension}";
        return candidate.Replace('\\', '/');
    }
}
