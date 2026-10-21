using System.IO;
using System.Text.Json;
using DropShelf.Models;

namespace DropShelf.Services;

public sealed class ShelfHistoryItem
{
    public string Path { get; set; } = string.Empty;
    public string Name { get; set; } = string.Empty;
}

public sealed class ShelfHistoryEntry
{
    public DateTime ClosedAt { get; set; }
    public List<ShelfHistoryItem> Items { get; set; } = [];
}

/// <summary>
/// 最近关闭的置物架，误关或退出后可以恢复。只记录文件和文件夹的路径与名称，
/// 临时附件、文本和链接不会记录；最多保留 10 个。
/// </summary>
public sealed class ShelfHistoryService
{
    public const int Capacity = 10;
    private const int MaximumItemsPerEntry = 500;
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly List<ShelfHistoryEntry> _entries = [];

    public ShelfHistoryService(string path)
    {
        HistoryPath = path;
        Load();
    }

    public string HistoryPath { get; }
    public IReadOnlyList<ShelfHistoryEntry> Entries => _entries;
    public ShelfHistoryEntry? Latest => _entries.Count > 0 ? _entries[0] : null;

    public event EventHandler? Changed;

    /// <summary>记录一个刚关闭的置物架；没有可记录的文件时返回 false。内容相同的旧记录会被新记录取代。</summary>
    public bool Record(IEnumerable<ShelfItem> items, DateTime? closedAt = null)
    {
        var recorded = items
            .Where(item => item.IsFileSystem && !item.IsTemporaryAttachment && item.RefreshAvailability())
            .Select(item => new ShelfHistoryItem { Path = item.Value, Name = item.DisplayName })
            .DistinctBy(item => item.Path, StringComparer.OrdinalIgnoreCase)
            .Take(MaximumItemsPerEntry)
            .ToList();
        if (recorded.Count == 0) return false;

        var key = KeyOf(recorded);
        _entries.RemoveAll(entry => KeyOf(entry.Items) == key);
        _entries.Insert(0, new ShelfHistoryEntry { ClosedAt = closedAt ?? DateTime.Now, Items = recorded });
        if (_entries.Count > Capacity) _entries.RemoveRange(Capacity, _entries.Count - Capacity);
        SaveAndNotify();
        return true;
    }

    public void Remove(ShelfHistoryEntry entry)
    {
        if (_entries.Remove(entry)) SaveAndNotify();
    }

    public void Clear()
    {
        if (_entries.Count == 0) return;
        _entries.Clear();
        SaveAndNotify();
    }

    private void Load()
    {
        try
        {
            if (!File.Exists(HistoryPath)) return;
            var entries = JsonSerializer.Deserialize<List<ShelfHistoryEntry>>(File.ReadAllText(HistoryPath), JsonOptions) ?? [];
            foreach (var entry in entries)
            {
                entry.Items = (entry.Items ?? [])
                    .Where(item => item is not null && !string.IsNullOrWhiteSpace(item.Path))
                    .Take(MaximumItemsPerEntry)
                    .ToList();
                foreach (var item in entry.Items)
                    if (string.IsNullOrWhiteSpace(item.Name)) item.Name = System.IO.Path.GetFileName(item.Path);
                if (entry.Items.Count > 0 && _entries.Count < Capacity) _entries.Add(entry);
            }
        }
        catch (Exception exception)
        {
            LogService.Error("读取最近关闭的置物架失败", exception);
            _entries.Clear();
        }
    }

    private void SaveAndNotify()
    {
        try
        {
            var directory = System.IO.Path.GetDirectoryName(HistoryPath);
            if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
            var temporaryPath = HistoryPath + ".tmp";
            File.WriteAllText(temporaryPath, JsonSerializer.Serialize(_entries, JsonOptions));
            File.Move(temporaryPath, HistoryPath, true);
        }
        catch (Exception exception)
        {
            LogService.Error("保存最近关闭的置物架失败", exception);
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private static string KeyOf(IEnumerable<ShelfHistoryItem> items) =>
        string.Join('\n', items.Select(item => item.Path.ToUpperInvariant()).Order(StringComparer.Ordinal));
}
