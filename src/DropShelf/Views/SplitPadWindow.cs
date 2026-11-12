using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Shell;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using WpfDragDropEffects = System.Windows.DragDropEffects;
using WpfDragEventArgs = System.Windows.DragEventArgs;

namespace DropShelf.Views;

/// <summary>
/// 拆分用的“新置物架”：从列表里只拖出一部分项目时，它贴着来源置物架的下边伸出来（下面放不下就在上边），
/// 把项目放上去就在这里拆出一个新的置物架。拖动结束就收起。全程只有一个，轮流给各个置物架用。
/// </summary>
internal sealed class SplitPadWindow : Window
{
    private const double PadHeight = 48;
    private const double Gap = 6;
    private static readonly TimeSpan ShowDuration = TimeSpan.FromMilliseconds(160);
    private static readonly TimeSpan HideDuration = TimeSpan.FromMilliseconds(110);
    private static SplitPadWindow? _shared;

    private readonly Border _root = new();
    private readonly Grid _surface = new();
    private readonly TranslateTransform _offset = new();
    private readonly Rectangle _frame = new() { RadiusX = 8, RadiusY = 8, StrokeThickness = 1.4, StrokeDashArray = [4, 3] };
    private readonly Border _fill = new() { CornerRadius = new CornerRadius(8), Opacity = 0 };
    private readonly TextBlock _glyph = new() { Text = "", FontSize = 13, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 1, 7, 0) };
    private readonly TextBlock _label = new() { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
    private readonly DropTargetHelper _dropHelper = new();
    private bool _hot;
    private bool _shown;
    private bool _below = true;

    private SplitPadWindow()
    {
        Title = "DropShelf";
        SetResourceReference(StyleProperty, "AppWindowStyle");
        WindowStyle = WindowStyle.SingleBorderWindow;
        ResizeMode = ResizeMode.NoResize;
        ShowInTaskbar = false;
        ShowActivated = false;
        Topmost = true;
        Focusable = false;
        AllowDrop = true;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = Top = -32000;
        Height = PadHeight;
        Background = Brushes.Transparent;
        WindowChrome.SetWindowChrome(this, new WindowChrome
        {
            CaptionHeight = 0, GlassFrameThickness = new Thickness(-1), ResizeBorderThickness = default, CornerRadius = default,
            UseAeroCaptionButtons = false
        });

        _frame.SetResourceReference(Shape.StrokeProperty, "AccentBrush");
        _frame.SetResourceReference(Shape.FillProperty, "AccentFaintBrush");
        _fill.SetResourceReference(Border.BackgroundProperty, "AccentSubtleBrush");
        _glyph.SetResourceReference(TextBlock.FontFamilyProperty, "IconFont");
        _glyph.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextBrush");
        _label.SetResourceReference(TextBlock.ForegroundProperty, "AccentTextBrush");
        _surface.Margin = new Thickness(6);
        _surface.RenderTransform = _offset;
        _surface.Children.Add(_frame);
        _surface.Children.Add(_fill);
        _surface.Children.Add(new StackPanel
        {
            Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            Children = { _glyph, _label }
        });
        _root.Child = _surface;
        Content = _root;

        SourceInitialized += (_, _) =>
        {
            WindowEffects.SetToolWindow(this, noActivate: true);
            WindowEffects.SetRoundedCorners(this);
            ApplyMaterial();
        };
        DragEnter += (_, e) =>
        {
            _dropHelper.Enter(this, e.Data, e.AllowedEffects & WpfDragDropEffects.Copy);
            UpdateDrag(e);
        };
        DragOver += (_, e) => UpdateDrag(e);
        DragLeave += (_, e) =>
        {
            e.Handled = true;
            _dropHelper.Leave();
            SetHot(false);
        };
        Drop += (_, e) =>
        {
            e.Handled = true;
            var session = ShelfDragSession.From(e.Data);
            e.Effects = session is null ? WpfDragDropEffects.None : WpfDragDropEffects.Copy;
            _dropHelper.Drop(e.Data, e.Effects);
            if (session is not null) session.Split = true;
        };
    }

    /// <summary>空闲时先把窗口建好，第一次拖出时不用现建。</summary>
    public static void Warm()
    {
        try
        {
            _shared ??= new SplitPadWindow();
            WindowEffects.Handle(_shared);
        }
        catch (Exception exception)
        {
            LogService.Error("准备拆分用的窗口失败", exception);
        }
    }

    /// <summary>程序退出时关掉。</summary>
    public static void CloseShared()
    {
        _shared?.Close();
        _shared = null;
    }

    /// <summary>
    /// 新置物架会出现在哪里（屏幕像素）：贴着来源置物架的下边、左边对齐、宽度是收起时的宽度；下面放不下时放到上边。
    /// below 表示在下边。来源窗口还没有句柄时返回 null。
    /// </summary>
    public static (RECT Rect, bool Below)? Place(ShelfWindow source, double collapsedWidth, double collapsedHeight)
    {
        var handle = new WindowInteropHelper(source).Handle;
        if (handle == IntPtr.Zero || !NativeMethods.GetWindowRect(handle, out var rect)) return null;
        var (work, scale) = WindowPlacement.GetMonitor(new POINT(rect.Left + rect.Width / 2, rect.Top + rect.Height / 2));
        var (width, height, gap) = ((int)Math.Round(collapsedWidth * scale), (int)Math.Round(PadHeight * scale), (int)Math.Round(Gap * scale));
        // 按新置物架的高度看下面够不够：拆出来的置物架要完整地落在屏幕里，不能盖住来源。
        var below = rect.Bottom + gap + (int)Math.Round(collapsedHeight * scale) <= work.Bottom || rect.Top - gap - (int)Math.Round(collapsedHeight * scale) < work.Top;
        var left = Math.Clamp(rect.Left, work.Left, Math.Max(work.Left, work.Right - width));
        var top = below ? rect.Bottom + gap : rect.Top - gap - height;
        return (new RECT { Left = left, Top = top, Right = left + width, Bottom = top + height }, below);
    }

    /// <summary>在来源置物架旁边显示；出错时不显示，拖出照常进行。</summary>
    public static void ShowFor(ShelfWindow source, double collapsedWidth, double collapsedHeight)
    {
        try
        {
            if (Place(source, collapsedWidth, collapsedHeight) is not { } place) return;
            _shared ??= new SplitPadWindow();
            _shared.Appear(place.Rect, place.Below, collapsedWidth);
        }
        catch (Exception exception)
        {
            LogService.Error("显示拆分用的窗口失败", exception);
        }
    }

    /// <summary>收起；返回它刚才所在的位置（没在显示时返回 null），拆出的新置物架就从这里展开。</summary>
    public static (RECT Rect, bool Below)? Dismiss()
    {
        if (_shared is not { _shown: true } pad) return null;
        var handle = new WindowInteropHelper(pad).Handle;
        (RECT, bool)? place = NativeMethods.GetWindowRect(handle, out var rect) ? (rect, pad._below) : null;
        pad.Disappear();
        return place;
    }

    private void Appear(RECT rect, bool below, double width)
    {
        _shown = true;
        _below = below;
        _label.Text = Strings.SplitPadLabel;
        Width = width;
        ApplyMaterial();
        SetHot(false, animate: false);
        var handle = WindowEffects.Handle(this);
        Position(handle, rect);
        Show();
        // 跨显示器时系统会按新的缩放调整大小，显示后再摆一次。
        Position(handle, rect);

        // 从来源置物架那一侧滑出来。
        var from = below ? -8 : 8;
        _surface.BeginAnimation(OpacityProperty, ShelfMotion.Gate(new DoubleAnimation(0, 1, ShowDuration)));
        _offset.BeginAnimation(TranslateTransform.YProperty,
            ShelfMotion.Gate(new DoubleAnimation(from, 0, ShowDuration) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } }));
    }

    private void Disappear()
    {
        _shown = false;
        _dropHelper.Leave();
        var fade = ShelfMotion.Gate(new DoubleAnimation(0, HideDuration));
        fade.Completed += (_, _) =>
        {
            if (!_shown) Hide();
        };
        _surface.BeginAnimation(OpacityProperty, fade);
    }

    private static void Position(IntPtr handle, RECT rect) =>
        NativeMethods.SetWindowPos(handle, IntPtr.Zero, rect.Left, rect.Top, rect.Width, rect.Height,
            NativeMethods.SWP_NOZORDER | NativeMethods.SWP_NOACTIVATE);

    /// <summary>和置物架一样的不透明底色，深浅跟着主题。</summary>
    private void ApplyMaterial()
    {
        WindowEffects.SetDarkMode(this, ThemeService.IsDark);
        _root.SetResourceReference(Border.BackgroundProperty, "ShelfSolidBrush");
    }

    private void UpdateDrag(WpfDragEventArgs e)
    {
        e.Handled = true;
        var valid = _shown && ShelfDragSession.From(e.Data) is not null;
        e.Effects = valid ? WpfDragDropEffects.Copy : WpfDragDropEffects.None;
        if (valid) DropTargetHelper.SetDescription(e.Data, DropImageType.Label, Strings.SplitPadLabel);
        _dropHelper.Over(e.Effects);
        SetHot(valid);
    }

    /// <summary>拖到上面时铺一层强调色。</summary>
    private void SetHot(bool hot, bool animate = true)
    {
        if (hot == _hot && animate) return;
        _hot = hot;
        _fill.BeginAnimation(OpacityProperty, animate && ShelfMotion.Enabled ? new DoubleAnimation(hot ? 1 : 0, TimeSpan.FromMilliseconds(120)) : null);
        if (!animate || !ShelfMotion.Enabled) _fill.Opacity = hot ? 1 : 0;
    }
}
