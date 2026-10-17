using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DropShelf.Interop;
using DropShelf.Models;
using ComDataObject = System.Runtime.InteropServices.ComTypes.IDataObject;
using WpfDragDropEffects = System.Windows.DragDropEffects;

namespace DropShelf.Services;

public enum DragOutResult
{
    Cancelled,
    Copied,
    Moved,
    Linked
}

/// <summary>
/// 用 Windows 资源管理器同款的数据对象把项目拖出：目标程序得到完整的文件信息，
/// 并显示带缩略图和数量的拖动图像。
/// </summary>
public static class ShelfDragService
{
    private const int EffectNone = 0;
    private const int EffectCopy = 1;
    private const int EffectMove = 2;
    private const int EffectLink = 4;
    private const int InvalidArgument = unchecked((int)0x80070057);

    public static DragOutResult DoDragDrop(Window owner, IReadOnlyList<ShelfItem> items, string shelfId, int button = NativeMethods.MK_LBUTTON)
    {
        var files = items.Where(item => item.IsFileSystem && item.RefreshAvailability()).ToList();
        var others = items.Where(item => !item.IsFileSystem).ToList();
        ComDataObject? data;
        int allowedEffects;
        IReadOnlyList<ShelfItem> dragged;

        if (files.Count > 0)
        {
            data = ShellInterop.CreateFileDataObject(files.Select(item => item.Value).ToArray());
            allowedEffects = files.Any(item => item.IsTemporaryAttachment)
                ? EffectCopy | EffectMove
                : EffectCopy | EffectMove | EffectLink;
            dragged = files;
        }
        else if (others.Count > 0)
        {
            data = ShellInterop.CreateEmptyDataObject();
            if (data is not null)
            {
                ShellInterop.TrySetUnicodeString(data, System.Windows.DataFormats.UnicodeText,
                    string.Join(Environment.NewLine, others.Select(item => item.Value)));
                if (others.Count == 1 && others[0].Kind == ShelfItemKind.Link)
                    ShellInterop.TrySetUnicodeString(data, "UniformResourceLocatorW", others[0].Value);
            }
            allowedEffects = EffectCopy;
            dragged = others;
        }
        else
        {
            return DragOutResult.Cancelled;
        }

        if (data is null) return DragOutResult.Cancelled;

        var helper = ShellInterop.CreateDragDropHelper<IDragSourceHelper2>();
        try
        {
            ShellInterop.TrySetUnicodeString(data, DropDataReader.ShelfIdFormat, shelfId);
            SetDragImage(helper, data, dragged, owner);

            var handle = new WindowInteropHelper(owner).Handle;
            System.Windows.Input.Mouse.Capture(null);
            var result = NativeMethods.SHDoDragDrop(handle, data, null, allowedEffects, out var effect);
            if (result == InvalidArgument)
                result = NativeMethods.SHDoDragDrop(handle, data, new DefaultDropSource(button), allowedEffects, out effect);

            var performed = ShellInterop.TryGetInt32(data, "Performed DropEffect");
            var logical = ShellInterop.TryGetInt32(data, "Logical Performed DropEffect");
            var sourceMissing = files.Any(item => !item.RefreshAvailability());
            return Resolve(result, effect, performed, logical, sourceMissing);
        }
        catch (Exception exception)
        {
            LogService.Error("拖出项目失败", exception);
            return DragOutResult.Cancelled;
        }
        finally
        {
            if (helper is not null) Marshal.ReleaseComObject(helper);
            Marshal.ReleaseComObject(data);
        }
    }

    /// <summary>
    /// 根据拖放结果判断项目去向。资源管理器在同盘“优化移动”时会返回“无操作”，
    /// 只能从逻辑结果或源文件是否消失来判断已经移动。
    /// </summary>
    public static DragOutResult Resolve(int result, int effect, int? performed, int? logical, bool sourceMissing)
    {
        if (result == NativeMethods.DRAGDROP_S_CANCEL && !sourceMissing) return DragOutResult.Cancelled;
        if (((logical ?? EffectNone) & EffectMove) != 0 || (effect & EffectMove) != 0) return DragOutResult.Moved;
        if (((performed ?? EffectNone) & EffectMove) != 0) return DragOutResult.Moved;
        if ((effect & EffectCopy) != 0) return DragOutResult.Copied;
        if ((effect & EffectLink) != 0) return DragOutResult.Linked;
        if (((logical ?? EffectNone) & EffectCopy) != 0) return DragOutResult.Copied;
        return sourceMissing ? DragOutResult.Moved : DragOutResult.Cancelled;
    }

    private static void SetDragImage(IDragSourceHelper2? helper, ComDataObject data, IReadOnlyList<ShelfItem> items, Window owner)
    {
        if (helper is null) return;
        try
        {
            helper.SetFlags(ShellInterop.DragSourceAllowDropDescriptionText);
            var scale = VisualTreeHelper.GetDpi(owner).DpiScaleX;
            var image = DragImageRenderer.Render(items, scale);
            if (image.Bitmap == IntPtr.Zero) return;
            if (helper.InitializeFromBitmap(ref image, data) != 0)
                NativeMethods.DeleteObject(image.Bitmap);
        }
        catch (Exception exception)
        {
            LogService.Error("生成拖动图像失败", exception);
        }
    }
}

/// <summary>绘制拖动时跟随光标的缩略图堆叠和数量角标。</summary>
internal static class DragImageRenderer
{
    private const double CanvasSize = 120;
    private const double CardSize = 78;

    public static SHDRAGIMAGE Render(IReadOnlyList<ShelfItem> items, double scale)
    {
        var pixels = (int)Math.Round(CanvasSize * scale);
        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            context.PushOpacity(0.94);
            var cards = items.TakeLast(3).ToArray();
            double[] angles = cards.Length switch { 1 => [0], 2 => [-7, 0], _ => [-9, 6, 0] };
            for (var index = 0; index < cards.Length; index++)
                DrawCard(context, cards[index], angles[index]);

            if (items.Count > 1) DrawBadge(context, items.Count, scale);
            context.Pop();
        }

        var bitmap = new RenderTargetBitmap(pixels, pixels, 96 * scale, 96 * scale, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        var stride = pixels * 4;
        var buffer = new byte[stride * pixels];
        bitmap.CopyPixels(buffer, stride, 0);

        var header = new BITMAPINFOHEADER
        {
            Size = (uint)Marshal.SizeOf<BITMAPINFOHEADER>(),
            Width = pixels,
            Height = -pixels,
            Planes = 1,
            BitCount = 32,
            Compression = 0
        };
        var handle = NativeMethods.CreateDIBSection(IntPtr.Zero, ref header, 0, out var bits, IntPtr.Zero, 0);
        if (handle == IntPtr.Zero || bits == IntPtr.Zero) return default;
        Marshal.Copy(buffer, 0, bits, buffer.Length);

        return new SHDRAGIMAGE
        {
            Size = new SIZE { Width = pixels, Height = pixels },
            Offset = new POINT(pixels / 2, (int)(pixels * 0.62)),
            Bitmap = handle,
            ColorKey = 0xFFFFFFFF
        };
    }

    private static void DrawCard(DrawingContext context, ShelfItem item, double angle)
    {
        var center = CanvasSize / 2;
        var rect = new Rect(center - CardSize / 2, center - CardSize / 2 + 6, CardSize, CardSize);
        context.PushTransform(new RotateTransform(angle, center, center + 6));
        var dark = ThemeService.IsDark;
        var fill = new SolidColorBrush(dark ? Color.FromRgb(0x3B, 0x3B, 0x3B) : Colors.White);
        var stroke = new Pen(new SolidColorBrush(dark ? Color.FromArgb(0x40, 255, 255, 255) : Color.FromArgb(0x26, 0, 0, 0)), 1);
        context.DrawRoundedRectangle(new SolidColorBrush(Color.FromArgb(0x22, 0, 0, 0)), null,
            new Rect(rect.X, rect.Y + 2, rect.Width, rect.Height), 9, 9);
        context.DrawRoundedRectangle(fill, stroke, rect, 9, 9);

        var inner = Rect.Inflate(rect, -8, -8);
        if (item.Thumbnail is BitmapSource thumbnail && thumbnail.PixelWidth > 0)
        {
            var ratio = Math.Min(inner.Width / thumbnail.Width, inner.Height / thumbnail.Height);
            var width = thumbnail.Width * ratio;
            var height = thumbnail.Height * ratio;
            context.DrawImage(thumbnail, new Rect(inner.X + (inner.Width - width) / 2, inner.Y + (inner.Height - height) / 2, width, height));
        }
        else
        {
            var glyph = new FormattedText(item.Glyph, CultureInfo.InvariantCulture, System.Windows.FlowDirection.LeftToRight,
                new Typeface(new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal),
                34, new SolidColorBrush(dark ? Color.FromRgb(0xDD, 0xDD, 0xDD) : Color.FromRgb(0x5A, 0x5A, 0x5A)), 1);
            context.DrawText(glyph, new System.Windows.Point(inner.X + (inner.Width - glyph.Width) / 2, inner.Y + (inner.Height - glyph.Height) / 2));
        }
        context.Pop();
    }

    private static void DrawBadge(DrawingContext context, int count, double scale)
    {
        var text = new FormattedText(count > 99 ? "99+" : count.ToString(CultureInfo.InvariantCulture), CultureInfo.InvariantCulture,
            System.Windows.FlowDirection.LeftToRight, new Typeface(new FontFamily("Segoe UI"), FontStyles.Normal, FontWeights.SemiBold, FontStretches.Normal),
            13, Brushes.White, scale);
        var width = Math.Max(24, text.Width + 12);
        var rect = new Rect(CanvasSize / 2 + CardSize / 2 - width / 2 - 4, 8, width, 24);
        context.DrawRoundedRectangle(new SolidColorBrush(ThemeService.Accent.Dark1), new Pen(Brushes.White, 2), rect, 12, 12);
        context.DrawText(text, new System.Windows.Point(rect.X + (rect.Width - text.Width) / 2, rect.Y + (rect.Height - text.Height) / 2));
    }
}

public enum DropImageType
{
    Invalid = -1,
    None = 0,
    Copy = 1,
    Move = 2,
    Link = 4,
    Label = 6,
    Warning = 7
}

/// <summary>拖入时让 Windows 继续显示来源程序的拖动图像，并显示“添加到置物架”等说明。</summary>
internal sealed class DropTargetHelper
{
    private IDropTargetHelper? _helper = ShellInterop.CreateDragDropHelper<IDropTargetHelper>();
    private bool _entered;

    public void Enter(Window window, System.Windows.IDataObject data, WpfDragDropEffects effects)
    {
        if (_helper is null || data is not ComDataObject com) return;
        try
        {
            var point = WindowPlacement.GetCursor();
            _helper.DragEnter(new WindowInteropHelper(window).Handle, com, ref point, (int)effects);
            _entered = true;
        }
        catch
        {
            _helper = null;
        }
    }

    public void Over(WpfDragDropEffects effects)
    {
        if (_helper is null || !_entered) return;
        try
        {
            var point = WindowPlacement.GetCursor();
            _helper.DragOver(ref point, (int)effects);
        }
        catch
        {
        }
    }

    public void Leave()
    {
        if (_helper is null || !_entered) return;
        _entered = false;
        try { _helper.DragLeave(); }
        catch { }
    }

    public void Drop(System.Windows.IDataObject data, WpfDragDropEffects effects)
    {
        if (_helper is null || !_entered || data is not ComDataObject com) return;
        _entered = false;
        try
        {
            var point = WindowPlacement.GetCursor();
            _helper.Drop(com, ref point, (int)effects);
        }
        catch
        {
        }
    }

    public static void SetDescription(System.Windows.IDataObject data, DropImageType type, string message)
    {
        if (data is not ComDataObject com) return;
        try { ShellInterop.TrySetDropDescription(com, (int)type, message); }
        catch { }
    }
}
