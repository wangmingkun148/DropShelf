using System.Diagnostics;
using System.IO;

namespace DropShelf.Services;

/// <summary>文件夹里有几项、一共多大。只读目录里登记的名称和大小，不打开任何文件。</summary>
public readonly record struct FolderContents(int Children, long? SizeBytes)
{
    /// <summary>统计大小最多花这么久、最多看这么多个文件；超过了就只给出项目数，不显示大小。</summary>
    public static readonly TimeSpan SizeBudget = TimeSpan.FromSeconds(3);
    public const int MaximumFiles = 200_000;

    /// <summary>
    /// 第一层的项目数和整个文件夹（含子文件夹）的大小；可以在后台线程上调用。
    /// 联接点和符号链接不进去也不计入，没有权限的子文件夹跳过；文件夹读不了时返回 null。
    /// </summary>
    public static FolderContents? Measure(string path, CancellationToken cancellationToken = default) =>
        Measure(path, SizeBudget, MaximumFiles, cancellationToken);

    public static FolderContents? Measure(string path, TimeSpan budget, int maximumFiles, CancellationToken cancellationToken = default)
    {
        try
        {
            var children = 0;
            foreach (var _ in Directory.EnumerateFileSystemEntries(path))
            {
                cancellationToken.ThrowIfCancellationRequested();
                children++;
            }

            var options = new EnumerationOptions
            {
                RecurseSubdirectories = true,
                IgnoreInaccessible = true,
                AttributesToSkip = FileAttributes.ReparsePoint,
                ReturnSpecialDirectories = false
            };
            var started = Stopwatch.GetTimestamp();
            long size = 0;
            var files = 0;
            foreach (var file in new DirectoryInfo(path).EnumerateFiles("*", options))
            {
                cancellationToken.ThrowIfCancellationRequested();
                size += file.Length;
                if (++files > maximumFiles || Stopwatch.GetElapsedTime(started) > budget) return new FolderContents(children, null);
            }
            return new FolderContents(children, size);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or System.Security.SecurityException
                                              or ArgumentException or NotSupportedException)
        {
            return null;
        }
    }
}
