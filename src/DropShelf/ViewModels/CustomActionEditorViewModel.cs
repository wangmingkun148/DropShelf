using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services.Gestures;

namespace DropShelf.ViewModels;

/// <summary>添加或编辑自定义操作的对话框。</summary>
public sealed class CustomActionEditorViewModel : ObservableObject
{
    private readonly CustomQuickAction _original;
    private readonly Action<CustomActionEditorViewModel> _save;
    private CustomActionKind _kind;
    private string _name;
    private string _folder;
    private TransferMode _folderMode;
    private string _sizeText;
    private string _programPath;
    private string _arguments;
    private bool _showErrors;

    public CustomActionEditorViewModel(CustomQuickAction action, bool isNew, Action<CustomActionEditorViewModel> save, Action cancel)
    {
        _original = action;
        _save = save;
        IsNew = isNew;
        _kind = action.Kind;
        _name = action.Name ?? string.Empty;
        _folder = action.Folder ?? string.Empty;
        _folderMode = action.FolderMode;
        _sizeText = action.ImageSizeKb.ToString(System.Globalization.CultureInfo.InvariantCulture);
        _programPath = action.ProgramPath ?? string.Empty;
        _arguments = action.Arguments ?? string.Empty;
        SaveCommand = new RelayCommand(_ => TrySave());
        CancelCommand = new RelayCommand(_ => cancel());
    }

    public bool IsNew { get; }
    public ICommand SaveCommand { get; }
    public ICommand CancelCommand { get; }

    public string Title => IsNew ? Strings.AddCustomAction : Strings.EditCustomAction;

    public CustomActionKind Kind
    {
        get => _kind;
        set
        {
            if (!SetField(ref _kind, value)) return;
            OnPropertyChanged(nameof(IsFolder));
            OnPropertyChanged(nameof(IsCompressImage));
            OnPropertyChanged(nameof(IsOpenWith));
            OnFieldChanged();
        }
    }

    public bool IsFolder => Kind == CustomActionKind.Folder;
    public bool IsCompressImage => Kind == CustomActionKind.CompressImage;
    public bool IsOpenWith => Kind == CustomActionKind.OpenWith;

    public string Name
    {
        get => _name;
        set => SetField(ref _name, value);
    }

    public string Folder
    {
        get => _folder;
        set
        {
            if (SetField(ref _folder, value)) OnFieldChanged();
        }
    }

    public TransferMode FolderMode
    {
        get => _folderMode;
        set
        {
            if (SetField(ref _folderMode, value)) OnFieldChanged();
        }
    }

    public string SizeText
    {
        get => _sizeText;
        set
        {
            if (SetField(ref _sizeText, value)) OnFieldChanged();
        }
    }

    public string ProgramPath
    {
        get => _programPath;
        set
        {
            if (SetField(ref _programPath, value)) OnFieldChanged();
        }
    }

    public string Arguments
    {
        get => _arguments;
        set => SetField(ref _arguments, value);
    }

    /// <summary>名称留空时使用的名称，显示为输入框的提示文字。</summary>
    public string NamePlaceholder => Strings.CustomActionAutoName(BuildAction(includeName: false));

    /// <summary>第一次点保存之后才显示错误，避免刚打开对话框就是一片红字。</summary>
    public string? Error => _showErrors ? Validate() : null;

    public bool HasError => Error is not null;

    public void RefreshText()
    {
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(NamePlaceholder));
        OnPropertyChanged(nameof(Error));
        OnPropertyChanged(nameof(HasError));
    }

    public CustomQuickAction BuildAction(bool includeName = true)
    {
        var action = _original.Clone();
        action.Kind = Kind;
        action.Name = includeName && !string.IsNullOrWhiteSpace(Name) ? Name.Trim() : null;
        action.Folder = Kind == CustomActionKind.Folder ? Folder.Trim() : null;
        action.FolderMode = FolderMode;
        action.ImageSizeKb = TryParseSize(out var size) ? size : CustomQuickAction.DefaultImageSizeKb;
        action.ProgramPath = Kind == CustomActionKind.OpenWith ? ProgramPath.Trim().Trim('"') : null;
        action.Arguments = Kind == CustomActionKind.OpenWith && !string.IsNullOrWhiteSpace(Arguments) ? Arguments.Trim() : null;
        return action;
    }

    public string? Validate() => Kind switch
    {
        CustomActionKind.Folder when string.IsNullOrWhiteSpace(Folder) => Strings.ChooseFolderFirst,
        CustomActionKind.Folder when !Directory.Exists(Folder.Trim()) => Strings.FolderNotFoundShort,
        CustomActionKind.CompressImage when !TryParseSize(out _) => Strings.ImageSizeInvalid(CustomQuickAction.MinimumImageSizeKb,
            CustomQuickAction.MaximumImageSizeKb),
        CustomActionKind.OpenWith when string.IsNullOrWhiteSpace(ProgramPath) => Strings.ChooseProgramFirst,
        CustomActionKind.OpenWith when !File.Exists(ProgramPath.Trim().Trim('"')) => Strings.ProgramNotFoundShort,
        _ => null
    };

    public bool TrySave()
    {
        _showErrors = true;
        OnPropertyChanged(nameof(Error));
        OnPropertyChanged(nameof(HasError));
        if (Validate() is not null) return false;
        _save(this);
        return true;
    }

    private bool TryParseSize(out int size) =>
        int.TryParse(SizeText.Trim(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out size) &&
        size is >= CustomQuickAction.MinimumImageSizeKb and <= CustomQuickAction.MaximumImageSizeKb;

    private void OnFieldChanged()
    {
        OnPropertyChanged(nameof(NamePlaceholder));
        OnPropertyChanged(nameof(Error));
        OnPropertyChanged(nameof(HasError));
    }
}
