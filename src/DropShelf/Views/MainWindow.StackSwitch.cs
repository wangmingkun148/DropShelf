using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using Image = System.Windows.Controls.Image;

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
        public required Image OldFace { get; init; }
        public required Image NewFace { get; init; }
        public MatrixTransform OldTransform { get; } = new();
        public MatrixTransform NewTransform { get; } = new();
        public RectangleGeometry OldCard { get; } = new();
        public RectangleGeometry NewCard { get; } = new();
        public RectangleGeometry Border { get; } = new();
    }

    private StackSlide? _stackSlide;

    /// <summary>
    /// 从露出的牌边抽出下一页。只有新旧两张可视区的临时位图，不缓存四张整页；
    /// 页面只在开始时排版一次，途中只改位图的矩阵、裁剪和三条简单的牌边。
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

        // 先把两张卡面准备好，再隐藏真实页面，不会在截图准备时露出空白。
        var capturing = Stopwatch.GetTimestamp();
        var oldFace = new Image { Source = VisualCapture.Of(from.Sheet), Stretch = Stretch.Fill, Width = fromArea.Width, Height = fromArea.Height };
        var border = to.Sheet.ReadLocalValue(Border.BorderBrushProperty);
        Image newFace;
        try
        {
            // 新卡展开时下沿会变，单独画外框，避免截图里的边框重叠。
            to.Sheet.BorderBrush = Brushes.Transparent;
            newFace = new Image { Source = VisualCapture.Of(to.Sheet), Stretch = Stretch.Fill, Width = toArea.Width, Height = toArea.Height };
        }
        finally
        {
            if (border == DependencyProperty.UnsetValue) to.Sheet.ClearValue(Border.BorderBrushProperty);
            else to.Sheet.SetValue(Border.BorderBrushProperty, border);
        }
        TraceMark("capture", capturing);

        var slide = new StackSlide
        {
            Page = page, FromArea = fromArea, ToArea = toArea,
            Bands = bands, Slots = slots, Selected = index, OldFace = oldFace, NewFace = newFace
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
        var animation = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(StackSwitchMs));
        animation.Completed += (_, _) =>
        {
            if (session == _flipSession) FinishStackSwitch(slide);
        };
        BeginAnimation(StackSwitchProgressProperty, animation);
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
