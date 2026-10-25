using System.Windows;
using DropShelf.Interop;
using DropShelf.Services;
using DropShelf.ViewModels;

namespace DropShelf.Views;

/// <summary>点击主窗口右下角的版本号打开：程序和数据的位置、退出与卸载。</summary>
public partial class AboutWindow : Window
{
    public AboutWindow(SettingsViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
        SourceInitialized += (_, _) => WindowEffects.SetDarkMode(this, ThemeService.IsDark);
    }
}
