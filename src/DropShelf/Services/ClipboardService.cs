using System.Collections.Specialized;
using System.IO;
using System.Runtime.InteropServices;
using WpfClipboard = System.Windows.Clipboard;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Services;

/// <summary>剪贴板常被其他程序短暂占用，所有写入都重试几次再放弃。</summary>
public static class ClipboardService
{
    public static bool TrySetText(string text) => Retry(() => WpfClipboard.SetText(text));

    /// <summary>以“复制文件”的方式写入剪贴板，可直接在资源管理器或聊天软件中粘贴。</summary>
    public static bool TrySetFiles(IReadOnlyList<string> paths, string? fallbackText = null)
    {
        var data = new WpfDataObject();
        if (paths.Count > 0)
        {
            var collection = new StringCollection();
            collection.AddRange(paths.ToArray());
            data.SetFileDropList(collection);
            data.SetData("Preferred DropEffect", new MemoryStream(BitConverter.GetBytes(1)));
        }
        var text = fallbackText ?? string.Join(Environment.NewLine, paths);
        if (!string.IsNullOrEmpty(text)) data.SetText(text);
        return Retry(() => WpfClipboard.SetDataObject(data, true));
    }

    public static System.Windows.IDataObject? TryGetDataObject()
    {
        for (var attempt = 0; attempt < 4; attempt++)
        {
            try
            {
                return WpfClipboard.GetDataObject();
            }
            catch (COMException)
            {
                Thread.Sleep(40);
            }
            catch (Exception exception)
            {
                LogService.Error("读取剪贴板失败", exception);
                return null;
            }
        }
        return null;
    }

    private static bool Retry(Action action)
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            try
            {
                action();
                return true;
            }
            catch (COMException) when (attempt < 4)
            {
                Thread.Sleep(50);
            }
            catch (Exception exception)
            {
                LogService.Error("写入剪贴板失败", exception);
                return false;
            }
        }
        return false;
    }
}
