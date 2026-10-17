using System.Collections.Concurrent;
using System.IO;
using System.Runtime.InteropServices;
using DropShelf.Interop;

namespace DropShelf.Services;

public static class ShellFileInfo
{
    private static readonly ConcurrentDictionary<string, string> TypeNames = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>取得 Windows 显示的文件类型名称，例如“PDF 文件”。只按扩展名查询，不访问磁盘。</summary>
    public static string GetTypeName(string path)
    {
        var extension = Path.GetExtension(path);
        if (string.IsNullOrEmpty(extension)) return "文件";
        return TypeNames.GetOrAdd(extension, static ext =>
        {
            try
            {
                var info = new SHFILEINFOW();
                var result = NativeMethods.SHGetFileInfo("file" + ext, NativeMethods.FILE_ATTRIBUTE_NORMAL, ref info,
                    (uint)Marshal.SizeOf<SHFILEINFOW>(), NativeMethods.SHGFI_TYPENAME | NativeMethods.SHGFI_USEFILEATTRIBUTES);
                if (result != IntPtr.Zero && !string.IsNullOrWhiteSpace(info.TypeName)) return info.TypeName;
            }
            catch
            {
            }
            return ext.TrimStart('.').ToUpperInvariant() + " 文件";
        });
    }

    public static long? TryGetFileSize(string path)
    {
        try
        {
            var info = new FileInfo(path);
            return info.Exists ? info.Length : null;
        }
        catch
        {
            return null;
        }
    }
}
