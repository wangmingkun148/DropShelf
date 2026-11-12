using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace DropShelf.Interop;

/// <summary>
/// 文件在磁盘卷上的标识（卷序列号加文件编号）。改名、在同一个磁盘里移动都不会变，所以原来的路径找不到时可以按它找到文件现在在哪。
/// 只在 NTFS 上使用：别的文件系统的编号不稳定。取标识和按标识查找都只打开文件的属性，不读内容，也不妨碍别的程序使用文件。
/// </summary>
public readonly record struct FileIdentity(uint VolumeSerial, ulong FileIndex)
{
    private const uint ShareAll = 0x1 | 0x2 | 0x4;
    /// <summary>FILE_READ_ATTRIBUTES：只读属性，不涉及文件内容，别的程序独占打开着的文件也能取到。</summary>
    private const uint ReadAttributes = 0x80;
    private const uint OpenExisting = 3;
    /// <summary>FILE_FLAG_BACKUP_SEMANTICS：打开文件夹时需要。</summary>
    private const uint BackupSemantics = 0x02000000;
    private const int MaximumPath = 32767;

    /// <summary>取文件或文件夹的标识；网络路径、非 NTFS 磁盘或打不开时返回 null。</summary>
    public static FileIdentity? TryGet(string path)
    {
        try
        {
            if (!IsLocalPath(path)) return null;
            using var handle = CreateFileW(path, ReadAttributes, ShareAll, IntPtr.Zero, OpenExisting, BackupSemantics, IntPtr.Zero);
            return handle.IsInvalid ? null : Read(handle);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return null;
        }
    }

    /// <summary>
    /// 按标识找文件现在的路径。originalPath 是它原来的路径，用来确定在哪个磁盘上找；找不到、已经进了回收站，
    /// 或者找到的不是同一类（文件变成了文件夹）时返回 null。
    /// </summary>
    public string? Resolve(string originalPath, bool isFolder)
    {
        try
        {
            var root = Path.GetPathRoot(originalPath);
            if (string.IsNullOrEmpty(root) || !IsLocalPath(originalPath)) return null;
            using var volume = CreateFileW(root, ReadAttributes, ShareAll, IntPtr.Zero, OpenExisting, BackupSemantics, IntPtr.Zero);
            if (volume.IsInvalid) return null;

            var descriptor = new FileIdDescriptor { Size = (uint)Marshal.SizeOf<FileIdDescriptor>(), Type = 0, FileId = unchecked((long)FileIndex) };
            using var handle = OpenFileById(volume, ref descriptor, ReadAttributes, ShareAll, IntPtr.Zero, BackupSemantics);
            if (handle.IsInvalid || Read(handle) != this) return null;
            if (!GetFileInformationByHandle(handle, out var information)) return null;
            if (((FileAttributes)information.FileAttributes).HasFlag(FileAttributes.Directory) != isFolder) return null;

            var buffer = new char[MaximumPath];
            var length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Length, 0);
            if (length == 0 || length >= buffer.Length) return null;
            var path = new string(buffer, 0, (int)length);
            if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) return null;
            if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) path = path[4..];
            // 删除到回收站也是在同一个磁盘里移动：这种算删除，不跟过去。
            return path.Contains(@"\$Recycle.Bin\", StringComparison.OrdinalIgnoreCase) ? null : path;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            return null;
        }
    }

    /// <summary>本地磁盘上的完整路径（C:\…）；网络路径和相对路径不取标识。</summary>
    private static bool IsLocalPath(string path) =>
        path.Length >= 3 && char.IsAsciiLetter(path[0]) && path[1] == ':' && path[2] is '\\' or '/';

    private static FileIdentity? Read(SafeFileHandle handle)
    {
        var name = new char[16];
        if (!GetVolumeInformationByHandleW(handle, IntPtr.Zero, 0, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, name, (uint)name.Length)) return null;
        var end = Array.IndexOf(name, '\0');
        if (!new string(name, 0, end < 0 ? name.Length : end).Equals("NTFS", StringComparison.OrdinalIgnoreCase)) return null;
        if (!GetFileInformationByHandle(handle, out var information)) return null;
        var index = ((ulong)information.FileIndexHigh << 32) | information.FileIndexLow;
        return index == 0 ? null : new FileIdentity(information.VolumeSerialNumber, index);
    }

    /// <summary>BY_HANDLE_FILE_INFORMATION：三个时间各占 8 字节但按 4 字节对齐，所以整体按 4 字节对齐，共 52 字节。</summary>
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public long CreationTime;
        public long LastAccessTime;
        public long LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    /// <summary>FILE_ID_DESCRIPTOR：大小、类型（0 为文件编号），后面是 16 字节的联合体，文件编号占前 8 字节。</summary>
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    private struct FileIdDescriptor
    {
        [FieldOffset(0)] public uint Size;
        [FieldOffset(4)] public int Type;
        [FieldOffset(8)] public long FileId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string fileName, uint desiredAccess, uint shareMode, IntPtr securityAttributes,
        uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle file, out ByHandleFileInformation information);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetVolumeInformationByHandleW(SafeFileHandle file, IntPtr volumeName, uint volumeNameSize, IntPtr serialNumber,
        IntPtr maximumComponentLength, IntPtr fileSystemFlags, [Out] char[] fileSystemName, uint fileSystemNameSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeFileHandle OpenFileById(SafeFileHandle volumeHint, ref FileIdDescriptor fileId, uint desiredAccess, uint shareMode,
        IntPtr securityAttributes, uint flagsAndAttributes);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, [Out] char[] filePath, uint filePathSize, uint flags);
}
