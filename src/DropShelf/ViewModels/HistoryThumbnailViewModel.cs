using System.Diagnostics;
using System.IO;
using System.Windows.Media;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

/// <summary>历史卡片上的一张缩略图；读取不到时显示文件或文件夹图标。</summary>
public sealed class HistoryThumbnailViewModel : ObservableObject
{
    private ImageSource? _image;
    private const string FolderGlyph = "";
    private const string FileGlyph = "";
    private string _glyph = string.Empty;

    public HistoryThumbnailViewModel(string path)
    {
        _ = LoadAsync(path);
    }

    /// <summary>是文件夹还是文件在后台查明后才显示图标，打开主窗口时不在界面线程上逐个访问磁盘。</summary>
    public string Glyph
    {
        get => _glyph;
        private set => SetField(ref _glyph, value);
    }

    public ImageSource? Image
    {
        get => _image;
        private set
        {
            if (SetField(ref _image, value)) OnPropertyChanged(nameof(HasImage));
        }
    }

    public bool HasImage => _image is not null;

    private async Task LoadAsync(string path)
    {
        Glyph = await Task.Run(() => Directory.Exists(path)) ? FolderGlyph : FileGlyph;
        Image = await ShellThumbnailService.LoadAsync(path, 96);
    }
}
