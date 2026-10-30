using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Text;
using DropShelf.Localization;

namespace DropShelf.Services;

/// <summary>快速压缩和解压：单个 .zip 或 .7z 解压到同名文件夹，其余打包成 .zip。解压前校验条目路径和大小，不会写到目标文件夹外，也不会把磁盘写满。</summary>
public static class ArchiveService
{
    private static readonly TimeSpan TarTimeout = TimeSpan.FromMinutes(30);

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
            var outputPath = FileOperationService.GetAvailablePath(destinationDirectory, archiveName, ".zip");
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

    public static bool IsNative7ZipAvailable() => File.Exists(GetWindowsTarPath());

    private static QuickActionResult ExtractZip(string source, string? outputDirectory)
    {
        string? temporaryDirectory = null;
        try
        {
            var parent = ResolveDestinationDirectory(source, outputDirectory);
            var destination = FileOperationService.GetAvailablePath(parent, Path.GetFileNameWithoutExtension(source));
            temporaryDirectory = Path.Combine(parent, $".dropshelf-{Guid.NewGuid():N}");
            Directory.CreateDirectory(temporaryDirectory);
            ExtractZipSafely(source, temporaryDirectory);
            ZoneIdentifier.Propagate(source, temporaryDirectory);
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
            var destination = FileOperationService.GetAvailablePath(parent, Path.GetFileNameWithoutExtension(source));
            temporaryDirectory = Path.Combine(parent, $".dropshelf-{Guid.NewGuid():N}");
            Directory.CreateDirectory(temporaryDirectory);

            var listing = Validate7ZipListing(tarPath, source, temporaryDirectory);
            if (listing.ExitCode != 0)
                throw new InvalidDataException(string.IsNullOrWhiteSpace(listing.Error)
                    ? "Windows 无法读取此 7z 文件"
                    : listing.Error.Trim());

            var extraction = RunWindowsTar(tarPath, ["-xf", source, "-C", temporaryDirectory], temporaryDirectory);
            if (extraction.ExitCode != 0)
                throw new InvalidDataException(string.IsNullOrWhiteSpace(extraction.Error)
                    ? "Windows 无法解压此 7z 文件"
                    : extraction.Error.Trim());
            RejectExtractedLinks(temporaryDirectory);
            ZoneIdentifier.Propagate(source, temporaryDirectory);

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
        // 网络位置等无法查询剩余空间时照常解压，仍受每个条目声明大小的限制。
        if (required > (DiskSpace.Available(directory) ?? long.MaxValue)) throw new IOException("磁盘空间不足，无法解压");
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
        // 超时后结束进程，读取列表的循环随之结束，按失败处理。
        using var timeout = new System.Threading.Timer(_ => StopProcess(process), null, TarTimeout, Timeout.InfiniteTimeSpan);
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
            StopProcess(process);
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

    /// <summary>
    /// 7z 无法预先得知解压后的大小：解压期间盯住磁盘剩余空间，低于保留量或超时就结束 tar，
    /// 调用方随后删除解出的部分内容。
    /// </summary>
    private static (int ExitCode, string Error) RunWindowsTar(string tarPath, IReadOnlyList<string> arguments, string outputDirectory)
    {
        var startInfo = CreateTarStartInfo(tarPath, arguments);
        using var process = Process.Start(startInfo) ?? throw new IOException("Windows 解压程序无法启动");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        var elapsed = Stopwatch.StartNew();
        while (!process.WaitForExit(500))
        {
            if (elapsed.Elapsed > TarTimeout)
            {
                StopProcess(process);
                throw new TimeoutException("7z 解压时间过长");
            }
            if (DiskSpace.Available(outputDirectory) < DiskSpace.Reserve)
            {
                StopProcess(process);
                throw new IOException("磁盘空间不足，无法解压");
            }
        }
        process.WaitForExit();
        _ = outputTask.GetAwaiter().GetResult();
        return (process.ExitCode, errorTask.GetAwaiter().GetResult());
    }

    /// <summary>结束进程并等它真正退出，之后才能删除它写了一半的文件。</summary>
    private static void StopProcess(Process process)
    {
        try
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            process.WaitForExit(5000);
        }
        catch
        {
        }
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
        .Where(FileOperationService.IsAvailablePath)
        .Select(Path.GetFullPath)
        .Distinct(StringComparer.OrdinalIgnoreCase)
        .ToArray();

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
