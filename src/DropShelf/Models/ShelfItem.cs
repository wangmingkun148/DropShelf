using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Windows.Media;
using DropShelf.Localization;

namespace DropShelf.Models;

public enum ShelfItemKind
{
    File,
    Folder,
    Text,
    Link
}

public sealed class ShelfItem : INotifyPropertyChanged
{
    private ImageSource? _thumbnail;
    private bool _isAvailable = true;
    private long? _sizeBytes;
    private string _typeName = string.Empty;

    public required string Id { get; init; }
    public required ShelfItemKind Kind { get; init; }

    /// <summary>文件或文件夹的完整路径、文本内容或网址。</summary>
    public required string Value { get; init; }

    /// <summary>链接的标题。</summary>
    public string? Title { get; init; }

    /// <summary>由 DropShelf 接收并暂存在临时目录中的附件。</summary>
    public bool IsTemporaryAttachment { get; init; }

    public bool IsFileSystem => Kind is ShelfItemKind.File or ShelfItemKind.Folder;

    public string DisplayName => Kind switch
    {
        ShelfItemKind.File => Path.GetFileName(Value),
        ShelfItemKind.Folder => GetFolderName(Value),
        ShelfItemKind.Link => string.IsNullOrWhiteSpace(Title) ? ShortenUrl(Value) : Title!,
        _ => Summarize(Value, 60)
    };

    public string Detail
    {
        get
        {
            if (!IsAvailable) return Strings.MovedOrDeleted;
            return Kind switch
            {
                ShelfItemKind.File => SizeBytes is { } size
                    ? string.IsNullOrEmpty(TypeName) ? FormatSize(size) : $"{FormatSize(size)} · {TypeName}"
                    : TypeName,
                ShelfItemKind.Folder => Strings.Folder,
                ShelfItemKind.Link => Uri.TryCreate(Value, UriKind.Absolute, out var uri) ? uri.Host : Strings.Link,
                _ => Strings.TextDetail(Value.Length)
            };
        }
    }

    public string Glyph => Kind switch
    {
        ShelfItemKind.Folder => "",
        ShelfItemKind.Text => "",
        ShelfItemKind.Link => "",
        _ => ""
    };

    public long? SizeBytes
    {
        get => _sizeBytes;
        set
        {
            if (_sizeBytes == value) return;
            _sizeBytes = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(Detail));
        }
    }

    public string TypeName
    {
        get => _typeName;
        set
        {
            if (_typeName == value) return;
            _typeName = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(Detail));
        }
    }

    public ImageSource? Thumbnail
    {
        get => _thumbnail;
        set
        {
            if (ReferenceEquals(_thumbnail, value)) return;
            _thumbnail = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(HasThumbnail));
        }
    }

    public bool HasThumbnail => _thumbnail is not null;

    public bool IsAvailable
    {
        get => _isAvailable;
        set
        {
            if (_isAvailable == value) return;
            _isAvailable = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(Detail));
        }
    }

    /// <summary>切换界面语言后刷新说明文字。</summary>
    public void RefreshText() => OnPropertyChanged(nameof(Detail));

    /// <summary>检查磁盘上的项目是否还在；文本与链接始终可用。</summary>
    public bool RefreshAvailability()
    {
        if (!IsFileSystem) return true;
        IsAvailable = Kind == ShelfItemKind.Folder ? Directory.Exists(Value) : File.Exists(Value);
        return IsAvailable;
    }

    public static string FormatSize(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB", "TB"];
        double value = bytes;
        var unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return unit == 0 ? $"{bytes} B" : $"{value:0.#} {units[unit]}";
    }

    public static string Summarize(string text, int maximumLength)
    {
        var firstLine = text.Trim().Split('\n', 2)[0].Trim();
        var collapsed = string.Join(' ', firstLine.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        if (collapsed.Length == 0) collapsed = text.Trim();
        return collapsed.Length > maximumLength ? collapsed[..maximumLength] + "…" : collapsed;
    }

    private static string GetFolderName(string path)
    {
        var trimmed = Path.TrimEndingDirectorySeparator(path);
        var name = Path.GetFileName(trimmed);
        return string.IsNullOrEmpty(name) ? path : name;
    }

    private static string ShortenUrl(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri)) return Summarize(url, 60);
        var text = uri.Host + uri.AbsolutePath.TrimEnd('/');
        return text.Length > 60 ? text[..60] + "…" : text;
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
