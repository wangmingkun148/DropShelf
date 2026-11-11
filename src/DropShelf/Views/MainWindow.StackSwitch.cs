using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using DropShelf.Interop;

namespace DropShelf.Views;

public partial class MainWindow
{
    private const double StackSwitchMs = 460, StackPull = 12;
    private static readonly DependencyProperty StackSwitchProgressProperty = DependencyProperty.Register(
        "StackSwitchProgress", typeof(double), typeof(MainWindow),
        new PropertyMetadata(0.0, (owner, args) => ((MainWindow)owner).PoseStackSwitch((double)args.NewValue)));

    private sealed class StackSlide
    {
        public required string Page { get; init; }
        public required Rect FromArea { get; init; }
        public required Rect ToArea { get; init; }
        public required Rect[] Bands { get; init; }
        public required int[] Slots { get; init; }
        public required int Selected { get; init; }
        public required FrameworkElement OldFace { get; init; }
        public required FrameworkElement NewFace { get; init; }
        /// <summary>新页面的外框在动画期间去掉，结束时还原成原来的值。</summary>
        public required Border NewSheet { get; init; }
        public required object NewSheetBorder { get; init; }
        public MatrixTransform OldTransform { get; } = new();
        public MatrixTransform NewTransform { get; } = new();
        public RectangleGeometry OldCard { get; } = new();
        public RectangleGeometry NewCard { get; } = new();
        public RectangleGeometry Border { get; } = new();
    }

    private StackSlide? _stackSlide;

    /// <summary>
    /// 从露出的牌边抽出下一页。只有新旧两张可视区的卡面，不缓存四张整页；
    /// 页面只在开始时排版一次，途中只改卡面的矩阵、裁剪和三条简单的牌边。
    /// </summary>
    private void SwitchStackPage(int index)
    {
        if (_morphing || _sidePanelFade is not null || _page == "Overview" ||
            _edges[index].Root.Tag is not string page || page == _page ||
            !_pages.TryGetValue(_page, out var from) || !_pages.TryGetValue(page, out var to)) return;

        StartTrace($"stack {page}");
        var preparing = Stopwatch.GetTimestamp();
        PreparePage(to, resetScroll: false);
        PageStage.UpdateLayout();
        var fromArea = PageArea(from);
        var toArea = PageArea(to);
        TraceMark("prepare", preparing);
        // 和翻牌共用上限：软件渲染或超大可视区直接换页，不分配超预算的整页位图。
        if (UsePlainFlip(fromArea) || UsePlainFlip(toArea))
        {
            ShowPage(page);
            SetPanelHeader(page, animate: true);
            EndTrace("plain");
            return;
        }

        var bands = _edges.Select(edge => BoundsOf(edge.Root)).ToArray();
        var others = OthersOf(page);
        var slots = _edges.Select((edge, k) => Array.FindIndex(others,
            tile => Equals(tile.Tag, k == index ? _page : edge.Root.Tag))).ToArray();

        // 先把两张卡面准备好，再隐藏真实页面，不会露出空白。
        var capturing = Stopwatch.GetTimestamp();
        var oldFace = LiveFace(from.Sheet, fromArea);
        // 新卡展开时下沿会变，单独画外框；卡面是实时画面，页面的边框要到动画结束再还原，否则和单独画的外框重叠。
        var border = to.Sheet.ReadLocalValue(Border.BorderBrushProperty);
        to.Sheet.BorderBrush = Brushes.Transparent;
        var newFace = LiveFace(to.Sheet, toArea);
        TraceMark("faces", capturing);

        var slide = new StackSlide
        {
            Page = page, FromArea = fromArea, ToArea = toArea,
            Bands = bands, Slots = slots, Selected = index, OldFace = oldFace, NewFace = newFace,
            NewSheet = to.Sheet, NewSheetBorder = border
        };
        _morphing = true;
        _stackSlide = slide;
        var session = ++_flipSession;
        _finishFlip = () => FinishStackSwitch(slide);
        ResetEdgeReveal();
        StackLayer.IsHitTestVisible = false;
        for (var k = 0; k < _edges.Length; k++)
        {
            _edges[k].Label.Visibility = Visibility.Collapsed;
            Panel.SetZIndex(_edges[k].Root, _edges.Length - slots[k]);
        }
        from.Host.Visibility = to.Host.Visibility = Visibility.Hidden;
        oldFace.RenderTransform = slide.OldTransform;
        newFace.RenderTransform = slide.NewTransform;
        StackBackLayer.Children.Add(oldFace);
        StackBackLayer.Clip = new CombinedGeometry(GeometryCombineMode.Exclude, slide.OldCard, slide.NewCard);
        StackFrontLayer.Children.Add(newFace);
        var outline = new Path { Data = slide.Border, IsHitTestVisible = false };
        outline.SetResourceReference(Shape.StrokeProperty, "SheetBorderBrush");
        StackFrontLayer.Children.Add(outline);
        StackFrontLayer.Clip = slide.NewCard;
        // 卡面是半透明的，后面的内容必须裁掉，不能让旧设置透过新设置。
        StackLayer.Clip = new GeometryGroup
        {
            FillRule = FillRule.EvenOdd,
            Children = { new RectangleGeometry(new Rect(PageStage.RenderSize)), slide.NewCard }
        };
        StackBackLayer.Visibility = StackFrontLayer.Visibility = Visibility.Visible;
        SetPanelHeader(page, animate: true);
        PoseStackSwitch(0);
        var animation = ShelfMotion.Gate(new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(StackSwitchMs)));
        animation.Completed += (_, _) =>
        {
            if (session == _flipSession) FinishStackSwitch(slide);
        };
        BeginAnimation(StackSwitchProgressProperty, animation);
    }

    /// <summary>
    /// 页面的实时画面，用来代替点击时用处理器截图（整页截两张要八九十毫秒）。由显卡按位图缓存绘制，
    /// 途中只改矩阵和裁剪，缓存不用重画；藏起来的是页面外层的宿主，页面本身照样能画进卡面。Viewbox 的换算见 VisualCapture。
    /// </summary>
    private Rectangle LiveFace(FrameworkElement element, Rect area)
    {
        var bounds = new Rect(0, 0, element.ActualWidth, element.ActualHeight);
        var viewbox = VisualTreeHelper.GetParent(element) is Visual parent ? element.TransformToAncestor(parent).TransformBounds(bounds) : bounds;
        return new Rectangle
        {
            Width = area.Width,
            Height = area.Height,
            IsHitTestVisible = false,
            Fill = new VisualBrush(element) { Viewbox = viewbox, ViewboxUnits = BrushMappingMode.Absolute, Stretch = Stretch.Fill },
            CacheMode = new BitmapCache(CacheScale.For(this)) { SnapsToDevicePixels = true },
        };
    }

    private void PoseStackSwitch(double progress)
    {
        if (_stackSlide is not { } slide) return;
        // 先向下抽出一点，再向上展开到前台；设置 UI 从展开的第一帧起就在牌面上。
        var pull = StackPull * Smooth(progress / 0.18);
        var travel = MotionEase.Ease(Math.Clamp((progress - 0.12) / 0.88, 0, 1));
        var start = slide.Bands[slide.Selected];
        start.Offset(0, pull);
        var front = Lerp(start, slide.ToArea, travel);
        var back = Lerp(slide.FromArea, BandOf(slide.ToArea, slide.Slots[slide.Selected]), travel);
        SetSheetPose(slide.NewTransform, slide.ToArea, front);
        SetSheetPose(slide.OldTransform, slide.FromArea, back);
        slide.NewCard.Rect = front;
        slide.NewCard.RadiusX = slide.NewCard.RadiusY = SheetRadius;
        slide.OldCard.Rect = back;
        slide.OldCard.RadiusX = slide.OldCard.RadiusY = SheetRadius;
        slide.Border.Rect = new Rect(front.X + 0.5, front.Y + 0.5, Math.Max(0, front.Width - 1), Math.Max(0, front.Height - 1));
        slide.Border.RadiusX = slide.Border.RadiusY = SheetRadius - 0.5;
        var faceFade = Smooth((travel - 0.55) / 0.4);
        slide.OldFace.Opacity = 1 - faceFade;
        for (var k = 0; k < _edges.Length; k++)
        {
            var rect = k == slide.Selected ? back : Lerp(slide.Bands[k], BandOf(slide.ToArea, slide.Slots[k]), travel);
            PlaceEdge(_edges[k], default, rect, 1);
            if (k == slide.Selected)
            {
                _edges[k].Frame.Opacity = faceFade;
                var top = SheetRadius * (1 - travel);
                var bottom = SheetRadius + (EdgeRadius - SheetRadius) * travel;
                _edges[k].Frame.CornerRadius = new CornerRadius(top, top, bottom, bottom);
            }
        }
    }

    private static void SetSheetPose(MatrixTransform transform, Rect area, Rect rect)
    {
        var scale = rect.Width / area.Width;
        transform.Matrix = new Matrix(scale, 0, 0, scale, rect.X, rect.Y);
    }

    private void FinishStackSwitch(StackSlide slide)
    {
        if (!ReferenceEquals(_stackSlide, slide)) return;
        _stackSlide = null;
        BeginAnimation(StackSwitchProgressProperty, null);
        if (slide.NewSheetBorder == DependencyProperty.UnsetValue) slide.NewSheet.ClearValue(Border.BorderBrushProperty);
        else slide.NewSheet.SetValue(Border.BorderBrushProperty, slide.NewSheetBorder);
        foreach (var layer in new[] { StackBackLayer, StackFrontLayer })
        {
            layer.Visibility = Visibility.Collapsed;
            layer.Children.Clear();
            layer.Clip = null;
        }
        StackLayer.Clip = null;
        ShowPage(slide.Page);
        EndMorph();
    }
}
