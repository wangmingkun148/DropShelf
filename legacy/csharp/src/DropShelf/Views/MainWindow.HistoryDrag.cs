using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DropShelf.Interop;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow
{
    // ───────────── 拖出最近关闭的置物架 ─────────────

    /// <summary>卡牌拖到牌堆上方这么高的范围内松手算放回；越靠近牌堆，空位对卡牌的吸引越强。</summary>
    private const double HistoryReturnReach = 60;
    private const double DetachedScale = 1.06;

    /// <summary>拖动不到这个距离时卡牌被牌堆吸住，只跟随一小部分；超过后脱离。</summary>
    private const double TearDistance = 48;
    private const double HoldFollow = 0.3;

    /// <summary>拖回牌堆上时卡牌最多被空位拉过去这一比例。</summary>
    private const double ReturnPull = 0.25;

    private HistoryEntryViewModel? _pressedEntry;
    private FrameworkElement? _draggedContainer;
    private Controls.FanPanel? _historyPanel;
    private int _historyIndex;
    private System.Windows.Point _historyPressPoint;
    private Vector _grabOffset;
    private System.Windows.Point _dragStartCursor;
    private bool _tornOff;
    private bool _draggingHistory;
    private CardGhostWindow? _ghost;

    private void OnHistoryMouseDown(object sender, MouseButtonEventArgs e)
    {
        _pressedEntry = null;
        var source = e.OriginalSource as DependencyObject;
        if (_ghost?.IsBusy == true ||
            FindAncestor<FrameworkElement>(source, element => element.DataContext is HistoryEntryViewModel) is not { DataContext: HistoryEntryViewModel entry } ||
            FindAncestor<ButtonBase>(source)?.Command != entry.RestoreCommand) return;
        _pressedEntry = entry;
        _historyPressPoint = e.GetPosition(this);
        _ghost ??= new CardGhostWindow();
    }

    private void OnHistoryMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (_draggingHistory || _pressedEntry is not { } entry || e.LeftButton != MouseButtonState.Pressed) return;
        var position = e.GetPosition(this);
        if (Math.Abs(position.X - _historyPressPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(position.Y - _historyPressPoint.Y) < SystemParameters.MinimumVerticalDragDistance) return;
        BeginHistoryDrag(entry);
    }

    private void OnHistoryMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_draggingHistory)
        {
            _pressedEntry = null;
            return;
        }
        e.Handled = true;
        EndHistoryDrag(cancel: false);
    }

    /// <summary>子元素失去捕获也会冒泡到这里，只处理拖动时列表本身失去捕获（如切换窗口）。</summary>
    private void OnHistoryLostCapture(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (ReferenceEquals(e.OriginalSource, HistoryList)) EndHistoryDrag(cancel: true);
    }

    /// <summary>
    /// 卡牌从当前的位置、大小和角度脱离牌堆，换成跟随光标的截图窗口，原位留出空位。
    /// 截图前先收起选中状态（强调色边框、“点击恢复”提示），拖着的是一张干净的卡牌。
    /// </summary>
    private void BeginHistoryDrag(HistoryEntryViewModel entry)
    {
        _pressedEntry = null;
        if (_ghost is null || HistoryList.ItemContainerGenerator.ContainerFromItem(entry) is not ContentPresenter container ||
            VisualTreeHelper.GetChildrenCount(container) == 0 || VisualTreeHelper.GetChild(container, 0) is not FrameworkElement card ||
            PoseOf(container) is not { } start || !Mouse.Capture(HistoryList, CaptureMode.Element)) return;

        _draggingHistory = true;
        _pressedEntry = entry;
        _draggedContainer = container;
        _dragStartCursor = CursorPoint();
        _grabOffset = _dragStartCursor - start.Center;
        _tornOff = false;
        _historyPanel = VisualTreeHelper.GetParent(container) as Controls.FanPanel;
        _historyIndex = HistoryList.ItemContainerGenerator.IndexFromContainer(container);
        _historyPanel?.ClearHot();
        (container.ContentTemplate?.FindName("RestoreHint", container) as UIElement)?.BeginAnimation(OpacityProperty, null);
        card.UpdateLayout();
        if (Snapshot(card)?.Image.Source is not { } image)
        {
            EndHistoryDrag(cancel: true);
            return;
        }
        container.Opacity = 0;
        _historyPanel?.SetGap(container, _historyIndex);
        _ghost.Layer = () => !_tornOff || HistoryReturnWeight(CursorPoint()) > 0 ? DragLayer : null;
        _ghost.Begin(image, new Size(card.ActualWidth, card.ActualHeight), start, () => HistoryDragPose(start, container));
    }

    /// <summary>
    /// 拖动分三段：先被牌堆吸住、只跟一小段；拉过一定距离后脱离并略微放大，顿一下跟上光标；
    /// 脱离后其余的牌并拢；拖回牌堆上方时按卡牌位置让出空位，越靠近牌堆，卡牌被空位吸得越紧、越接近原来的大小，提示松手会放到这里。
    /// </summary>
    private ScreenCardPose HistoryDragPose(ScreenCardPose start, FrameworkElement container)
    {
        // 松手的消息没送到时（如捕获被打断）卡牌会一直粘在光标上：按键实际已经松开就照常放下。
        if (!NativeMethods.IsButtonPhysicallyDown()) Dispatcher.BeginInvoke(() => EndHistoryDrag(cancel: false));
        var cursor = CursorPoint();
        var pulled = cursor - _dragStartCursor;
        if (!_tornOff && pulled.Length / VisualTreeHelper.GetDpi(this).DpiScaleX < TearDistance)
            return start with { Center = start.Center + pulled * HoldFollow };
        _tornOff = true;
        var free = cursor - _grabOffset;
        var weight = HistoryReturnWeight(cursor);
        _historyPanel?.SetGap(container, weight > 0 ? _historyPanel.SlotAt(_historyPanel.PointFromScreen(free).X) : -1);
        if (weight <= 0 || PoseOf(container) is not { } slot) return new ScreenCardPose(free, DetachedScale, 0);
        var pull = ReturnPull * weight;
        return new ScreenCardPose(free + (slot.Center - free) * pull, DetachedScale + (slot.Scale - DetachedScale) * weight, slot.Angle * pull);
    }

    /// <summary>在牌堆上松手时卡牌飞进让出的空位并移到这个位置，取消时飞回原位；其他位置松手时在光标处打开置物架，卡牌缩小淡出。</summary>
    private void EndHistoryDrag(bool cancel)
    {
        if (!_draggingHistory) return;
        _draggingHistory = false;
        var entry = _pressedEntry!;
        var container = _draggedContainer!;
        var panel = _historyPanel;
        _pressedEntry = null;
        _draggedContainer = null;
        _historyPanel = null;
        if (Mouse.Captured == HistoryList) HistoryList.ReleaseMouseCapture();
        if (_ghost is not { IsBusy: true } ghost)
        {
            container.Opacity = 1;
            panel?.SetGap(null, -1);
            return;
        }

        var cursor = CursorPoint();
        if (cancel || !_tornOff || HistoryReturnWeight(cursor) > 0)
        {
            var original = _historyIndex;
            var index = !cancel && _tornOff && panel?.Gap is >= 0 and var gap ? gap : original;
            panel?.SetGap(container, index);
            // 放回的牌立即参与选中，光标下的牌（通常就是它）直接抽出，卡牌落进抽出的位置。
            panel?.UpdateHot();
            ghost.Layer = () => DragLayer;
            ghost.Settle(() => PoseOf(container), () =>
            {
                container.Opacity = 1;
                if (index == original)
                {
                    panel?.SetGap(null, -1);
                    return;
                }
                // 牌堆保持放回后的排法，等卡牌落定、画面静止后再调整列表顺序；卡片沿用原来的，只换位置，记录在后台保存。
                Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, () => _controller.History.Move(entry.Entry, index));
            });
            return;
        }
        ghost.Layer = () => null;
        ghost.Vanish(cursor);
        _controller.RestoreAt(entry.Entry, new POINT((int)cursor.X, (int)cursor.Y));
    }

    /// <summary>光标离牌堆多近：0 表示不在放回范围内，从范围上沿到牌堆上沿平滑增加到 1。</summary>
    private double HistoryReturnWeight(System.Windows.Point screen)
    {
        var window = PointFromScreen(screen);
        var list = HistoryList.PointFromScreen(screen);
        if (window.X < 0 || window.Y < 0 || window.X > ActualWidth || window.Y > ActualHeight ||
            list.X < 0 || list.X > HistoryList.ActualWidth || list.Y < -HistoryReturnReach || list.Y > HistoryList.ActualHeight) return 0;
        var t = Math.Clamp((list.Y + HistoryReturnReach) / HistoryReturnReach, 0.01, 1);
        return t * t * (3 - 2 * t);
    }

    /// <summary>元素在屏幕上的中心、缩放和角度，包含牌堆给它的变换和动画。</summary>
    private static ScreenCardPose? PoseOf(FrameworkElement element)
    {
        if (!element.IsVisible || element.ActualWidth < 1 || PresentationSource.FromVisual(element) is null) return null;
        var origin = element.PointToScreen(default);
        var edge = element.PointToScreen(new System.Windows.Point(element.ActualWidth, 0)) - origin;
        var center = element.PointToScreen(new System.Windows.Point(element.ActualWidth / 2, element.ActualHeight / 2));
        return new ScreenCardPose(center, edge.Length / (element.ActualWidth * VisualTreeHelper.GetDpi(element).DpiScaleX),
            Math.Atan2(edge.Y, edge.X) * 180 / Math.PI);
    }

    private static System.Windows.Point CursorPoint()
    {
        var cursor = WindowPlacement.GetCursor();
        return new System.Windows.Point(cursor.X, cursor.Y);
    }
}
