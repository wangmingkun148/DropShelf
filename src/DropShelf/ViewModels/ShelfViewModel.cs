using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using DropShelf.Interop;
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
            if (Added == 0 && Duplicates > 0 && Unavailable == 0) return "这些项目已在置物架中";
            if (Added == 0) return Unavailable > 0 ? "项目无法访问，未能添加" : "没有可添加的内容";
            var message = $"已添加 {Added} 项";
            if (Duplicates > 0) message += $"，忽略 {Duplicates} 个重复项";
            if (Unavailable > 0) message += $"，{Unavailable} 项无法访问";
            return message;
        }
    }
}

public sealed class ShelfViewModel : ObservableObject
{
    private readonly HashSet<string> _cacheDirectories = new(StringComparer.OrdinalIgnoreCase);
    private IReadOnlyList<QuickActionInfo> _actions = [];
    private bool _isExpanded;
    private bool _isPinned;
    private bool _isBusy;
    private string _busyText = string.Empty;
    private bool _delayCacheCleanup;
    private bool _released;

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
    public string Title => Items.Count == 0 ? "置物架" : $"{Items.Count} 个项目";

    /// <summary>堆叠最上面是最新放入的项目。</summary>
    public ShelfItem? Stack0 => ItemFromEnd(1);
    public ShelfItem? Stack1 => ItemFromEnd(2);
    public ShelfItem? Stack2 => ItemFromEnd(3);

    public string StackCaption => Items.Count switch
    {
        0 => string.Empty,
        1 => Items[0].DisplayName,
        _ => $"{Items.Count} 个项目"
    };

    public string StackDetail => Items.Count == 1 ? Items[0].Detail : BuildSummary();

    public bool IsExpanded
    {
        get => _isExpanded;
        set => SetField(ref _isExpanded, value);
    }

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

    /// <summary>用户已经点击、钉住或放入过内容，空置物架也不再自动关闭。</summary>
    public bool HasInteracted { get; set; }

    public bool HasTemporaryFiles => Items.Any(item => item.IsTemporaryAttachment);

    public IReadOnlyCollection<string> CacheDirectories => _cacheDirectories;

    public event EventHandler<ShelfToast>? ToastRequested;

    public void Notify(string message, bool success = true) => ToastRequested?.Invoke(this, new ShelfToast(message, success));

    public ShelfAddSummary AddFiles(IEnumerable<string> paths, bool temporary = false) =>
        AddEntries(paths.Select(path => new DroppedEntry(ShelfItemKind.File, path, IsTemporary: temporary)));

    public ShelfAddSummary Add(DropReadResult result)
    {
        foreach (var cache in result.CacheDirectories) RegisterCache(cache);
        return AddEntries(result.Entries);
    }

    public ShelfAddSummary AddEntries(IEnumerable<DroppedEntry> entries)
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

            FileAttributes attributes;
            try
            {
                attributes = File.GetAttributes(entry.Value);
            }
            catch (Exception exception) when (exception is FileNotFoundException or DirectoryNotFoundException
                                                  or UnauthorizedAccessException or IOException or ArgumentException
                                                  or NotSupportedException)
            {
                unavailable++;
                continue;
            }

            var isFolder = attributes.HasFlag(FileAttributes.Directory);
            var item = new ShelfItem
            {
                Id = NewId(),
                Kind = isFolder ? ShelfItemKind.Folder : ShelfItemKind.File,
                Value = entry.Value,
                IsTemporaryAttachment = entry.IsTemporary
            };
            if (!isFolder)
            {
                item.SizeBytes = ShellFileInfo.TryGetFileSize(entry.Value);
                item.TypeName = ShellFileInfo.GetTypeName(entry.Value);
            }
            Items.Add(item);
            existingPaths.Add(entry.Value);
            _ = LoadThumbnailAsync(item);
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

    public void RegisterCache(string? cacheDirectory)
    {
        if (!string.IsNullOrWhiteSpace(cacheDirectory)) _cacheDirectories.Add(cacheDirectory);
    }

    public void RefreshAvailability()
    {
        foreach (var item in Items) item.RefreshAvailability();
    }

    /// <summary>处理拖出结果；返回置物架内容是否发生变化。</summary>
    public bool ApplyDragOut(IReadOnlyList<ShelfItem> dragged, DragOutResult result, bool removeAfterDragOut)
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
                if (!removeAfterDragOut) return false;
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
            Items[index] = replacement;
        }
    }

    public async Task<QuickActionResult> RunActionAsync(QuickActionType type, IReadOnlyList<ShelfItem> targets, IntPtr owner,
        Func<string?>? pickFolder)
    {
        var available = targets.Where(item => item.IsFileSystem && item.RefreshAvailability()).ToArray();
        if (available.Length == 0)
        {
            var empty = new QuickActionResult(false, Items.Count == 0 ? "先把文件放进置物架" : "这个操作只支持文件或文件夹");
            Notify(empty.Message, false);
            return empty;
        }
        return await RunActionCoreAsync(type, available.Select(item => item.Value).ToArray(),
            available.Any(item => item.IsTemporaryAttachment), owner, pickFolder);
    }

    public async Task<QuickActionResult> RunActionAsync(QuickActionType type, DropReadResult dropped, IntPtr owner,
        Func<string?>? pickFolder)
    {
        foreach (var cache in dropped.CacheDirectories) RegisterCache(cache);
        if (dropped.FilePaths.Count == 0)
        {
            var failed = new QuickActionResult(false, dropped.ErrorMessage ?? "这个操作只支持文件或文件夹");
            Notify(failed.Message, false);
            return failed;
        }
        return await RunActionCoreAsync(type, dropped.FilePaths, dropped.HasTemporaryFiles, owner, pickFolder);
    }

    private async Task<QuickActionResult> RunActionCoreAsync(QuickActionType type, IReadOnlyList<string> paths, bool temporary,
        IntPtr owner, Func<string?>? pickFolder)
    {
        HasInteracted = true;
        var info = QuickActions.Get(type);
        var showBusy = type is QuickActionType.CompressZip or QuickActionType.MoveTo;
        QuickActionResult result;
        try
        {
            if (showBusy && type != QuickActionType.MoveTo)
            {
                BusyText = info.BusyText;
                IsBusy = true;
            }
            result = await QuickActionService.ExecuteAsync(new QuickActionRequest(type, paths, temporary, owner,
                pickFolder is null ? null : () =>
                {
                    var folder = pickFolder();
                    if (folder is not null && showBusy)
                    {
                        BusyText = info.BusyText;
                        IsBusy = true;
                    }
                    return folder;
                }));
        }
        catch (Exception exception)
        {
            LogService.Error($"快速操作“{info.Label}”失败", exception);
            result = new QuickActionResult(false, "操作没有完成，原文件没有改变");
        }
        finally
        {
            IsBusy = false;
        }

        if (type == QuickActionType.Properties && temporary) _delayCacheCleanup = true;
        if (result.Moves is { Count: > 0 } moves) ApplyMoves(moves);
        if (result is { ProducedNewItems: true, OutputPaths: { Count: > 0 } outputs }) AddFiles(outputs);
        if (!result.Cancelled) Notify(result.Message, result.Succeeded);
        return result;
    }

    public bool CopyToClipboard(IReadOnlyList<ShelfItem> items)
    {
        if (items.Count == 0) return false;
        var files = items.Where(item => item.IsFileSystem && item.RefreshAvailability()).Select(item => item.Value).ToArray();
        var copied = files.Length > 0
            ? ClipboardService.TrySetFiles(files)
            : ClipboardService.TrySetText(string.Join(Environment.NewLine, items.Select(item => item.Value)));
        Notify(copied ? (files.Length > 0 ? $"已复制 {files.Length} 个文件" : "已复制文本") : "剪贴板正被占用，请重试", copied);
        return copied;
    }

    public bool CopyPaths(IReadOnlyList<ShelfItem> items)
    {
        var paths = items.Where(item => item.IsFileSystem).Select(item => item.Value).ToArray();
        if (paths.Length == 0)
        {
            Notify("没有可复制的路径", false);
            return false;
        }
        var copied = ClipboardService.TrySetText(string.Join(Environment.NewLine, paths));
        Notify(copied ? (paths.Length > 1 ? $"已复制 {paths.Length} 个路径" : "已复制路径") : "剪贴板正被占用，请重试", copied);
        return copied;
    }

    public void Open(ShelfItem item)
    {
        try
        {
            switch (item.Kind)
            {
                case ShelfItemKind.Text:
                    if (ClipboardService.TrySetText(item.Value)) Notify("已复制文本");
                    break;
                case ShelfItemKind.Link:
                    Process.Start(new ProcessStartInfo(item.Value) { UseShellExecute = true });
                    break;
                default:
                    if (!item.RefreshAvailability())
                    {
                        Notify("项目已被移动或删除", false);
                        return;
                    }
                    Process.Start(new ProcessStartInfo(item.Value) { UseShellExecute = true });
                    break;
            }
        }
        catch (Exception exception)
        {
            LogService.Error("打开项目失败", exception);
            Notify("无法打开这个项目", false);
        }
    }

    public void Reveal(IReadOnlyList<ShelfItem> items)
    {
        var paths = items.Where(item => item.IsFileSystem).Select(item => item.Value).ToArray();
        if (paths.Length == 0 || !ShellInterop.OpenFolderAndSelect(paths)) Notify("无法打开所在文件夹", false);
    }

    /// <summary>关闭置物架时清理 DropShelf 自己创建的临时附件。</summary>
    public void ReleaseResources()
    {
        if (_released) return;
        _released = true;
        var caches = _cacheDirectories.ToArray();
        _cacheDirectories.Clear();
        var delay = _delayCacheCleanup ? TimeSpan.FromMinutes(10) : TimeSpan.Zero;
        foreach (var cache in caches) _ = DeleteCacheLaterAsync(cache, delay);
        Items.Clear();
    }

    private static async Task DeleteCacheLaterAsync(string cache, TimeSpan delay)
    {
        if (delay > TimeSpan.Zero) await Task.Delay(delay).ConfigureAwait(false);
        await VirtualFileDropService.DeleteCacheAsync(cache).ConfigureAwait(false);
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
        var files = Items.Count(item => item.Kind == ShelfItemKind.File);
        var folders = Items.Count(item => item.Kind == ShelfItemKind.Folder);
        var texts = Items.Count(item => item.Kind == ShelfItemKind.Text);
        var links = Items.Count(item => item.Kind == ShelfItemKind.Link);
        var parts = new List<string>();
        if (files > 0) parts.Add($"{files} 个文件");
        if (folders > 0) parts.Add($"{folders} 个文件夹");
        if (texts > 0) parts.Add($"{texts} 段文本");
        if (links > 0) parts.Add($"{links} 个链接");
        var total = Items.Where(item => item.Kind == ShelfItemKind.File).Sum(item => item.SizeBytes ?? 0);
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

        OnPropertyChanged(nameof(Count));
        OnPropertyChanged(nameof(HasItems));
        OnPropertyChanged(nameof(IsEmpty));
        OnPropertyChanged(nameof(HasMultiple));
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(Stack0));
        OnPropertyChanged(nameof(Stack1));
        OnPropertyChanged(nameof(Stack2));
        OnPropertyChanged(nameof(StackCaption));
        OnPropertyChanged(nameof(StackDetail));
        if (Items.Count <= 1 && IsExpanded) IsExpanded = false;
    }

    private void OnItemPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ShelfItem.Detail) or nameof(ShelfItem.SizeBytes))
            OnPropertyChanged(nameof(StackDetail));
    }

    private async Task LoadThumbnailAsync(ShelfItem item)
    {
        try
        {
            var thumbnail = await ShellThumbnailService.LoadAsync(item.Value, 128);
            if (thumbnail is null) return;
            var dispatcher = System.Windows.Application.Current?.Dispatcher;
            if (dispatcher is null || dispatcher.CheckAccess())
            {
                item.Thumbnail = thumbnail;
                return;
            }
            await dispatcher.InvokeAsync(() =>
            {
                if (Items.Contains(item)) item.Thumbnail = thumbnail;
            });
        }
        catch
        {
            // 缩略图不影响置物架的主要功能。
        }
    }

    private static string NewId() => Guid.NewGuid().ToString("N");
}
