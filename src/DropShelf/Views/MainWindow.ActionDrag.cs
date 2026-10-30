using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow
{
    // ───────────── 拖动更换快速操作 ─────────────

    /// <summary>按下鼠标的操作卡片；移动超过拖动距离后才开始拖动。</summary>
    private ActionCardViewModel? _pressedCard;
    private System.Windows.Point _pressPoint;
    private bool _draggingCard;
    private ActionSlotViewModel? _dropTarget;
    private bool _overDeleteZone;

    private void OnActionCardMouseDown(object sender, MouseButtonEventArgs e)
    {
        var source = e.OriginalSource as DependencyObject;
        if (FindAncestor<ButtonBase>(source) is not null) return;
        _pressedCard = FindAncestor<FrameworkElement>(source, element => element.DataContext is ActionCardViewModel)?.DataContext as ActionCardViewModel;
        _pressPoint = e.GetPosition(DragLayer);
    }

    private void OnActionCardMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (_pressedCard is not { } card || e.LeftButton != MouseButtonState.Pressed) return;
        var position = e.GetPosition(DragLayer);
        if (!_draggingCard)
        {
            if (Math.Abs(position.X - _pressPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
                Math.Abs(position.Y - _pressPoint.Y) < SystemParameters.MinimumVerticalDragDistance) return;
            BeginCardDrag(card);
        }

        // 内置操作拖进底部区域时像碰到弹性边界：越往下越拖不动，最多陷进去一小段。
        var zoneTop = DragLayer.ActualHeight - DeleteZone.ActualHeight;
        var y = card.IsCustom || position.Y <= zoneTop
            ? position.Y
            : zoneTop + LockedSink * (1 - Math.Exp(-(position.Y - zoneTop) / LockedSink));
        DragGhostShift.X = position.X + 12;
        DragGhostShift.Y = y + 8;
        SetOverDeleteZone(position.Y >= zoneTop);
        SetDropTarget(_overDeleteZone ? null : HitTestSlot(e.GetPosition(ActionSlotsList)), card);
    }

    private void OnActionCardMouseUp(object sender, MouseButtonEventArgs e)
    {
        var card = _pressedCard;
        var target = _dropTarget;
        var dragged = _draggingCard;
        var overZone = _overDeleteZone;
        EndCardDrag();
        if (!dragged || card is null) return;
        if (overZone && card.IsCustom) ConfirmDeleteCustomAction(card);
        else if (overZone) BounceBackGhost();
        else if (target is not null) ViewModel.AssignSlot(target.Index, card.Action.Id);
    }

    /// <summary>内置操作在底部松手：跟随的卡片弹回区域上方并淡出。</summary>
    private void BounceBackGhost()
    {
        DragGhost.Visibility = Visibility.Visible;
        var top = DragLayer.ActualHeight - DeleteZone.ActualHeight - DragGhost.ActualHeight;
        DragGhostShift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(Math.Min(DragGhostShift.Y, top), TimeSpan.FromMilliseconds(320))
        {
            EasingFunction = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 }
        });
        var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(180)) { BeginTime = TimeSpan.FromMilliseconds(160) };
        fade.Completed += (_, _) =>
        {
            if (_draggingCard) return;
            DragGhost.Visibility = Visibility.Collapsed;
            ClearGhostAnimations();
        };
        DragGhost.BeginAnimation(OpacityProperty, fade);
    }

    private void ClearGhostAnimations()
    {
        DragGhostShift.BeginAnimation(TranslateTransform.YProperty, null);
        DragGhost.BeginAnimation(OpacityProperty, null);
    }

    /// <summary>要删除的自定义操作，确认对话框打开期间有值。</summary>
    private ActionCardViewModel? _pendingDelete;

    /// <summary>与编辑对话框相同样式的确认框：遮罩淡入，对话框从略小处放大到原尺寸。</summary>
    private void ConfirmDeleteCustomAction(ActionCardViewModel card)
    {
        _pendingDelete = card;
        ConfirmMessage.Text = Strings.ConfirmDeleteAction(card.Title);
        ConfirmOverlay.Visibility = Visibility.Visible;
        ConfirmOverlay.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(150)));
        ConfirmDialogScale.BeginAnimation(ScaleTransform.ScaleXProperty, Motion(0.96, 1));
        ConfirmDialogScale.BeginAnimation(ScaleTransform.ScaleYProperty, Motion(0.96, 1));
        ConfirmCancelButton.Focus();
    }

    private void OnConfirmDeleteClick(object sender, RoutedEventArgs e)
    {
        if (_pendingDelete?.Action.Custom is { } custom) ViewModel.DeleteCustomAction(custom);
        CloseConfirm();
    }

    private void OnConfirmCancelClick(object sender, RoutedEventArgs e) => CloseConfirm();

    private void CloseConfirm()
    {
        if (_pendingDelete is null) return;
        _pendingDelete = null;
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(120));
        fade.Completed += (_, _) =>
        {
            if (_pendingDelete is null) ConfirmOverlay.Visibility = Visibility.Collapsed;
        };
        ConfirmOverlay.BeginAnimation(OpacityProperty, fade);
    }

    /// <summary>子元素失去捕获也会冒泡到这里，只处理拖动时页面本身失去捕获（如切换窗口）。</summary>
    private void OnActionCardLostCapture(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (ReferenceEquals(e.OriginalSource, ActionsPage)) EndCardDrag();
    }

    private void BeginCardDrag(ActionCardViewModel card)
    {
        _draggingCard = true;
        card.IsDragSource = true;
        DragGhostGlyph.Text = card.Glyph;
        DragGhostText.Text = card.Title;
        ClearGhostAnimations();
        DragGhost.Visibility = Visibility.Visible;
        var zoneBrush = (System.Windows.Media.Brush)FindResource(card.IsCustom ? "DeleteBrush" : "LockedBrush");
        DeleteGlow.Fill = DeleteHint.Background = zoneBrush;
        DeleteHintGlyph.Text = (string)FindResource(card.IsCustom ? "Glyph.Delete" : "Glyph.Lock");
        DeleteHintText.Text = card.IsCustom ? Strings.ReleaseToDelete : Strings.BuiltInCannotDelete;
        Fade(DeleteZone, 1);
        Mouse.Capture(ActionsPage, CaptureMode.SubTree);
    }

    private void EndCardDrag()
    {
        if (_pressedCard is { } card) card.IsDragSource = false;
        SetDropTarget(null, null);
        SetOverDeleteZone(false);
        Fade(DeleteZone, 0);
        _pressedCard = null;
        DragGhost.Visibility = Visibility.Collapsed;
        if (!_draggingCard) return;
        _draggingCard = false;
        if (Mouse.Captured == ActionsPage) ActionsPage.ReleaseMouseCapture();
    }

    /// <summary>内置操作拖进底部区域时最多陷进去的深度。</summary>
    private const double LockedSink = 28;

    /// <summary>拖进底部区域时光效变亮，并提示松开删除或不能删除。</summary>
    private void SetOverDeleteZone(bool value)
    {
        if (value == _overDeleteZone) return;
        _overDeleteZone = value;
        Fade(DeleteGlow, value ? 0.8 : 0.35);
        Fade(DeleteHint, value ? 1 : 0);
    }

    private static void Fade(UIElement element, double to) =>
        element.BeginAnimation(OpacityProperty, new DoubleAnimation(to, TimeSpan.FromMilliseconds(150)));

    /// <summary>悬停的位置显示“交换”（从另一个位置拖来）或“替换”（从更多操作拖来）。</summary>
    private void SetDropTarget(ActionSlotViewModel? target, ActionCardViewModel? card)
    {
        if (ReferenceEquals(target, card)) target = null;
        if (ReferenceEquals(target, _dropTarget)) return;
        if (_dropTarget is not null) _dropTarget.DropHint = null;
        _dropTarget = target;
        if (target is not null) target.DropHint = card is ActionSlotViewModel ? Strings.DropHintSwap : Strings.DropHintReplace;
    }

    private ActionSlotViewModel? HitTestSlot(System.Windows.Point point) =>
        ActionSlotsList.IsVisible && ActionSlotsList.InputHitTest(point) is DependencyObject hit
            ? FindAncestor<FrameworkElement>(hit, element => element.DataContext is ActionSlotViewModel)?.DataContext as ActionSlotViewModel
            : null;
}
