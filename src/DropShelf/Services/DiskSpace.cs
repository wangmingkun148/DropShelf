using System.IO;

namespace DropShelf.Services;

internal static class DiskSpace
{
    /// <summary>解压和接收附件时给磁盘留下的最少剩余空间。</summary>
    public const long Reserve = 512L * 1024 * 1024;

    /// <summary>目录所在磁盘的剩余空间；网络位置等无法查询时为 null。</summary>
    public static long? Available(string directory)
    {
        try
        {
            return new DriveInfo(Path.GetPathRoot(Path.GetFullPath(directory))!).AvailableFreeSpace;
        }
        catch
        {
            return null;
        }
    }
}
