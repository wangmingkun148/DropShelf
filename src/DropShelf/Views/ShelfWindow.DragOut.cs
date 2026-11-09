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
        try
        {
            // 准备期间列表不再跟着鼠标改变选中的项目。
            Mouse.Capture(null);
            payload = await ShelfDragService.PrepareAsync(items);
            if (IsClosing || !NativeMethods.IsButtonPhysicallyDown()) return;
            DimDragged(items);
            result = ShelfDragService.DoDragDrop(this, payload, ViewModel.Id, out dragged, _dragOutMode);
        }
        finally
        {
            payload?.Dispose();
            IsDraggingOut = false;
        }

        ViewModel.HasInteracted = true;
        if (result != DragOutResult.Cancelled)
        {
            // 聊天软件和浏览器常在放下之后才读取文件，临时附件要多保留一会儿。
            if (dragged.Any(item => item.IsTemporaryAttachment)) ViewModel.KeepTemporaryFilesForAWhile();
            // 要移走的项目交给 RemoveItems：先播放移除动画，全部移走且设置为自动关闭时直接关闭置物架。
            ViewModel.ApplyDragOut(dragged, result, removeCopied: _host.Settings.CloseWhenEmpty);
        }
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
        RemoveItems([item]);
    }

    /// <summary>移除项目并播放移除动画；全部移走且设置为自动关闭时不播放，直接关闭置物架。</summary>
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

    private IReadOnlyList<ShelfItem> GetSelectedItems() =>
        ItemsList.SelectedItems.Cast<ShelfItem>().OrderBy(item => ViewModel.Items.IndexOf(item)).ToArray();

    /// <summary>展开且有选中项时只处理选中项，否则处理整个置物架。</summary>
    private IReadOnlyList<ShelfItem> GetTargets()
    {
        if (ViewModel.IsExpanded && ItemsList.SelectedItems.Count > 0) return GetSelectedItems();
        return ViewModel.Items.ToArray();
    }
}
