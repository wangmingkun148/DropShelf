using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
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
        var paths = request.Paths.Where(FileOperationService.IsAvailablePath).ToArray();
        if (paths.Length == 0) return new QuickActionResult(false, Strings.NoFilesToProcess);
        if (request.Action.Custom is { } custom) return await ExecuteCustomAsync(custom, request.Action.Label, paths, request);

        switch (request.Action.Type)
        {
            case QuickActionType.CompressZip:
                try
                {
                    var outputDirectory = request.HasTemporaryFiles ? GetPersistentOutputDirectory() : null;
                    var archived = await ArchiveService.ArchiveAsync(paths, outputDirectory);
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
                    var results = await ImagingThread.RunLongAsync(() => ImageCompressor.CompressAll(paths, maximumBytes, outputDirectory));
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
        var available = paths.Where(FileOperationService.IsAvailablePath).ToArray();
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
}
