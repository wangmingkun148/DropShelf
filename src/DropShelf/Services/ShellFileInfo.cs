using System.Collections.Concurrent;
using System.IO;
using System.Runtime.InteropServices;
using DropShelf.Interop;
using DropShelf.Localization;

namespace DropShelf.Services;

public static class ShellFileInfo
{
    private static readonly ConcurrentDictionary<string, string?> TypeNames = new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, string> ProgramNames = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>取得 Windows 显示的文件类型名称，例如“PDF 文件”。只按扩展名查询，不访问磁盘。</summary>
    public static string GetTypeName(string path)
    {
        var extension = Path.GetExtension(path);
        if (string.IsNullOrEmpty(extension)) return Strings.GenericFileType;
        // 只缓存 Windows 提供的名称；备用名称跟随界面语言，每次重新生成。
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
            return null;
        }) ?? Strings.FileType(extension.TrimStart('.').ToUpperInvariant());
    }

    /// <summary>程序的显示名称：较短的文件说明（如“Paint”），否则用文件名。</summary>
    public static string GetProgramName(string path) => ProgramNames.GetOrAdd(path, static program =>
    {
        try
        {
            if (File.Exists(program) && System.Diagnostics.FileVersionInfo.GetVersionInfo(program).FileDescription?.Trim() is { Length: > 0 and <= 24 } description)
                return description;
        }
        catch
        {
        }
        return Path.GetFileNameWithoutExtension(program);
    });

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
