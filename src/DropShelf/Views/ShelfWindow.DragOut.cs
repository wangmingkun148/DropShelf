using System.Diagnostics;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

public partial class ShelfWindow
{
    // ───────────── 拖出 ─────────────

    /// <summary>
    /// 拖出：文件很多时先在后台准备（检查是否还在、解析路径），准备期间松开了鼠标就不再拖。
    /// 拖动期间被拖的牌或行变淡，取消或留在置物架上时恢复，移走的由移除动画接着淡出。
    /// </summary>
    private async void BeginDragOut(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0 || ViewModel.IsBusy || IsDraggingOut) return;
        IsDraggingOut = true;
        DragOutResult result;
        IReadOnlyList<ShelfItem> dragged;
        DragOutPayload? payload = null;
        var session = new ShelfDragSession(this, items);
        (RECT Rect, bool Below)? seed = null;
        try
        {
            // 准备期间列表不再跟着鼠标改变选中的项目。
            Mouse.Capture(null);
            payload = await ShelfDragService.PrepareAsync(items);
            if (IsClosing || !NativeMethods.IsButtonPhysicallyDown()) return;
            // 有文件要拖出时，一起选中的文本和链接各写成一个文件跟着走，不再被落下。
            if (payload.Files.Count > 0 && payload.Others.Count > 0) payload.OtherFiles = ViewModel.WriteForDrag(payload.Others);
            DimDragged(items);
            // 从列表里只拖出一部分时，下边伸出“新置物架”，放上去就拆成一个新的置物架。
            if (ViewModel.IsExpanded && items.Count < ViewModel.Count)
            {
                var (width, height) = CollapsedSize();
                SplitPadWindow.ShowFor(this, width, height);
            }
            ShelfDragSession.Current = session;
            result = ShelfDragService.DoDragDrop(this, payload, ViewModel.Id, out dragged, _dragOutMode);
        }
        finally
        {
            ShelfDragSession.Current = null;
            seed = SplitPadWindow.Dismiss();
            payload?.Dispose();
            IsDraggingOut = false;
        }

        ViewModel.HasInteracted = true;
        if (session.Target is { IsClosing: false } target && !ReferenceEquals(target, this))
        {
            // 放到了另一个置物架上：项目原样交过去，这边按移走处理。
            target.Receive(items, ViewModel, session.DropPoint);
            LetGo(items);
        }
        else if (session.Split && seed is { } place)
        {
            SplitOff(items, place, WindowPlacement.GetCursor());
        }
        else if (result != DragOutResult.Cancelled)
        {
            // 聊天软件和浏览器常在放下之后才读取文件，临时附件要多保留一会儿。
            if (dragged.Any(item => item.IsTemporaryAttachment || !item.IsFileSystem)) ViewModel.KeepTemporaryFilesForAWhile();
            // 要移走的项目交给 RemoveItems：先播放移除动画，全部移走且设置为自动关闭时直接关闭置物架。
            ViewModel.ApplyDragOut(dragged, result, removeCopied: _host.Settings.CloseWhenEmpty);
        }
        else if (payload is { Refused: true } && dragged.Count > 0 && dragged.All(item => !item.IsFileSystem) && !IsCursorOnShelf())
            OfferConvert(dragged);
        RestoreDimmed();
    }

    /// <summary>拖出期间被拖的牌或行的透明度。</summary>
    private const double DragOutDim = 0.4;
    private static readonly TimeSpan DimDuration = TimeSpan.FromMilliseconds(150);
    private readonly List<UIElement> _dimmed = [];

    private void DimDragged(IReadOnlyList<ShelfItem> items)
    {
        if (!ShelfMotion.Enabled) return;
        var elements = ViewModel.IsExpanded
            ? items.Select(item => ItemsList.ItemContainerGenerator.ContainerFromItem(item)).OfType<UIElement>()
            : StackCards.Where(card => card.Card.DataContext is ShelfItem item && items.Contains(item)).Select(card => (UIElement)card.Card);
        foreach (var element in elements)
        {
            element.BeginAnimation(OpacityProperty, new DoubleAnimation(DragOutDim, DimDuration) { EasingFunction = MotionEase });
            _dimmed.Add(element);
        }
    }

    /// <summary>拖出结束，还留在置物架上的恢复原来的透明度；要移除的已经由移除动画接手，见 TakeDimmed。</summary>
    private void RestoreDimmed()
    {
        foreach (var element in _dimmed)
            element.BeginAnimation(OpacityProperty, new DoubleAnimation(element.Opacity, 1, DimDuration)
            {
                EasingFunction = MotionEase, FillBehavior = FillBehavior.Stop
            });
        _dimmed.Clear();
    }

    /// <summary>移除动画从变淡后的样子接着淡出：停下变淡的动画，透明度留在当前的值。</summary>
    private void TakeDimmed(UIElement element)
    {
        if (!_dimmed.Remove(element)) return;
        var now = element.Opacity;
        element.BeginAnimation(OpacityProperty, null);
        element.Opacity = now;
    }

    private bool PassedDragThreshold(System.Windows.Input.MouseEventArgs e)
    {
        var current = e.GetPosition(this);
        return Math.Abs(current.X - _pressPoint.X) >= SystemParameters.MinimumHorizontalDragDistance ||
               Math.Abs(current.Y - _pressPoint.Y) >= SystemParameters.MinimumVerticalDragDistance;
    }

    private void OnStackMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2 && ViewModel.Count == 1 && ViewModel.Stack0 is { } single)
        {
            _stackPressed = false;
            ViewModel.Open(single);
            e.Handled = true;
            return;
        }
        _stackPressed = true;
        _pressPoint = e.GetPosition(this);
    }

    private void OnStackMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (!_stackPressed || e.LeftButton != MouseButtonState.Pressed || !PassedDragThreshold(e)) return;
        _stackPressed = false;
        BeginDragOut(ViewModel.Items.ToArray());
    }

    private void OnStackMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_stackPressed) return;
        _stackPressed = false;
        ViewModel.HasInteracted = true;
    }

    private void OnStackRightClick(object sender, MouseButtonEventArgs e)
    {
        e.Handled = true;
        OpenMenu(BuildShelfMenu(), StackView);
    }

    private void Expand()
    {
        ViewModel.IsExpanded = true;
        ItemsList.SelectedItems.Clear();
        Dispatcher.BeginInvoke(() => ItemsList.Focus(), DispatcherPriority.Input);
    }

    private void OnListMouseDown(object sender, MouseButtonEventArgs e)
    {
        _listPressed = false;
        _deferSelection = false;
        var source = e.OriginalSource as DependencyObject;
        if (FindAncestor<ButtonBase>(source) is not null) return;
        if (FindAncestor<ListBoxItem>(source) is not { DataContext: ShelfItem item } container) return;
        _pressedItem = item;
        _listPressed = true;
        _pressPoint = e.GetPosition(this);
        if (container.IsSelected && Keyboard.Modifiers == ModifierKeys.None && ItemsList.SelectedItems.Count > 1)
        {
            // 保留多选，便于把选中的多个项目一起拖出；未拖动时在松开后再改为单选。
            _deferSelection = true;
            e.Handled = true;
        }
    }

    private void OnListMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (!_listPressed || e.LeftButton != MouseButtonState.Pressed || _pressedItem is null || !PassedDragThreshold(e)) return;
        _listPressed = false;
        _deferSelection = false;
        if (!ItemsList.SelectedItems.Contains(_pressedItem))
        {
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItems.Add(_pressedItem);
        }
        BeginDragOut(GetSelectedItems());
    }

    private void OnListMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (_listPressed && _deferSelection && _pressedItem is not null)
        {
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItems.Add(_pressedItem);
        }
        _listPressed = false;
        _deferSelection = false;
    }

    private void OnListDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (FindAncestor<ButtonBase>(e.OriginalSource as DependencyObject) is not null) return;
        if (FindAncestor<ListBoxItem>(e.OriginalSource as DependencyObject) is { DataContext: ShelfItem item })
            ViewModel.Open(item);
    }

    private void OnListRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (FindAncestor<ListBoxItem>(e.OriginalSource as DependencyObject) is not { DataContext: ShelfItem item } container) return;
        e.Handled = true;
        if (!container.IsSelected)
        {
            ItemsList.SelectedItems.Clear();
            ItemsList.SelectedItems.Add(item);
        }
        OpenMenu(BuildItemMenu(GetSelectedItems()), ItemsList);
    }

    private void OnRemoveItemClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not ShelfItem item) return;
        RemoveByUser([item]);
    }

    /// <summary>移除项目并播放移除动画；全部移走且设置为自动关闭时不播放，直接关闭置物架。用于拖出之后。</summary>
    private void RemoveItems(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0) return;
        ViewModel.HasInteracted = true;
        var closes = _host.Settings.CloseWhenEmpty && ViewModel.Items.All(items.Contains);
        RemoveWithMotion(items, () =>
        {
            ViewModel.Remove(items);
            if (ViewModel.IsEmpty && _host.Settings.CloseWhenEmpty) CloseAnimated();
        }, animate: !closes);
    }

    /// <summary>
    /// 手动移除（行尾的按钮、Delete、菜单里的移除和清空）：移除后提示“已移除 N 项”，几秒内可以撤销。
    /// 移空了且设置为自动关闭时，等提示消失再关闭，这期间同样可以撤销。
    /// </summary>
    private void RemoveByUser(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0) return;
        ViewModel.HasInteracted = true;
        var expanded = ViewModel.IsExpanded;
        RemoveWithMotion(items, () =>
        {
            if (ViewModel.Remove(items) is { } removal && !IsClosing) OfferUndo(removal, expanded);
        });
    }

    // ───────────── 合并和拆分 ─────────────

    /// <summary>
    /// 接收另一个置物架交来的项目，从 point（相对本窗口）飞进堆叠或列表，和拖入文件时的落下动画一样。
    /// 这边已经有的不重复放；一个也没放进来时提示。
    /// </summary>
    internal int Receive(IReadOnlyList<ShelfItem> items, ShelfViewModel from, WpfPoint point)
    {
        if (IsClosing) return 0;
        FinishMotion();
        var added = ViewModel.Adopt(items, from);
        if (added > 0) _landing = (point, added);
        else ViewModel.Notify(Strings.AlreadyOnShelf, false);
        return added;
    }

    /// <summary>
    /// 把一部分项目拆到新的置物架：新置物架在 place（“新置物架”小窗口所在的位置）展开，在下边时顶边对齐、在上边时底边对齐，
    /// 不盖住这个置物架；项目从 from（屏幕像素，没有时从新置物架靠近这边的一侧）飞进去，这边的行随后滑走、补位。
    /// </summary>
    private void SplitOff(IReadOnlyList<ShelfItem> items, (RECT Rect, bool Below) place, POINT? from)
    {
        if (items.Count == 0 || IsClosing) return;
        var height = (int)Math.Round(CollapsedSize().Height * DpiScale());
        var (rect, below) = place;
        var center = new POINT(rect.Left + rect.Width / 2, below ? rect.Top + height / 2 : rect.Bottom - height / 2);
        var shelf = _host.OpenNew(center);
        if (ReferenceEquals(shelf, this)) return;
        var origin = from ?? new POINT(center.X, below ? rect.Top : rect.Bottom);
        shelf.Receive(items, ViewModel, shelf.ClampInside(shelf.PointFromScreen(new WpfPoint(origin.X, origin.Y))));
        LetGo(items);
    }

    /// <summary>
    /// 项目已经交给别的置物架，这边放手：全部交出去且设置为自动关闭时，带着原来的样子淡出关闭（不先变成空置物架，也不记进最近关闭）；
    /// 否则播放移除动画，剩下的补位。
    /// </summary>
    private void LetGo(IReadOnlyList<ShelfItem> items)
    {
        if (_host.Settings.CloseWhenEmpty && ViewModel.Items.All(items.Contains))
        {
            _mergedAway = true;
            CloseAnimated();
        }
        else RemoveItems(items);
    }

    /// <summary>光标是不是在某个置物架上：拖回自己或拖到正忙的置物架上没被接收时，不当成“这里放不了文字”。</summary>
    private bool IsCursorOnShelf()
    {
        var cursor = WindowPlacement.GetCursor();
        return _host.OpenShelves.Any(shelf => shelf.IsVisible && shelf.ContainsScreenPoint(cursor));
    }

    /// <summary>右键菜单里的“移到新置物架”。</summary>
    private void SplitOff(IReadOnlyList<ShelfItem> items)
    {
        var (width, height) = CollapsedSize();
        if (SplitPadWindow.Place(this, width, height) is { } place) SplitOff(items, place, null);
    }

    /// <summary>把相对本窗口的一个点限制在窗口里面：飞进来的牌从窗口边上出现，不会在看不见的地方起飞。</summary>
    private WpfPoint ClampInside(WpfPoint point) =>
        new(Math.Clamp(point.X, 16, Math.Max(16, Width - 16)), Math.Clamp(point.Y, 16, Math.Max(16, Height - 16)));

    /// <summary>屏幕上的一个点（像素）是不是落在本窗口里。</summary>
    private bool ContainsScreenPoint(POINT point) =>
        NativeMethods.GetWindowRect(new System.Windows.Interop.WindowInteropHelper(this).Handle, out var rect) &&
        point.X >= rect.Left && point.X < rect.Right && point.Y >= rect.Top && point.Y < rect.Bottom;

    /// <summary>拖着标题栏移动时光标下面的另一个置物架：松手就合并进去。</summary>
    private ShelfWindow? _mergeTarget;
    /// <summary>正被另一个置物架拖到上面，显示着“松开以合并”。</summary>
    private bool _mergeHint;
    /// <summary>已经合并到别的置物架，正在关闭。</summary>
    private bool _mergedAway;

    /// <summary>可以接收合并：显示着、没在关闭或处理，也没有别的内容正在拖进来。</summary>
    private bool CanReceiveMerge => IsVisible && !IsClosing && !ViewModel.IsBusy && !_dragInside;

    /// <summary>拖着标题栏移动的途中：光标到了另一个置物架上就把它标成合并的对象。</summary>
    private void OnMovedByHand(object? sender, EventArgs e)
    {
        var cursor = WindowPlacement.GetCursor();
        SetMergeTarget(_host.OpenShelves.FirstOrDefault(shelf => !ReferenceEquals(shelf, this) && shelf.CanReceiveMerge && shelf.ContainsScreenPoint(cursor)));
    }

    /// <summary>
    /// 换合并的对象：对方显示放置区域和“松开以合并”，边框变成强调色；这边内容变淡、边框也变色并提示，表示松手就并过去。
    /// 离开后两边都恢复。
    /// </summary>
    private void SetMergeTarget(ShelfWindow? target)
    {
        if (ReferenceEquals(target, _mergeTarget)) return;
        _mergeTarget?.ShowMergeHint(false);
        _mergeTarget = target;
        target?.ShowMergeHint(true);
        Fade(Surface, target is null ? 1 : MergeDim, DimDuration);
        WindowEffects.SetBorderColor(this, target is null ? null : ThemeService.AccentFill);
        if (target is not null) ShowToast(new ShelfToast(Strings.ReleaseToMerge, true));
        else HideToast();
    }

    /// <summary>拖到另一个置物架上、松手就合并时，这边内容的透明度。</summary>
    private const double MergeDim = 0.62;

    private void ShowMergeHint(bool show)
    {
        if (show == _mergeHint || IsClosing) return;
        _mergeHint = show;
        if (show)
        {
            ShowDropOverlay();
            DropZoneText.Text = Strings.ReleaseToMerge;
        }
        else if (!_dragInside) EndDrag();
    }

    /// <summary>
    /// 合并：全部项目原样交给 target，从这个置物架所在的位置飞进它的堆叠或列表；这边带着原来的样子淡出关闭，
    /// 不留下一个空置物架，也不记进最近关闭。
    /// </summary>
    private void MergeInto(ShelfWindow target)
    {
        var items = ViewModel.Items.ToArray();
        if (items.Length == 0 || IsClosing || target.IsClosing) return;
        FinishMotion();
        var center = PointToScreen(new WpfPoint(ActualWidth / 2, ActualHeight / 2));
        var added = target.Receive(items, ViewModel, target.ClampInside(target.PointFromScreen(center)));
        if (added > 0 && added < items.Length) target.ViewModel.Notify(Strings.MergedShelves(added));
        _mergedAway = true;
        CloseAnimated();
    }

    /// <summary>选中的项目，按在置物架里的顺序：顺着列表走一遍挑出来，不用给每个选中项各找一次位置再排序。</summary>
    private IReadOnlyList<ShelfItem> GetSelectedItems()
    {
        var selected = ItemsList.SelectedItems.Cast<ShelfItem>().ToHashSet();
        return ViewModel.Items.Where(selected.Contains).ToArray();
    }

    /// <summary>展开且有选中项时只处理选中项，否则处理整个置物架。</summary>
    private IReadOnlyList<ShelfItem> GetTargets()
    {
        if (ViewModel.IsExpanded && ItemsList.SelectedItems.Count > 0) return GetSelectedItems();
        return ViewModel.Items.ToArray();
    }
}
