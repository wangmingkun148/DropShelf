using System.Diagnostics;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow
{
    // ───────────── 牌堆的边 ─────────────

    // 设置页底下按概览页的顺序露出其余几张牌的边：每条高 EdgeBand，比上面一条两侧各缩进 EdgeInset；
    // 鼠标移上去时这一条再往下露出 EdgeReveal，显示是哪一页，后面的边跟着下移。
    private const double EdgeBand = 6, EdgeInset = 16, EdgeReveal = 20, EdgeRadius = 6;
    /// <summary>概览页卡片的圆角，见 PageTile 的模板。</summary>
    private const double TileRadius = 8;
    /// <summary>收拢时近的边先走，相邻两条错开这么久（毫秒）。</summary>
    private const double EdgeStagger = 30;
    /// <summary>牌面上的字在走完这一段比例前淡出，之后只剩牌的轮廓，拉宽、压扁时不会把字拉变形。</summary>
    private const double EdgeFaceFade = 0.45;

    private sealed record StackEdge(Button Root, Border Frame, Border Face, TextBlock Icon, TextBlock Header,
        FrameworkElement Label, TranslateTransform Shift);

    private readonly StackEdge[] _edges;
    /// <summary>独立绘制模板里的卡面，按钮本身隐藏也不会把卡面一起隐藏。</summary>
    private readonly Border _tileFace = new() { Visibility = Visibility.Hidden, IsHitTestVisible = false };
    /// <summary>上次摆放牌的边时页面那张牌的位置。</summary>
    private Rect _edgeSheet = Rect.Empty;

    private StackEdge CreateEdge(int index)
    {
        var frame = new Border { BorderThickness = new Thickness(1, 0, 1, 1) };
        frame.SetResourceReference(Border.BackgroundProperty, "StackEdgeBrush");
        frame.SetResourceReference(Border.BorderBrushProperty, "SheetBorderBrush");
        var face = new Border();
        var icon = new TextBlock { FontSize = 12, Margin = new Thickness(0, 1, 8, 0), VerticalAlignment = VerticalAlignment.Center };
        icon.SetResourceReference(TextBlock.FontFamilyProperty, "IconFont");
        var header = new TextBlock { FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
        foreach (var text in new[] { icon, header }) text.SetResourceReference(TextBlock.ForegroundProperty, "TextSecondaryBrush");
        var label = new StackPanel
        {
            Orientation = System.Windows.Controls.Orientation.Horizontal, Height = EdgeReveal, Margin = new Thickness(14, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Bottom, Opacity = 0, Children = { icon, header }
        };
        var shift = new TranslateTransform();
        var root = new Button
        {
            Style = (Style)FindResource("StackEdgeButtonStyle"), ClipToBounds = true, RenderTransform = shift,
            Content = new Grid { Children = { frame, face, label } }
        };
        root.MouseEnter += (_, _) => RevealEdge(index);
        root.MouseLeave += (_, _) => RevealEdge(-1);
        root.GotKeyboardFocus += (_, _) => RevealEdge(index);
        root.LostKeyboardFocus += (_, _) => RevealEdge(-1);
        root.Click += (_, _) => SwitchStackPage(index);
        return new StackEdge(root, frame, face, icon, header, label, shift);
    }

    /// <summary>当前页之外的卡片：按概览页的顺序从当前页往后数，数到头再从第一张接着数，近的是紧挨着的下一页。</summary>
    private Controls.PageTile[] OthersOf(string page)
    {
        var tiles = PageCards.Children.OfType<Controls.PageTile>().ToArray();
        var index = Array.FindIndex(tiles, tile => Equals(tile.Tag, page));
        return [.. Enumerable.Range(1, tiles.Length - 1).Select(offset => tiles[(index + offset) % tiles.Length])];
    }

    private static void AssignEdge(StackEdge edge, Controls.PageTile tile)
    {
        edge.Root.Tag = tile.Tag;
        edge.Root.SetBinding(System.Windows.Automation.AutomationProperties.NameProperty,
            new System.Windows.Data.Binding(nameof(Controls.PageTile.Header)) { Source = tile });
        edge.Icon.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(Controls.PageTile.Icon)) { Source = tile });
        edge.Header.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(Controls.PageTile.Header)) { Source = tile });
    }

    /// <summary>
    /// 模板里的卡面独立画在概览页之外，再隐藏按钮。画刷不包含按钮及其祖先的透明度，
    /// 也不用连续同步生成四张截图；返回时同一张卡面一直画到落位，再交还给按钮。
    /// </summary>
    private void AttachTiles(Flip flip)
    {
        var captured = Stopwatch.GetTimestamp();
        ResetEdgeReveal();
        StackLayer.BeginAnimation(OpacityProperty, null);
        StackLayer.Opacity = 1;
        StackLayer.IsHitTestVisible = false;
        StackLayer.Visibility = Visibility.Visible;
        _tileFace.Background = TileFace(flip.Tile);
        Canvas.SetLeft(_tileFace, flip.Home.X);
        Canvas.SetTop(_tileFace, flip.Home.Y);
        _tileFace.Width = flip.Home.Width;
        _tileFace.Height = flip.Home.Height;
        flip.Tile.Opacity = 0;
        for (var k = 0; k < _edges.Length; k++)
        {
            var edge = _edges[k];
            AssignEdge(edge, flip.Others[k]);
            Panel.SetZIndex(edge.Root, _edges.Length - k);
            // 翻牌途中边的大小每帧都变，页名收起来，不用每帧重新排字。
            edge.Label.Visibility = Visibility.Collapsed;
            edge.Root.Visibility = flip.Plain ? Visibility.Hidden : Visibility.Visible;
            if (flip.Plain) continue;
            edge.Face.Background = TileFace(flip.Others[k]);
            flip.Others[k].Opacity = 0;
        }
        TraceMark("faces", captured);
    }

    private static VisualBrush TileFace(Controls.PageTile tile)
    {
        tile.ApplyTemplate();
        // 画模板的根，而不是马上会被设为透明的 Button。固定 Viewbox，变宽、变矮时整张卡面一起变化。
        var face = (FrameworkElement)VisualTreeHelper.GetChild(tile, 0);
        return new VisualBrush(face)
        {
            Stretch = Stretch.Fill, ViewboxUnits = BrushMappingMode.Absolute,
            Viewbox = new Rect(face.RenderSize), AutoLayoutContent = false
        };
    }

    /// <summary>
    /// 打开时其余卡片在换面后依次离开原位，滑进页面那张牌底下排成牌的边，近的先到，最远的和页面同时落定；
    /// 返回时一开始就从牌底下抽出来，飞回原位，和卡片同时落位，落位时一样略微回弹。被背面挡住的部分由挖空的形状裁掉。
    /// </summary>
    private void PoseEdges(Flip flip)
    {
        var start = flip.Opening ? OpenFrontMs : 0;
        var span = flip.Total - start - EdgeStagger * (_edges.Length - 1);
        for (var k = 0; k < _edges.Length; k++)
        {
            var u = Math.Clamp((flip.Progress * flip.Total - start - EdgeStagger * k) / span, 0, 1);
            PlaceEdge(_edges[k], flip.OtherHomes[k], BandOf(flip.Area, k), flip.Opening ? OpenTravel.Ease(u) : 1 - BackTravel.Ease(u));
        }
    }

    /// <summary>在卡片原位 home 和露出的边 band 之间按 travel 摆放一条边：圆角由卡片的四个角变成只有下面两个，卡面淡出，外框淡入。</summary>
    private static void PlaceEdge(StackEdge edge, Rect home, Rect band, double travel)
    {
        var rect = Lerp(home, band, travel);
        Canvas.SetLeft(edge.Root, rect.X);
        Canvas.SetTop(edge.Root, rect.Y);
        edge.Root.Width = Math.Max(0, rect.Width);
        edge.Root.Height = Math.Max(0, rect.Height);
        var t = Math.Clamp(travel, 0, 1);
        var top = TileRadius * (1 - t);
        var bottom = TileRadius + (EdgeRadius - TileRadius) * t;
        edge.Frame.CornerRadius = new CornerRadius(top, top, bottom, bottom);
        var frame = Smooth(t / EdgeFaceFade);
        edge.Frame.Opacity = frame;
        edge.Face.Opacity = 1 - frame;
    }

    /// <summary>第 k 条边（0 最近）静止时露在页面那张牌 sheet 底下的位置。</summary>
    private static Rect BandOf(Rect sheet, int k)
    {
        var inset = EdgeInset * (k + 1);
        return new Rect(sheet.Left + inset, sheet.Bottom + EdgeBand * k, Math.Max(0, sheet.Width - 2 * inset), EdgeBand);
    }

    /// <summary>翻牌结束：概览页上的卡片恢复显示，解除临时卡面对模板的引用。</summary>
    private void SettleTiles()
    {
        foreach (var tile in PageCards.Children.OfType<UIElement>()) tile.Opacity = 1;
        _tileFace.Background = null;
        _tileFace.Visibility = Visibility.Hidden;
        _tileMatrix.Matrix = Matrix.Identity;
        foreach (var edge in _edges) edge.Face.Background = null;
    }

    /// <summary>设置页静止时露出其余几张牌的边，回到概览页时收起。</summary>
    private void ShowEdges()
    {
        StackLayer.BeginAnimation(OpacityProperty, null);
        StackLayer.Opacity = 1;
        if (_page == "Overview")
        {
            StackLayer.Visibility = Visibility.Collapsed;
            return;
        }
        var others = OthersOf(_page);
        for (var k = 0; k < _edges.Length; k++)
        {
            AssignEdge(_edges[k], others[k]);
            Panel.SetZIndex(_edges[k].Root, _edges.Length - k);
            _edges[k].Label.Visibility = Visibility.Visible;
            _edges[k].Root.Visibility = Visibility.Visible;
        }
        StackLayer.IsHitTestVisible = true;
        StackLayer.Visibility = Visibility.Visible;
        _edgeSheet = Rect.Empty;
        LayoutEdges();
    }

    /// <summary>按页面那张牌现在的位置摆好牌的边，牌没动时什么也不做。</summary>
    private void LayoutEdges()
    {
        if (StackLayer.Visibility != Visibility.Visible || !_pages.TryGetValue(_page, out var view)) return;
        var sheet = PageArea(view);
        if (sheet == _edgeSheet) return;
        _edgeSheet = sheet;
        ResetEdgeReveal();
        for (var k = 0; k < _edges.Length; k++) PlaceEdge(_edges[k], default, BandOf(sheet, k), 1);
    }

    /// <summary>鼠标移到第 index 条边上（-1 为移开）：这一条往下多露出一截，显示是哪一页；后面的边跟着下移，仍然露在它下面。</summary>
    private void RevealEdge(int index)
    {
        if (_morphing || _page == "Overview") return;
        for (var k = 0; k < _edges.Length; k++)
        {
            var edge = _edges[k];
            edge.Root.BeginAnimation(HeightProperty, Reveal(k == index ? EdgeBand + EdgeReveal : EdgeBand));
            edge.Shift.BeginAnimation(TranslateTransform.YProperty, Reveal(index >= 0 && k > index ? EdgeReveal : 0));
            edge.Label.BeginAnimation(OpacityProperty, Reveal(k == index ? 1 : 0));
        }
    }

    private static DoubleAnimation Reveal(double to) => new(to, TimeSpan.FromMilliseconds(180)) { EasingFunction = MotionEase };

    private void ResetEdgeReveal()
    {
        foreach (var edge in _edges)
        {
            edge.Root.BeginAnimation(HeightProperty, null);
            edge.Shift.BeginAnimation(TranslateTransform.YProperty, null);
            edge.Shift.Y = 0;
            edge.Label.BeginAnimation(OpacityProperty, null);
            edge.Label.Opacity = 0;
        }
    }
}
