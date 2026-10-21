using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

public partial class MainWindow : Window
{
    private readonly AppController _controller;
    private bool _closingForShutdown;

    public MainWindow(AppController controller)
    {
        InitializeComponent();
        _controller = controller;
        DataContext = new SettingsViewModel(controller);
        ApplySavedBounds(controller.Settings);

        SourceInitialized += OnSourceInitialized;
        StateChanged += (_, _) => UpdateMaximizedPadding();
        ThemeService.ThemeChanged += OnThemeChanged;
    }

    public void CloseForShutdown()
    {
        _closingForShutdown = true;
        SaveBounds();
        Close();
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        if (WindowEffects.TryApplyBackdrop(this, SystemBackdrop.Mica))
            Background = System.Windows.Media.Brushes.Transparent;
        WindowEffects.SetDarkMode(this, ThemeService.IsDark);
    }

    private void OnThemeChanged(object? sender, EventArgs e) => WindowEffects.SetDarkMode(this, ThemeService.IsDark);

    private SettingsViewModel ViewModel => (SettingsViewModel)DataContext;

    /// <summary>“更换”菜单：列出全部操作，当前的打勾；最后一项新建自定义操作并直接放到这个位置。</summary>
    private void OnChangeSlotClick(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int index } button) return;
        var current = ViewModel.VisibleSlots.FirstOrDefault(slot => slot.Index == index)?.Action.Id;
        var menu = new ContextMenu { PlacementTarget = button, Placement = PlacementMode.Bottom };
        foreach (var action in ViewModel.AvailableActions)
        {
            var item = new MenuItem
            {
                Header = new TextBlock { Text = action.Title, MaxWidth = 280, TextTrimming = TextTrimming.CharacterEllipsis },
                Icon = new TextBlock { Text = action.Glyph },
                IsChecked = action.Id == current
            };
            var id = action.Id;
            item.Click += (_, _) => ViewModel.AssignSlot(index, id);
            menu.Items.Add(item);
        }
        menu.Items.Add(new Separator());
        var create = new MenuItem { Header = Strings.NewCustomActionMenu, Icon = new TextBlock { Text = "\uE710" }, IsEnabled = ViewModel.CanAddCustomAction };
        create.Click += (_, _) => ViewModel.BeginNewCustomAction(index);
        menu.Items.Add(create);
        menu.IsOpen = true;
    }

    private void OnBrowseFolderClick(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Editor is not { } editor) return;
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = Strings.ChooseTargetFolder, Multiselect = false };
        if (Directory.Exists(editor.Folder)) dialog.InitialDirectory = editor.Folder;
        if (dialog.ShowDialog(this) == true) editor.Folder = dialog.FolderName;
    }

    private void OnBrowseProgramClick(object sender, RoutedEventArgs e)
    {
        if (ViewModel.Editor is not { } editor) return;
        var dialog = new Microsoft.Win32.OpenFileDialog { Title = Strings.ChooseProgram, Filter = Strings.ProgramFilter, DereferenceLinks = true };
        if (File.Exists(editor.ProgramPath)) dialog.InitialDirectory = Path.GetDirectoryName(editor.ProgramPath);
        if (dialog.ShowDialog(this) == true) editor.ProgramPath = dialog.FileName;
    }

    private void OnNavigationChanged(object sender, SelectionChangedEventArgs e)
    {
        // 初始化 XAML 时就会触发一次，此时页面元素尚未创建。
        if (PageScroll is null || NavList.SelectedItem is not ListBoxItem { Tag: string page }) return;
        OverviewPage.Visibility = page == "Overview" ? Visibility.Visible : Visibility.Collapsed;
        ShelfPage.Visibility = page == "Shelf" ? Visibility.Visible : Visibility.Collapsed;
        ActionsPage.Visibility = page == "Actions" ? Visibility.Visible : Visibility.Collapsed;
        AppearancePage.Visibility = page == "Appearance" ? Visibility.Visible : Visibility.Collapsed;
        AboutPage.Visibility = page == "About" ? Visibility.Visible : Visibility.Collapsed;
        PageScroll.ScrollToTop();
    }

    /// <summary>自定义标题栏的窗口最大化时会超出屏幕边缘，需要留出边框宽度。</summary>
    private void UpdateMaximizedPadding()
    {
        var border = SystemParameters.WindowResizeBorderThickness;
        RootGrid.Margin = WindowState == WindowState.Maximized
            ? new Thickness(border.Left + 4, border.Top + 4, border.Right + 4, border.Bottom + 4)
            : new Thickness(0);
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        SaveBounds();
        if (!_closingForShutdown)
        {
            // 关闭主窗口只是隐藏到通知区域，手势识别继续运行。
            e.Cancel = true;
            Hide();
            _controller.OnMainWindowHidden();
        }
        base.OnClosing(e);
    }

    protected override void OnClosed(EventArgs e)
    {
        ThemeService.ThemeChanged -= OnThemeChanged;
        base.OnClosed(e);
    }

    private void ApplySavedBounds(AppSettings settings)
    {
        Width = settings.WindowWidth;
        Height = settings.WindowHeight;
        if (!IsVisiblePosition(settings.WindowLeft, settings.WindowTop, Width)) return;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = settings.WindowLeft;
        Top = settings.WindowTop;
    }

    private void SaveBounds()
    {
        if (WindowState != WindowState.Normal || !IsLoaded) return;
        _controller.SaveWindowBounds(new Rect(Left, Top, ActualWidth, ActualHeight));
    }

    private static bool IsVisiblePosition(double left, double top, double width) =>
        double.IsFinite(left) && double.IsFinite(top) &&
        left + width >= SystemParameters.VirtualScreenLeft + 120 &&
        top >= SystemParameters.VirtualScreenTop - 10 &&
        left <= SystemParameters.VirtualScreenLeft + SystemParameters.VirtualScreenWidth - 120 &&
        top <= SystemParameters.VirtualScreenTop + SystemParameters.VirtualScreenHeight - 80;
}
