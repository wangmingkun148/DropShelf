using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DropShelf.Services;

public static class ShellThumbnailService
{
    /// <summary>最近用过的缩略图，同一文件放进多个置物架或从最近关闭中恢复时不用重新生成。只在图片处理线程上访问。</summary>
    private const int CacheCapacity = 32;
    private static readonly Dictionary<string, LinkedListNode<(string Key, ImageSource Image)>> CacheIndex = new(StringComparer.OrdinalIgnoreCase);
    private static readonly LinkedList<(string Key, ImageSource Image)> CacheOrder = new();

    public static async Task<ImageSource?> LoadAsync(string path, int size = 128, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try
        {
            return await ImagingThread.RunAsync(() => LoadCached(path, size), cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            return null;
        }
    }

    private static ImageSource? LoadCached(string path, int size)
    {
        // 文件被修改后键随之改变，旧的缩略图自然被挤出。
        var key = $"{size}|{File.GetLastWriteTimeUtc(path).Ticks}|{path}";
        if (CacheIndex.TryGetValue(key, out var node))
        {
            CacheOrder.Remove(node);
            CacheOrder.AddFirst(node);
            return node.Value.Image;
        }
        var image = LoadFromWindows(path, size);
        if (image is null) return null;
        CacheIndex[key] = CacheOrder.AddFirst((key, image));
        if (CacheOrder.Count > CacheCapacity)
        {
            CacheIndex.Remove(CacheOrder.Last!.Value.Key);
            CacheOrder.RemoveLast();
        }
        return image;
    }

    private static ImageSource? LoadFromWindows(string path, int size)
    {
        IShellItemImageFactory? imageFactory = null;
        IntPtr bitmapHandle = IntPtr.Zero;
        try
        {
            var interfaceId = typeof(IShellItemImageFactory).GUID;
            NativeMethods.SHCreateItemFromParsingName(path, IntPtr.Zero, ref interfaceId, out imageFactory);
            var result = imageFactory.GetImage(
                new NativeSize(size, size),
                // 不允许返回更大的图：系统按需要的尺寸缩好，缓存里不留用不到的像素。
                ShellItemImageFlags.ResizeToFit,
                out bitmapHandle);
            if (result < 0 || bitmapHandle == IntPtr.Zero) return null;

            var image = IgnoreEmptyAlpha(Imaging.CreateBitmapSourceFromHBitmap(
                bitmapHandle,
                IntPtr.Zero,
                Int32Rect.Empty,
                BitmapSizeOptions.FromEmptyOptions()));
            image.Freeze();
            return image;
        }
        finally
        {
            if (bitmapHandle != IntPtr.Zero) NativeMethods.DeleteObject(bitmapHandle);
            if (imageFactory is not null && Marshal.IsComObject(imageFactory)) Marshal.ReleaseComObject(imageFactory);
        }
    }

    /// <summary>
    /// 照片这类不带透明度的缩略图，系统返回的位图透明度通道是无效的，可能全是 0；按透明度显示会完全看不见，
    /// 置物架上只剩一张空白卡片。透明度全为 0 时当作不透明的图片。
    /// </summary>
    private static BitmapSource IgnoreEmptyAlpha(BitmapSource image)
    {
        if (image.Format.BitsPerPixel != 32) return image;
        var stride = image.PixelWidth * 4;
        var pixels = new byte[stride * image.PixelHeight];
        image.CopyPixels(pixels, stride, 0);
        for (var index = 3; index < pixels.Length; index += 4)
            if (pixels[index] != 0) return image;
        return BitmapSource.Create(image.PixelWidth, image.PixelHeight, image.DpiX, image.DpiY, PixelFormats.Bgr32, null, pixels, stride);
    }

    [ComImport]
    [Guid("BCC18B79-BA16-442F-80C4-8A59C30C463B")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemImageFactory
    {
        [PreserveSig]
        int GetImage(NativeSize size, ShellItemImageFlags flags, out IntPtr bitmapHandle);
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct NativeSize
    {
        public readonly int Width;
        public readonly int Height;

        public NativeSize(int width, int height)
        {
            Width = width;
            Height = height;
        }
    }

    [Flags]
    private enum ShellItemImageFlags
    {
        ResizeToFit = 0x0000,
        BiggerSizeOk = 0x0001
    }

    private static class NativeMethods
    {
        [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
        public static extern void SHCreateItemFromParsingName(
            [MarshalAs(UnmanagedType.LPWStr)] string path,
            IntPtr bindingContext,
            ref Guid interfaceId,
            [MarshalAs(UnmanagedType.Interface)] out IShellItemImageFactory imageFactory);

        [DllImport("gdi32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool DeleteObject(IntPtr objectHandle);
    }
}
