using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using DropShelf.Interop;
using DropShelf.Localization;
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

    /// <summary>处理后的新文件替换置物架中的原项目（如压缩后的图片），原文件保留在磁盘上。</summary>
    public IReadOnlyList<(string Source, string Output)>? Replacements { get; init; }

    /// <summary>操作生成了新文件（如压缩包），放进新的置物架方便继续拖走。</summary>
    public bool ProducedNewItems { get; init; }

    /// <summary>其他程序稍后才会读取文件（属性窗口、用程序打开），临时附件需要晚一些再清理。</summary>
    public bool KeepsTemporaryFiles { get; init; }
}

public sealed record QuickActionRequest(
    QuickActionInfo Action,
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
        if (paths.Length == 0) return new QuickActionResult(false, Strings.NoFilesToProcess);
        if (request.Action.Custom is { } custom) return await ExecuteCustomAsync(custom, request.Action.Label, paths, request);

        switch (request.Action.Type)
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
                    return new QuickActionResult(false, Strings.OutputFolderUnavailable);
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
                    ? new QuickActionResult(true, pathsToCopy.Count > 1 ? Strings.CopiedPaths(pathsToCopy.Count) : Strings.CopiedPath)
                    : new QuickActionResult(false, Strings.ClipboardBusyOtherApp);

            case QuickActionType.MoveTo:
                var destination = request.PickFolder?.Invoke();
                if (string.IsNullOrWhiteSpace(destination))
                    return new QuickActionResult(false, Strings.MoveCancelled) { Cancelled = true };
                return await MoveToFolderAsync(paths, destination);

            case QuickActionType.Reveal:
                return ShellInterop.OpenFolderAndSelect(paths)
                    ? new QuickActionResult(true, Strings.ShownInExplorer)
                    : new QuickActionResult(false, Strings.CannotOpenFolder);

            default:
                return ShowProperties(request.Owner, paths) with { KeepsTemporaryFiles = request.HasTemporaryFiles };
        }
    }

    private static async Task<QuickActionResult> ExecuteCustomAsync(CustomQuickAction custom, string label, string[] paths,
        QuickActionRequest request)
    {
        switch (custom.Kind)
        {
            case CustomActionKind.Folder:
                var folder = custom.Folder ?? string.Empty;
                if (!Directory.Exists(folder)) return new QuickActionResult(false, Strings.FolderMissing(GetFolderName(folder)));
                return custom.FolderMode == TransferMode.Copy
                    ? await CopyToFolderAsync(paths, folder)
                    : await MoveToFolderAsync(paths, folder);

            case CustomActionKind.CompressImage:
                try
                {
                    var outputDirectory = request.HasTemporaryFiles ? GetPersistentOutputDirectory() : null;
                    var maximumBytes = custom.ImageSizeKb * 1024L;
                    var results = await ImagingThread.RunAsync(() => ImageCompressor.CompressAll(paths, maximumBytes, outputDirectory));
                    return SummarizeImageCompression(results, custom.ImageSizeKb);
                }
                catch (Exception exception)
                {
                    LogService.Error("压缩图片失败", exception);
                    return new QuickActionResult(false, Strings.ImageCompressFailed);
                }

            default:
                return OpenWith(custom, label, paths) with { KeepsTemporaryFiles = request.HasTemporaryFiles };
        }
    }

    private static async Task<QuickActionResult> MoveToFolderAsync(IReadOnlyList<string> paths, string destination)
    {
        // 已经在目标文件夹中的项目不需要移动，否则会被重命名为“(2)”。
        var alreadyThere = paths.Where(path => IsDirectChild(path, destination)).ToArray();
        var results = await new FileOperationService().MoveAsync(paths.Except(alreadyThere, StringComparer.OrdinalIgnoreCase), destination);
        var moved = results.Where(result => result.Succeeded && result.Destination is not null)
            .Select(result => (result.Source, result.Destination!))
            .ToArray();
        var done = moved.Length + alreadyThere.Length;
        var failed = results.Count - moved.Length;
        var message = failed == 0
            ? Strings.MovedTo(done, GetFolderName(destination))
            : done == 0 ? Strings.MoveFailed : Strings.MovedPartially(done, failed);
        return new QuickActionResult(done > 0, message, moved.Select(item => item.Item2).ToArray()) { Moves = moved };
    }

    private static async Task<QuickActionResult> CopyToFolderAsync(IReadOnlyList<string> paths, string destination)
    {
        var results = await new FileOperationService().CopyAsync(paths, destination);
        var copied = results.Count(result => result.Succeeded);
        var failed = results.Count - copied;
        var message = failed == 0
            ? Strings.CopiedTo(copied, GetFolderName(destination))
            : copied == 0 ? Strings.CopyFailed : Strings.CopiedPartially(copied, failed);
        return new QuickActionResult(copied > 0, message);
    }

    private static QuickActionResult SummarizeImageCompression(IReadOnlyList<ImageCompressionResult> results, int sizeKb)
    {
        var compressed = results.Where(result => result.Status == ImageCompressionStatus.Compressed && result.Output is not null)
            .Select(result => (result.Source, result.Output!))
            .ToArray();
        var small = results.Count(result => result.Status == ImageCompressionStatus.AlreadySmall);
        var failed = results.Count(result => result.Status == ImageCompressionStatus.Failed);
        var images = compressed.Length + small + failed;
        var size = Strings.ImageSizeText(sizeKb);

        if (images == 0) return new QuickActionResult(false, Strings.NoImagesToCompress);
        if (compressed.Length == 0)
            return failed == 0
                ? new QuickActionResult(true, Strings.ImagesAlreadySmall(size))
                : new QuickActionResult(false, Strings.ImageCompressFailed);

        var message = Strings.ImagesCompressed(compressed.Length, size);
        if (small > 0) message += Strings.ImagesSkippedSmall(small);
        if (failed > 0) message += Strings.ImagesFailed(failed);
        return new QuickActionResult(true, message, compressed.Select(item => item.Item2).ToArray()) { Replacements = compressed };
    }

    private static QuickActionResult OpenWith(CustomQuickAction custom, string label, IReadOnlyList<string> paths)
    {
        var program = custom.ProgramPath ?? string.Empty;
        if (!File.Exists(program)) return new QuickActionResult(false, Strings.ProgramMissing(label));
        try
        {
            Process.Start(new ProcessStartInfo(program, BuildArguments(custom.Arguments, paths))
            {
                UseShellExecute = true,
                WorkingDirectory = Path.GetDirectoryName(program) ?? string.Empty
            });
            return new QuickActionResult(true, Strings.OpenedWith(paths.Count, label));
        }
        catch (Exception exception)
        {
            LogService.Error($"用程序打开失败：{program}", exception);
            return new QuickActionResult(false, Strings.OpenWithFailed(label));
        }
    }

    /// <summary>
    /// 生成程序参数：{files} 替换为加上引号的全部路径；没有 {files} 时把路径放在最后。
    /// </summary>
    public static string BuildArguments(string? template, IReadOnlyList<string> paths)
    {
        var files = string.Join(' ', paths.Select(QuoteArgument));
        if (string.IsNullOrWhiteSpace(template)) return files;
        return template.Contains("{files}", StringComparison.OrdinalIgnoreCase)
            ? template.Replace("{files}", files, StringComparison.OrdinalIgnoreCase)
            : $"{template.Trim()} {files}";
    }

    /// <summary>路径加引号；以反斜杠结尾的路径（如磁盘根目录）要补一个反斜杠，否则会转义结尾的引号。</summary>
    private static string QuoteArgument(string path) => path.EndsWith('\\') ? $"\"{path}\\\"" : $"\"{path}\"";

    private static bool IsDirectChild(string path, string folder)
    {
        try
        {
            var parent = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)));
            return parent is not null && string.Equals(Path.TrimEndingDirectorySeparator(parent),
                Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder)), StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    public static string GetFolderName(string folder)
    {
        var trimmed = Path.TrimEndingDirectorySeparator(folder);
        var name = Path.GetFileName(trimmed);
        return string.IsNullOrEmpty(name) ? trimmed : name;
    }

    public static QuickActionResult ShowProperties(IntPtr owner, IReadOnlyList<string> paths)
    {
        var available = paths.Where(IsAvailablePath).ToArray();
        if (available.Length == 0) return new QuickActionResult(false, Strings.NoFilesForProperties);

        try
        {
            if (available.Length > 1 && ShellInterop.CreateShellDataObject(available) is { } data)
            {
                try
                {
                    if (NativeMethods.SHMultiFileProperties(data, 0) == 0)
                        return new QuickActionResult(true, Strings.OpenedPropertiesFor(available.Length));
                }
                finally
                {
                    Marshal.ReleaseComObject(data);
                }
            }

            return NativeMethods.SHObjectProperties(owner, ShopFilePath, available[0], null)
                ? new QuickActionResult(true, Strings.OpenedProperties)
                : new QuickActionResult(false, Strings.PropertiesUnavailable);
        }
        catch (Exception exception)
        {
            LogService.Error("打开文件属性失败", exception);
            return new QuickActionResult(false, Strings.PropertiesFailed);
        }
    }

    public static Task<QuickActionResult> ArchiveAsync(IReadOnlyList<string> paths, string? outputDirectory = null) =>
        Task.Run(() => Archive(paths, outputDirectory));

    public static QuickActionResult Archive(IReadOnlyList<string> paths, string? outputDirectory = null)
    {
        var sources = NormalizeSources(paths);
        if (sources.Length == 0) return new QuickActionResult(false, Strings.NothingToArchive);
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
        if (sources.Length == 0) return new QuickActionResult(false, Strings.NothingToCompress);

        string? temporaryPath = null;
        try
        {
            var destinationDirectory = ResolveDestinationDirectory(sources[0], outputDirectory);
            var archiveName = sources.Length == 1 ? ResolveSourceName(sources[0]) : Strings.DefaultArchiveName;
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
            return new QuickActionResult(true, Strings.Created(Path.GetFileName(outputPath)), [outputPath]);
        }
        catch (Exception exception)
        {
            LogService.Error("快速压缩失败", exception);
            return new QuickActionResult(false, Strings.CompressFailed);
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
                ? new QuickActionResult(true, Strings.SavedToDownloads(outputs.Length), outputs)
                : new QuickActionResult(false, Strings.AttachmentSaveFailed);
        }
        catch (Exception exception)
        {
            LogService.Error("保存附件副本失败", exception);
            return new QuickActionResult(false, Strings.AttachmentSaveFailed);
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
            return new QuickActionResult(true, Strings.ExtractedTo(Path.GetFileName(destination)), [destination]);
        }
        catch (Exception exception)
        {
            LogService.Error("快速解压失败", exception);
            return new QuickActionResult(false, Strings.ExtractFailed);
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
                return new QuickActionResult(false, Strings.SevenZipUnsupported);

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
            return new QuickActionResult(true, Strings.ExtractedTo(Path.GetFileName(destination)), [destination]);
        }
        catch (Exception exception)
        {
            LogService.Error("Windows 内置 7z 解压失败", exception);
            return new QuickActionResult(false, Strings.SevenZipFailed);
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
        EnsureFreeSpace(destination, archive.Entries.Aggregate(0L, (total, entry) => checked(total + entry.Length)));
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
            CopyAtMost(input, output, entry.Length);
        }
    }

    /// <summary>压缩包声明的解压后总大小放不下时直接失败，不会先把磁盘写满。</summary>
    private static void EnsureFreeSpace(string directory, long required)
    {
        long available;
        try
        {
            available = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(directory))!).AvailableFreeSpace;
        }
        catch
        {
            // 网络位置等无法查询剩余空间时照常解压，仍受每个条目声明大小的限制。
            return;
        }
        if (required > available) throw new IOException("磁盘空间不足，无法解压");
    }

    /// <summary>实际解出的内容超过条目声明的大小时视为损坏或恶意的压缩包，防止声明很小、解出很大的压缩包写满磁盘。</summary>
    private static void CopyAtMost(Stream input, Stream output, long limit)
    {
        var buffer = new byte[81920];
        long written = 0;
        int read;
        while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
        {
            written += read;
            if (written > limit) throw new InvalidDataException("压缩包条目的实际大小超过声明的大小");
            output.Write(buffer, 0, read);
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
