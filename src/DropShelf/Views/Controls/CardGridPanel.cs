using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;

namespace DropShelf.Views.Controls;

/// <summary>
/// 置物架平铺时的牌桌：固定大小的格子从左到右、从上到下排开，列数由宽度决定，多出的宽度平均分给列间距。
/// 只为看得见的几行（上下各多留一行）生成牌，项目再多也只有这么些界面元素，缩略图也随牌的生成和回收加载、释放。
/// 上下各留一点空白，第一行的牌悬停抬起时不会被裁掉。滚轮一次滚一行。
/// </summary>
public sealed class CardGridPanel : VirtualizingPanel, IScrollInfo
{
    /// <summary>牌面的大小。平铺时在格子顶部居中，堆叠里的牌和拖入时的虚线牌也是这么大，界面和动画都从这里取。</summary>
    public const double CardWidth = 64;
    public const double CardHeight = 86;
    public static GridLength CardRowHeight { get; } = new(CardHeight);
    public const double SlotWidth = 70;
    public const double SlotHeight = 120;
    public const double RowGap = 10;
    public const double Inset = 8;
    private const double RowPitch = SlotHeight + RowGap;

    private Size _extent;
    private Size _viewport;
    private double _offset;
    private int _columns = 1;

    public static int ColumnsFor(double width) => Math.Max(1, (int)(width / SlotWidth));

    public static int RowsFor(int count, double width) => (count + ColumnsFor(width) - 1) / ColumnsFor(width);

    /// <summary>放下 rows 行牌需要的高度，含上下的空白。</summary>
    public static double HeightFor(int rows) => rows <= 0 ? 0 : 2 * Inset + rows * RowPitch - RowGap;

    protected override Size MeasureOverride(Size availableSize)
    {
        // 先取 InternalChildren，ItemContainerGenerator 才会就绪。
        var children = InternalChildren;
        var generator = ItemContainerGenerator;
        var count = ItemsControl.GetItemsOwner(this)?.Items.Count ?? 0;
        var width = double.IsInfinity(availableSize.Width) ? SlotWidth : availableSize.Width;
        _columns = ColumnsFor(width);
        var rows = (count + _columns - 1) / _columns;
        var extent = new Size(width, HeightFor(rows));
        UpdateScrollInfo(extent, new Size(width, double.IsInfinity(availableSize.Height) ? extent.Height : availableSize.Height));

        var firstRow = Math.Max(0, (int)Math.Floor((_offset - Inset) / RowPitch) - 1);
        var lastRow = Math.Min(rows - 1, (int)Math.Floor((_offset + _viewport.Height - Inset) / RowPitch) + 1);
        var first = firstRow * _columns;
        var last = Math.Min(count - 1, (lastRow + 1) * _columns - 1);
        if (generator is not null && first <= last)
        {
            var start = generator.GeneratorPositionFromIndex(first);
            var childIndex = start.Offset == 0 ? start.Index : start.Index + 1;
            using (generator.StartAt(start, GeneratorDirection.Forward, true))
            {
                for (var index = first; index <= last; index++, childIndex++)
                {
                    if (generator.GenerateNext(out var fresh) is not UIElement child) break;
                    if (fresh)
                    {
                        if (childIndex >= children.Count) AddInternalChild(child);
                        else InsertInternalChild(childIndex, child);
                        generator.PrepareItemContainer(child);
                    }
                    child.Measure(new Size(SlotWidth, SlotHeight));
                }
            }
        }
        CleanUp(first, last);
        return _viewport;
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var generator = ItemContainerGenerator;
        var step = _columns > 1 ? (finalSize.Width - SlotWidth) / (_columns - 1) : 0;
        for (var i = 0; i < InternalChildren.Count; i++)
        {
            var index = generator.IndexFromGeneratorPosition(new GeneratorPosition(i, 0));
            var (row, column) = Math.DivRem(index, _columns);
            InternalChildren[i].Arrange(new Rect(column * step, Inset + row * RowPitch - _offset, SlotWidth, SlotHeight));
        }
        return finalSize;
    }

    /// <summary>回收不在 first 到 last 之间的牌。</summary>
    private void CleanUp(int first, int last)
    {
        var children = InternalChildren;
        var generator = ItemContainerGenerator;
        if (generator is null) return;
        for (var i = children.Count - 1; i >= 0; i--)
        {
            var position = new GeneratorPosition(i, 0);
            var index = generator.IndexFromGeneratorPosition(position);
            if (index >= first && index <= last) continue;
            generator.Remove(position, 1);
            RemoveInternalChildRange(i, 1);
        }
    }

    protected override void OnItemsChanged(object sender, ItemsChangedEventArgs args)
    {
        switch (args.Action)
        {
            case NotifyCollectionChangedAction.Remove:
            case NotifyCollectionChangedAction.Replace:
                RemoveInternalChildRange(args.Position.Index, args.ItemUICount);
                break;
            case NotifyCollectionChangedAction.Move:
                RemoveInternalChildRange(args.OldPosition.Index, args.ItemUICount);
                break;
            case NotifyCollectionChangedAction.Reset:
                if (InternalChildren.Count > 0) RemoveInternalChildRange(0, InternalChildren.Count);
                break;
        }
        base.OnItemsChanged(sender, args);
        InvalidateMeasure();
    }

    /// <summary>列表要显示某一项（键盘选择、ScrollIntoView）而它还没生成时，滚到它所在的那一行。</summary>
    protected override void BringIndexIntoView(int index) => ScrollRowIntoView(index);

    private void ScrollRowIntoView(int index)
    {
        if (index < 0) return;
        var top = index / _columns * RowPitch;
        var bottom = top + SlotHeight + 2 * Inset;
        if (top < _offset) SetVerticalOffset(top);
        else if (bottom > _offset + _viewport.Height) SetVerticalOffset(bottom - _viewport.Height);
    }

    private void UpdateScrollInfo(Size extent, Size viewport)
    {
        var offset = Math.Clamp(_offset, 0, Math.Max(0, extent.Height - viewport.Height));
        if (extent == _extent && viewport == _viewport && offset == _offset) return;
        _extent = extent;
        _viewport = viewport;
        _offset = offset;
        ScrollOwner?.InvalidateScrollInfo();
    }

    // ───────────── IScrollInfo：只能纵向滚动 ─────────────

    public bool CanVerticallyScroll { get; set; }
    public bool CanHorizontallyScroll { get; set; }
    public double ExtentWidth => _extent.Width;
    public double ExtentHeight => _extent.Height;
    public double ViewportWidth => _viewport.Width;
    public double ViewportHeight => _viewport.Height;
    public double HorizontalOffset => 0;
    public double VerticalOffset => _offset;
    public ScrollViewer? ScrollOwner { get; set; }

    public void LineUp() => SetVerticalOffset(_offset - RowPitch / 2);
    public void LineDown() => SetVerticalOffset(_offset + RowPitch / 2);
    public void PageUp() => SetVerticalOffset(_offset - _viewport.Height);
    public void PageDown() => SetVerticalOffset(_offset + _viewport.Height);
    public void MouseWheelUp() => SetVerticalOffset(_offset - RowPitch);
    public void MouseWheelDown() => SetVerticalOffset(_offset + RowPitch);
    public void LineLeft() { }
    public void LineRight() { }
    public void PageLeft() { }
    public void PageRight() { }
    public void MouseWheelLeft() { }
    public void MouseWheelRight() { }
    public void SetHorizontalOffset(double offset) { }

    public void SetVerticalOffset(double offset)
    {
        offset = Math.Clamp(offset, 0, Math.Max(0, _extent.Height - _viewport.Height));
        if (offset == _offset) return;
        _offset = offset;
        ScrollOwner?.InvalidateScrollInfo();
        InvalidateMeasure();
    }

    public Rect MakeVisible(Visual visual, Rect rectangle)
    {
        var generator = ItemContainerGenerator;
        for (var i = 0; i < InternalChildren.Count; i++)
        {
            var child = InternalChildren[i];
            if (!ReferenceEquals(child, visual) && !child.IsAncestorOf(visual)) continue;
            ScrollRowIntoView(generator.IndexFromGeneratorPosition(new GeneratorPosition(i, 0)));
            break;
        }
        return rectangle;
    }
}
