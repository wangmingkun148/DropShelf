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
