using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views.Controls;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

public partial class ShelfWindow
{
    // ───────────── 快速操作与菜单 ─────────────

    private async void OnActionClick(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is not QuickActionInfo info || ViewModel.IsBusy) return;
        await ViewModel.RunActionAsync(info, GetTargets(), new WindowInteropHelper(this).Handle, PickFolder);
    }

    private string? PickFolder()
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = Strings.ChooseMoveFolder, Multiselect = false };
        return dialog.ShowDialog(this) == true ? dialog.FolderName : null;
    }

    private void OnMoreClick(object sender, RoutedEventArgs e) => OpenMenu(BuildShelfMenu(), MoreButton);

    private void OnViewToggleClick(object sender, RoutedEventArgs e)
    {
        ViewModel.HasInteracted = true;
        if (ViewModel.IsExpanded) ViewModel.IsExpanded = false;
        else Expand();
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => CloseAnimated();

    private void OnHeaderMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ButtonState != MouseButtonState.Pressed) return;
        // 有东西的置物架拖到另一个置物架上松手就合并：移动途中留意光标下面是不是别的置物架。
        var mergeable = ViewModel.HasItems && !ViewModel.IsBusy && !IsDraggingOut && !IsClosing;
        try
        {
            StopSlide();
            ViewModel.HasInteracted = true;
            if (mergeable) LocationChanged += OnMovedByHand;
            DragMove();
        }
        catch (InvalidOperationException)
        {
        }
        finally
        {
            LocationChanged -= OnMovedByHand;
        }

        // 松手时还在那个置物架上、并且已经停够了就合并：对方收起放置区域，这边不恢复原样，直接带着变淡的样子并过去。
        // 还在蓄力时松手只是挪了窗口，对方收起提示。
        if (_mergeTarget is { IsClosing: false } target && !target.ViewModel.IsBusy && target.ContainsScreenPoint(WindowPlacement.GetCursor()))
        {
            _mergeTarget = null;
            target.ShowMergeHint(MergeHint.None);
            HideToast();
            MergeInto(target);
        }
        else SetMergeTarget(null);
        SetMergeCandidate(null);
    }

    private ContextMenu BuildShelfMenu()
    {
        var targets = GetTargets();
        var hasItems = ViewModel.HasItems;
        var menu = new ContextMenu();
        if (ViewModel.IsExpanded)
            menu.Items.Add(CreateMenuItem(Strings.MenuCollapse, "\uE81E", () => ViewModel.IsExpanded = false, "Esc"));
        else if (hasItems)
            menu.Items.Add(CreateMenuItem(Strings.MenuExpand, "\uF0E2", Expand));
        menu.Items.Add(CreateMenuItem(Strings.MenuSelectAll, "", SelectAll, "Ctrl+A", ViewModel.HasMultiple));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopy, "", () => ViewModel.CopyToClipboard(targets), "Ctrl+C", hasItems));
        menu.Items.Add(CreateMenuItem(Strings.MenuPaste, "", Paste, "Ctrl+V"));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopyPath, "", () => ViewModel.CopyPaths(targets), enabled: targets.Any(item => item.IsFileSystem)));
        menu.Items.Add(CreateMenuItem(Strings.MenuReveal, "", () => ViewModel.Reveal(targets), enabled: targets.Any(item => item.IsFileSystem)));
        if (targets.Any(item => !item.IsFileSystem))
            menu.Items.Add(CreateMenuItem(Strings.MenuConvertToFile, "\uE8A5", () => ConvertToFiles(targets)));
        menu.Items.Add(new Separator());
        if (_host.History is { Latest: { } latest } history)
        {
            menu.Items.Add(CreateMenuItem(Strings.RestoreLastShelf, "", () => Restore(latest)));
            menu.Items.Add(HistoryMenu.Create(history.Entries, Restore, history.Clear));
        }
        menu.Items.Add(CreateMenuItem(Strings.MenuClear, "", () => RemoveByUser(ViewModel.Items.ToArray()), enabled: hasItems));
        menu.Items.Add(CreateMenuItem(Strings.MenuSettings, "", _host.ShowSettings));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem(Strings.CloseShelf, "", CloseAnimated, "Esc"));
        return menu;
    }

    /// <summary>恢复的项目放进当前的空置物架；当前置物架已有内容时另开一个。</summary>
    private void Restore(ShelfHistoryEntry entry) => _host.Restore(entry, ViewModel.IsEmpty ? this : null);

    private ContextMenu BuildItemMenu(IReadOnlyList<ShelfItem> items)
    {
        var menu = new ContextMenu();
        var single = items.Count == 1 ? items[0] : null;
        menu.Items.Add(CreateMenuItem(single?.Kind == ShelfItemKind.Text ? Strings.MenuCopyText : Strings.MenuOpen, "",
            () => { if (single is not null) ViewModel.Open(single); }, "Enter", single is not null));
        menu.Items.Add(CreateMenuItem(Strings.MenuReveal, "", () => ViewModel.Reveal(items), enabled: items.Any(item => item.IsFileSystem)));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopy, "", () => ViewModel.CopyToClipboard(items), "Ctrl+C"));
        menu.Items.Add(CreateMenuItem(Strings.MenuCopyPath, "", () => ViewModel.CopyPaths(items), enabled: items.Any(item => item.IsFileSystem)));
        if (items.Any(item => !item.IsFileSystem))
            menu.Items.Add(CreateMenuItem(Strings.MenuConvertToFile, "\uE8A5", () => ConvertToFiles(items)));
        if (items.Count < ViewModel.Count)
            menu.Items.Add(CreateMenuItem(Strings.MenuMoveToNewShelf, "\uE78B", () => SplitOff(items)));
        menu.Items.Add(new Separator());
        menu.Items.Add(CreateMenuItem(Strings.MenuRemove, "", () => RemoveByUser(items), "Delete"));
        return menu;
    }

    private static MenuItem CreateMenuItem(string header, string glyph, Action action, string? gesture = null, bool enabled = true)
    {
        var item = new MenuItem
        {
            Header = header,
            Icon = new TextBlock { Text = glyph },
            InputGestureText = gesture ?? string.Empty,
            IsEnabled = enabled
        };
        item.Click += (_, _) => action();
        return item;
    }

    private static void OpenMenu(ContextMenu menu, UIElement target)
    {
        menu.PlacementTarget = target;
        menu.Placement = PlacementMode.MousePoint;
        menu.IsOpen = true;
    }

    private void SelectAll()
    {
        if (!ViewModel.HasMultiple) return;
        if (!ViewModel.IsExpanded) Expand();
        Dispatcher.BeginInvoke(() => ItemsList.SelectAll(), DispatcherPriority.Input);
    }

    private async void Paste()
    {
        var data = ClipboardService.TryGetDataObject();
        if (data is null)
        {
            ViewModel.Notify(Strings.ClipboardEmpty, false);
            return;
        }
        try
        {
            var read = await DropDataReader.ReadAsync(data, cancellationToken: _lifetime.Token);
            var facts = await ShelfViewModel.ReadFactsAsync(read);
            if (IsClosing)
            {
                foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                return;
            }
            var summary = ViewModel.Add(read, facts);
            // 和拖进来的一样落进堆叠或列表：从光标那里飞进来，光标在窗口外时从离它最近的窗口边上飞进来。
            if (summary.Added > 0 && IsVisible)
            {
                var cursor = WindowPlacement.GetCursor();
                _landing = (ClampInside(PointFromScreen(new WpfPoint(cursor.X, cursor.Y))), summary.Added);
            }
            ViewModel.Notify(summary.Added == 0 ? read.ErrorMessage ?? summary.Message : summary.Message, summary.Added > 0);
        }
        catch (Exception exception)
        {
            LogService.Error("从剪贴板粘贴失败", exception);
            ViewModel.Notify(Strings.PasteFailed, false);
        }
    }

    private void OnPreviewKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        var control = Keyboard.Modifiers.HasFlag(ModifierKeys.Control);
        switch (e.Key)
        {
            case Key.Escape:
                if (ViewModel.IsExpanded) ViewModel.IsExpanded = false;
                else CloseAnimated();
                e.Handled = true;
                break;
            case Key.Delete when ViewModel.IsExpanded && ItemsList.SelectedItems.Count > 0:
                RemoveByUser(GetSelectedItems());
                e.Handled = true;
                break;
            case Key.Z when control:
                if (UndoRemoval()) e.Handled = true;
                break;
            case Key.Enter when ViewModel.IsExpanded && ItemsList.SelectedItems.Count == 1:
                ViewModel.Open(GetSelectedItems()[0]);
                e.Handled = true;
                break;
            case Key.A when control:
                SelectAll();
                e.Handled = true;
                break;
            case Key.C when control:
                ViewModel.CopyToClipboard(GetTargets());
                e.Handled = true;
                break;
            case Key.V when control:
                Paste();
                e.Handled = true;
                break;
        }
    }
}
