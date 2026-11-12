using System.Windows;
using DropShelf.Interop;
using DropShelf.Localization;
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
        SourceInitialized += (_, _) => WindowEffects.ApplyMica(this, CaptionButtons);
    }

    private void OnCaptionCloseClick(object sender, RoutedEventArgs e) => SystemCommands.CloseWindow(this);

    /// <summary>选个位置导出翻译模板，默认放在语言包文件夹里；写好后在资源管理器里选中它。</summary>
    private void OnExportLanguageTemplateClick(object sender, RoutedEventArgs e)
    {
        var viewModel = (SettingsViewModel)DataContext;
        var dialog = new Microsoft.Win32.SaveFileDialog
        {
            Title = Strings.ExportLanguageTemplate.TrimEnd('…'), Filter = Strings.LanguageTemplateFilter, FileName = "template.json", AddExtension = true,
            DefaultExt = ".json"
        };
        try
        {
            System.IO.Directory.CreateDirectory(viewModel.LanguagePackFolder);
            dialog.InitialDirectory = viewModel.LanguagePackFolder;
        }
        catch (Exception exception) when (exception is System.IO.IOException or UnauthorizedAccessException)
        {
            // 建不了这个文件夹就从默认的位置开始选。
        }
        if (dialog.ShowDialog(this) != true) return;
        if (viewModel.ExportLanguageTemplate(dialog.FileName)) ShellInterop.OpenFolderAndSelect([dialog.FileName]);
        else System.Windows.MessageBox.Show(this, Strings.LanguageTemplateFailed, "DropShelf", MessageBoxButton.OK, MessageBoxImage.Warning);
    }
}
