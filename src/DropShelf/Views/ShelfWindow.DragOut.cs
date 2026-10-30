using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.Views.Controls;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

public partial class ShelfWindow
{
    // ───────────── 拖出 ─────────────

    private void BeginDragOut(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0 || ViewModel.IsBusy) return;
        IsDraggingOut = true;
        DragOutResult result;
        IReadOnlyList<ShelfItem> dragged;
        try
        {
            result = ShelfDragService.DoDragDrop(this, items, ViewModel.Id, out dragged, _dragOutMode);
        }
        finally
        {
            IsDraggingOut = false;
        }

        ViewModel.HasInteracted = true;
        if (result == DragOutResult.Cancelled) return;
        // 聊天软件和浏览器常在放下之后才读取文件，临时附件要多保留一会儿。
        if (dragged.Any(item => item.IsTemporaryAttachment)) ViewModel.KeepTemporaryFilesForAWhile();
        var closeWhenEmpty = _host.Settings.CloseWhenEmpty;
        ViewModel.ApplyDragOut(dragged, result, removeCopied: closeWhenEmpty);
        if (ViewModel.IsEmpty && closeWhenEmpty) CloseAnimated();
    }

    private bool PassedDragThreshold(System.Windows.Input.MouseEventArgs e)
    {
        var current = e.GetPosition(this);
        return Math.Abs(current.X - _pressPoint.X) >= SystemParameters.MinimumHorizontalDragDistance ||
               Math.Abs(current.Y - _pressPoint.Y) >= SystemParameters.MinimumVerticalDragDistance;
    }

    private void OnStackMouseDown(object sender, MouseButtonEventArgs e)
    {
        _stackPressed = true;
        _pressPoint = e.GetPosition(this);
    }

    private void OnStackMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (!_stackPressed || e.LeftButton != MouseButtonState.Pressed || !PassedDragThreshold(e)) return;
        _stackPressed = false;
        BeginDragOut(ViewModel.Items.ToArray());
    }

    /// <summary>点一下牌堆（没有拖动）就摊开成平铺。</summary>
    private void OnStackMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_stackPressed) return;
        _stackPressed = false;
        ViewModel.HasInteracted = true;
        // 这一下若是双击的第一下，第二下会落在刚摊开的牌桌上，不能再把它收回去。
        _expandedByClick = true;
        Expand();
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
        if (e.ClickCount == 1) _expandedByClick = false;
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

    /// <summary>双击牌打开它，双击牌之间的空白处收回堆叠。</summary>
    private void OnListDoubleClick(object sender, MouseButtonEventArgs e)
    {
        var source = e.OriginalSource as DependencyObject;
        if (FindAncestor<ButtonBase>(source) is not null || FindAncestor<ScrollBar>(source) is not null || _expandedByClick) return;
        if (FindAncestor<ListBoxItem>(source) is { DataContext: ShelfItem item })
        {
            ViewModel.Open(item);
            return;
        }
        ViewModel.HasInteracted = true;
        ViewModel.IsExpanded = false;
        e.Handled = true;
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

    private void RemoveItems(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0) return;
        ViewModel.HasInteracted = true;
        RemoveWithMotion(items, () =>
        {
            ViewModel.Remove(items);
            if (ViewModel.IsEmpty && _host.Settings.CloseWhenEmpty) CloseAnimated();
        });
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
