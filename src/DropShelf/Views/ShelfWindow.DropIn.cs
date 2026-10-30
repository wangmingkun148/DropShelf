using System.Diagnostics;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using DropShelf.Interop;
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
    // ───────────── 拖入 ─────────────

    private void OnDragEnter(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        _lastDragOverTick = Environment.TickCount64;
        if (!_dragInside)
        {
            _dragInside = true;
            _lastDescription = null;
            _dragFromSelf = DropDataReader.GetSourceShelfId(e.Data) == ViewModel.Id;
            _dragKind = _dragFromSelf ? DropContentKind.None : DropDataReader.Classify(e.Data);
            if (!_dragFromSelf && _dragKind != DropContentKind.None && !ViewModel.IsBusy) ShowDropOverlay(e.Data);
            _dropHelper.Enter(this, e.Data, e.AllowedEffects & PreferredEffect(e.AllowedEffects));
        }
        UpdateDrag(e);
    }

    private void OnDragOver(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        _lastDragOverTick = Environment.TickCount64;
        UpdateDrag(e);
    }

    private void UpdateDrag(WpfDragEventArgs e)
    {
        if (_dragFromSelf)
        {
            e.Effects = WpfDragDropEffects.None;
            _dropHelper.Over(e.Effects);
            return;
        }

        if (_dragKind == DropContentKind.None || ViewModel.IsBusy)
        {
            e.Effects = WpfDragDropEffects.None;
            SetDescription(e.Data, DropImageType.Warning, ViewModel.IsBusy ? Strings.PleaseWait : Strings.UnsupportedContent);
            _dropHelper.Over(e.Effects);
            return;
        }

        // 拖到底部按钮区域时展开快速操作；展开后区域变高，移回上方的放置区域再收起。
        if (ViewModel.HasActions) SetTilesExpanded(e.GetPosition(ActionTilesHost).Y >= 0);
        var action = HitTestTiles(e);
        SetHoveredAction(action);
        if (action is not null && !DropDataReader.CanProvideFiles(_dragKind))
        {
            e.Effects = WpfDragDropEffects.None;
            SetDescription(e.Data, DropImageType.Warning, Strings.FilesOnlyAction);
        }
        else
        {
            e.Effects = PreferredEffect(e.AllowedEffects);
            SetDescription(e.Data, DropImageType.Label, action?.Label ?? (ViewModel.HasItems ? Strings.AddToShelf : Strings.PutOnShelf));
        }
        _dropHelper.Over(e.Effects);
    }

    private void OnDragLeave(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        if (WindowPlacement.IsCursorInside(this))
        {
            // 在窗口内部的子元素之间移动也会触发离开事件，稍后确认拖动确实结束再收起。
            _leaveCheck.Stop();
            _leaveCheck.Start();
            return;
        }
        EndDrag();
        _dropHelper.Leave();
    }

    private void OnLeaveCheck(object? sender, EventArgs e)
    {
        _leaveCheck.Stop();
        if (!_dragInside || Environment.TickCount64 - _lastDragOverTick < 200) return;
        EndDrag();
        _dropHelper.Leave();
    }

    private async void OnDrop(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        var dropPoint = e.GetPosition(this);
        var action = _hoveredAction;
        var fromSelf = _dragFromSelf;
        var kind = _dragKind;
        var fan = action is null && !fromSelf && !ViewModel.IsExpanded ? FanPoses() : null;
        EndDrag();

        if (fromSelf || kind == DropContentKind.None || ViewModel.IsBusy ||
            (action is not null && !DropDataReader.CanProvideFiles(kind)))
        {
            e.Effects = WpfDragDropEffects.None;
            _dropHelper.Drop(e.Data, e.Effects);
            if (!fromSelf && kind == DropContentKind.None) ViewModel.Notify(Strings.SourceProvidedNothing, false);
            return;
        }

        e.Effects = PreferredEffect(e.AllowedEffects);
        _dropHelper.Drop(e.Data, e.Effects);
        ViewModel.HasInteracted = true;
        if (fan is not null) HoldFan(fan);

        try
        {
            if (action is not null)
            {
                var files = await DropDataReader.ReadAsync(e.Data, filesOnly: true, _lifetime.Token);
                if (IsClosing)
                {
                    foreach (var cache in files.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                    return;
                }
                var result = await ViewModel.RunActionAsync(action, files, new WindowInteropHelper(this).Handle, PickFolder);
                if (result.Succeeded) await CloseIfStillEmptyAsync();
            }
            else
            {
                var read = await DropDataReader.ReadAsync(e.Data, cancellationToken: _lifetime.Token);
                if (IsClosing)
                {
                    foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                    return;
                }
                var summary = ViewModel.Add(read);
                if (summary.Added > 0) _landing = (dropPoint, summary.Added);
                else CloseFan();
                if (summary.Added == 0) ViewModel.Notify(read.ErrorMessage ?? summary.Message, false);
                else if (summary.Duplicates > 0 || summary.Unavailable > 0 || read.FailedCount > 0)
                    ViewModel.Notify(read.FailedCount > 0 ? Strings.WithFailedAttachments(summary.Message, read.FailedCount) : summary.Message);
                if (ViewModel.IsEmpty) await CloseIfStillEmptyAsync();
            }
        }
        catch (Exception exception)
        {
            LogService.Error("处理拖入内容失败", exception);
            ViewModel.Notify(Strings.DropFailed, false);
            CloseFan();
        }
    }

    private void ShowDropOverlay(System.Windows.IDataObject data)
    {
        FinishMotion();
        DropZoneText.Text = ViewModel.HasItems ? Strings.ReleaseToAdd : Strings.ReleaseToHold;
        var incoming = IncomingCount(data);
        IncomingText.Text = $"+{incoming}";
        IncomingBadge.Visibility = incoming > 0 ? Visibility.Visible : Visibility.Collapsed;
        FanOutStack();
        Body.Visibility = Visibility.Hidden;
        ResetTiles();
        DropOverlay.Visibility = Visibility.Visible;
        // 立即完成布局，紧接着判断光标是否在按钮区域时才能拿到正确的位置。
        DropOverlay.UpdateLayout();
        WindowEffects.SetBorderColor(this, ThemeService.AccentFill);
    }

    /// <summary>拖进来的文件数；读不到文件列表（文字、链接、虚拟文件）时为 0，不显示数字。</summary>
    private static int IncomingCount(System.Windows.IDataObject data)
    {
        try
        {
            return data.GetDataPresent(System.Windows.DataFormats.FileDrop) && data.GetData(System.Windows.DataFormats.FileDrop) is string[] files
                ? files.Length
                : 0;
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>
    /// 摊牌：原来那一摞最上面两张从叠着的样子向左右各转开一点，中间的虚线空位浮起来，等松手。
    /// 牌面和堆叠里一样；只有一项时只摊开一张，空置物架只显示空位。停住的样子就是 XAML 中的静止角度和位移。
    /// </summary>
    private void FanOutStack()
    {
        // 每张牌的转角和位移从叠着时的样子（见 StackCanvas 里的 Card0、Card1）动到摊开的样子。
        var moves = new (Animatable Target, DependencyProperty Property, double From, double To)[]
        {
            (FanLeftTurn, RotateTransform.AngleProperty, 0, -13), (FanLeftShift, TranslateTransform.XProperty, 0, -34),
            (FanLeftShift, TranslateTransform.YProperty, 0, 4), (FanRightTurn, RotateTransform.AngleProperty, 7, 12),
            (FanRightShift, TranslateTransform.XProperty, 11, 34), (FanRightShift, TranslateTransform.YProperty, 2, 4),
            (FanSlotScale, ScaleTransform.ScaleXProperty, 0.9, 1), (FanSlotScale, ScaleTransform.ScaleYProperty, 0.9, 1),
            (FanSlotShift, TranslateTransform.YProperty, 6, -3)
        };
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        foreach (var (target, property, from, to) in moves)
            target.BeginAnimation(property, ShelfMotion.Enabled ? new DoubleAnimation(from, to, FanDuration) { EasingFunction = ease } : null);
        FanSlot.BeginAnimation(OpacityProperty, ShelfMotion.Enabled ? new DoubleAnimation(0, 1, FanDuration) { EasingFunction = ease } : null);
    }

    /// <summary>摊开的两张牌在 StackCanvas 里的姿态：松手后堆叠里的牌从这里合拢，看起来是同一摞牌。没有摊开的牌时为 null。</summary>
    private LocalCardPose?[] FanPoses() => [PoseIn(FanLeft, StackCanvas), PoseIn(FanRight, StackCanvas)];

    private static LocalCardPose? PoseIn(FrameworkElement card, Visual target)
    {
        if (card.Visibility != Visibility.Visible || PresentationSource.FromVisual(card) is null) return null;
        var transform = card.TransformToVisual(target);
        var center = transform.Transform(new WpfPoint(card.Width / 2, card.Height / 2));
        var edge = transform.Transform(new WpfPoint(card.Width, card.Height / 2)) - center;
        return new LocalCardPose(center.X, center.Y, Math.Atan2(edge.Y, edge.X) * 180 / Math.PI, edge.Length / (card.Width / 2));
    }

    private void EndDrag()
    {
        _leaveCheck.Stop();
        _dragInside = false;
        SetHoveredAction(null);
        DropOverlay.Visibility = Visibility.Collapsed;
        Body.Visibility = Visibility.Visible;
        ResetTiles();
        WindowEffects.SetBorderColor(this, null);
    }

    /// <summary>快速操作收起为底部按钮栏的高度：按钮栏保持可见，提示可以拖到这里。</summary>
    private void ResetTiles()
    {
        _tilesExpanded = false;
        foreach (var (element, property, value) in TileStates(expanded: false))
        {
            element.BeginAnimation(property, null);
            element.SetValue(property, value);
        }
    }

    private void SetTilesExpanded(bool expanded)
    {
        if (expanded == _tilesExpanded) return;
        _tilesExpanded = expanded;
        var duration = expanded ? TilesExpandDuration : TilesCollapseDuration;
        foreach (var (element, property, value) in TileStates(expanded))
        {
            if (ShelfMotion.Enabled)
                element.BeginAnimation(property, new DoubleAnimation(value, duration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
            else
            {
                element.BeginAnimation(property, null);
                element.SetValue(property, value);
            }
        }
    }

    /// <summary>展开时快速操作从按钮栏向上长高并淡入，按钮栏同时淡出。</summary>
    private IEnumerable<(UIElement Element, DependencyProperty Property, double Value)> TileStates(bool expanded)
    {
        var collapsedHeight = ViewModel.HasActions ? ActionBarHeight : 0;
        if (expanded) ActionTiles.Measure(new System.Windows.Size(ActionTilesHost.ActualWidth, double.PositiveInfinity));
        yield return (ActionTilesHost, HeightProperty, expanded ? ActionTiles.DesiredSize.Height : collapsedHeight);
        yield return (ActionTiles, OpacityProperty, expanded ? 1 : 0);
        yield return (ActionBarHost, OpacityProperty, expanded ? 0 : 1);
    }

    private QuickActionInfo? HitTestTiles(WpfDragEventArgs e)
    {
        if (!_tilesExpanded) return null;
        foreach (var (tile, _, _, info) in _tiles)
        {
            if (tile.ActualWidth <= 0) continue;
            var point = e.GetPosition(tile);
            if (point.X >= -3 && point.Y >= -3 && point.X <= tile.ActualWidth + 3 && point.Y <= tile.ActualHeight + 3) return info;
        }
        return null;
    }

    private void SetDescription(System.Windows.IDataObject data, DropImageType type, string message)
    {
        var key = $"{(int)type}:{message}";
        if (key == _lastDescription) return;
        _lastDescription = key;
        DropTargetHelper.SetDescription(data, type, message);
    }

    private static WpfDragDropEffects PreferredEffect(WpfDragDropEffects allowed)
    {
        if (allowed.HasFlag(WpfDragDropEffects.Copy)) return WpfDragDropEffects.Copy;
        if (allowed.HasFlag(WpfDragDropEffects.Link)) return WpfDragDropEffects.Link;
        if (allowed.HasFlag(WpfDragDropEffects.Move)) return WpfDragDropEffects.Move;
        return WpfDragDropEffects.Copy;
    }

    private async Task CloseIfStillEmptyAsync()
    {
        if (!ViewModel.IsEmpty) return;
        await Task.Delay(1500);
        if (ViewModel.IsEmpty && !ViewModel.IsBusy && !IsDraggingOut) CloseAnimated();
    }
}
