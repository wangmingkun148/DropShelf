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
using DropShelf.ViewModels;
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
        var started = Stopwatch.GetTimestamp();
        e.Handled = true;
        _lastDragOverTick = Environment.TickCount64;
        var fresh = !_dragInside;
        if (fresh)
        {
            _dragInside = true;
            _dragFromSelf = DropDataReader.GetSourceShelfId(e.Data) == ViewModel.Id;
            _dragSession = _dragFromSelf ? null : ShelfDragSession.From(e.Data);
            _dragKind = _dragFromSelf ? DropContentKind.None : DropDataReader.Classify(e.Data);
            if (!_dragFromSelf && _dragKind != DropContentKind.None && !ViewModel.IsBusy) ShowDropOverlay(e.Data);
        }
        // 出场期间拖出窗口又回来时，拖入的状态还留着（见 OnDragLeave），只需要重新接上拖动图像和说明。
        if (fresh || _imageLeft)
        {
            _imageLeft = false;
            _lastDescription = null;
            _dropHelper.Enter(this, e.Data, e.AllowedEffects & PreferredEffect(e.AllowedEffects));
        }
        UpdateDrag(e);
        _openTrace?.Outside(started);
    }

    /// <summary>
    /// 两次处理悬停之间至少隔这么久。光标每动一下来源程序就问一次，高回报率的鼠标晃动时一秒上千次，
    /// 每次都要判断光标在哪张卡片上、再跨进程挪一次拖动图像，界面线程全花在这上面，出场动画就画不动了；
    /// 屏幕一帧也就这么长，问得再勤也显示不出来。间隔内的询问直接给出上次的结果。
    /// </summary>
    private static readonly TimeSpan DragOverInterval = TimeSpan.FromMilliseconds(8);
    private long _dragOverHandled;
    private WpfDragDropEffects _dragEffects;

    private void OnDragOver(object sender, WpfDragEventArgs e)
    {
        var started = Stopwatch.GetTimestamp();
        e.Handled = true;
        _lastDragOverTick = Environment.TickCount64;
        if (Stopwatch.GetElapsedTime(_dragOverHandled, started) < DragOverInterval) e.Effects = _dragEffects;
        else UpdateDrag(e);
        _openTrace?.Outside(started);
    }

    /// <summary>按光标位置更新拖入的反馈并给出放置效果。</summary>
    private void UpdateDrag(WpfDragEventArgs e)
    {
        ApplyDrag(e);
        _dragEffects = e.Effects;
        _dragOverHandled = Stopwatch.GetTimestamp();
    }

    private void ApplyDrag(WpfDragEventArgs e)
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
        // 出场动画期间先不展开：晃动呼出时光标还在来回甩，展开和收起每帧都要重新排版，正好压在出场动画上；播完后下一次移动再判断。
        if (ViewModel.HasActions && !IsPopping) SetTilesExpanded(e.GetPosition(ActionTilesHost).Y >= 0);
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
            SetDescription(e.Data, DropImageType.Label, action?.Label ??
                (_dragSession is not null ? Strings.MoveToThisShelf : ViewModel.HasItems ? Strings.AddToShelf : Strings.PutOnShelf));
        }
        _dropHelper.Over(e.Effects);
    }

    /// <summary>拖进来的是另一个置物架上的项目：松手后不读拖放数据，由那个置物架把项目原样交过来，见 ShelfDragSession。</summary>
    private ShelfDragSession? _dragSession;

    /// <summary>拖出了窗口、已经交还拖动图像，但拖入的状态还留着等它回来，见 OnDragLeave。</summary>
    private bool _imageLeft;

    private void OnDragLeave(object sender, WpfDragEventArgs e)
    {
        var started = Stopwatch.GetTimestamp();
        e.Handled = true;
        var inside = WindowPlacement.IsCursorInside(this);
        if (!inside)
        {
            _dropHelper.Leave();
            _imageLeft = true;
        }
        // 在窗口内部的子元素之间移动也会触发离开事件，稍后确认拖动确实结束再收起。
        // 出场动画期间真的拖出去了也先留着：晃动呼出时光标常常甩出窗口又回来，每次都收起、再向来源程序重新读取并展开，
        // 这些都压在出场动画上；回来时接着用，没回来就由同一个计时器收起。
        if (inside || IsPopping)
        {
            _leaveCheck.Stop();
            _leaveCheck.Start();
        }
        else EndDrag();
        _openTrace?.Outside(started);
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
        // 悬停的处理有间隔（见 OnDragOver），松手时按松手的位置再判断一次落在哪张快速操作卡片上。
        var action = HitTestTiles(e);
        var fromSelf = _dragFromSelf;
        var kind = _dragKind;
        var session = _dragSession;
        // 来自另一个置物架的项目稍后才交过来，那时摊开的牌已经收好，这里照常淡回去。
        var fan = action is null && session is null && !fromSelf && !ViewModel.IsExpanded ? FanPoses() : null;
        EndDrag(instant: fan is not null);

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
        // 来自另一个置物架：记下放在了这里，拖动结束后那边把项目交过来；放到快速操作上时照常读取文件来处理。
        if (session is not null && action is null)
        {
            session.DropOn(this, dropPoint);
            return;
        }
        if (fan is not null) HoldFan(fan);

        try
        {
            if (action is not null)
            {
                DropReadResult files;
                ViewModel.ShowReceiving(Strings.ReadingDrop);
                try
                {
                    files = await Receive(DropDataReader.ReadAsync(e.Data, filesOnly: true, _lifetime.Token));
                }
                finally
                {
                    ViewModel.ShowReceiving(null);
                }
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
                // 文件很多、在网络盘上，或磁盘很久没用时，读取可能要好几秒：这期间显示正在读取，不要看起来像什么都没放进去。
                DropReadResult read;
                IReadOnlyDictionary<string, ShelfViewModel.FileFacts?>? facts;
                ViewModel.ShowReceiving(Strings.ReadingDrop);
                try
                {
                    read = await Receive(DropDataReader.ReadAsync(e.Data, cancellationToken: _lifetime.Token));
                    if (read.FilePaths.Count > 1) ViewModel.ShowReceiving(Strings.ReadingFiles(read.FilePaths.Count));
                    facts = await Receive(ShelfViewModel.ReadFactsAsync(read));
                }
                finally
                {
                    ViewModel.ShowReceiving(null);
                }
                if (IsClosing)
                {
                    foreach (var cache in read.CacheDirectories) _ = VirtualFileDropService.ReleaseCacheAsync(cache);
                    return;
                }
                var summary = ViewModel.Add(read, facts);
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

    /// <summary>正在读取的拖入内容有几批；这期间算作忙碌，见 IsAnimating。</summary>
    private int _receiving;

    private async Task<T> Receive<T>(Task<T> work)
    {
        _receiving++;
        try
        {
            return await work;
        }
        finally
        {
            _receiving--;
        }
    }

    /// <summary>data 为 null 时（合并，没有拖放数据）角标显示 incoming。</summary>
    private void ShowDropOverlay(System.Windows.IDataObject? data, int incoming = 0)
    {
        FinishMotion();
        DropZoneText.Text = _dragSession is not null ? Strings.ReleaseToMoveHere : ViewModel.HasItems ? Strings.ReleaseToAdd : Strings.ReleaseToHold;
        if (data is not null) incoming = IncomingCount(data);
        IncomingText.Text = $"+{incoming}";
        IncomingBadge.Visibility = incoming > 0 ? Visibility.Visible : Visibility.Collapsed;
        FanOutStack();
        // 平常拖入时不带合并的蓄力底色；合并的提示自己接着设置，见 ArmMerge。
        if (_mergeHint == MergeHint.None) ResetMergeCharge();
        ResetTiles();
        DropOverlay.IsHitTestVisible = true;
        if (DropOverlay.Visibility != Visibility.Visible) DropOverlay.Opacity = 0;
        DropOverlay.Visibility = Visibility.Visible;
        // 出场动画期间直接到位：内容整体正在淡入，里面再交叉淡入淡出看不出来，却要每帧重画一遍缓存的画面；
        // 这时也不判断按钮区域（见 UpdateDrag），布局留给下一帧。
        var entering = IsPopping;
        // 立即完成布局，紧接着判断光标是否在按钮区域时才能拿到正确的位置。
        if (!entering) DropOverlay.UpdateLayout();
        // 内容区淡出，摊开的牌和放置区域淡入；只改透明度，不重新排版。
        Fade(DropOverlay, 1, entering ? TimeSpan.Zero : OverlayFadeIn);
        Fade(Body, 0, entering ? TimeSpan.Zero : OverlayFadeOut, () => Body.Visibility = Visibility.Hidden);
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
        foreach (var (target, property, from, to) in moves)
            target.BeginAnimation(property, ShelfMotion.Enabled ? new DoubleAnimation(from, to, FanDuration) { EasingFunction = MotionEase } : null);
        FanSlot.BeginAnimation(OpacityProperty, ShelfMotion.Enabled ? new DoubleAnimation(0, 1, FanDuration) { EasingFunction = MotionEase } : null);
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

    private static readonly TimeSpan OverlayFadeIn = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan OverlayFadeOut = TimeSpan.FromMilliseconds(100);

    /// <summary>
    /// 拖动离开或松手：放置区域淡出、内容区淡入。instant 时直接换回：松手后堆叠要从摊开的样子接着落牌（见 HoldFan），两边的牌正好重合，交叉淡化反而会让牌变淡一下。
    /// </summary>
    private void EndDrag(bool instant = false)
    {
        _leaveCheck.Stop();
        _dragInside = false;
        _dragSession = null;
        SetHoveredAction(null);
        Body.Visibility = Visibility.Visible;
        // 淡出期间放置区域不再挡住下面的内容。
        DropOverlay.IsHitTestVisible = false;
        Fade(Body, 1, instant ? TimeSpan.Zero : OverlayFadeIn);
        Fade(DropOverlay, 0, instant ? TimeSpan.Zero : OverlayFadeOut, () =>
        {
            DropOverlay.Visibility = Visibility.Collapsed;
            ResetMergeCharge();
        });
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
        var fade = new DoubleAnimation(from, to, duration) { EasingFunction = MotionEase, FillBehavior = FillBehavior.Stop };
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
        foreach (var (element, property, value) in TileStates(expanded: false)) Ease(element, property, value, null);
    }

    private void SetTilesExpanded(bool expanded)
    {
        if (expanded == _tilesExpanded) return;
        _tilesExpanded = expanded;
        TimeSpan? duration = !ShelfMotion.Enabled ? null : expanded ? TilesExpandDuration : TilesCollapseDuration;
        foreach (var (element, property, value) in TileStates(expanded)) Ease(element, property, value, duration);
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
        // 拖动途中系统不停地来问，说明没变时不用重新设置，也不用每次拼一个字符串来比较。
        if (_lastDescription is { } last && last == (type, message)) return;
        _lastDescription = (type, message);
        DropTargetHelper.SetDescription(data, type, message);
    }

    private static WpfDragDropEffects PreferredEffect(WpfDragDropEffects allowed)
    {
        if (allowed.HasFlag(WpfDragDropEffects.Copy)) return WpfDragDropEffects.Copy;
        if (allowed.HasFlag(WpfDragDropEffects.Link)) return WpfDragDropEffects.Link;
        if (allowed.HasFlag(WpfDragDropEffects.Move)) return WpfDragDropEffects.Move;
        return WpfDragDropEffects.Copy;
    }

    /// <summary>
    /// 处理完置物架仍是空的（直接放到了快速操作上，或者什么也没放进来）：设置为自动关闭空置物架时才关闭，关闭了这一项就留着。
    /// 正显示着结果提示时等提示消失再关，提示能完整看完，这期间又放进了东西就不关，见 HideToast；没有提示时稍等一会儿再关。
    /// </summary>
    private async Task CloseIfStillEmptyAsync()
    {
        if (!ViewModel.IsEmpty || IsClosing || !_host.Settings.CloseWhenEmpty) return;
        if (_toastTimer.IsEnabled)
        {
            _closeAfterToast = true;
            return;
        }
        await Task.Delay(1500);
        if (ViewModel.IsEmpty && !ViewModel.IsBusy && !IsDraggingOut && !_dragInside && _receiving == 0 && _host.Settings.CloseWhenEmpty)
            CloseAnimated();
    }
}
