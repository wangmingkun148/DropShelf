using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.ViewModels;

public sealed record ShelfToast(string Message, bool Success);

public sealed record ShelfAddSummary(int Added, int Duplicates, int Unavailable)
{
    public string Message
    {
        get
        {
            if (Added == 0 && Duplicates > 0 && Unavailable == 0) return Strings.AlreadyOnShelf;
            if (Added == 0) return Unavailable > 0 ? Strings.ItemsUnavailable : Strings.NothingToAdd;
            var message = Strings.Added(Added);
            if (Duplicates > 0) message += Strings.SkippedDuplicates(Duplicates);
            if (Unavailable > 0) message += Strings.UnavailableItems(Unavailable);
            return message;
        }
    }
}

/// <summary>一次移除的记录：移除的项目和它们原来的位置（从前到后），撤销时按原位放回。</summary>
public sealed record ShelfRemoval(IReadOnlyList<(ShelfItem Item, int Index)> Entries);

public sealed class ShelfViewModel : ObservableObject
{
    private readonly HashSet<string> _cacheDirectories = new(StringComparer.OrdinalIgnoreCase);
    private IReadOnlyList<QuickActionInfo> _actions = [];
    private bool _isExpanded;
    private bool _isPinned = true;
    private bool _isBusy;
    private string _busyText = string.Empty;
    private bool _delayCacheCleanup;
    private bool _released;
    private int _batchDepth;
    private bool _batchChanged;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly Dictionary<ShelfItem, CancellationTokenSource> _thumbnails = [];
    private readonly HashSet<ShelfItem> _visibleRows = [];
    /// <summary>和 Items 内容相同，随它增减：判断某个项目还在不在置物架上时不用把列表从头找一遍。</summary>
    private readonly HashSet<ShelfItem> _members = [];

    public ShelfViewModel()
    {
        Items.CollectionChanged += OnItemsChanged;
    }

    public string Id { get; } = Guid.NewGuid().ToString("N");
    public ObservableCollection<ShelfItem> Items { get; } = [];

    public IReadOnlyList<QuickActionInfo> Actions
    {
        get => _actions;
        set
        {
            if (!SetField(ref _actions, value)) return;
            OnPropertyChanged(nameof(HasActions));
        }
    }

    public bool HasActions => _actions.Count > 0;
    public int Count => Items.Count;
    public bool HasItems => Items.Count > 0;
    public bool IsEmpty => Items.Count == 0;
    public bool HasMultiple => Items.Count > 1;
    public string Title => Items.Count == 0 ? Strings.Shelf : Strings.ItemCount(Items.Count);
    public string WindowTitle => Strings.ShelfWindowTitle(Title);

    /// <summary>堆叠最上面是最新放入的项目。</summary>
    public ShelfItem? Stack0 => ItemFromEnd(1);
    public ShelfItem? Stack1 => ItemFromEnd(2);
    public ShelfItem? Stack2 => ItemFromEnd(3);

    /// <summary>堆叠角标上的花色：数量最多的文件类型。</summary>
    public FileSuit StackSuit => FileSuits.Dominant(Items.Select(FileSuits.Of));

    public string StackCaption => Items.Count switch
    {
        0 => string.Empty,
        1 => Items[0].DisplayName,
        _ => Strings.ItemCount(Items.Count)
    };

    public string StackDetail => Items.Count == 1 ? Items[0].Detail : BuildSummary();

    public bool IsExpanded
    {
        get => _isExpanded;
        set => SetField(ref _isExpanded, value);
    }

    /// <summary>缩略图的像素尺寸，由窗口按堆叠卡片的实际显示大小设置。</summary>
    public int ThumbnailSize { get; set; } = 128;

    /// <summary>置顶：保持在其他窗口上方，默认开启；关闭后和普通窗口一样会被遮挡。</summary>
    public bool IsPinned
    {
        get => _isPinned;
        set
        {
            if (SetField(ref _isPinned, value)) HasInteracted = true;
        }
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set => SetField(ref _isBusy, value);
    }

    public string BusyText
    {
        get => _busyText;
        private set => SetField(ref _busyText, value);
    }

    /// <summary>
    /// 读取拖入的内容期间算作忙碌：读得慢时显示处理中的遮罩（很快读完时不显示），这期间也不接收新的拖入。text 为 null 时结束。
    /// </summary>
    public void ShowReceiving(string? text)
    {
        if (text is not null) BusyText = text;
        IsBusy = text is not null;
    }

    /// <summary>通过晃动手势打开，松开鼠标时若仍为空就自动关闭。</summary>
    public bool OpenedByGesture { get; set; }

    /// <summary>用户已经点击、切换置顶或放入过内容，空置物架也不再自动关闭。</summary>
    public bool HasInteracted { get; set; }

    public bool HasTemporaryFiles => Items.Any(item => item.IsTemporaryAttachment);

    public IReadOnlyCollection<string> CacheDirectories => _cacheDirectories;

    public event EventHandler<ShelfToast>? ToastRequested;

    /// <summary>操作生成了新文件（如压缩包），需要放进另一个新的置物架。</summary>
    public event EventHandler<IReadOnlyList<string>>? NewItemsProduced;

    public void Notify(string message, bool success = true) => ToastRequested?.Invoke(this, new ShelfToast(message, success));

    public ShelfAddSummary AddFiles(IEnumerable<string> paths, bool temporary = false) =>
        AddEntries(paths.Select(path => new DroppedEntry(ShelfItemKind.File, path, IsTemporary: temporary)));

    /// <summary>facts 是 <see cref="ReadFactsAsync(DropReadResult)"/> 在后台读好的文件信息；没有时在这里逐个读取。</summary>
    public ShelfAddSummary Add(DropReadResult result, IReadOnlyDictionary<string, FileFacts?>? facts = null)
    {
        foreach (var cache in result.CacheDirectories) AdoptCache(cache);
        return AddEntries(result.Entries, facts);
    }

    /// <summary>
    /// 在后台读好拖入的文件信息，再交给 <see cref="Add"/>：一次拖入很多文件、文件在网络盘上时，
    /// 界面线程不用逐个查询，紧接着的落下动画不被拖住。没有文件（文本、链接）时不用读，直接返回 null。
    /// </summary>
    public static async Task<IReadOnlyDictionary<string, FileFacts?>?> ReadFactsAsync(DropReadResult result) =>
        result.FilePaths is { Count: > 0 } paths ? await ReadFactsAsync(paths) : null;

    /// <summary>先在后台限量读取文件信息（网络路径可能很慢），再一次放入，顺序和去重规则与 <see cref="AddFiles"/> 相同。</summary>
    public async Task<ShelfAddSummary> AddFilesAsync(IReadOnlyList<string> paths)
    {
        var facts = await ReadFactsAsync(paths);
        if (_released) return new ShelfAddSummary(0, 0, paths.Count);
        return AddEntries(paths.Select(path => new DroppedEntry(ShelfItemKind.File, path)), facts);
    }

    private static async Task<IReadOnlyDictionary<string, FileFacts?>> ReadFactsAsync(IReadOnlyList<string> paths)
    {
        var facts = new System.Collections.Concurrent.ConcurrentDictionary<string, FileFacts?>(StringComparer.OrdinalIgnoreCase);
        await Parallel.ForEachAsync(paths.Where(path => !string.IsNullOrWhiteSpace(path)).Distinct(StringComparer.OrdinalIgnoreCase),
            new ParallelOptions { MaxDegreeOfParallelism = 4 }, (path, _) =>
            {
                facts[path] = ReadFacts(path);
                return ValueTask.CompletedTask;
            });
        return facts;
    }

    public ShelfAddSummary AddEntries(IEnumerable<DroppedEntry> entries) => AddEntries(entries, null);

    private ShelfAddSummary AddEntries(IEnumerable<DroppedEntry> entries, IReadOnlyDictionary<string, FileFacts?>? facts)
    {
        var summary = new ShelfAddSummary(0, 0, 0);
        Batch(() => summary = AddEntriesCore(entries, facts));
        return summary;
    }

    /// <summary>一次放入或移除很多项目时，数量、标题和汇总等派生属性在全部完成后只通知一次。</summary>
    private void Batch(Action change)
    {
        _batchDepth++;
        try
        {
            change();
        }
        finally
        {
            if (--_batchDepth == 0 && _batchChanged)
            {
                _batchChanged = false;
                NotifyItemsChanged();
            }
        }
    }

    /// <summary>文件是不是文件夹、有多大，以及它在磁盘上的标识（取不到时为 null）。</summary>
    public readonly record struct FileFacts(bool IsFolder, long? SizeBytes, FileIdentity? Identity = null);

    /// <summary>读取文件夹标记和文件大小；已删除、无权限等读不到时返回 null。</summary>
    private static FileFacts? ReadFacts(string path)
    {
        FileAttributes attributes;
        try
        {
            attributes = File.GetAttributes(path);
        }
        catch (Exception exception) when (exception is FileNotFoundException or DirectoryNotFoundException
                                              or UnauthorizedAccessException or IOException or ArgumentException
                                              or NotSupportedException)
        {
            return null;
        }
        var isFolder = attributes.HasFlag(FileAttributes.Directory);
        return new FileFacts(isFolder, isFolder ? null : ShellFileInfo.TryGetFileSize(path), FileIdentity.TryGet(path));
    }

    private ShelfAddSummary AddEntriesCore(IEnumerable<DroppedEntry> entries, IReadOnlyDictionary<string, FileFacts?>? known)
    {
        var added = 0;
        var duplicates = 0;
        var unavailable = 0;
        var existingPaths = Items.Where(item => item.IsFileSystem).Select(item => item.Value)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        var existingValues = Items.Where(item => !item.IsFileSystem).Select(item => item.Value).ToHashSet(StringComparer.Ordinal);

        foreach (var entry in entries)
        {
            if (string.IsNullOrWhiteSpace(entry.Value)) continue;
            if (entry.Kind is ShelfItemKind.Text or ShelfItemKind.Link)
            {
                if (!existingValues.Add(entry.Value))
                {
                    duplicates++;
                    continue;
                }
                Items.Add(new ShelfItem { Id = NewId(), Kind = entry.Kind, Value = entry.Value, Title = entry.Title });
                added++;
                continue;
            }

            if (existingPaths.Contains(entry.Value))
            {
                duplicates++;
                continue;
            }

            if ((known is not null && known.TryGetValue(entry.Value, out var read) ? read : ReadFacts(entry.Value)) is not { } facts)
            {
                unavailable++;
                continue;
            }

            var item = new ShelfItem
            {
                Id = NewId(),
                Kind = facts.IsFolder ? ShelfItemKind.Folder : ShelfItemKind.File,
                Value = entry.Value,
                IsTemporaryAttachment = entry.IsTemporary,
                Identity = facts.Identity
            };
            if (!facts.IsFolder)
            {
                item.SizeBytes = facts.SizeBytes;
                item.TypeName = ShellFileInfo.GetTypeName(entry.Value);
            }
            Items.Add(item);
            if (facts.IsFolder) _ = MeasureFolderAsync(item);
            existingPaths.Add(entry.Value);
            added++;
        }

        if (added > 0) HasInteracted = true;
        return new ShelfAddSummary(added, duplicates, unavailable);
    }

    /// <summary>移除项目，返回这次移除的记录（供撤销）；一个也没移除时返回 null。</summary>
    public ShelfRemoval? Remove(IEnumerable<ShelfItem> items)
    {
        var removing = items.Where(_members.Contains).ToHashSet();
        if (removing.Count == 0) return null;
        var entries = new List<(ShelfItem, int)>(removing.Count);
        for (var index = 0; index < Items.Count; index++)
            if (removing.Contains(Items[index])) entries.Add((Items[index], index));
        Batch(() =>
        {
            // 全部移除时一次清空，列表只刷新一次；否则从后往前找，每个项目只经过一遍，后面的项目也不用反复前移。
            if (removing.Count == Items.Count)
            {
                Items.Clear();
                return;
            }
            for (var index = Items.Count - 1; index >= 0 && removing.Count > 0; index--)
                if (removing.Remove(Items[index])) Items.RemoveAt(index);
        });
        return new ShelfRemoval(entries);
    }

    /// <summary>
    /// 撤销移除：按原来的位置放回去，这期间置物架没有别的变化时顺序和移除前一样。
    /// 这期间又放上来的相同文件或内容不重复放回。返回放回的项目。
    /// </summary>
    public IReadOnlyList<ShelfItem> Restore(ShelfRemoval removal)
    {
        var restored = new List<ShelfItem>();
        if (_released) return restored;
        Batch(() =>
        {
            var paths = Items.Where(item => item.IsFileSystem).Select(item => item.Value).ToHashSet(StringComparer.OrdinalIgnoreCase);
            var values = Items.Where(item => !item.IsFileSystem).Select(item => item.Value).ToHashSet(StringComparer.Ordinal);
            foreach (var (item, index) in removal.Entries)
            {
                if (!(item.IsFileSystem ? paths : values).Add(item.Value)) continue;
                Items.Insert(Math.Min(index, Items.Count), item);
                restored.Add(item);
            }
        });
        if (restored.Count > 0) HasInteracted = true;
        return restored;
    }

    public void Clear() => Items.Clear();

    // ───────────── 合并和拆分 ─────────────

    /// <summary>已经交给别的置物架、这边还没移除的项目：不再替它们加载缩略图，也不清掉它们的缩略图（对方接着用）。</summary>
    private readonly HashSet<ShelfItem> _handedOver = [];

    /// <summary>
    /// 接收另一个置物架交来的项目（合并、拆分、架子之间拖动）：项目原样过来，文本、链接、缩略图和已经统计好的文件夹信息都保留；
    /// 这边已经有的相同文件或内容不重复放。临时附件所在的缓存目录由两边共同持有，对方关闭后文件还在。
    /// 对方随后自己把这些项目移除。返回放进来的数量。
    /// </summary>
    public int Adopt(IReadOnlyList<ShelfItem> items, ShelfViewModel from)
    {
        if (_released || ReferenceEquals(from, this)) return 0;
        foreach (var cache in items.Where(item => item.IsTemporaryAttachment).Select(item => VirtualFileDropService.GetCacheDirectory(item.Value))
                     .OfType<string>().Distinct(StringComparer.OrdinalIgnoreCase).ToArray())
        {
            VirtualFileDropService.RetainCache(cache);
            AdoptCache(cache);
        }
        if (from._delayCacheCleanup) _delayCacheCleanup = true;
        from.HandOver(items);

        var added = 0;
        Batch(() =>
        {
            var paths = Items.Where(item => item.IsFileSystem).Select(item => item.Value).ToHashSet(StringComparer.OrdinalIgnoreCase);
            var values = Items.Where(item => !item.IsFileSystem).Select(item => item.Value).ToHashSet(StringComparer.Ordinal);
            foreach (var item in items)
            {
                if (!(item.IsFileSystem ? paths : values).Add(item.Value)) continue;
                Items.Add(item);
                // 对方关闭时会取消还没做完的统计，这边接着做。
                if (item is { Kind: ShelfItemKind.Folder, ChildCount: null }) _ = MeasureFolderAsync(item);
                added++;
            }
        });
        if (added > 0) HasInteracted = true;
        return added;
    }

    private void HandOver(IReadOnlyList<ShelfItem> items)
    {
        foreach (var item in items)
        {
            if (!_members.Contains(item)) continue;
            _handedOver.Add(item);
            _visibleRows.Remove(item);
            if (!_thumbnails.Remove(item, out var load)) continue;
            load.Cancel();
            load.Dispose();
        }
    }

    // ───────────── 文本和链接当文件用 ─────────────

    private string? _ownCache;
    /// <summary>和文件一起拖出时替文本、链接写好的文件；项目还在、文件也还在时下次接着用。</summary>
    private readonly Dictionary<ShelfItem, string> _dragFiles = [];

    /// <summary>置物架自己的临时目录，放文本和链接转成的文件；和接收的临时附件一样，关闭置物架后清理。</summary>
    private string OwnCache()
    {
        if (_ownCache is not null && Directory.Exists(_ownCache)) return _ownCache;
        _ownCache = VirtualFileDropService.CreateCacheDirectory();
        _cacheDirectories.Add(_ownCache);
        return _ownCache;
    }

    /// <summary>
    /// 把文本和链接原地转成文件（文本存成 .txt，网页链接存成 .url），位置不变；之后它们和别的文件一样可以拖到任何地方、交给快速操作。
    /// 文件放在置物架的临时目录里，拖出或移动时才到了目的地。返回转换前后的项目；写不了时提示并返回已经转好的。
    /// </summary>
    public IReadOnlyList<(ShelfItem Original, ShelfItem File)> ConvertToFiles(IReadOnlyList<ShelfItem> items)
    {
        var converted = new List<(ShelfItem, ShelfItem)>();
        if (_released) return converted;
        try
        {
            Batch(() =>
            {
                foreach (var item in items)
                {
                    if (item.IsFileSystem) continue;
                    var index = Items.IndexOf(item);
                    if (index < 0) continue;
                    var path = ShelfItemFiles.Write(item, OwnCache());
                    var file = new ShelfItem
                    {
                        Id = item.Id,
                        Kind = ShelfItemKind.File,
                        Value = path,
                        IsTemporaryAttachment = true,
                        SizeBytes = ShellFileInfo.TryGetFileSize(path),
                        TypeName = ShellFileInfo.GetTypeName(path)
                    };
                    Items[index] = file;
                    converted.Add((item, file));
                }
            });
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            LogService.Error("把文本或链接转成文件失败", exception);
            Notify(Strings.ConvertToFileFailed, false);
        }
        if (converted.Count > 0) HasInteracted = true;
        return converted;
    }

    /// <summary>
    /// 和文件一起拖出时，替文本和链接各写一个文件跟着走，置物架上的项目不变。写不了时返回空，只拖出文件。
    /// </summary>
    public IReadOnlyList<string> WriteForDrag(IReadOnlyList<ShelfItem> items)
    {
        var paths = new List<string>(items.Count);
        try
        {
            foreach (var item in items)
            {
                if (item.IsFileSystem || !_members.Contains(item)) continue;
                if (!_dragFiles.TryGetValue(item, out var path) || !File.Exists(path))
                    _dragFiles[item] = path = ShelfItemFiles.Write(item, OwnCache());
                paths.Add(path);
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            LogService.Error("替文本或链接准备拖出的文件失败", exception);
            return [];
        }
        return paths;
    }

    /// <summary>拖出之后、以及稍后确认文件已被移走时，交给窗口移除项目，窗口可以先播放移除的动画；没有窗口时直接移除。</summary>
    public Action<IReadOnlyList<ShelfItem>>? RemoveVisibly { get; set; }

    private void RemoveShown(IEnumerable<ShelfItem> items)
    {
        var removing = items.Where(_members.Contains).ToArray();
        if (removing.Length == 0) return;
        if (RemoveVisibly is { } remove && !_released) remove(removing);
        else Remove(removing);
    }

    /// <summary>接管读取结果替置物架持有的缓存；已经持有或置物架已关闭时释放多出来的这一次。</summary>
    private void AdoptCache(string? cacheDirectory)
    {
        if (string.IsNullOrWhiteSpace(cacheDirectory)) return;
        if (_released || !_cacheDirectories.Add(cacheDirectory)) _ = VirtualFileDropService.ReleaseCacheAsync(cacheDirectory);
    }

    public void RefreshAvailability()
    {
        foreach (var item in Items) item.RefreshAvailability();
    }

    private bool _checkingAvailability;

    /// <summary>
    /// 在后台检查磁盘上的项目是否还在，查完回到界面线程更新：项目在断开的网络盘上时，点一下置物架不会卡住。
    /// 上一次还没查完时跳过。
    /// </summary>
    public async void RefreshAvailabilityInBackground()
    {
        var items = Items.Where(item => item.IsFileSystem).ToArray();
        if (_checkingAvailability || _released || items.Length == 0) return;
        _checkingAvailability = true;
        try
        {
            var present = new bool[items.Length];
            var moved = new string?[items.Length];
            await Task.Run(() => Parallel.For(0, items.Length, new ParallelOptions { MaxDegreeOfParallelism = 4 }, index =>
            {
                present[index] = items[index].ExistsOnDisk();
                // 原来的路径找不到了：看看是不是在别处被改名或移动了。
                if (!present[index]) moved[index] = FindMoved(items[index]);
            }));
            if (_released) return;
            for (var index = 0; index < items.Length; index++)
            {
                if (moved[index] is { } path && CanFollow(items[index], path)) items[index].Relocate(path);
                else items[index].IsAvailable = present[index];
            }
        }
        catch (Exception exception)
        {
            LogService.Error("检查项目是否还在失败", exception);
        }
        finally
        {
            _checkingAvailability = false;
        }
    }

    // ───────────── 跟着改名和移动 ─────────────

    /// <summary>正在拖出：这期间文件不见了多半是被目标程序移走的，不去跟。</summary>
    public bool IsDraggingOut { get; set; }

    /// <summary>拖出后正等着确认已被移走的项目：它们要从置物架上移除，不跟到新的位置去。</summary>
    private readonly HashSet<ShelfItem> _leaving = [];

    /// <summary>
    /// 项目原来的路径找不到时，按文件标识找它现在的路径（在别处被改名，或在同一个磁盘里被移动）；
    /// 路径还在、没有标识、进了回收站或找不到时返回 null。可以在后台线程上调用。
    /// </summary>
    private static string? FindMoved(ShelfItem item)
    {
        if (!item.IsFileSystem || item.Identity is not { } identity || item.ExistsOnDisk()) return null;
        var path = identity.Resolve(item.Value, item.Kind == ShelfItemKind.Folder);
        return string.Equals(path, item.Value, StringComparison.OrdinalIgnoreCase) ? null : path;
    }

    /// <summary>可以把项目换成新的路径：项目还在置物架上、不是正要移走的，新路径也没有别的项目占着。</summary>
    private bool CanFollow(ShelfItem item, string path) =>
        !IsDraggingOut && _members.Contains(item) && !_leaving.Contains(item) && IndexOfPath(path) < 0;

    /// <summary>
    /// 拖出、打开或交给快速操作之前：这几个项目里路径找不到的，当场按文件标识找回来，免得刚在别处改了名就拖不出去。
    /// 只在项目不多时用，多了交给后台的检查。
    /// </summary>
    public void Relink(IReadOnlyList<ShelfItem> items)
    {
        if (_released || items.Count > RelinkLimit) return;
        foreach (var item in items)
            if (FindMoved(item) is { } path && CanFollow(item, path)) item.Relocate(path);
    }

    private const int RelinkLimit = 16;

    /// <summary>处理拖出结果；返回置物架内容是否发生变化。</summary>
    public bool ApplyDragOut(IReadOnlyList<ShelfItem> dragged, DragOutResult result, bool removeCopied)
    {
        switch (result)
        {
            case DragOutResult.Moved:
                var pending = dragged.Where(item => item.IsFileSystem && item.RefreshAvailability()).ToList();
                RemoveShown(dragged.Except(pending));
                // 资源管理器可能在后台继续移动大文件，稍后再确认一次。
                if (pending.Count > 0)
                {
                    _leaving.UnionWith(pending);
                    _ = RemoveWhenGoneAsync(pending);
                }
                return pending.Count < dragged.Count;

            case DragOutResult.Copied:
            case DragOutResult.Linked:
                if (!removeCopied) return false;
                RemoveShown(dragged);
                return true;

            default:
                return false;
        }
    }

    public void ApplyMoves(IReadOnlyList<(string Source, string Destination)> moves)
    {
        foreach (var (source, destination) in moves)
        {
            var index = IndexOfPath(source);
            if (index < 0) continue;
            var old = Items[index];
            var replacement = new ShelfItem
            {
                Id = old.Id,
                Kind = old.Kind,
                Value = destination,
                SizeBytes = old.SizeBytes,
                ChildCount = old.ChildCount,
                TypeName = old.TypeName,
                Thumbnail = old.Thumbnail,
                Identity = FileIdentity.TryGet(destination)
            };
            // 已有的缩略图跟着新项目，之后照常按是否显示来保留或释放。
            if (old.Thumbnail is not null && _thumbnails.Remove(old, out var load)) _thumbnails[replacement] = load;
            Items[index] = replacement;
        }
    }

    public async Task<QuickActionResult> RunActionAsync(QuickActionInfo action, IReadOnlyList<ShelfItem> targets, IntPtr owner,
        Func<string?>? pickFolder)
    {
        Relink(targets);
        // 压缩、移动和交给程序打开都要有文件：文本和链接先转成文件，和别的文件一起处理。
        if (action.TakesText && targets.Any(item => !item.IsFileSystem))
        {
            var converted = ConvertToFiles(targets).ToDictionary(pair => pair.Original, pair => pair.File);
            targets = targets.Select(item => converted.GetValueOrDefault(item, item)).ToArray();
        }
        var available = targets.Where(item => item.IsFileSystem && item.RefreshAvailability()).ToArray();
        if (available.Length == 0)
        {
            var empty = new QuickActionResult(false, Items.Count == 0 ? Strings.AddFilesFirst : Strings.FilesAndFoldersOnly);
            Notify(empty.Message, false);
            return empty;
        }
        return await RunActionCoreAsync(action, available.Select(item => item.Value).ToArray(),
            available.Any(item => item.IsTemporaryAttachment), owner, pickFolder);
    }

    public async Task<QuickActionResult> RunActionAsync(QuickActionInfo action, DropReadResult dropped, IntPtr owner,
        Func<string?>? pickFolder)
    {
        foreach (var cache in dropped.CacheDirectories) AdoptCache(cache);
        if (dropped.FilePaths.Count == 0)
        {
            var failed = new QuickActionResult(false, dropped.ErrorMessage ?? Strings.FilesAndFoldersOnly);
            Notify(failed.Message, false);
            return failed;
        }
        return await RunActionCoreAsync(action, dropped.FilePaths, dropped.HasTemporaryFiles, owner, pickFolder);
    }

    private async Task<QuickActionResult> RunActionCoreAsync(QuickActionInfo action, IReadOnlyList<string> paths, bool temporary,
        IntPtr owner, Func<string?>? pickFolder)
    {
        HasInteracted = true;
        // 操作自己再持有一次输入所在的缓存，处理途中关闭置物架也不会删掉正在使用的文件。
        var held = _cacheDirectories.ToArray();
        foreach (var cache in held) VirtualFileDropService.RetainCache(cache);
        // “移动到”先弹出文件夹选择，选好后才显示进度。
        var asksForFolder = action.Type == QuickActionType.MoveTo;
        QuickActionResult result;
        try
        {
            if (action.ShowsProgress && !asksForFolder)
            {
                BusyText = action.BusyText;
                IsBusy = true;
            }
            result = await QuickActionService.ExecuteAsync(new QuickActionRequest(action, paths, temporary, owner,
                pickFolder is null ? null : () =>
                {
                    var folder = pickFolder();
                    if (folder is not null && action.ShowsProgress)
                    {
                        BusyText = action.BusyText;
                        IsBusy = true;
                    }
                    return folder;
                }));
        }
        catch (Exception exception)
        {
            LogService.Error($"快速操作“{action.Label}”失败", exception);
            result = new QuickActionResult(false, Strings.ActionFailed);
        }
        finally
        {
            IsBusy = false;
        }

        var delay = result.KeepsTemporaryFiles ? TemporaryFileGrace : TimeSpan.Zero;
        foreach (var cache in held) _ = DeleteCacheLaterAsync(cache, delay);
        if (_released)
        {
            // 置物架已在处理途中关闭：不再更新它，生成的文件放进新的置物架。
            var produced = result.ProducedNewItems ? result.OutputPaths : result.Replacements?.Select(pair => pair.Output).ToArray();
            if (produced is { Count: > 0 }) NewItemsProduced?.Invoke(this, produced);
            return result;
        }
        if (result.KeepsTemporaryFiles) _delayCacheCleanup = true;
        if (result.Moves is { Count: > 0 } moves) ApplyMoves(moves);
        if (result.Replacements is { Count: > 0 } replacements) ApplyReplacements(replacements);
        if (result is { ProducedNewItems: true, OutputPaths: { Count: > 0 } outputs })
        {
            // 空置物架直接放入；已有内容时放进新的置物架，不和原来的项目混在一起。
            if (IsEmpty) AddFiles(outputs);
            else NewItemsProduced?.Invoke(this, outputs);
        }
        if (!result.Cancelled) Notify(result.Message, result.Succeeded);
        return result;
    }

    /// <summary>用处理后的文件替换置物架中的原项目；原项目不在置物架上时（直接拖到按钮上）把新文件放进来。</summary>
    public void ApplyReplacements(IReadOnlyList<(string Source, string Output)> replacements)
    {
        var additions = new List<string>();
        foreach (var (source, output) in replacements)
        {
            var index = IndexOfPath(source);
            if (index < 0 || IndexOfPath(output) >= 0)
            {
                additions.Add(output);
                continue;
            }
            var item = new ShelfItem
            {
                Id = Items[index].Id,
                Kind = ShelfItemKind.File,
                Value = output,
                SizeBytes = ShellFileInfo.TryGetFileSize(output),
                TypeName = ShellFileInfo.GetTypeName(output),
                Identity = FileIdentity.TryGet(output)
            };
            Items[index] = item;
        }
        if (additions.Count > 0) AddFiles(additions);
    }

    /// <summary>临时附件已交给其他程序（拖出、复制、用程序打开），关闭置物架后稍晚再清理，避免对方读取时文件已被删除。</summary>
    public void KeepTemporaryFilesForAWhile() => _delayCacheCleanup = true;

    public bool CopyToClipboard(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0) return false;
        var available = items.Where(item => item.IsFileSystem && item.RefreshAvailability()).ToArray();
        var files = available.Select(item => item.Value).ToArray();
        var copied = files.Length > 0
            ? ClipboardService.TrySetFiles(files)
            : ClipboardService.TrySetText(string.Join(Environment.NewLine, items.Select(item => item.Value)));
        // 剪贴板里只有临时附件的路径，关闭置物架后可能才去粘贴。
        if (copied && available.Any(item => item.IsTemporaryAttachment)) KeepTemporaryFilesForAWhile();
        Notify(copied ? (files.Length > 0 ? Strings.CopiedFiles(files.Length) : Strings.CopiedText) : Strings.ClipboardBusy, copied);
        return copied;
    }

    public bool CopyPaths(IReadOnlyList<ShelfItem> items)
    {
        var files = items.Where(item => item.IsFileSystem).ToArray();
        var paths = files.Select(item => item.Value).ToArray();
        if (paths.Length == 0)
        {
            Notify(Strings.NoPathsToCopy, false);
            return false;
        }
        var copied = ClipboardService.TrySetText(string.Join(Environment.NewLine, paths));
        // 临时附件的路径可能在关闭置物架之后才被使用。
        if (copied && files.Any(item => item.IsTemporaryAttachment)) KeepTemporaryFilesForAWhile();
        Notify(copied ? (paths.Length > 1 ? Strings.CopiedPaths(paths.Length) : Strings.CopiedPath) : Strings.ClipboardBusy, copied);
        return copied;
    }

    public void Open(ShelfItem item)
    {
        try
        {
            switch (item.Kind)
            {
                case ShelfItemKind.Text:
                case ShelfItemKind.Link when !DropDataReader.IsSingleUrl(item.Value):
                    if (ClipboardService.TrySetText(item.Value)) Notify(Strings.CopiedText);
                    break;
                case ShelfItemKind.Link:
                    Process.Start(new ProcessStartInfo(item.Value) { UseShellExecute = true });
                    break;
                default:
                    Relink([item]);
                    if (!item.RefreshAvailability())
                    {
                        Notify(Strings.ItemMissing, false);
                        return;
                    }
                    // 打开的程序可能在置物架关闭后才读取文件。
                    if (item.IsTemporaryAttachment) KeepTemporaryFilesForAWhile();
                    Process.Start(new ProcessStartInfo(item.Value) { UseShellExecute = true });
                    break;
            }
        }
        catch (Exception exception)
        {
            LogService.Error("打开项目失败", exception);
            Notify(Strings.CannotOpenItem, false);
        }
    }

    public void Reveal(IReadOnlyList<ShelfItem> items)
    {
        var paths = items.Where(item => item.IsFileSystem).Select(item => item.Value).ToArray();
        if (paths.Length == 0 || !ShellInterop.OpenFolderAndSelect(paths)) Notify(Strings.CannotOpenFolder, false);
    }

    /// <summary>切换界面语言后刷新标题、汇总和项目说明。</summary>
    public void RefreshText()
    {
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(WindowTitle));
        OnPropertyChanged(nameof(StackCaption));
        OnPropertyChanged(nameof(StackDetail));
        foreach (var item in Items) item.RefreshText();
    }

    /// <summary>关闭置物架时清理 DropShelf 自己创建的临时附件。</summary>
    public void ReleaseResources()
    {
        if (_released) return;
        _released = true;
        // 还在排队的缩略图不再需要。
        _lifetime.Cancel();
        var caches = _cacheDirectories.ToArray();
        _cacheDirectories.Clear();
        var delay = _delayCacheCleanup ? TemporaryFileGrace : TimeSpan.Zero;
        foreach (var cache in caches) _ = DeleteCacheLaterAsync(cache, delay);
        Items.Clear();
    }

    private static readonly TimeSpan TemporaryFileGrace = TimeSpan.FromMinutes(10);

    private static async Task DeleteCacheLaterAsync(string cache, TimeSpan delay)
    {
        if (delay > TimeSpan.Zero) await Task.Delay(delay).ConfigureAwait(false);
        await VirtualFileDropService.ReleaseCacheAsync(cache).ConfigureAwait(false);
    }

    private async Task RemoveWhenGoneAsync(IReadOnlyList<ShelfItem> items)
    {
        await Task.Delay(TimeSpan.FromSeconds(2.5));
        _leaving.ExceptWith(items);
        RemoveShown(items.Where(item => _members.Contains(item) && !item.RefreshAvailability()));
    }

    private int IndexOfPath(string path)
    {
        for (var index = 0; index < Items.Count; index++)
        {
            if (Items[index].IsFileSystem && string.Equals(Items[index].Value, path, StringComparison.OrdinalIgnoreCase))
                return index;
        }
        return -1;
    }

    private ShelfItem? ItemFromEnd(int position) => Items.Count >= position ? Items[^position] : null;

    private string BuildSummary()
    {
        if (Items.Count == 0) return string.Empty;
        int files = 0, folders = 0, texts = 0, links = 0;
        long total = 0;
        foreach (var item in Items)
        {
            switch (item.Kind)
            {
                case ShelfItemKind.File:
                    files++;
                    total += item.SizeBytes ?? 0;
                    break;
                case ShelfItemKind.Folder:
                    folders++;
                    total += item.SizeBytes ?? 0;
                    break;
                case ShelfItemKind.Text: texts++; break;
                case ShelfItemKind.Link: links++; break;
            }
        }
        var parts = new List<string>();
        if (files > 0) parts.Add(Strings.FileCount(files));
        if (folders > 0) parts.Add(Strings.FolderCount(folders));
        if (texts > 0) parts.Add(Strings.TextCount(texts));
        if (links > 0) parts.Add(Strings.LinkCount(links));
        if (total > 0) parts.Add(ShelfItem.FormatSize(total));
        return string.Join(" · ", parts);
    }

    private void OnItemsChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null)
            foreach (ShelfItem item in e.OldItems)
            {
                item.PropertyChanged -= OnItemPropertyChanged;
                _members.Remove(item);
                _dragFiles.Remove(item);
                _handedOver.Remove(item);
                _leaving.Remove(item);
            }
        if (e.NewItems is not null)
            foreach (ShelfItem item in e.NewItems)
            {
                item.PropertyChanged += OnItemPropertyChanged;
                _members.Add(item);
            }
        if (e.Action == NotifyCollectionChangedAction.Reset)
        {
            // 清空时拿不到被移除的项目，按原来的成员取消订阅。
            foreach (var item in _members) item.PropertyChanged -= OnItemPropertyChanged;
            _members.Clear();
            foreach (var item in Items)
            {
                item.PropertyChanged += OnItemPropertyChanged;
                _members.Add(item);
            }
        }

        if (_batchDepth > 0) _batchChanged = true;
        else NotifyItemsChanged();
    }

    private void NotifyItemsChanged()
    {
        OnPropertyChanged(nameof(Count));
        OnPropertyChanged(nameof(HasItems));
        OnPropertyChanged(nameof(IsEmpty));
        OnPropertyChanged(nameof(HasMultiple));
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(WindowTitle));
        OnPropertyChanged(nameof(Stack0));
        OnPropertyChanged(nameof(Stack1));
        OnPropertyChanged(nameof(Stack2));
        OnPropertyChanged(nameof(StackSuit));
        OnPropertyChanged(nameof(StackCaption));
        OnPropertyChanged(nameof(StackDetail));
        if (Items.Count == 0 && IsExpanded) IsExpanded = false;
        UpdateThumbnails();
    }

    /// <summary>平铺里的牌生成或回收时由窗口告知，只给生成出来的牌加载缩略图。</summary>
    public void SetRowVisible(ShelfItem item, bool visible)
    {
        if (visible ? _visibleRows.Add(item) : _visibleRows.Remove(item)) UpdateThumbnails();
    }

    /// <summary>
    /// 只有堆叠顶部三张和平铺里生成出来的牌持有缩略图：其余的取消加载并释放，内存不随项目数量增长。
    /// 重新显示时多数能从缩略图服务的缓存里直接取到。
    /// </summary>
    private void UpdateThumbnails()
    {
        var wanted = new HashSet<ShelfItem>(_visibleRows.Where(_members.Contains));
        for (var position = 1; position <= 3; position++)
            if (ItemFromEnd(position) is { } top) wanted.Add(top);
        wanted.ExceptWith(_handedOver);

        foreach (var (item, load) in _thumbnails.ToArray())
        {
            if (wanted.Contains(item)) continue;
            load.Cancel();
            load.Dispose();
            _thumbnails.Remove(item);
            item.Thumbnail = null;
        }
        if (_released) return;
        foreach (var item in wanted)
        {
            if (!item.IsFileSystem || _thumbnails.ContainsKey(item)) continue;
            var load = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
            _thumbnails[item] = load;
            if (item.Thumbnail is null) _ = LoadThumbnailAsync(item, load.Token);
        }
    }

    private void OnItemPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_batchDepth > 0) return;
        if (e.PropertyName is nameof(ShelfItem.Detail) or nameof(ShelfItem.SizeBytes))
            OnPropertyChanged(nameof(StackDetail));
        // 文件在别处改了名：只有一个项目时堆叠下面显示的就是它的名称。
        else if (e.PropertyName is nameof(ShelfItem.DisplayName)) OnPropertyChanged(nameof(StackCaption));
    }

    /// <summary>同时统计的文件夹不超过两个：一次拖入很多文件夹时不把磁盘占满。</summary>
    private static readonly SemaphoreSlim FolderMeasuring = new(2);

    /// <summary>
    /// 在后台统计文件夹里有几项、一共多大，统计完回到界面线程写进项目，说明文字和汇总随之更新。
    /// 置物架关闭时取消；读不了的文件夹照旧只显示“文件夹”。
    /// </summary>
    private async Task MeasureFolderAsync(ShelfItem item)
    {
        var cancellationToken = _lifetime.Token;
        try
        {
            await FolderMeasuring.WaitAsync(cancellationToken).ConfigureAwait(false);
            FolderContents? measured;
            try
            {
                measured = await Task.Run(() => FolderContents.Measure(item.Value, cancellationToken), cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                FolderMeasuring.Release();
            }
            if (measured is not { } contents) return;
            void Apply()
            {
                if (cancellationToken.IsCancellationRequested) return;
                item.SizeBytes = contents.SizeBytes;
                item.ChildCount = contents.Children;
            }
            var dispatcher = System.Windows.Application.Current?.Dispatcher;
            if (dispatcher is null || dispatcher.CheckAccess()) Apply();
            else await dispatcher.InvokeAsync(Apply);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            LogService.Error("统计文件夹内容失败", exception);
        }
    }

    private async Task LoadThumbnailAsync(ShelfItem item, CancellationToken cancellationToken)
    {
        try
        {
            var thumbnail = await ShellThumbnailService.LoadAsync(item.Value, ThumbnailSize, cancellationToken);
            if (thumbnail is null) return;
            // 取消只发生在界面线程上，在这里检查不会和释放错开。
            var dispatcher = System.Windows.Application.Current?.Dispatcher;
            if (dispatcher is null || dispatcher.CheckAccess())
            {
                if (!cancellationToken.IsCancellationRequested) item.Thumbnail = thumbnail;
                return;
            }
            await dispatcher.InvokeAsync(() =>
            {
                if (!cancellationToken.IsCancellationRequested) item.Thumbnail = thumbnail;
            });
        }
        catch
        {
            // 缩略图不影响置物架的主要功能。
        }
    }

    private static string NewId() => Guid.NewGuid().ToString("N");
}
