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

    public HistoryThumbnailViewModel(string path)
    {
        Glyph = Directory.Exists(path) ? "" : "";
        _ = LoadAsync(path);
    }

    public string Glyph { get; }

    public ImageSource? Image
    {
        get => _image;
        private set
        {
            if (SetField(ref _image, value)) OnPropertyChanged(nameof(HasImage));
        }
    }

    public bool HasImage => _image is not null;

    private async Task LoadAsync(string path) => Image = await ShellThumbnailService.LoadAsync(path, 96);
}
