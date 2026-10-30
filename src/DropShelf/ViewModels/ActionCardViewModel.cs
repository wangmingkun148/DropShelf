using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Models;
using DropShelf.Services.Gestures;

namespace DropShelf.ViewModels;

/// <summary>快速操作页中的一张操作卡片，可以拖到某个位置上。自定义操作可以编辑和删除。</summary>
public class ActionCardViewModel(QuickActionInfo action, SettingsViewModel owner) : ObservableObject
{
    private bool _isDragSource;

    public QuickActionInfo Action { get; } = action;
    public string Title => Action.Title;
    public string Description => Action.Description;
    public string Glyph => Action.Glyph;
    public bool IsCustom => Action.Custom is not null;
    public ICommand EditCommand { get; } = new RelayCommand(_ => owner.EditCustomAction(action.Custom!));
    public ICommand DeleteCommand { get; } = new RelayCommand(_ => owner.DeleteCustomAction(action.Custom!));

    /// <summary>正在被拖动，卡片变淡。</summary>
    public bool IsDragSource
    {
        get => _isDragSource;
        set => SetField(ref _isDragSource, value);
    }
}
