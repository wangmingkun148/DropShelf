using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace DropShelf.Interop;

[ComImport]
[Guid("B63EA76D-1F85-456F-A19C-48159EFA858B")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IShellItemArray
{
    [PreserveSig]
    int BindToHandler(IntPtr bindContext, ref Guid handlerId, ref Guid interfaceId, out IntPtr result);
}

[ComImport]
[Guid("4657278B-411B-11D2-839A-00C04FD918D0")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IDropTargetHelper
{
    [PreserveSig] int DragEnter(IntPtr target, [MarshalAs(UnmanagedType.Interface)] IDataObject data, ref POINT point, int effect);
    [PreserveSig] int DragLeave();
    [PreserveSig] int DragOver(ref POINT point, int effect);
    [PreserveSig] int Drop([MarshalAs(UnmanagedType.Interface)] IDataObject data, ref POINT point, int effect);
    [PreserveSig] int Show([MarshalAs(UnmanagedType.Bool)] bool show);
}

[ComImport]
[Guid("83E07D0D-0C5F-4163-BF1A-60B274051E40")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IDragSourceHelper2
{
    [PreserveSig] int InitializeFromBitmap(ref SHDRAGIMAGE image, [MarshalAs(UnmanagedType.Interface)] IDataObject data);
    [PreserveSig] int InitializeFromWindow(IntPtr window, ref POINT point, [MarshalAs(UnmanagedType.Interface)] IDataObject data);
    [PreserveSig] int SetFlags(int flags);
}

[ComImport]
[Guid("00000121-0000-0000-C000-000000000046")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IDropSource
{
    [PreserveSig] int QueryContinueDrag([MarshalAs(UnmanagedType.Bool)] bool escapePressed, int keyState);
    [PreserveSig] int GiveFeedback(int effect);
}

/// <summary>按住的鼠标键松开时放下，按 Esc 时取消，光标交给系统按放置说明绘制。</summary>
[ComVisible(true)]
internal sealed class DefaultDropSource : IDropSource
{
    private readonly int _button;

    public DefaultDropSource(int button) => _button = button;

    public int QueryContinueDrag(bool escapePressed, int keyState)
    {
        if (escapePressed) return NativeMethods.DRAGDROP_S_CANCEL;
        return (keyState & _button) == 0 ? NativeMethods.DRAGDROP_S_DROP : 0;
    }

    public int GiveFeedback(int effect) => NativeMethods.DRAGDROP_S_USEDEFAULTCURSORS;
}

internal static class ShellInterop
{
    public static readonly Guid DragDropHelperClassId = new("4657278A-411B-11D2-839A-00C04FD918D0");
    public static readonly Guid DataObjectHandlerId = new("B8C0BD9F-ED24-455C-83E6-D5390C4FE8C4");
    public static readonly Guid DataObjectInterfaceId = new("0000010E-0000-0000-C000-000000000046");
    public const int DragSourceAllowDropDescriptionText = 0x0001;

    public static T? CreateDragDropHelper<T>() where T : class
    {
        try
        {
            var type = Type.GetTypeFromCLSID(DragDropHelperClassId);
            return type is null ? null : Activator.CreateInstance(type) as T;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>为一组磁盘路径创建 Windows 资源管理器使用的同款数据对象。</summary>
    public static IDataObject? CreateShellDataObject(IReadOnlyList<string> paths)
    {
        var pidls = new List<IntPtr>(paths.Count);
        try
        {
            foreach (var path in paths)
            {
                if (SHParse(path) is { } pidl && pidl != IntPtr.Zero) pidls.Add(pidl);
            }

            if (pidls.Count == 0) return null;
            if (NativeMethods.SHCreateShellItemArrayFromIDLists((uint)pidls.Count, pidls.ToArray(), out var array) != 0 || array is null)
                return null;

            try
            {
                var handlerId = DataObjectHandlerId;
                var interfaceId = DataObjectInterfaceId;
                if (array.BindToHandler(IntPtr.Zero, ref handlerId, ref interfaceId, out var pointer) != 0 || pointer == IntPtr.Zero)
                    return null;
                try
                {
                    return Marshal.GetObjectForIUnknown(pointer) as IDataObject;
                }
                finally
                {
                    Marshal.Release(pointer);
                }
            }
            finally
            {
                Marshal.ReleaseComObject(array);
            }
        }
        finally
        {
            foreach (var pidl in pidls) NativeMethods.ILFree(pidl);
        }
    }

    /// <summary>
    /// 创建文件拖放数据：优先使用资源管理器同款数据对象，失败时退回标准 CF_HDROP 文件列表。
    /// </summary>
    public static IDataObject? CreateFileDataObject(IReadOnlyList<string> paths)
    {
        if (CreateShellDataObject(paths) is { } shell) return shell;
        var data = CreateEmptyDataObject();
        if (data is null) return null;
        return TrySetBytes(data, System.Windows.DataFormats.FileDrop, BuildDropFiles(paths)) ? data : null;
    }

    /// <summary>生成 DROPFILES 结构：20 字节头部 + 以两个空字符结尾的 UTF-16 路径列表。</summary>
    public static byte[] BuildDropFiles(IReadOnlyList<string> paths)
    {
        var list = Encoding.Unicode.GetBytes(string.Concat(paths.Select(path => path + char.MinValue)) + char.MinValue);
        var bytes = new byte[20 + list.Length];
        BitConverter.GetBytes(20).CopyTo(bytes, 0);
        BitConverter.GetBytes(1).CopyTo(bytes, 16);
        list.CopyTo(bytes, 20);
        return bytes;
    }

    /// <summary>创建空的 Windows 数据对象，可写入文本、链接等任意格式。</summary>
    public static IDataObject? CreateEmptyDataObject()
    {
        var interfaceId = DataObjectInterfaceId;
        if (NativeMethods.SHCreateDataObject(IntPtr.Zero, 0, IntPtr.Zero, null, ref interfaceId, out var pointer) != 0 ||
            pointer == IntPtr.Zero)
            return null;
        try
        {
            return Marshal.GetObjectForIUnknown(pointer) as IDataObject;
        }
        finally
        {
            Marshal.Release(pointer);
        }
    }

    public static IntPtr? SHParse(string path)
    {
        try
        {
            return NativeMethods.SHParseDisplayName(path, IntPtr.Zero, out var pidl, 0, out _) == 0 ? pidl : null;
        }
        catch
        {
            return null;
        }
    }

    public static short GetFormatId(string format) =>
        unchecked((short)System.Windows.DataFormats.GetDataFormat(format).Id);

    public static bool TrySetBytes(IDataObject data, string format, byte[] bytes)
    {
        var global = NativeMethods.GlobalAlloc(NativeMethods.GMEM_MOVEABLE | NativeMethods.GMEM_ZEROINIT, (UIntPtr)Math.Max(1, bytes.Length));
        if (global == IntPtr.Zero) return false;
        var pointer = NativeMethods.GlobalLock(global);
        if (pointer == IntPtr.Zero)
        {
            NativeMethods.GlobalFree(global);
            return false;
        }

        try
        {
            Marshal.Copy(bytes, 0, pointer, bytes.Length);
        }
        finally
        {
            NativeMethods.GlobalUnlock(global);
        }

        var formatEtc = new FORMATETC
        {
            cfFormat = GetFormatId(format),
            dwAspect = DVASPECT.DVASPECT_CONTENT,
            lindex = -1,
            ptd = IntPtr.Zero,
            tymed = TYMED.TYMED_HGLOBAL
        };
        var medium = new STGMEDIUM { tymed = TYMED.TYMED_HGLOBAL, unionmember = global, pUnkForRelease = null };
        try
        {
            data.SetData(ref formatEtc, ref medium, true);
            return true;
        }
        catch
        {
            NativeMethods.GlobalFree(global);
            return false;
        }
    }

    public static bool TrySetUnicodeString(IDataObject data, string format, string value) =>
        TrySetBytes(data, format, Encoding.Unicode.GetBytes(value + "\0"));

    public static bool TrySetInt32(IDataObject data, string format, int value) =>
        TrySetBytes(data, format, BitConverter.GetBytes(value));

    public static byte[]? TryGetBytes(IDataObject data, string format)
    {
        var formatEtc = new FORMATETC
        {
            cfFormat = GetFormatId(format),
            dwAspect = DVASPECT.DVASPECT_CONTENT,
            lindex = -1,
            ptd = IntPtr.Zero,
            tymed = TYMED.TYMED_HGLOBAL
        };
        try
        {
            if (data.QueryGetData(ref formatEtc) != 0) return null;
            data.GetData(ref formatEtc, out var medium);
            try
            {
                if (medium.tymed != TYMED.TYMED_HGLOBAL || medium.unionmember == IntPtr.Zero) return null;
                var size = (int)Math.Min(int.MaxValue, (long)NativeMethods.GlobalSize(medium.unionmember));
                var pointer = NativeMethods.GlobalLock(medium.unionmember);
                if (pointer == IntPtr.Zero) return null;
                try
                {
                    var bytes = new byte[size];
                    Marshal.Copy(pointer, bytes, 0, size);
                    return bytes;
                }
                finally
                {
                    NativeMethods.GlobalUnlock(medium.unionmember);
                }
            }
            finally
            {
                NativeMethods.ReleaseStgMedium(ref medium);
            }
        }
        catch
        {
            return null;
        }
    }

    public static int? TryGetInt32(IDataObject data, string format) =>
        TryGetBytes(data, format) is { Length: >= 4 } bytes ? BitConverter.ToInt32(bytes, 0) : null;

    public static bool TrySetDropDescription(IDataObject data, int type, string message, string insert = "")
    {
        var description = new DROPDESCRIPTION { Type = type, Message = message, Insert = insert };
        var size = Marshal.SizeOf<DROPDESCRIPTION>();
        var bytes = new byte[size];
        var handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            Marshal.StructureToPtr(description, handle.AddrOfPinnedObject(), false);
        }
        finally
        {
            handle.Free();
        }
        return TrySetBytes(data, "DropDescription", bytes);
    }

    public static string? GetKnownFolderPath(Guid folderId)
    {
        var pointer = IntPtr.Zero;
        try
        {
            return NativeMethods.SHGetKnownFolderPath(ref folderId, 0, IntPtr.Zero, out pointer) == 0
                ? Marshal.PtrToStringUni(pointer)
                : null;
        }
        catch
        {
            return null;
        }
        finally
        {
            if (pointer != IntPtr.Zero) Marshal.FreeCoTaskMem(pointer);
        }
    }

    public static bool OpenFolderAndSelect(IReadOnlyList<string> paths)
    {
        var groups = paths
            .Where(path => File.Exists(path) || Directory.Exists(path))
            .GroupBy(path => Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(path)) ?? path, StringComparer.OrdinalIgnoreCase)
            .Take(6)
            .ToArray();
        var opened = false;
        foreach (var group in groups)
        {
            var folder = SHParse(group.Key);
            if (folder is not { } folderPidl || folderPidl == IntPtr.Zero) continue;
            var children = group.Select(SHParse).Where(pidl => pidl is { } value && value != IntPtr.Zero).Select(pidl => pidl!.Value).ToArray();
            try
            {
                var relative = children.Select(NativeMethods.ILFindLastID).ToArray();
                if (NativeMethods.SHOpenFolderAndSelectItems(folderPidl, (uint)relative.Length, relative, 0) == 0) opened = true;
            }
            finally
            {
                NativeMethods.ILFree(folderPidl);
                foreach (var child in children) NativeMethods.ILFree(child);
            }
        }
        return opened;
    }
}
