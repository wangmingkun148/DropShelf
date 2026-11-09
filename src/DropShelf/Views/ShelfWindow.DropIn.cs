using System.Diagnostics;
using System.Globalization;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;

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
            if (!_dragFromSelf && _dragKind != DropContentKind.None && !ViewModel.IsBusy) ShowDropOverlay();
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
        }
    }

    private static readonly TimeSpan OverlayFadeIn = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan OverlayFadeOut = TimeSpan.FromMilliseconds(100);

    /// <summary>拖进来时内容区淡出，放置区域淡入；只改透明度，不重新排版。</summary>
    private void ShowDropOverlay()
    {
        FinishMotion();
        DropZoneText.Text = ViewModel.HasItems ? Strings.ReleaseToAdd : Strings.ReleaseToHold;
        ResetTiles();
        DropOverlay.IsHitTestVisible = true;
        if (DropOverlay.Visibility != Visibility.Visible) DropOverlay.Opacity = 0;
        DropOverlay.Visibility = Visibility.Visible;
        // 立即完成布局，紧接着判断光标是否在按钮区域时才能拿到正确的位置。
        DropOverlay.UpdateLayout();
        Fade(DropOverlay, 1, OverlayFadeIn);
        Fade(Body, 0, OverlayFadeOut, () => Body.Visibility = Visibility.Hidden);
        WindowEffects.SetBorderColor(this, ThemeService.AccentFill);
    }

    private void EndDrag()
    {
        _leaveCheck.Stop();
        _dragInside = false;
        SetHoveredAction(null);
        Body.Visibility = Visibility.Visible;
        // 淡出期间放置区域不再挡住下面的内容。
        DropOverlay.IsHitTestVisible = false;
        Fade(Body, 1, OverlayFadeIn);
        Fade(DropOverlay, 0, OverlayFadeOut, () => DropOverlay.Visibility = Visibility.Collapsed);
        ResetTiles();
        WindowEffects.SetBorderColor(this, null);
    }

    private readonly Dictionary<UIElement, AnimationTimeline> _fades = [];

    /// <summary>
    /// 淡入淡出到 to，结束时调用 done；中途又换了方向时旧的 done 不再调用。关闭了动画效果时直接到位。
    /// 透明度直接设为终点，动画只在播放期间盖在上面，结束后自然露出终点的值。
    /// </summary>
    private void Fade(UIElement element, double to, TimeSpan duration, Action? done = null)
    {
        var from = element.Opacity;
        element.BeginAnimation(OpacityProperty, null);
        element.Opacity = to;
        _fades.Remove(element);
        if (!ShelfMotion.Enabled || from == to || duration <= TimeSpan.Zero)
        {
            done?.Invoke();
            return;
        }
        var fade = new DoubleAnimation(from, to, duration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }, FillBehavior = FillBehavior.Stop };
        _fades[element] = fade;
        fade.Completed += (_, _) =>
        {
            if (!_fades.TryGetValue(element, out var current) || current != fade) return;
            _fades.Remove(element);
            done?.Invoke();
        };
        element.BeginAnimation(OpacityProperty, fade);
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
        foreach (var (tile, _, _, _, _, info) in _tiles)
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
