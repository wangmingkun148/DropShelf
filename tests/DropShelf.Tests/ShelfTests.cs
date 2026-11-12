using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf;
using DropShelf.Interop;
using DropShelf.Localization;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using WpfApplication = System.Windows.Application;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static partial class Program
{
    // ───────────── 置物架逻辑 ─────────────

    private static void TestOneHundredFilesAndDuplicates()
    {
        var folder = CreateFolder("hundred-files");
        var paths = Enumerable.Range(1, 100).Select(index =>
        {
            var path = Path.Combine(folder, $"sample-{index:000}.txt");
            File.WriteAllText(path, index.ToString());
            return path;
        }).ToArray();

        var viewModel = new ShelfViewModel();
        var countNotifications = 0;
        viewModel.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(ShelfViewModel.Count)) countNotifications++; };
        var first = viewModel.AddFiles(paths);
        Assert(countNotifications == 1 && viewModel.StackDetail.StartsWith(Strings.FileCount(100)), "批量添加时数量通知没有合并或汇总不正确");
        var second = viewModel.AddFiles(paths);
        Assert(viewModel.Items.Count == 100, $"预期 100 项，实际 {viewModel.Items.Count} 项");
        Assert(first.Added == 100 && second.Duplicates == 100 && second.Message == "这些项目已在置物架中", "添加结果统计不正确");
        var missing = viewModel.AddFiles([Path.Combine(folder, "missing.txt")]);
        Assert(missing.Unavailable == 1 && viewModel.Items.Count == 100, "不存在的文件被添加");
        countNotifications = 0;
        viewModel.Remove(viewModel.Items.Take(60));
        Assert(countNotifications == 1 && viewModel.Items.Count == 40 && viewModel.StackDetail.StartsWith(Strings.FileCount(40)), "批量移除时数量通知没有合并或汇总不正确");
    }

    private static void TestAddingLockedFileReadsNoContent()
    {
        var folder = CreateFolder("path-only-drop");
        var path = Path.Combine(folder, "large-locked.bin");
        File.WriteAllBytes(path, [1, 2, 3]);
        using var lockStream = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([path]);
        Assert(viewModel.Items.Count == 1, "拖入时尝试读取了文件内容");
        Assert(viewModel.Items[0].SizeBytes == 3, "没有记录文件大小");
    }

    private static void TestClearKeepsFiles()
    {
        var folder = CreateFolder("clear-keeps-files");
        var path = Path.Combine(folder, "keep.txt");
        File.WriteAllText(path, "keep me");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([path, folder]);
        Assert(viewModel.Items.Any(item => item.Kind == ShelfItemKind.Folder), "文件夹没有识别为文件夹");
        viewModel.Clear();
        Assert(viewModel.Items.Count == 0, "置物架没有清空");
        Assert(File.ReadAllText(path) == "keep me", "清空置物架改变了磁盘文件");
    }

    private static void TestRemoveUndo()
    {
        var folder = CreateFolder("remove-undo");
        var paths = Enumerable.Range(1, 5).Select(index =>
        {
            var path = Path.Combine(folder, $"undo-{index}.txt");
            File.WriteAllText(path, index.ToString());
            return path;
        }).ToArray();
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles(paths);
        var order = viewModel.Items.ToArray();

        var removal = viewModel.Remove([order[3], order[1]]);
        Assert(removal is { Entries.Count: 2 } && viewModel.Items.Count == 3, "移除没有返回可撤销的记录");
        Assert(removal!.Entries[0].Index == 1 && removal.Entries[1].Index == 3, "移除记录里的位置不是从前到后");
        var restored = viewModel.Restore(removal);
        Assert(restored.Count == 2 && viewModel.Items.SequenceEqual(order), "撤销后项目没有回到原来的位置");
        Assert(viewModel.Remove([order[0]]) is not null && viewModel.Remove([order[0]]) is null, "移除不在置物架上的项目也返回了记录");
        viewModel.Restore(new ShelfRemoval([(order[0], 0)]));

        // 清空后又放上来一个相同的文件，撤销时不重复放回。
        var all = viewModel.Remove(viewModel.Items.ToArray());
        viewModel.AddFiles([paths[0]]);
        var back = viewModel.Restore(all!);
        Assert(back.Count == 4 && viewModel.Items.Count == 5, "撤销时重复放回了已经在置物架上的文件");
        Assert(viewModel.Items.Select(item => item.Value).SequenceEqual(paths), "清空后撤销的顺序不对");
    }

    private static void TestTextAndLinkEntries()
    {
        var viewModel = new ShelfViewModel();
        var summary = viewModel.AddEntries([
            new DroppedEntry(ShelfItemKind.Text, "一段文本"),
            new DroppedEntry(ShelfItemKind.Link, "https://example.com", "示例"),
            new DroppedEntry(ShelfItemKind.Text, "一段文本")
        ]);
        Assert(summary.Added == 2 && summary.Duplicates == 1, "文本或链接去重不正确");
        Assert(viewModel.Items[1].Title == "示例", "链接标题丢失");
    }

    private static void TestTextBecomesFiles()
    {
        var text = new ShelfItem { Id = "1", Kind = ShelfItemKind.Text, Value = "会议纪要：a/b?\n第二行" };
        var link = new ShelfItem { Id = "2", Kind = ShelfItemKind.Link, Value = "https://example.com/路径?q=1", Title = "示例: 首页" };
        var bare = new ShelfItem { Id = "3", Kind = ShelfItemKind.Link, Value = "https://example.com/a" };
        var odd = new ShelfItem { Id = "4", Kind = ShelfItemKind.Text, Value = "???" };
        var reserved = new ShelfItem { Id = "5", Kind = ShelfItemKind.Text, Value = "con" };
        Assert(ShelfItemFiles.FileName(text) == "会议纪要：a b.txt", $"文本的文件名不正确：{ShelfItemFiles.FileName(text)}");
        Assert(ShelfItemFiles.FileName(link) == "示例 首页.url" && ShelfItemFiles.FileName(bare) == "example.com.url", "链接的文件名不正确");
        Assert(ShelfItemFiles.FileName(odd) == "文本.txt" && ShelfItemFiles.FileName(reserved) == "_con.txt", "无法使用的文件名没有换掉");

        var folder = CreateFolder("text-files");
        var first = ShelfItemFiles.Write(text, folder);
        var second = ShelfItemFiles.Write(text, folder);
        Assert(first != second && Path.GetFileName(second) == "会议纪要：a b (2).txt", "同名时覆盖了已有的文件");
        Assert(File.ReadAllText(first) == text.Value && File.ReadAllBytes(first)[0] == 0xEF, "文本文件的内容或编码不正确");
        var shortcut = File.ReadAllText(ShelfItemFiles.Write(link, folder));
        Assert(shortcut.StartsWith("[InternetShortcut]") && shortcut.Contains("URL=https://example.com/%E8%B7%AF%E5%BE%84?q=1"), $"快捷方式的内容不正确：{shortcut}");

        var viewModel = new ShelfViewModel();
        viewModel.AddEntries([new DroppedEntry(ShelfItemKind.Text, "第一段"), new DroppedEntry(ShelfItemKind.Link, "https://example.com", "示例")]);
        var carried = viewModel.WriteForDrag(viewModel.Items.ToArray());
        Assert(carried.Count == 2 && carried.All(File.Exists) && viewModel.Items.All(item => !item.IsFileSystem), "和文件一起拖出时没有替文本写好文件，或改动了置物架上的项目");
        Assert(viewModel.WriteForDrag(viewModel.Items.ToArray()).SequenceEqual(carried), "再次拖出时没有接着用写好的文件");

        var ids = viewModel.Items.Select(item => item.Id).ToArray();
        var converted = viewModel.ConvertToFiles(viewModel.Items.ToArray());
        Assert(converted.Count == 2 && viewModel.Items.All(item => item.Kind == ShelfItemKind.File && item.IsTemporaryAttachment && File.Exists(item.Value)),
            "文本和链接没有原地转成文件");
        Assert(viewModel.Items.Select(item => item.Id).SequenceEqual(ids) && Path.GetExtension(viewModel.Items[1].Value) == ".url", "转成文件后位置或类型不对");
        var cache = viewModel.CacheDirectories.Single();
        viewModel.ReleaseResources();
        Assert(PumpUntil(() => !Directory.Exists(cache), 3000), "关闭置物架后没有清理文本转成的文件");

        Assert(QuickActions.Get(QuickActionType.CompressZip).TakesText && QuickActions.Get(QuickActionType.MoveTo).TakesText &&
               !QuickActions.Get(QuickActionType.Properties).TakesText && !QuickActions.Get(QuickActionType.CopyFullPath).TakesText,
            "哪些操作把文本当文件处理不正确");
    }

    private static async Task TestAdoptBetweenShelves()
    {
        var folder = CreateFolder("adopt");
        var paths = Enumerable.Range(1, 3).Select(index =>
        {
            var path = Path.Combine(folder, $"adopt-{index}.txt");
            File.WriteAllText(path, index.ToString());
            return path;
        }).ToArray();
        var source = new ShelfViewModel();
        source.AddFiles(paths);
        source.AddEntries([new DroppedEntry(ShelfItemKind.Text, "备注"), new DroppedEntry(ShelfItemKind.Link, "https://example.com", "示例")]);
        var target = new ShelfViewModel();
        target.AddFiles([paths[0]]);

        // 缩略图跟着项目走：对方接着用，这边移除时不清掉。
        var thumbnail = new System.Windows.Media.Imaging.WriteableBitmap(4, 4, 96, 96, System.Windows.Media.PixelFormats.Bgra32, null);
        thumbnail.Freeze();
        var moving = source.Items.ToArray();
        moving[2].Thumbnail = thumbnail;

        var added = target.Adopt(moving, source);
        Assert(added == 4 && target.Items.Count == 5, $"交过来的项目数量不对：{added}");
        Assert(ReferenceEquals(target.Items[1], moving[1]) && ReferenceEquals(target.Items[^1], moving[4]) && target.Items[^1].Title == "示例",
            "项目没有原样交过来，或顺序不对");
        Assert(target.HasInteracted && source.Items.Count == 5, "接收之后对方应该自己移除，这边不替它移除");
        source.Remove(moving);
        Assert(source.IsEmpty && target.Items.Count == 5 && ReferenceEquals(moving[2].Thumbnail, thumbnail), "来源移除项目时清掉了已经交出去的缩略图");
        Assert(target.Adopt(target.Items.ToArray(), target) == 0 && target.Items.Count == 5, "不应该把项目交给自己");
        source.ReleaseResources();
        Assert(new ShelfViewModel().Adopt([], source) == 0 && source.Adopt(target.Items.ToArray(), target) == 0, "已关闭的置物架不应该再接收项目");

        // 临时附件：缓存目录由两边共同持有，来源关闭后文件还在，最后一个关闭后才清理。
        var cache = VirtualFileDropService.CreateCacheDirectory();
        var file = Path.Combine(cache, "附件.txt");
        File.WriteAllText(file, "shared");
        var first = new ShelfViewModel();
        first.Add(new DropReadResult([new DroppedEntry(ShelfItemKind.File, file, IsTemporary: true)], [cache], 0, null));
        var second = new ShelfViewModel();
        Assert(second.Adopt(first.Items.ToArray(), first) == 1 && second.CacheDirectories.Count == 1, "接收临时附件时没有一起持有缓存");
        first.ReleaseResources();
        await Task.Delay(300);
        Assert(File.Exists(file), "来源置物架关闭后，交出去的临时附件被删除");
        second.ReleaseResources();
        await Task.Delay(300);
        Assert(!Directory.Exists(cache), "两个置物架都关闭后临时附件没有清理");
    }

    private static void TestStackSummary()
    {
        var folder = CreateFolder("stack-summary");
        var a = Path.Combine(folder, "a.txt");
        var b = Path.Combine(folder, "b.txt");
        File.WriteAllBytes(a, new byte[1024]);
        File.WriteAllBytes(b, new byte[2048]);
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([a]);
        Assert(viewModel.StackCaption == "a.txt" && viewModel.StackDetail.StartsWith("1 KB"), "单个项目的堆叠说明不正确");
        viewModel.AddFiles([b, folder]);
        Assert(ReferenceEquals(viewModel.Stack0, viewModel.Items[^1]), "堆叠最上面不是最新项目");
        Assert(viewModel.Stack2 is not null && viewModel.HasMultiple, "三张堆叠卡片不完整");
        Assert(viewModel.StackCaption == "3 个项目" && viewModel.StackDetail == "2 个文件 · 1 个文件夹 · 3 KB", $"汇总不正确：{viewModel.StackDetail}");
        Assert(viewModel.Title == "3 个项目", "标题不正确");
    }

    private static void TestFolderContents()
    {
        var folder = CreateFolder("folder-contents");
        File.WriteAllBytes(Path.Combine(folder, "a.bin"), new byte[1024]);
        var inner = Directory.CreateDirectory(Path.Combine(folder, "inner")).FullName;
        File.WriteAllBytes(Path.Combine(inner, "b.bin"), new byte[2048]);
        Directory.CreateDirectory(Path.Combine(folder, "empty"));

        Assert(FolderContents.Measure(folder) is { Children: 3, SizeBytes: 3072 }, "文件夹的项目数或大小不正确");
        Assert(FolderContents.Measure(Path.Combine(folder, "empty")) is { Children: 0, SizeBytes: 0 }, "空文件夹的统计不正确");
        Assert(FolderContents.Measure(Path.Combine(folder, "missing")) is null, "不存在的文件夹没有返回空");
        Assert(FolderContents.Measure(folder, FolderContents.SizeBudget, maximumFiles: 1) is { Children: 3, SizeBytes: null }, "文件太多时仍给出了大小");

        var item = new ShelfItem { Id = "1", Kind = ShelfItemKind.Folder, Value = folder };
        Assert(item.Detail == "文件夹", "还没统计时文件夹的说明不正确");
        item.SizeBytes = 3072;
        item.ChildCount = 3;
        Assert(item.Detail == "3 项 · 3 KB", $"文件夹的说明不正确：{item.Detail}");
        item.SizeBytes = null;
        Assert(item.Detail == "3 项", "不知道大小时文件夹的说明不正确");
        item.ChildCount = 0;
        Assert(item.Detail == "空文件夹", "空文件夹的说明不正确");
    }

    private static void TestApplyDragOut()
    {
        var folder = CreateFolder("drag-out");
        var paths = Enumerable.Range(1, 3).Select(index =>
        {
            var path = Path.Combine(folder, $"item-{index}.txt");
            File.WriteAllText(path, "content");
            return path;
        }).ToArray();

        var viewModel = new ShelfViewModel();
        viewModel.AddFiles(paths);
        Assert(!viewModel.ApplyDragOut([viewModel.Items[0]], DragOutResult.Cancelled, true) && viewModel.Items.Count == 3, "取消拖动时移除了项目");
        Assert(!viewModel.ApplyDragOut([viewModel.Items[0]], DragOutResult.Copied, false) && viewModel.Items.Count == 3, "设置为保留时仍移除了项目");
        Assert(viewModel.ApplyDragOut([viewModel.Items[0]], DragOutResult.Copied, true) && viewModel.Items.Count == 2, "复制拖出后没有移除项目");
        Assert(File.Exists(paths[0]), "移除置物架引用时删除了磁盘文件");

        var moved = viewModel.Items[0];
        File.Move(moved.Value, moved.Value + ".moved");
        Assert(viewModel.ApplyDragOut([moved], DragOutResult.Moved, false) && !viewModel.Items.Contains(moved), "源文件已移动但项目仍留在置物架");
    }

    private static void TestResolveDragOut()
    {
        const int dropped = NativeMethods.DRAGDROP_S_DROP;
        const int cancelled = NativeMethods.DRAGDROP_S_CANCEL;
        Assert(ShelfDragService.Resolve(cancelled, 0, null, null, false) == DragOutResult.Cancelled, "取消没有识别");
        Assert(ShelfDragService.Resolve(dropped, 1, null, null, false) == DragOutResult.Copied, "复制没有识别");
        Assert(ShelfDragService.Resolve(dropped, 2, null, null, false) == DragOutResult.Moved, "移动没有识别");
        Assert(ShelfDragService.Resolve(dropped, 4, null, null, false) == DragOutResult.Linked, "创建快捷方式没有识别");
        Assert(ShelfDragService.Resolve(dropped, 0, 0, 2, false) == DragOutResult.Moved, "资源管理器的优化移动没有识别");
        Assert(ShelfDragService.Resolve(dropped, 0, null, null, true) == DragOutResult.Moved, "源文件消失时没有判定为移动");
        Assert(ShelfDragService.Resolve(dropped, 0, null, null, false) == DragOutResult.Cancelled, "目标拒绝放下时被误判为成功");
    }

    private static void TestApplyMoves()
    {
        var folder = CreateFolder("apply-moves");
        var source = Path.Combine(folder, "before.txt");
        File.WriteAllText(source, "x");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([source]);
        var id = viewModel.Items[0].Id;
        viewModel.ApplyMoves([(source, Path.Combine(folder, "after.txt"))]);
        Assert(viewModel.Items.Count == 1 && viewModel.Items[0].Value.EndsWith("after.txt") && viewModel.Items[0].Id == id, "移动后路径没有更新");
    }

    private static void TestShelfHistory()
    {
        var folder = CreateFolder("history");
        var files = Enumerable.Range(1, 3).Select(index =>
        {
            var path = Path.Combine(folder, $"h{index}.txt");
            File.WriteAllText(path, "x");
            return path;
        }).ToArray();
        var historyPath = Path.Combine(_root, "history-test.json");
        var history = new ShelfHistoryService(historyPath);

        var viewModel = new ShelfViewModel();
        viewModel.AddFiles(files);
        viewModel.AddEntries([new DroppedEntry(ShelfItemKind.Text, "secret text"), new DroppedEntry(ShelfItemKind.Link, "https://example.com")]);
        viewModel.AddFiles([files[0] + ".tmp-copy"]);
        var temporary = Path.Combine(folder, "attachment.txt");
        File.WriteAllText(temporary, "x");
        viewModel.AddFiles([temporary], temporary: true);

        Assert(history.Record(viewModel.Items), "没有记录关闭的置物架");
        var entry = history.Latest!;
        Assert(entry.Items.Select(item => item.Path).SequenceEqual(files) && entry.Items[0].Name == "h1.txt", "记录的路径或名称不正确");
        history.Flush();
        var json = File.ReadAllText(historyPath);
        Assert(!json.Contains("secret text") && !json.Contains("example.com") && !json.Contains("attachment.txt"), "记录中包含了文本、链接或临时附件");
        Assert(!history.Record([]), "空置物架不应记录");

        history.Record(viewModel.Items.Reverse());
        Assert(history.Entries.Count == 1, "相同内容的置物架重复记录");
        for (var index = 0; index < 12; index++)
        {
            var path = Path.Combine(folder, $"single-{index}.txt");
            File.WriteAllText(path, "x");
            var single = new ShelfViewModel();
            single.AddFiles([path]);
            history.Record(single.Items);
        }
        Assert(history.Entries.Count == ShelfHistoryService.Capacity && history.Latest!.Items[0].Name == "single-11.txt", "最多保留 10 个");
        history.Flush();

        var reloaded = new ShelfHistoryService(historyPath);
        Assert(reloaded.Entries.Count == ShelfHistoryService.Capacity && reloaded.Latest!.Items[0].Path.EndsWith("single-11.txt"), "重新启动后记录丢失");
        var moved = reloaded.Entries[0];
        reloaded.Move(moved, 3);
        reloaded.Flush();
        Assert(reloaded.Entries[3] == moved && new ShelfHistoryService(historyPath).Entries[3].Items[0].Path == moved.Items[0].Path, "调整记录顺序没有生效或没有保存");
        reloaded.Move(moved, 0);
        reloaded.Remove(reloaded.Latest!);
        Assert(reloaded.Entries.Count == ShelfHistoryService.Capacity - 1, "恢复后记录没有移除");
        reloaded.Clear();
        reloaded.Flush();
        Assert(new ShelfHistoryService(historyPath).Entries.Count == 0, "清除记录没有保存");
        // 拖动卡片收尾时列表可能已被清空，延迟执行的换序不应出错。
        reloaded.Move(moved, 3);
        Assert(reloaded.Entries.Count == 0, "对已移除的记录换序不应改动列表");
    }

    private static void TestDragOutEffects()
    {
        Assert(ShelfDragService.GetAllowedEffects(TransferMode.Move, false) == 7, "移动模式应同时允许复制，便于拖到聊天软件");
        Assert(ShelfDragService.GetAllowedEffects(TransferMode.Copy, false) == 5, "复制模式不应允许移动");
        Assert(ShelfDragService.GetAllowedEffects(TransferMode.Move, true) == 3 && ShelfDragService.GetAllowedEffects(TransferMode.Copy, true) == 1,
            "临时附件不应提供快捷方式");
    }
}
