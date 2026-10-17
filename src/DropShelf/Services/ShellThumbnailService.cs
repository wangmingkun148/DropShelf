using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DropShelf.Services;

public static class ShellThumbnailService
{
    private static readonly SemaphoreSlim LoadingSlots = new(3, 3);

    public static async Task<ImageSource?> LoadAsync(string path, int size = 128)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;

        await LoadingSlots.WaitAsync().ConfigureAwait(false);
        try
        {
            return await Task.Run(() => LoadFromWindows(path, size)).ConfigureAwait(false);
        }
        catch
        {
            return null;
        }
        finally
        {
            LoadingSlots.Release();
        }
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
                ShellItemImageFlags.ResizeToFit | ShellItemImageFlags.BiggerSizeOk,
                out bitmapHandle);
            if (result < 0 || bitmapHandle == IntPtr.Zero) return null;

            var image = Imaging.CreateBitmapSourceFromHBitmap(
                bitmapHandle,
                IntPtr.Zero,
                Int32Rect.Empty,
                BitmapSizeOptions.FromEmptyOptions());
            image.Freeze();
            return image;
        }
        finally
        {
            if (bitmapHandle != IntPtr.Zero) NativeMethods.DeleteObject(bitmapHandle);
            if (imageFactory is not null && Marshal.IsComObject(imageFactory)) Marshal.ReleaseComObject(imageFactory);
        }
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
