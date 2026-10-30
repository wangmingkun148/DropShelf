using System.Diagnostics;
using System.IO;
using System.Windows.Input;
using DropShelf.Infrastructure;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

/// <summary>概览页中的一个最近关闭的置物架，用缩略图堆叠表示，与置物架本身的样子一致。</summary>
public sealed class HistoryEntryViewModel
{
    public HistoryEntryViewModel(ShelfHistoryEntry entry, AppController controller)
    {
        Entry = entry;
        Title = Strings.HistoryEntryTitle(entry);
        Detail = $"{Strings.ItemCount(entry.Items.Count)} · {Strings.HistoryEntryTime(entry.ClosedAt)}";
        Count = entry.Items.Count;
        Suit = FileSuits.Dominant(entry.Items.Select(item => FileSuits.Of(item.Path)));
        ClosedTime = entry.ClosedAt.ToString("HH:mm", System.Globalization.CultureInfo.InvariantCulture);
        ClosedDay = Strings.HistoryDay(entry.ClosedAt);
        // 最后放入的项目在最上面。
        var top = entry.Items.TakeLast(3).Reverse().Select(item => new HistoryThumbnailViewModel(item.Path)).ToArray();
        Stack0 = top.ElementAtOrDefault(0);
        Stack1 = top.ElementAtOrDefault(1);
        Stack2 = top.ElementAtOrDefault(2);
        RestoreCommand = new RelayCommand(_ => controller.RestoreNearMainWindow(entry));
        RemoveCommand = new RelayCommand(_ => controller.History.Remove(entry));
    }

    public ShelfHistoryEntry Entry { get; }
    public string Title { get; }
    public string Detail { get; }

    /// <summary>牌角上的数字和花色：项目数和数量最多的文件类型。</summary>
    public int Count { get; }
    public FileSuit Suit { get; }
    public string ClosedTime { get; }
    public string ClosedDay { get; }
    public HistoryThumbnailViewModel? Stack0 { get; }
    public HistoryThumbnailViewModel? Stack1 { get; }
    public HistoryThumbnailViewModel? Stack2 { get; }
    public ICommand RestoreCommand { get; }
    public ICommand RemoveCommand { get; }
}
