using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using DropShelf.Localization;
using WpfDataObject = System.Windows.IDataObject;
using ComDataObject = System.Runtime.InteropServices.ComTypes.IDataObject;
using NativeMethods = DropShelf.Interop.NativeMethods;

namespace DropShelf.Services;

public sealed record VirtualFileImportResult(
    IReadOnlyList<string> Paths,
    string? CacheDirectory,
    int FailedCount,
    string? ErrorMessage);

public static class VirtualFileDropService
{
    public const string FileDescriptorUnicodeFormat = "FileGroupDescriptorW";
    public const string FileDescriptorAnsiFormat = "FileGroupDescriptor";
    public const string FileContentsFormat = "FileContents";

    private const int MaximumAttachmentCount = 256;
    private const int FileAttributeDirectory = 0x10;
    private const uint FileDescriptorAttributesValid = 0x00000004;
    private const uint FileDescriptorWriteTimeValid = 0x00000020;
    internal static readonly string CacheRoot = Path.Combine(Path.GetTempPath(), "DropShelf", "Attachments");

    public static bool CanImport(WpfDataObject data) =>
        data.GetDataPresent(FileDescriptorUnicodeFormat, false) ||
        data.GetDataPresent(FileDescriptorAnsiFormat, false);

    public static async Task<VirtualFileImportResult> ImportAsync(WpfDataObject data)
    {
        if (!CanImport(data)) return new VirtualFileImportResult([], null, 0, Strings.NoVirtualAttachments);
        if (data is not ComDataObject comDataObject)
            return new VirtualFileImportResult([], null, 0, Strings.NonStandardAttachments);

        IReadOnlyList<VirtualFileDescriptor> descriptors;
        try
        {
            descriptors = ReadDescriptors(data);
        }
        catch (Exception exception)
        {
            LogService.Error("读取虚拟附件名称失败", exception);
            return new VirtualFileImportResult([], null, 0, Strings.AttachmentInfoUnreadable);
        }

        if (descriptors.Count == 0)
            return new VirtualFileImportResult([], null, 0, Strings.NoFileAttachments);

        var cacheDirectory = CreateCacheDirectory();
        List<PreparedVirtualFile>? preparedFiles = null;
        int preparationFailures;

        try
        {
            // 必须在 WPF 的 Drop 回调线程中取得 STGMEDIUM；部分来源程序不会从后台线程交出附件流。
            preparedFiles = PrepareContents(comDataObject, descriptors, cacheDirectory, out preparationFailures);
            if (preparedFiles.Count == 0)
            {
                await DeleteCacheAsync(cacheDirectory);
                return new VirtualFileImportResult([], null, preparationFailures, Strings.AttachmentUnavailable);
            }

            return await Task.Run(() => ImportPreparedContents(preparedFiles, cacheDirectory, preparationFailures));
        }
        catch (Exception exception)
        {
            LogService.Error("接收虚拟附件失败", exception);
            if (preparedFiles is not null) ReleasePreparedContents(preparedFiles);
            await DeleteCacheAsync(cacheDirectory);
            return new VirtualFileImportResult([], null, descriptors.Count, Strings.AttachmentReceiveFailed);
        }
    }

    /// <summary>为一次接收创建独立的临时目录，关闭置物架后整体删除。</summary>
    public static string CreateCacheDirectory()
    {
        var directory = Path.Combine(CacheRoot, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        return directory;
    }

    /// <summary>只读取附件名称，不取出内容；用于判断拖入的是否为网页链接快捷方式。</summary>
    public static IReadOnlyList<string> GetDescriptorNames(WpfDataObject data)
    {
        try
        {
            return CanImport(data) ? ReadDescriptors(data).Select(descriptor => descriptor.Name).ToArray() : [];
        }
        catch
        {
            return [];
        }
    }

    public static Task DeleteCacheAsync(string? cacheDirectory)
    {
        if (!IsOwnedCacheDirectory(cacheDirectory)) return Task.CompletedTask;
        return Task.Run(() =>
        {
            try
            {
                if (Directory.Exists(cacheDirectory)) Directory.Delete(cacheDirectory, true);
            }
            catch (Exception exception)
            {
                LogService.Error("清理临时附件失败", exception);
            }
        });
    }

    public static Task CleanupStaleCacheAsync() => Task.Run(() =>
    {
        try
        {
            if (!Directory.Exists(CacheRoot)) return;
            var cutoff = DateTime.UtcNow.AddDays(-1);
            foreach (var directory in Directory.EnumerateDirectories(CacheRoot))
            {
                try
                {
                    if (Directory.GetLastWriteTimeUtc(directory) < cutoff && IsOwnedCacheDirectory(directory))
                        Directory.Delete(directory, true);
                }
                catch
                {
                    // 单个旧缓存无法清理时不影响程序启动。
                }
            }
        }
        catch
        {
            // 缓存清理不是主功能。
        }
    });

    private static List<PreparedVirtualFile> PrepareContents(
        ComDataObject data,
        IReadOnlyList<VirtualFileDescriptor> descriptors,
        string cacheDirectory,
        out int failed)
    {
        var prepared = new List<PreparedVirtualFile>();
        failed = 0;
        var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var formatId = (short)System.Windows.DataFormats.GetDataFormat(FileContentsFormat).Id;

        foreach (var descriptor in descriptors)
        {
            var safeName = CreateUniqueSafeName(descriptor.Name, usedNames);
            var destination = Path.Combine(cacheDirectory, safeName);
            var format = new FORMATETC
            {
                cfFormat = formatId,
                dwAspect = DVASPECT.DVASPECT_CONTENT,
                lindex = descriptor.Index,
                ptd = IntPtr.Zero,
                tymed = TYMED.TYMED_ISTREAM | TYMED.TYMED_HGLOBAL | TYMED.TYMED_FILE
            };
            try
            {
                data.GetData(ref format, out var medium);
                prepared.Add(new PreparedVirtualFile(descriptor, destination, medium));
            }
            catch (Exception exception)
            {
                failed++;
                LogService.Error($"来源程序未能交出附件 {safeName}", exception);
            }
        }

        return prepared;
    }

    private static VirtualFileImportResult ImportPreparedContents(
        IReadOnlyList<PreparedVirtualFile> preparedFiles,
        string cacheDirectory,
        int failed)
    {
        var paths = new List<string>();
        foreach (var prepared in preparedFiles)
        {
            try
            {
                CopyPreparedContent(prepared.Medium, prepared.Destination);
                var descriptor = prepared.Descriptor;
                if (descriptor.LastWriteTimeUtc is { } lastWriteTime)
                    File.SetLastWriteTimeUtc(prepared.Destination, lastWriteTime);
                paths.Add(prepared.Destination);
            }
            catch (Exception exception)
            {
                failed++;
                LogService.Error($"无法写入虚拟附件 {Path.GetFileName(prepared.Destination)}", exception);
                try { if (File.Exists(prepared.Destination)) File.Delete(prepared.Destination); } catch { }
            }
            finally
            {
                prepared.Release();
            }
        }

        return new VirtualFileImportResult(
            paths,
            cacheDirectory,
            failed,
            paths.Count == 0 ? Strings.AttachmentReceiveFailed : null);
    }

    private static void CopyPreparedContent(STGMEDIUM medium, string destination)
    {
        using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 81920, FileOptions.SequentialScan);
        switch (medium.tymed)
        {
            case TYMED.TYMED_ISTREAM:
                CopyComStream(medium.unionmember, output);
                break;
            case TYMED.TYMED_HGLOBAL:
                CopyGlobalMemory(medium.unionmember, output);
                break;
            case TYMED.TYMED_FILE:
                var sourcePath = Marshal.PtrToStringUni(medium.unionmember);
                if (string.IsNullOrWhiteSpace(sourcePath)) throw new IOException("附件来源没有提供临时文件");
                using (var input = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    input.CopyTo(output);
                break;
            default:
                throw new NotSupportedException($"不支持的附件传输方式：{medium.tymed}");
        }
    }

    private static void ReleasePreparedContents(IEnumerable<PreparedVirtualFile> preparedFiles)
    {
        foreach (var prepared in preparedFiles) prepared.Release();
    }

    private static void CopyComStream(IntPtr streamPointer, Stream output)
    {
        if (streamPointer == IntPtr.Zero) throw new IOException("附件内容流为空");
        var streamObject = Marshal.GetObjectForIUnknown(streamPointer);
        try
        {
            if (streamObject is not IStream input) throw new IOException("附件内容流不可读");
            var buffer = new byte[81920];
            var bytesReadPointer = Marshal.AllocCoTaskMem(sizeof(int));
            try
            {
                while (true)
                {
                    Marshal.WriteInt32(bytesReadPointer, 0);
                    input.Read(buffer, buffer.Length, bytesReadPointer);
                    var bytesRead = Marshal.ReadInt32(bytesReadPointer);
                    if (bytesRead <= 0) break;
                    output.Write(buffer, 0, bytesRead);
                }
            }
            finally
            {
                Marshal.FreeCoTaskMem(bytesReadPointer);
            }
        }
        finally
        {
            if (Marshal.IsComObject(streamObject)) Marshal.ReleaseComObject(streamObject);
        }
    }

    private static void CopyGlobalMemory(IntPtr handle, Stream output)
    {
        if (handle == IntPtr.Zero) throw new IOException("附件内存内容为空");
        var size = checked((long)NativeMethods.GlobalSize(handle));
        if (size == 0) return;
        var pointer = NativeMethods.GlobalLock(handle);
        if (pointer == IntPtr.Zero) throw new IOException("无法读取附件内存内容");
        try
        {
            var buffer = new byte[81920];
            long offset = 0;
            while (offset < size)
            {
                var count = (int)Math.Min(buffer.Length, size - offset);
                Marshal.Copy(new IntPtr(pointer.ToInt64() + offset), buffer, 0, count);
                output.Write(buffer, 0, count);
                offset += count;
            }
        }
        finally
        {
            NativeMethods.GlobalUnlock(handle);
        }
    }

    private static IReadOnlyList<VirtualFileDescriptor> ReadDescriptors(WpfDataObject data)
    {
        var unicode = data.GetDataPresent(FileDescriptorUnicodeFormat, false);
        var value = data.GetData(unicode ? FileDescriptorUnicodeFormat : FileDescriptorAnsiFormat, false);
        var bytes = ReadDescriptorBytes(value);
        if (bytes.Length < sizeof(uint)) return [];

        var count = BitConverter.ToUInt32(bytes, 0);
        if (count == 0 || count > MaximumAttachmentCount) return [];

        return unicode
            ? ParseDescriptors<FileDescriptorUnicode>(bytes, (int)count, descriptor => descriptor.FileName,
                descriptor => descriptor.Flags, descriptor => descriptor.FileAttributes, descriptor => descriptor.LastWriteTime)
            : ParseDescriptors<FileDescriptorAnsi>(bytes, (int)count, descriptor => descriptor.FileName,
                descriptor => descriptor.Flags, descriptor => descriptor.FileAttributes, descriptor => descriptor.LastWriteTime);
    }

    private static IReadOnlyList<VirtualFileDescriptor> ParseDescriptors<T>(
        byte[] bytes,
        int count,
        Func<T, string> getName,
        Func<T, uint> getFlags,
        Func<T, uint> getAttributes,
        Func<T, System.Runtime.InteropServices.ComTypes.FILETIME> getLastWriteTime) where T : struct
    {
        var descriptorSize = Marshal.SizeOf<T>();
        if (bytes.Length < sizeof(uint) + descriptorSize * count) return [];

        var results = new List<VirtualFileDescriptor>(count);
        var handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            var basePointer = handle.AddrOfPinnedObject();
            for (var index = 0; index < count; index++)
            {
                var pointer = IntPtr.Add(basePointer, sizeof(uint) + index * descriptorSize);
                var descriptor = Marshal.PtrToStructure<T>(pointer);
                var flags = getFlags(descriptor);
                if ((flags & FileDescriptorAttributesValid) != 0 &&
                    (getAttributes(descriptor) & FileAttributeDirectory) != 0) continue;
                var name = getName(descriptor)?.TrimEnd('\0') ?? string.Empty;
                if (string.IsNullOrWhiteSpace(name)) name = $"attachment-{index + 1}";
                var lastWriteTime = (flags & FileDescriptorWriteTimeValid) != 0
                    ? ToDateTimeUtc(getLastWriteTime(descriptor))
                    : null;
                results.Add(new VirtualFileDescriptor(index, name, lastWriteTime));
            }
        }
        finally
        {
            handle.Free();
        }

        return results;
    }

    private static byte[] ReadDescriptorBytes(object? value)
    {
        switch (value)
        {
            case byte[] bytes:
                return bytes;
            case MemoryStream memory:
                return memory.ToArray();
            case Stream stream:
                using (var copy = new MemoryStream())
                {
                    if (stream.CanSeek) stream.Position = 0;
                    stream.CopyTo(copy);
                    return copy.ToArray();
                }
            default:
                throw new InvalidDataException("附件描述格式无效");
        }
    }

    internal static string CreateUniqueSafeName(string suppliedName, ISet<string> usedNames)
    {
        var name = Path.GetFileName(suppliedName.Replace('/', Path.DirectorySeparatorChar).Replace('\\', Path.DirectorySeparatorChar));
        foreach (var invalid in Path.GetInvalidFileNameChars()) name = name.Replace(invalid, '_');
        name = name.Trim().TrimEnd('.');
        if (string.IsNullOrWhiteSpace(name)) name = "attachment";
        if (name.Length > 140)
        {
            var extension = Path.GetExtension(name);
            name = name[..Math.Max(1, 140 - extension.Length)] + extension;
        }

        var baseName = Path.GetFileNameWithoutExtension(name);
        var fileExtension = Path.GetExtension(name);
        if (ReservedFileNames.Contains(baseName))
        {
            baseName = "_" + baseName;
            name = baseName + fileExtension;
        }
        var candidate = name;
        for (var copy = 2; !usedNames.Add(candidate); copy++)
            candidate = $"{baseName} ({copy}){fileExtension}";
        return candidate;
    }

    private static DateTime? ToDateTimeUtc(System.Runtime.InteropServices.ComTypes.FILETIME fileTime)
    {
        var value = ((long)(uint)fileTime.dwHighDateTime << 32) | (uint)fileTime.dwLowDateTime;
        if (value <= 0) return null;
        try { return DateTime.FromFileTimeUtc(value); }
        catch { return null; }
    }

    internal static bool IsOwnedCacheDirectory(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return false;
        try
        {
            var root = Path.GetFullPath(CacheRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var candidate = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            return candidate.StartsWith(root, StringComparison.OrdinalIgnoreCase) &&
                   !candidate.Equals(root, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private sealed record VirtualFileDescriptor(int Index, string Name, DateTime? LastWriteTimeUtc);

    private sealed class PreparedVirtualFile(
        VirtualFileDescriptor descriptor,
        string destination,
        STGMEDIUM medium)
    {
        private int _released;
        public VirtualFileDescriptor Descriptor { get; } = descriptor;
        public string Destination { get; } = destination;
        public STGMEDIUM Medium = medium;

        public void Release()
        {
            if (Interlocked.Exchange(ref _released, 1) != 0) return;
            NativeMethods.ReleaseStgMedium(ref Medium);
        }
    }

    private static readonly HashSet<string> ReservedFileNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 4)]
    private struct FileDescriptorUnicode
    {
        public uint Flags;
        public Guid ClassId;
        public NativeSize Size;
        public NativePoint Point;
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string FileName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi, Pack = 4)]
    private struct FileDescriptorAnsi
    {
        public uint Flags;
        public Guid ClassId;
        public NativeSize Size;
        public NativePoint Point;
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string FileName;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeSize { public int Width; public int Height; }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint { public int X; public int Y; }

}
