using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow
{
    // ───────────── 状态栏顶部的页面标题 ─────────────

    private static readonly TimeSpan HeaderDuration = TimeSpan.FromMilliseconds(340);
    private static readonly IEasingFunction RollEase = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.35 };
    private static readonly IEasingFunction PopEase = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.5 };

    /// <summary>
    /// 状态栏显示时，设置页的标题放在状态栏顶部：打开设置页时图标缩小消失，“概览”缩小飞进上方的返回链接，
    /// 页面名称从上往下滚出；返回时页面名称继续向下滚走，“概览”从返回链接放大飞回，图标弹回。
    /// </summary>
    private void SetPanelHeader(string page, bool animate)
    {
        var onOverview = page == "Overview";
        // 从一个设置页抽到另一个设置页：返回链接和图标都不动，只换页名。
        var swap = !onOverview && PanelBackButton.IsHitTestVisible;
        // 绑定到卡片的标题，切换语言后跟着更新。
        if (!onOverview && PageCards.Children.OfType<Controls.PageTile>().FirstOrDefault(tile => Equals(tile.Tag, page)) is { } pageTile)
            PanelPageName.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(Controls.PageTile.Header)) { Source = pageTile });
        PanelBackButton.IsHitTestVisible = !onOverview;
        foreach (var (target, property) in new (Animatable, DependencyProperty)[]
                 {
                     (SidePanelIconScale, ScaleTransform.ScaleXProperty), (SidePanelIconScale, ScaleTransform.ScaleYProperty),
                     (PanelTitleScale, ScaleTransform.ScaleXProperty), (PanelTitleScale, ScaleTransform.ScaleYProperty),
                     (PanelTitleShift, TranslateTransform.XProperty), (PanelTitleShift, TranslateTransform.YProperty),
                     (PanelPageNameShift, TranslateTransform.YProperty)
                 })
            target.BeginAnimation(property, null);
        foreach (var element in new UIElement[] { SidePanelIcon, PanelPageTitle, PanelBackButton, PanelPageName })
            element.BeginAnimation(OpacityProperty, null);

        // 先设好最终状态，动画结束或被清除后停在这里。
        SidePanelIcon.Opacity = onOverview ? 1 : 0;
        PanelBackButton.Opacity = PanelPageName.Opacity = onOverview ? 0 : 1;
        PanelPageTitle.Visibility = _sidePanelShown && onOverview ? Visibility.Visible : Visibility.Collapsed;
        if (!animate || !_sidePanelShown) return;
        if (swap)
        {
            PanelPageNameShift.BeginAnimation(TranslateTransform.YProperty, Header(-40, 0, RollEase, 380));
            PanelPageName.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 200));
            return;
        }

        // “概览”与返回链接里的“概览”之间的位移和缩放，按文字左上角对齐。
        PanelPageTitle.Visibility = Visibility.Visible;
        PanelPageTitle.UpdateLayout();
        var offset = PositionOf(PanelCrumbText) - PositionOf(PanelPageTitle);
        var ratio = PanelCrumbText.FontSize / PanelPageTitle.FontSize;
        var (shiftFrom, shiftTo, scaleFrom, scaleTo) = onOverview ? (offset, default(Vector), ratio, 1.0) : (default(Vector), offset, 1.0, ratio);
        PanelTitleShift.BeginAnimation(TranslateTransform.XProperty, Header(shiftFrom.X, shiftTo.X, MotionEase));
        PanelTitleShift.BeginAnimation(TranslateTransform.YProperty, Header(shiftFrom.Y, shiftTo.Y, MotionEase));
        PanelTitleScale.BeginAnimation(ScaleTransform.ScaleXProperty, Header(scaleFrom, scaleTo, MotionEase));
        PanelTitleScale.BeginAnimation(ScaleTransform.ScaleYProperty, Header(scaleFrom, scaleTo, MotionEase));

        if (onOverview)
        {
            PanelPageNameShift.BeginAnimation(TranslateTransform.YProperty, Header(0, 40, LeaveEase, 200));
            PanelPageName.BeginAnimation(OpacityProperty, Header(1, 0, LeaveEase, 200));
            PanelBackButton.BeginAnimation(OpacityProperty, Header(1, 0, LeaveEase, 120));
            PanelPageTitle.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 140));
            SidePanelIcon.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 200, delay: 120));
            SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleXProperty, Header(0.5, 1, PopEase, 380, delay: 120));
            SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleYProperty, Header(0.5, 1, PopEase, 380, delay: 120));
            return;
        }

        SidePanelIcon.BeginAnimation(OpacityProperty, Header(1, 0, LeaveEase, 180));
        SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleXProperty, Header(1, 0.5, LeaveEase, 180));
        SidePanelIconScale.BeginAnimation(ScaleTransform.ScaleYProperty, Header(1, 0.5, LeaveEase, 180));
        PanelBackButton.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 160, delay: 220));
        PanelPageNameShift.BeginAnimation(TranslateTransform.YProperty, Header(-40, 0, RollEase, 380, delay: 120));
        PanelPageName.BeginAnimation(OpacityProperty, Header(0, 1, MotionEase, 200, delay: 120));
        // 飞到返回链接处淡出，之后收起并复位，下次返回时从返回链接处飞出。
        var hide = Header(1, 0, MotionEase, 120, delay: 220);
        hide.Completed += (_, _) =>
        {
            if (_page == "Overview") return;
            PanelPageTitle.Visibility = Visibility.Collapsed;
            PanelPageTitle.BeginAnimation(OpacityProperty, null);
            foreach (var property in new[] { TranslateTransform.XProperty, TranslateTransform.YProperty }) PanelTitleShift.BeginAnimation(property, null);
            foreach (var property in new[] { ScaleTransform.ScaleXProperty, ScaleTransform.ScaleYProperty }) PanelTitleScale.BeginAnimation(property, null);
        };
        PanelPageTitle.BeginAnimation(OpacityProperty, hide);
    }

    /// <summary>延迟期间停在起点；用 BeginTime 延迟的动画开始前会显示属性原来的值（即最终状态），出现闪烁。</summary>
    private static AnimationTimeline Header(double from, double to, IEasingFunction ease, double milliseconds = 0, double delay = 0)
    {
        var duration = milliseconds > 0 ? milliseconds : HeaderDuration.TotalMilliseconds;
        return new DoubleAnimationUsingKeyFrames
        {
            KeyFrames =
            {
                new DiscreteDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.Zero)),
                new DiscreteDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(delay))),
                new EasingDoubleKeyFrame(to, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(delay + duration)), ease)
            }
        };
    }

    // ───────────── 左侧状态栏 ─────────────

    // 内容区宽到放得下状态栏和完整页面时显示；两个阈值之间不切换，拖动窗口边缘时不会反复出现、消失。
    private const double SidePanelShowWidth = 1260;
    private const double SidePanelHideWidth = 1160;
    private const double SidePanelWidth = 300;
    private const double PageMargin = 36;
    private static readonly Duration MotionDuration = TimeSpan.FromMilliseconds(340);
    private static readonly IEasingFunction MotionEase = new CubicEase { EasingMode = EasingMode.EaseOut };
    private bool _sidePanelWanted;

    /// <summary>页面内容的页边距，所有页面共用；状态栏出现时左边距加上状态栏的宽度。</summary>
    public static readonly DependencyProperty PageInsetProperty = DependencyProperty.Register(nameof(PageInset), typeof(Thickness), typeof(MainWindow),
        new PropertyMetadata(new Thickness(PageMargin, 26, PageMargin, PageMargin)));

    public Thickness PageInset
    {
        get => (Thickness)GetValue(PageInsetProperty);
        set => SetValue(PageInsetProperty, value);
    }

    // 设置页那张牌比页面内容两侧各宽出 SheetPadding（牌里的留白，见 SheetContentStyle），离顶部 SheetTop，底下留 StackRoom 给牌的边和鼠标移上去时露出的部分。
    private const double SheetPadding = 24, SheetTop = 14, StackRoom = 64;

    private static Thickness SheetInsetFor(Thickness page) => new(page.Left - SheetPadding, SheetTop, page.Right - SheetPadding, StackRoom);

    public static readonly DependencyProperty SheetInsetProperty = DependencyProperty.Register(nameof(SheetInset), typeof(Thickness), typeof(MainWindow),
        new PropertyMetadata(SheetInsetFor(new Thickness(PageMargin, 26, PageMargin, PageMargin))));

    public Thickness SheetInset
    {
        get => (Thickness)GetValue(SheetInsetProperty);
        set => SetValue(SheetInsetProperty, value);
    }

    private FrameworkElement CurrentContent => _pages.TryGetValue(_page, out var view) ? view.Content : PageContent;
    private bool _sidePanelShown;
    private bool _sidePanelPending;
    /// <summary>状态栏出现或收起期间页面还在平移：这时不开始翻牌，否则量到的是过渡中的位置，缓存也会每帧重建。</summary>
    private DoubleAnimation? _sidePanelFade;

    private void OnContentLayerSizeChanged(object sender, SizeChangedEventArgs e)
    {
        CancelFlip();
        var width = e.NewSize.Width;
        if (_sidePanelWanted ? width >= SidePanelHideWidth : width < SidePanelShowWidth) return;
        _sidePanelWanted = !_sidePanelWanted;
        if (!IsLoaded)
        {
            SetSidePanel(animate: false);
            return;
        }
        if (_sidePanelPending) return;
        // 排版过程中不能再排版：等这一帧排完再切换，切换后测量各元素的新位置。
        _sidePanelPending = true;
        Dispatcher.BeginInvoke(() =>
        {
            _sidePanelPending = false;
            SetSidePanel(animate: true);
        });
    }

    /// <summary>
    /// 先记下各元素当前看到的位置，再一次性改成新布局并排版，然后让它们从原位置滑到新位置：
    /// 页面横向让出状态栏，概览标题飞到状态栏图标右侧（或飞回），下面的内容补上标题让出的空间。
    /// 动画只改变位移和透明度，每帧不需要重新排版；状态栏动画期间缓存为位图。
    /// </summary>
    private void SetSidePanel(bool animate)
    {
        var show = _sidePanelWanted;
        if (show == _sidePanelShown) return;
        CancelFlip();
        _sidePanelShown = show;

        var onOverview = _page == "Overview";
        var flyingTitle = show ? PanelPageTitle : OverviewTitle;
        var titleShift = show ? PanelTitleShift : OverviewTitleShift;
        var pageBefore = PositionOf(CurrentContent);
        var bodyBefore = PositionOf(OverviewBody);
        var titleBefore = PositionOf(show ? OverviewTitle : PanelPageTitle);
        var buttonSnapshot = animate && show && onOverview ? Snapshot(OverviewOpenButton) : null;
        var panelWasHidden = SidePanel.Visibility != Visibility.Visible;
        var panelOpacity = panelWasHidden ? 0 : SidePanel.Opacity;
        var panelOffset = panelWasHidden ? -24 : SidePanelShift.X;

        PageInset = new Thickness(show ? PageMargin + SidePanelWidth : PageMargin, 26, PageMargin, PageMargin);
        SheetInset = SheetInsetFor(PageInset);
        OverviewHeader.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        // 设置页的标题和返回按钮移到状态栏顶部。
        foreach (var page in new[] { GeneralPage, ShelfPage, ActionsPage, AppearancePage })
            page.Children[0].Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        PanelPageTitle.Visibility = show && onOverview ? Visibility.Visible : Visibility.Collapsed;
        SidePanel.Visibility = Visibility.Visible;
        SidePanel.IsHitTestVisible = show;
        foreach (var (target, property) in new (Animatable, DependencyProperty)[]
                 {
                     (PageContentShift, TranslateTransform.XProperty), (OverviewBodyShift, TranslateTransform.YProperty),
                     (OverviewTitleShift, TranslateTransform.XProperty), (OverviewTitleShift, TranslateTransform.YProperty),
                     (PanelTitleShift, TranslateTransform.XProperty), (PanelTitleShift, TranslateTransform.YProperty),
                     (SidePanelShift, TranslateTransform.XProperty)
                 })
        {
            target.BeginAnimation(property, null);
            target.SetValue(property, 0.0);
        }
        SidePanel.BeginAnimation(OpacityProperty, null);
        OverviewOpenButton.BeginAnimation(OpacityProperty, null);

        _sidePanelFade = null;
        if (!animate)
        {
            OnSidePanelSettled();
            return;
        }

        ContentLayer.UpdateLayout();
        var pageOffset = pageBefore.X - PositionOf(CurrentContent).X;
        PageContentShift.BeginAnimation(TranslateTransform.XProperty, Motion(pageOffset, 0));
        if (onOverview)
        {
            OverviewBodyShift.BeginAnimation(TranslateTransform.YProperty, Motion(bodyBefore.Y - PositionOf(OverviewBody).Y, 0));
            // 页面内的标题还会随页面一起平移，要扣除这部分。
            var titleAfter = PositionOf(flyingTitle);
            titleShift.BeginAnimation(TranslateTransform.XProperty, Motion(titleBefore.X - titleAfter.X - (show ? 0 : pageOffset), 0));
            titleShift.BeginAnimation(TranslateTransform.YProperty, Motion(titleBefore.Y - titleAfter.Y, 0));
            if (!show) OverviewOpenButton.BeginAnimation(OpacityProperty, Motion(0, 1));
        }
        if (buttonSnapshot is { } snapshot) FadeOutSnapshot(snapshot, -pageOffset);

        SidePanelShift.BeginAnimation(TranslateTransform.XProperty, Motion(panelOffset, show ? 0 : -24));
        SidePanel.CacheMode ??= new BitmapCache(CacheScale.For(SidePanel)) { SnapsToDevicePixels = true };
        var fade = Motion(panelOpacity, show ? 1 : 0);
        fade.Completed += (_, _) =>
        {
            if (ReferenceEquals(_sidePanelFade, fade)) _sidePanelFade = null;
            OnSidePanelSettled();
        };
        _sidePanelFade = fade;
        SidePanel.BeginAnimation(OpacityProperty, fade);
    }

    private System.Windows.Point PositionOf(UIElement element) => element.TranslatePoint(default, ContentLayer);

    private static DoubleAnimation Motion(double from, double to) =>
        new(from, to, MotionDuration) { EasingFunction = MotionEase };

    /// <summary>动画结束后去掉位图缓存；收起的状态栏不再参与排版。</summary>
    private void OnSidePanelSettled()
    {
        if (SidePanel.HasAnimatedProperties && SidePanel.Opacity != (_sidePanelShown ? 1 : 0)) return;
        SidePanel.CacheMode = null;
        if (!_sidePanelShown) SidePanel.Visibility = Visibility.Collapsed;
    }

    /// <summary>概览页右上角的按钮随标题行一起收起；先截一张图留在原处，跟着页面平移并淡出。</summary>
    private (System.Windows.Controls.Image Image, System.Windows.Point Position)? Snapshot(FrameworkElement element)
    {
        if (!element.IsVisible || element.ActualWidth < 1) return null;
        var image = new System.Windows.Controls.Image { Source = VisualCapture.Of(element), Width = element.ActualWidth, Height = element.ActualHeight, RenderTransform = new TranslateTransform() };
        return (image, element.TranslatePoint(default, DragLayer));
    }

    private void FadeOutSnapshot((System.Windows.Controls.Image Image, System.Windows.Point Position) snapshot, double distance)
    {
        var (image, position) = snapshot;
        Canvas.SetLeft(image, position.X);
        Canvas.SetTop(image, position.Y);
        DragLayer.Children.Add(image);
        image.RenderTransform.BeginAnimation(TranslateTransform.XProperty, Motion(0, distance));
        var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(180));
        fade.Completed += (_, _) => DragLayer.Children.Remove(image);
        image.BeginAnimation(OpacityProperty, fade);
    }
}
