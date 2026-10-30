using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace DropShelf.Views;

internal static class VisualCapture
{
    /// <summary>按设备像素截下元素当前的样子。</summary>
    public static BitmapSource Of(FrameworkElement element)
    {
        var dpi = VisualTreeHelper.GetDpi(element);
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(element.ActualWidth * dpi.DpiScaleX), (int)Math.Ceiling(element.ActualHeight * dpi.DpiScaleY),
            dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            // 只截元素自身的范围：被裁掉的溢出部分不能算进画面，否则截图会被压扁。
            // VisualBrush 的绝对 Viewbox 在元素外层的坐标里，带着元素的偏移和变换，要换算过去，否则居中的大页面截出来整体错开。
            var bounds = new Rect(0, 0, element.ActualWidth, element.ActualHeight);
            var viewbox = VisualTreeHelper.GetParent(element) is Visual parent ? element.TransformToAncestor(parent).TransformBounds(bounds) : bounds;
            context.DrawRectangle(new VisualBrush(element) { Viewbox = viewbox, ViewboxUnits = BrushMappingMode.Absolute }, null, bounds);
        }
        bitmap.Render(visual);
        bitmap.Freeze();
        return bitmap;
    }
}
