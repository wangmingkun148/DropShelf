using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using System.Windows.Media;
using DropShelf.Infrastructure;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services.Gestures;

namespace DropShelf.ViewModels;

/// <summary>置物架底部的一个位置及其当前的快速操作。</summary>
public sealed class ActionSlotViewModel(int index, string position, QuickActionInfo action, SettingsViewModel owner)
    : ActionCardViewModel(action, owner)
{
    private string? _dropHint;

    public int Index { get; } = index;

    /// <summary>在置物架上的位置，如“左”“右下”。</summary>
    public string Position { get; } = position;

    /// <summary>拖动的操作悬停在这里时显示“替换”或“交换”，否则为空。</summary>
    public string? DropHint
    {
        get => _dropHint;
        set
        {
            if (SetField(ref _dropHint, value)) OnPropertyChanged(nameof(IsDropTarget));
        }
    }

    public bool IsDropTarget => _dropHint is not null;
}
