using System.IO;
using System.IO.Compression;
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
    // ───────────── 文件与快速操作 ─────────────

    private static async Task TestCopyKeepsFileMetadata()
    {
        var source = Path.Combine(CreateFolder("metadata-source"), "download.txt");
        File.WriteAllText(source, "downloaded");
        File.WriteAllText(source + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        var modified = new DateTime(2020, 1, 2, 3, 4, 5, DateTimeKind.Utc);
        File.SetLastWriteTimeUtc(source, modified);

        var results = await new FileOperationService().CopyAsync([source], CreateFolder("metadata-destination"));
        var copy = results.Single().Destination!;
        Assert(results[0].Succeeded && File.ReadAllText(copy + ":Zone.Identifier").Contains("ZoneId=3"), "复制后丢失了下载来源标记");
        Assert(File.GetLastWriteTimeUtc(copy) == modified, "复制后修改时间被改成了当前时间");
    }

    private static async Task TestFolderLinksAreRejected()
    {
        var source = CreateFolder("link-source");
        var linked = CreateFolder("link-target");
        File.WriteAllText(Path.Combine(linked, "inside.txt"), "linked");
        var junction = Path.Combine(source, "junction");
        using (var mklink = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{junction}\" \"{linked}\"")
               { CreateNoWindow = true, UseShellExecute = false })!)
            mklink.WaitForExit();
        Assert(Directory.Exists(junction), "无法创建测试用的联接点");

        var destination = CreateFolder("link-destination");
        var results = await new FileOperationService().CopyAsync([source], destination);
        Assert(results.Count == 1 && !results[0].Succeeded && results[0].ErrorMessage == Strings.FolderContainsLinks, "含联接点的文件夹没有明确报告失败");
        Assert(!Directory.EnumerateFileSystemEntries(destination).Any(), "失败时留下了不完整的副本");
        Assert(Directory.Exists(junction) && File.Exists(Path.Combine(linked, "inside.txt")), "源文件夹被改动");
    }

    private static async Task TestCopyDoesNotOverwrite()
    {
        var sourceFolder = CreateFolder("copy-source");
        var destinationFolder = CreateFolder("copy-destination");
        var source = Path.Combine(sourceFolder, "same-name.txt");
        var existing = Path.Combine(destinationFolder, "same-name.txt");
        File.WriteAllText(source, "new content");
        File.WriteAllText(existing, "existing content");

        var results = await new FileOperationService().CopyAsync([source], destinationFolder);
        Assert(results.Count == 1 && results[0].Succeeded, "复制没有成功");
        Assert(File.ReadAllText(existing) == "existing content", "已有同名文件被覆盖");
        Assert(File.ReadAllText(Path.Combine(destinationFolder, "same-name (2).txt")) == "new content", "未生成正确的重名副本");
    }

    private static async Task TestMissingSourceIsReported()
    {
        var destination = CreateFolder("missing-destination");
        var results = await new FileOperationService().CopyAsync([Path.Combine(_root, "not-created.txt")], destination);
        Assert(results.Count == 1 && !results[0].Succeeded && !string.IsNullOrWhiteSpace(results[0].ErrorMessage), "不存在的源文件没有被报告为失败");
    }

    private static async Task TestLockedFileIsReported()
    {
        var sourceFolder = CreateFolder("locked-source");
        var destination = CreateFolder("locked-destination");
        var source = Path.Combine(sourceFolder, "locked.txt");
        File.WriteAllText(source, "locked content");
        await using var lockStream = new FileStream(source, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        var results = await new FileOperationService().CopyAsync([source], destination);
        Assert(results.Count == 1 && !results[0].Succeeded && File.Exists(source), "被占用文件没有被报告为失败");
    }

    private static async Task TestNestedDestinationIsRejected()
    {
        var source = CreateFolder("nested-source");
        File.WriteAllText(Path.Combine(source, "file.txt"), "content");
        var results = await new FileOperationService().CopyAsync([source], Path.Combine(source, "child"));
        Assert(results.Count == 1 && !results[0].Succeeded, "复制到文件夹自身内部未被阻止");

        var same = await new FileOperationService().CopyAsync([source], source);
        Assert(same.Count == 1 && !same[0].Succeeded && !Directory.Exists(Path.Combine(source, "nested-source")),
            "复制到文件夹自身未被阻止");

        // 目标路径本身或它的上级是指向源文件夹的联接点。
        var junction = Path.Combine(CreateFolder("nested-links"), "to-source");
        CreateJunction(junction, source);
        var viaLink = await new FileOperationService().CopyAsync([source], Path.Combine(junction, "inner"));
        Assert(viaLink.Count == 1 && !viaLink[0].Succeeded && !Directory.Exists(Path.Combine(source, "inner", "nested-source")),
            "通过联接点复制到文件夹自身内部未被阻止");
        var parentJunction = Path.Combine(CreateFolder("nested-parent-links"), "root");
        CreateJunction(parentJunction, Path.GetDirectoryName(source)!);
        var viaParent = await new FileOperationService().CopyAsync([source], Path.Combine(parentJunction, "nested-source", "deep"));
        Assert(viaParent.Count == 1 && !viaParent[0].Succeeded, "通过上级目录的联接点复制到文件夹自身内部未被阻止");
    }

    private static void CreateJunction(string link, string target)
    {
        using (var mklink = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"")
               { CreateNoWindow = true, UseShellExecute = false })!)
            mklink.WaitForExit();
        Assert(Directory.Exists(link), "无法创建测试用的联接点");
    }

    private static void TestArchiveActions()
    {
        var folder = CreateFolder("quick-action-zip");
        var source = Path.Combine(folder, "quick-action.txt");
        File.WriteAllText(source, "quick action content");

        var compressed = ArchiveService.Archive([source]);
        Assert(compressed.Succeeded && File.Exists(compressed.OutputPath), compressed.Message);
        using (var archive = ZipFile.OpenRead(compressed.OutputPath!))
        {
            var entry = archive.Entries.SingleOrDefault(item => item.FullName == "quick-action.txt");
            Assert(entry is not null, "ZIP 中没有原文件");
            using var reader = new StreamReader(entry!.Open());
            Assert(reader.ReadToEnd() == "quick action content", "ZIP 中的内容不正确");
        }

        var extracted = ArchiveService.Archive([compressed.OutputPath!]);
        Assert(extracted.Succeeded && Directory.Exists(extracted.OutputPath), extracted.Message);
        Assert(File.ReadAllText(Path.Combine(extracted.OutputPath!, "quick-action.txt")) == "quick action content", "ZIP 解压后的文件内容不正确");

        // 来自网络的压缩包：解出的文件带上同样的来源区域。
        File.WriteAllText(compressed.OutputPath! + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        var downloaded = ArchiveService.Archive([compressed.OutputPath!]);
        Assert(downloaded.Succeeded && File.ReadAllText(Path.Combine(downloaded.OutputPath!, "quick-action.txt") + ":Zone.Identifier").Contains("ZoneId=3"),
            "解压来自网络的 ZIP 后没有传递下载来源标记");

        var unsafeZip = Path.Combine(folder, "unsafe.zip");
        using (var archive = ZipFile.Open(unsafeZip, ZipArchiveMode.Create)) archive.CreateEntry("../escaped.txt");
        Assert(!ArchiveService.Archive([unsafeZip]).Succeeded && !File.Exists(Path.Combine(folder, "escaped.txt")), "含有不安全路径的 ZIP 被错误解压");

        var rootEntryZip = Path.Combine(folder, "root-entry.zip");
        using (var archive = ZipFile.Open(rootEntryZip, ZipArchiveMode.Create))
        {
            archive.CreateEntry("./");
            using var writer = new StreamWriter(archive.CreateEntry("inside.txt").Open());
            writer.Write("inside root entry archive");
        }
        var rootEntryResult = ArchiveService.Archive([rootEntryZip]);
        Assert(rootEntryResult.Succeeded && File.ReadAllText(Path.Combine(rootEntryResult.OutputPath!, "inside.txt")) == "inside root entry archive",
            "包含根目录项的 ZIP 解压不正确");

        if (ArchiveService.IsNative7ZipAvailable())
        {
            var sevenZipSource = Path.Combine(folder, "native-seven.txt");
            var sevenZipPath = Path.Combine(folder, "native-seven.7z");
            File.WriteAllText(sevenZipSource, "native 7z content");
            var tar = new System.Diagnostics.ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"))
            {
                UseShellExecute = false,
                CreateNoWindow = true
            };
            foreach (var argument in new[] { "-caf", sevenZipPath, "-C", folder, Path.GetFileName(sevenZipSource) }) tar.ArgumentList.Add(argument);
            using (var process = System.Diagnostics.Process.Start(tar)!) process.WaitForExit();
            var extracted7Zip = ArchiveService.Archive([sevenZipPath]);
            Assert(extracted7Zip.Succeeded && File.ReadAllText(Path.Combine(extracted7Zip.OutputPath!, "native-seven.txt")) == "native 7z content",
                "Windows 内置 7z 解压结果不正确");
        }
    }

    private static async Task TestCopyPathAction()
    {
        var folder = CreateFolder("copy-path");
        var a = Path.Combine(folder, "a.txt");
        var b = Path.Combine(folder, "b.txt");
        File.WriteAllText(a, "a");
        File.WriteAllText(b, "b");
        var result = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActions.Get(QuickActionType.CopyFullPath), [a, b], false, IntPtr.Zero));
        Assert(result.Succeeded, result.Message);
        Assert(System.Windows.Clipboard.GetText() == a + Environment.NewLine + b, "剪贴板中的路径不正确");
    }

    private static async Task TestMoveToAction()
    {
        var folder = CreateFolder("move-to");
        var destination = CreateFolder("move-to-destination");
        var source = Path.Combine(folder, "move-me.txt");
        File.WriteAllText(source, "move");

        var cancelled = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActions.Get(QuickActionType.MoveTo), [source], false, IntPtr.Zero, () => null));
        Assert(cancelled.Cancelled && File.Exists(source), "取消选择文件夹后仍然移动了文件");

        var moved = await QuickActionService.ExecuteAsync(new QuickActionRequest(QuickActions.Get(QuickActionType.MoveTo), [source], false, IntPtr.Zero, () => destination));
        Assert(moved.Succeeded && moved.Moves!.Single().Destination == Path.Combine(destination, "move-me.txt"), moved.Message);
        Assert(!File.Exists(source) && File.ReadAllText(Path.Combine(destination, "move-me.txt")) == "move", "文件没有移动到目标位置");
    }

    private static async Task TestArchiveAddsOutputToShelf()
    {
        var folder = CreateFolder("archive-shelf");
        var source = Path.Combine(folder, "report.txt");
        File.WriteAllText(source, "report");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([source]);
        var toasts = new List<ShelfToast>();
        viewModel.ToastRequested += (_, toast) => toasts.Add(toast);
        IReadOnlyList<string>? produced = null;
        viewModel.NewItemsProduced += (_, outputs) => produced = outputs;
        var result = await viewModel.RunActionAsync(QuickActions.Get(QuickActionType.CompressZip), viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(result.Succeeded && viewModel.Items.Count == 1 && produced?.Single().EndsWith("report.zip") == true,
            "压缩结果没有放进新的置物架");
        Assert(toasts.Single().Success && !viewModel.IsBusy, "操作结果没有提示");

        var empty = await new ShelfViewModel().RunActionAsync(QuickActions.Get(QuickActionType.CompressZip), [], IntPtr.Zero, null);
        Assert(!empty.Succeeded, "空置物架执行操作没有提示");
    }

    private static void TestCopyFilesToClipboard()
    {
        var folder = CreateFolder("clipboard-files");
        var file = Path.Combine(folder, "copy.txt");
        File.WriteAllText(file, "copy");
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([file]);
        Assert(viewModel.CopyToClipboard(viewModel.Items.ToArray()), "复制文件失败");
        var list = System.Windows.Clipboard.GetFileDropList();
        Assert(list.Count == 1 && list[0] == file, "剪贴板中没有文件");
    }

    private static async Task TestCustomFolderAction()
    {
        var folder = CreateFolder("custom-folder-source");
        var target = CreateFolder("custom-folder-target");
        var copySource = Path.Combine(folder, "copy.txt");
        var moveSource = Path.Combine(folder, "move.txt");
        File.WriteAllText(copySource, "copy");
        File.WriteAllText(moveSource, "move");

        var copy = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = target, FolderMode = TransferMode.Copy });
        var copied = await QuickActionService.ExecuteAsync(new QuickActionRequest(copy, [copySource], false, IntPtr.Zero));
        Assert(copied.Succeeded && File.Exists(copySource) && File.ReadAllText(Path.Combine(target, "copy.txt")) == "copy", copied.Message);

        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([moveSource]);
        var move = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = target });
        var moved = await viewModel.RunActionAsync(move, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(moved.Succeeded && !File.Exists(moveSource) && viewModel.Items.Single().Value == Path.Combine(target, "move.txt"), moved.Message);

        var again = await viewModel.RunActionAsync(move, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(again.Succeeded && !File.Exists(Path.Combine(target, "move (2).txt")), "已在目标文件夹中的项目被重命名");

        var missing = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.Folder, Folder = Path.Combine(target, "gone") });
        Assert(!(await QuickActionService.ExecuteAsync(new QuickActionRequest(missing, [copySource], false, IntPtr.Zero))).Succeeded,
            "目标文件夹不存在时没有提示");
    }

    private static void TestImageCompression()
    {
        var folder = CreateFolder("compress-image");
        var large = Path.Combine(folder, "photo.png");
        File.WriteAllBytes(large, CreateNoisyPng(900, 700));
        const long limit = 80 * 1024;
        Assert(new FileInfo(large).Length > limit, "测试图片不够大");

        var result = ImageCompressor.Compress(large, limit);
        Assert(result.Status == ImageCompressionStatus.Compressed && result.Output is not null, "图片没有压缩");
        Assert(Path.GetFileName(result.Output!) == "photo-80KB.jpg" && new FileInfo(result.Output!).Length <= limit, "压缩后的图片超过了限制");
        var decoder = System.Windows.Media.Imaging.BitmapDecoder.Create(new Uri(result.Output!), System.Windows.Media.Imaging.BitmapCreateOptions.None,
            System.Windows.Media.Imaging.BitmapCacheOption.OnLoad);
        Assert(decoder is System.Windows.Media.Imaging.JpegBitmapDecoder && decoder.Frames[0].PixelWidth <= 900, "输出不是 JPG");
        Assert(File.Exists(large), "原图被改动");

        Assert(ImageCompressor.Compress(result.Output!, limit).Status == ImageCompressionStatus.AlreadySmall, "已经足够小的图片不应再压缩");
        var text = Path.Combine(folder, "note.txt");
        File.WriteAllText(text, "x");
        Assert(ImageCompressor.Compress(text, limit).Status == ImageCompressionStatus.NotImage, "非图片文件没有跳过");
        var broken = Path.Combine(folder, "broken.jpg");
        File.WriteAllText(broken, new string('x', 200_000));
        Assert(ImageCompressor.Compress(broken, limit).Status == ImageCompressionStatus.Failed, "损坏的图片没有报告失败");
        Assert(ImageCompressor.FormatLimit(2 * 1024 * 1024) == "2MB" && Strings.ImageSizeText(2048) == "2 MB", "大小文字不正确");

        var banner = Path.Combine(folder, "banner.png");
        File.WriteAllBytes(banner, CreateNoisyPng(3000, 40));
        var bannerResult = ImageCompressor.Compress(banner, 10 * 1024);
        Assert(bannerResult.Status == ImageCompressionStatus.Failed ||
               (bannerResult.Status == ImageCompressionStatus.Compressed && new FileInfo(bannerResult.Output!).Length <= 10 * 1024),
            "细长图片压缩后超过了限制却报告成功");

        // 分成多个条带复制时，每一行都要与原图一致。
        var source = System.Windows.Media.Imaging.BitmapFrame.Create(new MemoryStream(CreateNoisyPng(1200, 1500)));
        var bgr = new System.Windows.Media.Imaging.FormatConvertedBitmap(source, System.Windows.Media.PixelFormats.Bgr24, null, 0);
        var copy = ImageCompressor.Materialize(bgr);
        var stride = (1200 * 3 + 3) & ~3;
        var expected = new byte[stride * 1500];
        var actual = new byte[stride * 1500];
        bgr.CopyPixels(expected, stride, 0);
        copy.CopyPixels(actual, stride, 0);
        Assert(copy.PixelWidth == 1200 && copy.PixelHeight == 1500 && expected.AsSpan().SequenceEqual(actual), "分段复制的像素与原图不一致");
    }

    private static async Task TestCompressImageReplacesItems()
    {
        var folder = CreateFolder("compress-replace");
        var large = Path.Combine(folder, "scan.png");
        File.WriteAllBytes(large, CreateNoisyPng(800, 600));
        var viewModel = new ShelfViewModel();
        viewModel.AddFiles([large]);
        var id = viewModel.Items[0].Id;
        var action = QuickActionInfo.ForCustom(new CustomQuickAction { Kind = CustomActionKind.CompressImage, ImageSizeKb = 60 });
        var result = await viewModel.RunActionAsync(action, viewModel.Items.ToArray(), IntPtr.Zero, null);
        Assert(result.Succeeded && viewModel.Items.Count == 1 && viewModel.Items[0].Value.EndsWith("scan-60KB.jpg") && viewModel.Items[0].Id == id,
            $"压缩后的图片没有替换原图：{result.Message}");
        Assert(File.Exists(large), "原图被删除");
    }

    private static void TestOpenWithArguments()
    {
        Assert(QuickActionService.BuildArguments(null, [@"C:\a b\x.png", @"D:\"]) == "\"C:\\a b\\x.png\" \"D:\\\\\"", "路径引号不正确");
        Assert(QuickActionService.BuildArguments("-q 80 {files} --out", [@"C:\x.png"]) == "-q 80 \"C:\\x.png\" --out", "{files} 没有替换");
        Assert(QuickActionService.BuildArguments("--edit", [@"C:\x.png"]) == "--edit \"C:\\x.png\"", "没有 {files} 时文件应放在最后");
        var program = new CustomQuickAction { Kind = CustomActionKind.OpenWith, ProgramPath = @"C:\Windows\System32\notepad.exe" };
        Assert(QuickActionInfo.ForCustom(program).Label.Length > 0, "程序名称为空");
    }
}
