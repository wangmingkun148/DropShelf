using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static partial class Program
{
    // ───────────── 界面 ─────────────

    private static void TestShelfWindowActions()
    {
        var controller = new AppController(Dispatcher.CurrentDispatcher);
        var window = new ShelfWindow(new ShelfViewModel(), controller.Shelves) { Left = -20000, Top = -20000 };
        window.Show();
        Pump(150);
        var bar = (System.Windows.Controls.Primitives.UniformGrid)window.FindName("ActionBar")!;
        Assert(bar.Children.Count == (int)controller.Settings.QuickActionLayout, "快速操作按钮数量不正确");
        Assert(window.IsReusable, "空置物架应可复用");
        Assert(window.Title == "DropShelf 置物架 - 置物架", "置物架窗口标题不正确");
        var modeText = (System.Windows.Controls.TextBlock)window.FindName("TransferModeText")!;
        Assert(modeText.Text == "移动", "默认拖出方式应显示为移动");

        LanguageService.SetMode(AppLanguage.English);
        try
        {
            Assert(((System.Windows.Controls.Button)bar.Children[0]).ToolTip is string tip && tip.StartsWith("Compress: "),
                "切换语言后快速操作提示没有更新");
            Assert(window.Title == "DropShelf shelf - Shelf" && modeText.Text == "Move", "切换语言后置物架文字没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }

        controller.Settings.DragOutMode = TransferMode.Copy;
        controller.Settings.QuickActionLayout = QuickActionLayout.Two;
        window.ApplySettings(controller.Settings);
        var tiles = (System.Windows.Controls.Primitives.UniformGrid)window.FindName("ActionTiles")!;
        Assert(modeText.Text == "复制", "拖出方式没有更新");
        Assert(bar.Children.Count == 2 && ((System.Windows.Controls.Button)bar.Children[0]).Content is DockPanel && tiles.Rows == 1,
            "两个按钮时应显示名称并使用更大的拖放区域");
        var twoHeight = window.Height;
        controller.Settings.QuickActionLayout = QuickActionLayout.Four;
        window.ApplySettings(controller.Settings);
        Assert(Math.Abs(window.Height - twoHeight) < 0.5, "两个和四个按钮的置物架高度应相同");
        Assert(tiles.Rows == 1 && tiles.Children.Count == 4, "四个操作时拖放区域应与按钮排成一行");

        controller.Settings.QuickActionLayout = QuickActionLayout.Off;
        window.ApplySettings(controller.Settings);
        Assert(bar.Children.Count == 0 && ((FrameworkElement)window.FindName("ActionBarHost")!).Visibility == Visibility.Collapsed,
            "关闭快速操作后仍显示按钮");

        var folder = CreateFolder("shelf-window");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");
        var toggle = (System.Windows.Controls.Button)window.FindName("ViewToggleButton")!;
        Assert(toggle.Visibility == Visibility.Collapsed, "空置物架不应显示文件堆/列表切换按钮");
        window.ViewModel.AddFiles([file]);
        Pump(100);
        Assert(toggle.Visibility == Visibility.Visible, "只有一项时也应显示文件堆/列表切换按钮");
        window.ViewModel.AddFiles([folder]);
        Pump(100);
        Assert(!window.IsReusable, "有内容的置物架不应被复用");
        Assert(window.ViewModel.IsPinned && window.Topmost, "置物架默认应置顶");
        window.ViewModel.IsPinned = false;
        Assert(!window.Topmost, "取消钉子后置物架不应再置顶");
        window.ViewModel.IsPinned = true;
        toggle.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        // 切换时窗口大小平滑过渡、牌飞到各行，等动画播完再检查。
        Pump(700);
        Assert(window.ViewModel.IsExpanded, "点击切换按钮没有展开列表");
        Assert(window.Width >= QuickShelfSizeOptions.ResolveExpandedWidth(controller.Settings.QuickShelfSize), "展开后宽度没有变化");

        // 展开时继续放入项目：窗口变高要等列表收到新增项目之后，否则布局会抛出异常。
        var twoRowsHeight = window.Height;
        var extra = Path.Combine(folder, "b.txt");
        File.WriteAllText(extra, "y");
        window.ViewModel.AddFiles([extra]);
        Pump(100);
        Assert(((System.Windows.Controls.ListBox)window.FindName("ItemsList")!).Items.Count == 3 && window.Height > twoRowsHeight, "展开时放入项目后列表或高度没有更新");
        window.ViewModel.Remove([window.ViewModel.Items[^1]]);
        Pump(100);
        toggle.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(700);
        Assert(!window.ViewModel.IsExpanded &&
               Math.Abs(window.Width - QuickShelfSizeOptions.ResolveWidth(controller.Settings.QuickShelfSize)) < 0.5 &&
               ((FrameworkElement)window.FindName("StackView")!).Visibility == Visibility.Visible &&
               ((FrameworkElement)window.FindName("ItemsList")!).Visibility == Visibility.Collapsed &&
               ((FrameworkElement)window.FindName("Card0")!).Opacity == 1,
            "收起列表的动画结束后应回到堆叠，窗口恢复收起时的宽度");

        // 压缩生成的压缩包放进旁边新开的置物架，不混进原来的项目。
        controller.Settings.QuickActionLayout = QuickActionLayout.Four;
        window.ApplySettings(controller.Settings);
        var openBefore = controller.Shelves.OpenShelves.Count;
        var compressed = window.ViewModel.RunActionAsync(QuickActions.Get(QuickActionType.CompressZip), [window.ViewModel.Items[0]], IntPtr.Zero, null);
        while (!compressed.IsCompleted) Pump(20);
        Assert(compressed.Result.Succeeded && window.ViewModel.Items.Count == 2, "压缩包不应放进原来的置物架");
        var archiveShelf = controller.Shelves.OpenShelves.Skip(openBefore).Single();
        Assert(archiveShelf.ViewModel.Items.Single().Value.EndsWith(".zip", StringComparison.OrdinalIgnoreCase), "压缩包没有放进新的置物架");
        archiveShelf.Close();
        window.Close();
        Assert(window.ViewModel.Items.Count == 0, "关闭后没有释放项目");

        var latest = controller.History.Latest;
        Assert(latest is not null && latest.Items.Select(item => item.Path).SequenceEqual([file, folder]), "关闭的置物架没有记入最近关闭");
        controller.Shelves.Restore(latest!);
        Pump(150);
        var reopened = controller.Shelves.OpenShelves.Last();
        Assert(reopened.ViewModel.Items.Count == 2 && controller.History.Entries.All(entry => entry != latest), "最近关闭的置物架没有恢复");
        reopened.Close();
        controller.History.Clear();
        File.Delete(new SettingsService().SettingsPath);
    }

    private static void TestMainWindowSettings()
    {
        var service = new SettingsService();
        service.Save(new AppSettings { HasCompletedFirstRun = true });
        var controller = new AppController(Dispatcher.CurrentDispatcher);
        var window = new MainWindow(controller) { Left = -20000, Top = -20000, WindowStartupLocation = WindowStartupLocation.Manual };
        window.Show();
        Pump(200);

        foreach (var name in new[] { "PageCards", "VersionButton", "StatusIndicator", "GettingStartedSection", "HistorySection", "HistoryList",
                     "LanguageSelector", "StartupEnabledBox", "HotkeyEditor", "TriggerSelector", "SensitivitySlider", "EdgeDwellSelector",
                     "ShelfSizeSelector", "DragOutModeSelector", "CloseWhenEmptyBox", "ClearHistoryButton", "ThemeSelector",
                     "AccentSelector", "ActionLayoutSelector", "ActionSlotsList", "MoreActionsList", "AddCustomActionButton", "EditorOverlay" })
            Assert(window.FindName(name) is not null, $"主窗口缺少 {name}");

        var sidePanel = (FrameworkElement)window.FindName("SidePanel")!;
        var overviewHeader = (FrameworkElement)window.FindName("OverviewHeader")!;
        var panelTitle = (FrameworkElement)window.FindName("PanelPageTitle")!;
        Assert(sidePanel.Visibility == Visibility.Collapsed && overviewHeader.Visibility == Visibility.Visible, "窗口较窄时不应显示左侧状态栏");
        window.Width = 1500;
        Pump(700);
        Assert(sidePanel.Visibility == Visibility.Visible && sidePanel.Opacity == 1 && overviewHeader.Visibility == Visibility.Collapsed &&
               panelTitle.Visibility == Visibility.Visible, "窗口足够宽时应显示左侧状态栏，概览标题移到状态栏");
        window.Width = 900;
        Pump(700);
        Assert(sidePanel.Visibility == Visibility.Collapsed && overviewHeader.Visibility == Visibility.Visible &&
               panelTitle.Visibility == Visibility.Collapsed, "窗口变窄后应收起左侧状态栏，标题回到页面");

        var shortCards = window.SettingsCardHeight;
        window.Height = 1100;
        Pump(700);
        Assert(window.SettingsCardHeight > shortCards + 40, "窗口变高后设置卡片应拉长");
        var firstTile = (DropShelf.Views.Controls.PageTile)((System.Windows.Controls.Panel)window.FindName("PageCards")!).Children[0];
        Assert(firstTile.ActualHeight < 144 || firstTile.IsTall, "卡片够高时应切换成大卡片样式");
        window.Height = 640;
        Pump(700);
        Assert(Math.Abs(window.SettingsCardHeight - shortCards) < 1, "窗口变矮后设置卡片应恢复");
        Assert(!firstTile.IsTall || firstTile.ActualHeight >= 132, "窗口变矮后卡片应回到单行样式");

        controller.History.Clear();
        var pageCards = (System.Windows.Controls.Primitives.UniformGrid)window.FindName("PageCards")!;
        Assert(pageCards.Children.Count == 4, "概览页应有四张设置卡片");
        ((System.Windows.Controls.Button)pageCards.Children[1]).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        var shelfHost = (System.Windows.Controls.Grid)window.FindName("ShelfHost")!;
        var shelfScroll = (System.Windows.Controls.ScrollViewer)window.FindName("ShelfScroll")!;
        var overviewHost = (FrameworkElement)window.FindName("OverviewHost")!;
        Pump(900);
        Assert(shelfHost.Visibility == Visibility.Visible && overviewHost.Visibility == Visibility.Collapsed, "点击设置卡片没有切换页面");
        Assert(shelfHost is { Clip: null, IsHitTestVisible: true } && shelfHost.RenderTransform.Value.IsIdentity && shelfHost.Children.Count == 1 &&
               shelfScroll is { CacheMode: null, Opacity: 1 } && overviewHost.Clip is null, "翻牌结束后页面应回到静止状态：没有变换、裁剪、缓存和背面的边框");
        var stackLayer = (System.Windows.Controls.Canvas)window.FindName("StackLayer")!;
        var shelfSheet = (FrameworkElement)window.FindName("ShelfSheet")!;
        var edges = stackLayer.Children.OfType<System.Windows.Controls.Button>().ToList();
        var sheetBottom = shelfSheet.TranslatePoint(new System.Windows.Point(0, shelfSheet.ActualHeight), stackLayer).Y;
        Assert(stackLayer is { Visibility: Visibility.Visible, Clip: null, IsHitTestVisible: true } && edges.Count == 3 &&
               edges.All(edge => edge.Visibility == Visibility.Visible && System.Windows.Controls.Canvas.GetTop(edge) >= sheetBottom - 0.5 && edge.ActualWidth < shelfSheet.ActualWidth),
            "设置页底下应露出其余三张牌的边");
        Assert(pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1), "翻牌结束后概览页的卡片应恢复显示");
        var pageName = (System.Windows.Controls.TextBlock)window.FindName("PanelPageName")!;
        try
        {
            LanguageService.SetMode(AppLanguage.English);
            Pump(50);
            Assert(pageName.Text == ((DropShelf.Views.Controls.PageTile)pageCards.Children[1]).Header && !pageName.Text.Any(c => c > 0x2E80),
                "切换语言后侧栏页面标题没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
            Pump(50);
        }
        var backLayer = (System.Windows.Controls.Canvas)window.FindName("StackBackLayer")!;
        var frontLayer = (System.Windows.Controls.Canvas)window.FindName("StackFrontLayer")!;
        var stage = (FrameworkElement)window.FindName("PageStage")!;
        // 露出的牌边是真按钮；鼠标点击实际命中按钮，键盘也可激活同一个跳转。
        foreach (var edge in edges)
        {
            var point = edge.TranslatePoint(new System.Windows.Point(edge.ActualWidth / 2, edge.ActualHeight / 2), window);
            var hit = window.InputHitTest(point) as DependencyObject;
            while (hit is System.Windows.Media.Visual && hit != edge) hit = System.Windows.Media.VisualTreeHelper.GetParent(hit);
            Assert(hit == edge, "露出的牌边必须能被鼠标命中，不能被设置页的透明区域挡住");
        }
        edges[^1].RaiseEvent(new MouseEventArgs(Mouse.PrimaryDevice, 0) { RoutedEvent = Mouse.MouseEnterEvent });
        Pump(240);
        var lowest = edges.Max(edge => edge.TranslatePoint(new System.Windows.Point(0, edge.ActualHeight), stage).Y);
        Assert(stage.ActualHeight - lowest >= 20, "牌边悬停展开后距离窗口底部仍应至少留 20 DIP");
        var savedOffset = Math.Min(80, shelfScroll.ScrollableHeight);
        shelfScroll.ScrollToVerticalOffset(savedOffset);
        Pump(50);
        foreach (var page in new[] { "General", "Appearance", "Actions", "Shelf" })
        {
            edges.Single(edge => Equals(edge.Tag, page)).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            if (frontLayer.Visibility == Visibility.Visible)
            {
                var image = frontLayer.Children.OfType<System.Windows.Controls.Image>().Single();
                var bitmap = (System.Windows.Media.Imaging.BitmapSource)image.Source;
                var pixels = new byte[bitmap.PixelWidth * bitmap.PixelHeight * 4];
                bitmap.CopyPixels(pixels, bitmap.PixelWidth * 4, 0);
                Assert(Enumerable.Range(0, pixels.Length / 4).Count(i => pixels[i * 4 + 3] > 127) > 50,
                    "抽牌开始前卡面上就必须有设置 UI，不能是一张透明位图");
                Pump(190);
                Assert(frontLayer.Clip is System.Windows.Media.RectangleGeometry geometry && geometry.Rect.Height > 40 &&
                       backLayer.Visibility == Visibility.Visible, "动画中途应已抽出带设置内容的新卡，旧卡同时退回");
                if (page == "General")
                {
                    window.Width += 1;
                    Pump(60);
                    window.Width -= 1;
                }
            }
            Pump(650);
            Assert(((FrameworkElement)window.FindName(page + "Host")!).Visibility == Visibility.Visible &&
                   overviewHost.Visibility == Visibility.Collapsed &&
                   new[] { backLayer, frontLayer }.All(layer => layer.Visibility == Visibility.Collapsed && layer.Children.Count == 0 && layer.Clip is null) &&
                   stackLayer is { Clip: null, IsHitTestVisible: true },
                "抽牌完成或被窗口变化打断后应显示目标页，并释放两张临时卡面");
            Assert(edges.Select(edge => (string)edge.Tag).Distinct().Count() == 3 && edges.All(edge => !Equals(edge.Tag, page)),
                "切换后牌堆应恰好包含另外三页，旧页也能再点回来");
        }
        Assert(Math.Abs(shelfScroll.VerticalOffset - savedOffset) < 1, "设置页之间切换不应重置各页的滚动位置");
        Assert(((FrameworkElement)window.FindName("HistorySection")!).Visibility == Visibility.Collapsed, "没有记录时不应显示最近关闭");

        var viewModel = (SettingsViewModel)window.DataContext;
        ((System.Windows.Controls.Slider)window.FindName("SensitivitySlider")!).Value = 20;
        Assert(viewModel.SensitivityDescription == Strings.SensitivityDetail(5, 63), "拖动滑块时说明没有立即更新");
        Pump(400);
        var labelScales = new[] { "SteadyLabel", "BalancedLabel", "ResponsiveLabel" }
            .Select(name => ((System.Windows.Media.ScaleTransform)((FrameworkElement)window.FindName(name)!).RenderTransform).ScaleX).ToArray();
        Assert(labelScales[0] > labelScales[1] && labelScales[1] > labelScales[2] && Math.Abs(labelScales[2] - 0.85) < 0.001,
            "滑块偏向稳健时，稳健应最大、均衡其次、灵敏保持最小");
        ((System.Windows.Controls.ListBox)window.FindName("TriggerSelector")!).SelectedValue = AutoOpenTrigger.Edge;
        Assert(!viewModel.UsesShake && viewModel.UsesEdge, "只用屏幕边缘时不应显示晃动灵敏度");
        ((System.Windows.Controls.ListBox)window.FindName("EdgeDwellSelector")!).SelectedValue = EdgeDwellTime.Long;
        ((System.Windows.Controls.ListBox)window.FindName("DragOutModeSelector")!).SelectedValue = TransferMode.Copy;
        ((System.Windows.Controls.ListBox)window.FindName("ShelfSizeSelector")!).SelectedValue = QuickShelfSize.Small;
        ((System.Windows.Controls.ListBox)window.FindName("ThemeSelector")!).SelectedValue = AppTheme.Dark;
        ((System.Windows.Controls.ListBox)window.FindName("AccentSelector")!).SelectedValue = "#038387";
        ((System.Windows.Controls.ListBox)window.FindName("ActionLayoutSelector")!).SelectedValue = QuickActionLayout.Two;
        ((System.Windows.Controls.CheckBox)window.FindName("CloseWhenEmptyBox")!).IsChecked = false;
        viewModel.AssignSlot(1, "Reveal");
        Assert(viewModel.ShelfSummary == "屏幕边缘呼出 · 拖出时复制" && viewModel.ActionsSummary == "压缩、显示位置" &&
               viewModel.AppearanceSummary == "深色 · 自选强调色", "设置卡片上的摘要不正确");
        Assert(viewModel.VisibleSlots.Select(slot => slot.Position).SequenceEqual(["左", "右"]), "两个操作时位置应为左、右");
        Assert(viewModel.MoreActions.All(card => viewModel.VisibleSlots.All(slot => slot.Action.Id != card.Action.Id)) &&
               viewModel.MoreActions.Count == QuickActions.Available(controller.Settings.CustomActions).Count - 2, "更多操作应列出其余全部操作");
        viewModel.Hotkey = new HotkeyGesture(ModifierKeys.Control | ModifierKeys.Alt, Key.D);
        viewModel.DismissGettingStartedCommand.Execute(null);
        Pump(50);
        Assert(((FrameworkElement)window.FindName("GettingStartedSection")!).Visibility == Visibility.Collapsed, "快速上手关闭后仍显示");

        var historyFolder = CreateFolder("main-history");
        for (var index = 0; index < 5; index++)
        {
            var path = Path.Combine(historyFolder, $"{index}.txt");
            File.WriteAllText(path, "x");
            controller.History.Record([new ShelfItem { Id = $"h{index}", Kind = ShelfItemKind.File, Value = path }]);
        }
        Pump(50);
        Assert(((FrameworkElement)window.FindName("HistorySection")!).Visibility == Visibility.Visible &&
               viewModel.HistoryEntries.Count == 5, "最近关闭应全部显示在牌堆中");
        Assert(viewModel.HistoryEntries[0].Stack0 is { Glyph.Length: 1 } && viewModel.HistoryEntries[0].Stack1 is null, "历史卡片的缩略图堆叠不正确");
        // 牌堆在概览页上，回到概览页排版后才会排开。
        var shelfPage = (System.Windows.Controls.Panel)window.FindName("ShelfPage")!;
        var backButton = (System.Windows.Controls.Button)((System.Windows.Controls.Panel)shelfPage.Children[0]).Children[0];
        backButton.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(900);
        Assert(((FrameworkElement)window.FindName("OverviewPage")!) is { Opacity: 1, CacheMode: null }, "切换页面的淡入动画结束后页面应完全显示并去掉位图缓存");
        Assert(shelfHost.Visibility == Visibility.Collapsed && shelfHost.Children.Count == 1 && overviewHost.Clip is null &&
               pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1 && tile.RenderTransform.Value.IsIdentity), "返回概览页后卡片应落回原位，设置页收起");
        // 翻牌途中窗口大小变了，算好的位置失效，应直接摆到终点；返回时概览页不能停在透明状态。
        var generalHost = (FrameworkElement)window.FindName("GeneralHost")!;
        ((System.Windows.Controls.Button)pageCards.Children[0]).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(250);
        window.Width += 10;
        Pump(100);
        Assert(generalHost.Visibility == Visibility.Visible && overviewHost.Visibility == Visibility.Collapsed && generalHost.Clip is null &&
               generalHost.RenderTransform.Value.IsIdentity, "打开途中被打断后应直接显示设置页");
        var generalBack = (System.Windows.Controls.Button)((System.Windows.Controls.Panel)((System.Windows.Controls.Panel)window.FindName("GeneralPage")!).Children[0]).Children[0];
        generalBack.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
        Pump(30);
        window.Width -= 10;
        Pump(100);
        Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed && overviewHost.Clip is null &&
               ((FrameworkElement)window.FindName("OverviewPage")!) is { Opacity: 1, CacheMode: null } &&
               pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1 && tile.RenderTransform.Value.IsIdentity), "返回途中被打断后概览页应完全显示，卡片回到原位");
        Pump(400);
        // 设置页里按 Esc 或鼠标后退键也能回到概览。
        foreach (var (name, goBack) in new (string, Action)[]
                 {
                     ("Esc", () => window.RaiseEvent(new System.Windows.Input.KeyEventArgs(Keyboard.PrimaryDevice, PresentationSource.FromVisual(window)!, 0, Key.Escape) { RoutedEvent = Keyboard.KeyDownEvent })),
                     ("鼠标后退键", () => window.RaiseEvent(new MouseButtonEventArgs(Mouse.PrimaryDevice, 0, MouseButton.XButton1) { RoutedEvent = Mouse.MouseDownEvent })),
                 })
        {
            ((System.Windows.Controls.Button)pageCards.Children[0]).RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Pump(900);
            goBack();
            Pump(900);
            Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed, $"设置页里按{name}应回到概览");
        }
        // 拖动翻牌：转过一半前只转动卡片，过了侧立换成页面，往回拖又换回卡片；松手时没过侧立就退回，过了或甩得够快就翻过去。
        // 软件渲染时背面空白，拖动直接播完一次翻牌。
        var generalTile = (DropShelf.Views.Controls.PageTile)pageCards.Children[0];
        var overviewPage = (FrameworkElement)window.FindName("OverviewPage")!;
        var generalScroll = (System.Windows.Controls.ScrollViewer)window.FindName("GeneralScroll")!;
        if (window.BeginFlipDrag(generalTile))
        {
            void AssertCardFacesVisible(string when)
            {
                stackLayer.UpdateLayout();
                var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap((int)Math.Ceiling(stackLayer.ActualWidth),
                    (int)Math.Ceiling(stackLayer.ActualHeight), 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
                bitmap.Render(stackLayer);
                foreach (var edge in edges)
                {
                    var point = edge.TranslatePoint(new System.Windows.Point(edge.ActualWidth / 2, edge.ActualHeight / 2), stackLayer);
                    var pixel = new byte[4];
                    bitmap.CopyPixels(new Int32Rect((int)point.X, (int)point.Y, 1, 1), pixel, 4, 0);
                    Assert(pixel[3] > 0, $"{when}其余卡牌的卡面必须连续可见，不能只在结束时恢复真实按钮");
                }
            }
            AssertCardFacesVisible("打开的第一帧");
            var tileFace = (UIElement)stackLayer.Children[^1];
            window.DragFlip(0.3);
            Pump(50);
            Assert(generalTile.Opacity == 0 && tileFace.Visibility == Visibility.Visible && !tileFace.RenderTransform.Value.IsIdentity &&
                   generalHost.Visibility == Visibility.Hidden, "拖到侧立前应只转动卡片的截图");
            window.DragFlip(0.7);
            Pump(50);
            Assert(tileFace.Visibility == Visibility.Hidden && generalHost.Visibility == Visibility.Visible && overviewHost.Clip is not null && stackLayer.Clip is not null,
                "拖过侧立后应换成页面");
            window.DragFlip(0.2);
            Pump(50);
            Assert(tileFace.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Hidden && overviewHost.Clip is null, "往回拖过侧立后应换回卡片");
            window.ReleaseFlip(0);
            Pump(700);
            Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed && overviewPage is { Opacity: 1, CacheMode: null } &&
                   generalTile is { Opacity: 1 } && generalTile.RenderTransform.Value.IsIdentity, "没拖过侧立就松手应退回概览页");
            Assert(window.BeginFlipDrag(generalTile), "退回后应能再次拖动");
            window.DragFlip(0.6);
            window.ReleaseFlip(0);
            Pump(700);
            Assert(generalHost is { Visibility: Visibility.Visible, Clip: null, IsHitTestVisible: true } && generalHost.RenderTransform.Value.IsIdentity &&
                   generalScroll is { CacheMode: null, Opacity: 1 } && overviewHost.Visibility == Visibility.Collapsed, "拖过侧立后松手应翻开页面");
            Assert(window.BeginFlipDrag(null), "设置页应能拖回概览页");
            window.DragFlip(0.3);
            window.ReleaseFlip(0);
            Pump(700);
            Assert(generalHost is { Visibility: Visibility.Visible, Clip: null } && overviewHost.Visibility == Visibility.Collapsed, "返回时没拖过侧立就松手应留在设置页");
            Assert(window.BeginFlipDrag(null), "退回后应能再次拖回概览页");
            window.DragFlip(1);
            Pump(50);
            AssertCardFacesVisible("返回即将落位时");
            window.DragFlip(0.2);
            window.ReleaseFlip(0.01);
            Pump(700);
        }
        else
        {
            Pump(900);
            generalBack.RaiseEvent(new RoutedEventArgs(System.Windows.Controls.Primitives.ButtonBase.ClickEvent));
            Pump(900);
        }
        Assert(overviewHost.Visibility == Visibility.Visible && generalHost.Visibility == Visibility.Collapsed && overviewPage is { Opacity: 1, CacheMode: null } &&
               pageCards.Children.OfType<UIElement>().All(tile => tile.Opacity == 1 && tile.RenderTransform.Value.IsIdentity) &&
               stackLayer.Visibility == Visibility.Collapsed, "甩动应翻回概览页，卡片落回原位，牌的边收起");
        var historyList = (System.Windows.Controls.ItemsControl)window.FindName("HistoryList")!;
        void AssertFanArranged(string when)
        {
            var cards = Enumerable.Range(0, 5).Select(index => (UIElement)historyList.ItemContainerGenerator.ContainerFromIndex(index)).ToArray();
            Assert(cards.All(card => card.RenderTransform is System.Windows.Media.TransformGroup) &&
                   System.Windows.Controls.Panel.GetZIndex(cards[2]) > System.Windows.Controls.Panel.GetZIndex(cards[0]) &&
                   cards.All(card => !DropShelf.Views.Controls.FanPanel.GetIsHot(card)), $"{when}牌堆应以中间的牌在最上面排开，默认不选出任何一张");
        }
        AssertFanArranged("");
        // 调整顺序时卡片沿用原来的，只换位置。
        var firstEntry = viewModel.HistoryEntries[0];
        var firstCard = historyList.ItemContainerGenerator.ContainerFromItem(firstEntry);
        controller.History.Move(firstEntry.Entry, 3);
        Pump(50);
        Assert(viewModel.HistoryEntries[3] == firstEntry && historyList.ItemContainerGenerator.ContainerFromItem(firstEntry) == firstCard,
            "调整顺序后卡片应沿用原来的，不重新生成");
        AssertFanArranged("调整顺序后");
        controller.History.Move(firstEntry.Entry, 0);
        Pump(50);
        // 切换语言会重新生成同样数量的牌，也要重新排开。
        viewModel.Language = AppLanguage.English;
        Pump(50);
        viewModel.Language = AppLanguage.Chinese;
        Pump(50);
        AssertFanArranged("切换语言后");
        var pageScroll = (System.Windows.Controls.ScrollViewer)window.FindName("OverviewScroll")!;
        foreach (var height in new[] { 640.0, 1100.0 })
        {
            window.Height = height;
            Pump(700);
            var fanBottom = historyList.TranslatePoint(new Point(0, historyList.ActualHeight), pageScroll).Y;
            Assert(Math.Abs(fanBottom - pageScroll.ViewportHeight) < 1, $"窗口高 {height} 时牌堆应贴着窗口底边");
        }
        Assert(window.SettingsCardHeight > shortCards + 40, "有牌堆时窗口变高后设置卡片也应拉长");
        window.Height = 640;
        controller.History.Clear();

        viewModel.BeginNewCustomAction();
        Assert(viewModel.IsEditorOpen && ((FrameworkElement)window.FindName("EditorOverlay")!).Visibility == Visibility.Visible, "自定义操作对话框没有打开");
        var editor = viewModel.Editor!;
        Assert(!editor.TrySave() && editor.Error is not null, "没有选择文件夹时不应保存");
        editor.Kind = CustomActionKind.CompressImage;
        editor.SizeText = "abc";
        Assert(!editor.TrySave(), "无效大小不应保存");
        editor.SizeText = "200";
        Assert(editor.NamePlaceholder == "≤ 200 KB" && editor.TrySave() && !viewModel.IsEditorOpen, "自定义操作没有保存");

        var saved = service.Load();
        Assert(saved.ShakeSensitivity == 20, "灵敏度没有在停下后保存");
        Assert(saved.AutoOpenTrigger == AutoOpenTrigger.Edge && saved.EdgeDwell == EdgeDwellTime.Long, "呼出方式没有保存");
        Assert(saved.DragOutMode == TransferMode.Copy, "拖出方式没有保存");
        Assert(saved.QuickShelfSize == QuickShelfSize.Small, "尺寸没有立即保存");
        Assert(saved.Theme == AppTheme.Dark && ThemeService.IsDark, "主题没有保存或生效");
        Assert(!saved.CloseWhenEmpty && !saved.ShowGettingStarted, "开关设置没有保存");
        Assert(saved.AccentColor == "#038387" && ThemeService.Accent.Base == ThemeService.ParseAccent("#038387"), "强调色没有保存或生效");
        Assert(saved.QuickActionLayout == QuickActionLayout.Two && viewModel.VisibleSlots.Count == 2, "按钮数量没有保存");
        var created = saved.CustomActions!.Single(action => action.ImageSizeKb == 200);
        Assert(viewModel.MoreActions.Any(card => card.Action.Id == created.Id), "新建的操作应出现在更多操作中");
        viewModel.AssignSlot(0, created.Id);
        Assert(service.Load().QuickActionSlots![0] == created.Id && service.Load().QuickActionSlots![1] == "Reveal", "拖到位置上的操作没有保存");
        Assert(saved.Hotkey == "Ctrl+Alt+D", "快捷键没有保存");

        viewModel.EditCustomAction(created);
        viewModel.Editor!.Name = "压缩证件照";
        viewModel.Editor.TrySave();
        Assert(service.Load().CustomActions!.Single(action => action.Id == created.Id).Name == "压缩证件照", "编辑自定义操作没有保存");
        viewModel.DeleteCustomAction(created);
        Assert(service.Load().CustomActions!.All(action => action.Id != created.Id) && !service.Load().QuickActionSlots!.Contains(created.Id),
            "删除的自定义操作仍在置物架上");

        Assert(generalTile.Header == "常规", "设置卡片文字不正确");
        try
        {
            ((System.Windows.Controls.ListBox)window.FindName("LanguageSelector")!).SelectedValue = AppLanguage.English;
            Pump(50);
            Assert(service.Load().Language == AppLanguage.English && !LanguageService.IsChinese, "语言设置没有保存或生效");
            Assert(generalTile.Header == "General" && viewModel.GeneralSummary.StartsWith("English"), "切换语言后主窗口文字没有更新");
            viewModel.Language = AppLanguage.Chinese;
            Pump(50);
            Assert(generalTile.Header == "常规" && LanguageService.IsChinese, "切换回中文后主窗口文字没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }

        ThemeService.SetMode(AppTheme.Light);
        window.Close();
        File.Delete(service.SettingsPath);
    }

    private static void TestOnboardingSteps()
    {
        var window = new OnboardingWindow { Left = -20000, Top = -20000, WindowStartupLocation = WindowStartupLocation.Manual };
        window.Show();
        Pump(150);
        var next = (System.Windows.Controls.Button)window.FindName("NextButton")!;
        Assert(window.CurrentStep == 0 && (string)next.Content == "下一步", "教程起始状态不正确");
        for (var step = 1; step < 4; step++) window.ShowStep(step);
        Assert(window.CurrentStep == 3 && (string)next.Content == "开始使用", "教程最后一步不正确");
        var hotkey = AppController.Current?.Settings.HotkeyGesture ?? HotkeyGesture.Default;
        Assert(((System.Windows.Controls.TextBlock)window.FindName("StepBodyText")!).Text.Contains(hotkey.ToString()), "教程没有说明当前的快捷键");
        LanguageService.SetMode(AppLanguage.English);
        try
        {
            Assert((string)next.Content == "Get started" &&
                   ((System.Windows.Controls.TextBlock)window.FindName("StepTitleText")!).Text == "Quick actions and shortcut",
                "切换语言后教程文字没有更新");
        }
        finally
        {
            LanguageService.SetMode(AppLanguage.Chinese);
        }
        window.ShowStep(99);
        Assert(window.CurrentStep == 3, "越界步骤没有限制");
        window.Close();
    }

    private static void TestProbeWindow()
    {
        var probe = new ProbeWindow();
        probe.ShowAt(new POINT(-10000, -10000));
        Pump(100);
        Assert(probe.IsVisible, "探针窗口没有显示");
        probe.HideProbe();
        Assert(!probe.IsVisible, "探针窗口没有隐藏");
        probe.Close();
    }
}
