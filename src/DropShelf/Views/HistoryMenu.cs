using System.Windows.Controls;
using DropShelf.Localization;
using DropShelf.Services;

namespace DropShelf.Views;

/// <summary>“最近关闭的置物架”子菜单，托盘菜单和置物架的“更多”菜单共用。</summary>
internal static class HistoryMenu
{
    private const int PreviewNames = 8;

    public static MenuItem Create(IReadOnlyList<ShelfHistoryEntry> entries, Action<ShelfHistoryEntry> restore, Action clear)
    {
        var root = new MenuItem { Header = Strings.RecentShelves, Icon = new TextBlock { Text = "" } };
        foreach (var entry in entries)
        {
            var item = new MenuItem
            {
                // 用 TextBlock 作为标题，文件名中的下划线不会被当成快捷键。
                Header = new TextBlock { Text = Strings.HistoryEntryTitle(entry), MaxWidth = 260, TextTrimming = System.Windows.TextTrimming.CharacterEllipsis },
                Icon = new TextBlock { Text = entry.Items.Count == 1 ? "" : "" },
                InputGestureText = Strings.HistoryEntryTime(entry.ClosedAt),
                ToolTip = string.Join(Environment.NewLine, entry.Items.Take(PreviewNames).Select(name => name.Name)) +
                          (entry.Items.Count > PreviewNames ? Environment.NewLine + "…" : string.Empty)
            };
            item.Click += (_, _) => restore(entry);
            root.Items.Add(item);
        }
        root.Items.Add(new Separator());
        var clearItem = new MenuItem { Header = Strings.ClearHistory, Icon = new TextBlock { Text = "" } };
        clearItem.Click += (_, _) => clear();
        root.Items.Add(clearItem);
        return root;
    }
}
