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

    public ShelfAddSummary Add(DropReadResult result)
    {
        foreach (var cache in result.CacheDirectories) AdoptCache(cache);
        return AddEntries(result.Entries);
    }

    /// <summary>先在后台限量读取文件信息（网络路径可能很慢），再一次放入，顺序和去重规则与 <see cref="AddFiles"/> 相同。</summary>
    public async Task<ShelfAddSummary> AddFilesAsync(IReadOnlyList<string> paths)
    {
        var facts = new System.Collections.Concurrent.ConcurrentDictionary<string, FileFacts?>(StringComparer.OrdinalIgnoreCase);
        await Parallel.ForEachAsync(paths.Where(path => !string.IsNullOrWhiteSpace(path)).Distinct(StringComparer.OrdinalIgnoreCase),
            new ParallelOptions { MaxDegreeOfParallelism = 4 }, (path, _) =>
            {
                facts[path] = ReadFacts(path);
                return ValueTask.CompletedTask;
            });
        if (_released) return new ShelfAddSummary(0, 0, paths.Count);
        return AddEntries(paths.Select(path => new DroppedEntry(ShelfItemKind.File, path)), facts);
    }

    public ShelfAddSummary AddEntries(IEnumerable<DroppedEntry> entries) => AddEntries(entries, null);

    private ShelfAddSummary AddEntries(IEnumerable<DroppedEntry> entries, IReadOnlyDictionary<string, FileFacts?>? facts)
    {
        // 一次放入很多项目时，数量、标题和汇总等派生属性在全部放入后只通知一次。
        _batchDepth++;
        try
        {
            return AddEntriesCore(entries, facts);
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

    private readonly record struct FileFacts(bool IsFolder, long? SizeBytes);

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
        return new FileFacts(isFolder, isFolder ? null : ShellFileInfo.TryGetFileSize(path));
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
                IsTemporaryAttachment = entry.IsTemporary
            };
            if (!facts.IsFolder)
            {
                item.SizeBytes = facts.SizeBytes;
                item.TypeName = ShellFileInfo.GetTypeName(entry.Value);
            }
            Items.Add(item);
            existingPaths.Add(entry.Value);
            added++;
        }

        if (added > 0) HasInteracted = true;
        return new ShelfAddSummary(added, duplicates, unavailable);
    }

    public void Remove(IEnumerable<ShelfItem> items)
    {
        foreach (var item in items.ToArray()) Items.Remove(item);
    }

    public void Clear() => Items.Clear();

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

    /// <summary>处理拖出结果；返回置物架内容是否发生变化。</summary>
    public bool ApplyDragOut(IReadOnlyList<ShelfItem> dragged, DragOutResult result, bool removeCopied)
    {
        switch (result)
        {
            case DragOutResult.Moved:
                var pending = new List<ShelfItem>();
                foreach (var item in dragged)
                {
                    if (!item.IsFileSystem || !item.RefreshAvailability()) Items.Remove(item);
                    else pending.Add(item);
                }
                // 资源管理器可能在后台继续移动大文件，稍后再确认一次。
                if (pending.Count > 0) _ = RemoveWhenGoneAsync(pending);
                return pending.Count < dragged.Count;

            case DragOutResult.Copied:
            case DragOutResult.Linked:
                if (!removeCopied) return false;
                Remove(dragged);
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
                TypeName = old.TypeName,
                Thumbnail = old.Thumbnail
            };
            // 已有的缩略图跟着新项目，之后照常按是否显示来保留或释放。
            if (old.Thumbnail is not null && _thumbnails.Remove(old, out var load)) _thumbnails[replacement] = load;
            Items[index] = replacement;
        }
    }

    public async Task<QuickActionResult> RunActionAsync(QuickActionInfo action, IReadOnlyList<ShelfItem> targets, IntPtr owner,
        Func<string?>? pickFolder)
    {
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
                TypeName = ShellFileInfo.GetTypeName(output)
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
        foreach (var item in items)
        {
            if (Items.Contains(item) && !item.RefreshAvailability()) Items.Remove(item);
        }
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
                case ShelfItemKind.Folder: folders++; break;
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
            foreach (ShelfItem item in e.OldItems) item.PropertyChanged -= OnItemPropertyChanged;
        if (e.NewItems is not null)
            foreach (ShelfItem item in e.NewItems) item.PropertyChanged += OnItemPropertyChanged;
        if (e.Action == NotifyCollectionChangedAction.Reset)
            foreach (var item in Items) item.PropertyChanged += OnItemPropertyChanged;

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
        OnPropertyChanged(nameof(StackCaption));
        OnPropertyChanged(nameof(StackDetail));
        if (Items.Count == 0 && IsExpanded) IsExpanded = false;
        UpdateThumbnails();
    }

    /// <summary>列表里的行显示或移出时由窗口告知，只给显示出来的行加载缩略图。</summary>
    public void SetRowVisible(ShelfItem item, bool visible)
    {
        if (visible ? _visibleRows.Add(item) : _visibleRows.Remove(item)) UpdateThumbnails();
    }

    /// <summary>
    /// 只有堆叠顶部三张和列表里显示出来的行持有缩略图：其余的取消加载并释放，内存不随项目数量增长。
    /// 重新显示时多数能从缩略图服务的缓存里直接取到。
    /// </summary>
    private void UpdateThumbnails()
    {
        var wanted = new HashSet<ShelfItem>(_visibleRows.Where(Items.Contains));
        for (var position = 1; position <= 3; position++)
            if (ItemFromEnd(position) is { } top) wanted.Add(top);

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
