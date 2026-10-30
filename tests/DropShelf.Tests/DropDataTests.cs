using System.IO;
using System.Text;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;
using DropShelf;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using WpfApplication = System.Windows.Application;
using WpfDataObject = System.Windows.DataObject;

namespace DropShelf.Tests;

internal static partial class Program
{
    // ───────────── 拖放与剪贴板数据 ─────────────

    private static void TestClassifyDropData()
    {
        var folder = CreateFolder("classify");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");

        var files = new WpfDataObject();
        files.SetData(System.Windows.DataFormats.FileDrop, new[] { file });
        Assert(DropDataReader.Classify(files) == DropContentKind.Files, "文件没有识别");

        var text = new WpfDataObject();
        text.SetData(System.Windows.DataFormats.UnicodeText, "hello");
        Assert(DropDataReader.Classify(text) == DropContentKind.Text, "文本没有识别");

        var link = new WpfDataObject();
        link.SetData("UniformResourceLocatorW", new MemoryStream(Encoding.Unicode.GetBytes("https://example.com\0")));
        Assert(DropDataReader.Classify(link) == DropContentKind.Link, "链接没有识别");

        var image = new WpfDataObject();
        image.SetData("PNG", new MemoryStream(CreatePng()));
        Assert(DropDataReader.Classify(image) == DropContentKind.Image, "图片没有识别");
        Assert(DropDataReader.Classify(new WpfDataObject()) == DropContentKind.None, "空数据被误识别");
        Assert(DropDataReader.CanProvideFiles(DropContentKind.Image) && !DropDataReader.CanProvideFiles(DropContentKind.Text),
            "快速操作可用内容判断不正确");
    }

    private static async Task TestReadDropData()
    {
        var folder = CreateFolder("read-drop");
        var file = Path.Combine(folder, "a.txt");
        File.WriteAllText(file, "x");
        var files = new WpfDataObject();
        files.SetData(System.Windows.DataFormats.FileDrop, new[] { file, folder });
        var read = await DropDataReader.ReadAsync(files);
        Assert(read.Entries.Count == 2 && read.Entries[1].Kind == ShelfItemKind.Folder && read.FilePaths.Count == 2, "文件读取不正确");

        var url = new WpfDataObject();
        url.SetData(System.Windows.DataFormats.UnicodeText, "  https://example.com/page  ");
        var link = await DropDataReader.ReadAsync(url);
        Assert(link.Entries.Single().Kind == ShelfItemKind.Link && link.Entries[0].Value == "https://example.com/page", "网址文本没有识别为链接");

        var moz = new WpfDataObject();
        moz.SetData("text/x-moz-url", new MemoryStream(Encoding.Unicode.GetBytes("https://example.org\n示例网站")));
        var mozLink = await DropDataReader.ReadAsync(moz);
        Assert(mozLink.Entries.Single().Title == "示例网站", "链接标题没有读取");

        var text = new WpfDataObject();
        text.SetData(System.Windows.DataFormats.UnicodeText, "第一行\n第二行");
        Assert((await DropDataReader.ReadAsync(text)).Entries.Single().Kind == ShelfItemKind.Text, "多行文本识别不正确");
        Assert((await DropDataReader.ReadAsync(text, filesOnly: true)).Entries.Count == 0, "快速操作错误地接受了文本");

        var image = new WpfDataObject();
        image.SetData("PNG", new MemoryStream(CreatePng()));
        var picture = await DropDataReader.ReadAsync(image, filesOnly: true);
        Assert(picture.Entries.Single().IsTemporary && File.Exists(picture.Entries[0].Value), "图片没有保存为临时文件");
        Assert(File.ReadAllBytes(picture.Entries[0].Value).Take(4).SequenceEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47 }), "保存的图片不是 PNG");
        foreach (var cache in picture.CacheDirectories) await VirtualFileDropService.ReleaseCacheAsync(cache);
        Assert(!File.Exists(picture.Entries[0].Value), "图片临时目录没有清理");
    }

    private static void TestSourceShelfId()
    {
        var data = new WpfDataObject();
        data.SetData(DropDataReader.ShelfIdFormat, new MemoryStream(Encoding.Unicode.GetBytes("abc123\0")));
        Assert(DropDataReader.GetSourceShelfId(data) == "abc123", "没有读取到来源置物架");
        Assert(DropDataReader.GetSourceShelfId(new WpfDataObject()) is null, "普通拖动被误认为来自置物架");
    }

    private static void TestShellDataObject()
    {
        var first = CreateFolder("shell-data-a");
        var second = CreateFolder("shell-data-b");
        var a = Path.Combine(first, "a.txt");
        var b = Path.Combine(second, "b 空格.txt");
        File.WriteAllText(a, "a");
        File.WriteAllText(b, "b");

        foreach (var data in new[] { ShellInterop.CreateFileDataObject([a, b, second]), CreateFallback([a, b, second]) })
        {
            Assert(data is not null, "没有创建拖出数据");
            var files = new WpfDataObject(data!).GetData(System.Windows.DataFormats.FileDrop) as string[];
            Assert(files is { Length: 3 } && files.Contains(a) && files.Contains(b) && files.Contains(second), "拖出数据中的文件不完整");
            Assert(ShellInterop.TrySetUnicodeString(data!, DropDataReader.ShelfIdFormat, "shelf-1") &&
                   DropDataReader.GetSourceShelfId(new WpfDataObject(data!)) == "shelf-1", "拖出数据无法标记来源置物架");
            System.Runtime.InteropServices.Marshal.ReleaseComObject(data!);
        }

        static System.Runtime.InteropServices.ComTypes.IDataObject? CreateFallback(IReadOnlyList<string> paths)
        {
            var data = ShellInterop.CreateEmptyDataObject();
            return data is not null && ShellInterop.TrySetBytes(data, System.Windows.DataFormats.FileDrop, ShellInterop.BuildDropFiles(paths)) ? data : null;
        }
    }

    private static async Task TestVirtualAttachmentImport()
    {
        var content = Encoding.UTF8.GetBytes("virtual attachment content");
        var descriptor = new byte[4 + 592];
        BitConverter.GetBytes(1u).CopyTo(descriptor, 0);
        Encoding.Unicode.GetBytes("chat-attachment.txt\0").CopyTo(descriptor, 4 + 72);

        var data = new WpfDataObject();
        data.SetData(VirtualFileDropService.FileDescriptorUnicodeFormat, new MemoryStream(descriptor));
        data.SetData(VirtualFileDropService.FileContentsFormat, new MemoryStream(content));
        Assert(DropDataReader.Classify(data) == DropContentKind.VirtualFiles, "标准 Windows 虚拟附件未被识别");
        Assert(VirtualFileDropService.GetDescriptorNames(data).Single() == "chat-attachment.txt", "附件名称读取不正确");

        var result = await DropDataReader.ReadAsync(data);
        Assert(result.Entries.Count == 1 && result.FailedCount == 0, result.ErrorMessage ?? "虚拟附件没有成功接收");
        var path = result.Entries[0].Value;
        Assert(Path.GetFileName(path) == "chat-attachment.txt" && File.ReadAllBytes(path).SequenceEqual(content), "虚拟附件内容不正确");

        var viewModel = new ShelfViewModel();
        viewModel.Add(result);
        Assert(viewModel.Items.Single().IsTemporaryAttachment && viewModel.CacheDirectories.Count == 1, "临时附件没有登记");
        viewModel.ReleaseResources();
        await Task.Delay(300);
        Assert(!Directory.Exists(result.CacheDirectories[0]), "关闭置物架后临时附件没有清理");
    }

    private static async Task TestAttachmentSharedBetweenShelves()
    {
        var cache = VirtualFileDropService.CreateCacheDirectory();
        var file = Path.Combine(cache, "附件.txt");
        File.WriteAllText(file, "shared");
        var first = new ShelfViewModel();
        first.Add(new DropReadResult([new DroppedEntry(ShelfItemKind.File, file, IsTemporary: true)], [cache], 0, null));

        var data = new WpfDataObject();
        data.SetData(System.Windows.DataFormats.FileDrop, new[] { file });
        var read = await DropDataReader.ReadAsync(data);
        // 读取完成、还没放进新置物架时原置物架就关闭了：读取结果已持有缓存。
        first.ReleaseResources();
        await Task.Delay(300);
        Assert(File.Exists(file), "读取后原置物架关闭，临时附件被删除");
        var second = new ShelfViewModel();
        second.Add(read);
        Assert(second.Items.Single().IsTemporaryAttachment && second.Items[0].Value == file, "从其他置物架拖来的临时附件没有标记为临时附件");
        second.ReleaseResources();
        await Task.Delay(300);
        Assert(!Directory.Exists(cache), "所有置物架关闭后临时附件没有清理");

        // 最后一个持有者释放后、后台删除执行前又被持有：不能删除。
        var raced = VirtualFileDropService.CreateCacheDirectory();
        File.WriteAllText(Path.Combine(raced, "keep.txt"), "keep");
        var deletion = VirtualFileDropService.ReleaseCacheAsync(raced);
        VirtualFileDropService.RetainCache(raced);
        await deletion;
        Assert(File.Exists(Path.Combine(raced, "keep.txt")), "重新持有后缓存仍被删除");
        await VirtualFileDropService.ReleaseCacheAsync(raced);
        Assert(!Directory.Exists(raced), "释放后缓存没有清理");

        // 操作进行中关闭置物架：操作自己持有缓存，文件仍能处理完。
        var busyCache = VirtualFileDropService.CreateCacheDirectory();
        var attachment = Path.Combine(busyCache, "处理中.txt");
        File.WriteAllText(attachment, "busy");
        var shelf = new ShelfViewModel();
        shelf.Add(new DropReadResult([new DroppedEntry(ShelfItemKind.File, attachment, IsTemporary: true)], [busyCache], 0, null));
        var target = CreateFolder("close-during-action");
        var moveTo = QuickActions.Get(QuickActionType.MoveTo);
        var moved = await shelf.RunActionAsync(moveTo, shelf.Items.ToArray(), IntPtr.Zero, () =>
        {
            shelf.ReleaseResources();
            Thread.Sleep(300);
            return target;
        });
        Assert(moved.Succeeded && File.ReadAllText(Path.Combine(target, "处理中.txt")) == "busy", "处理途中关闭置物架，正在使用的临时附件被提前删除");
        await Task.Delay(300);
        Assert(!Directory.Exists(busyCache), "操作结束后临时附件没有清理");
    }

    private static async Task TestVolatileFileSnapshot()
    {
        var temporary = Path.Combine(Path.GetTempPath(), "DropShelf.Tests.Chat", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        var source = Path.Combine(temporary, "聊天图片.png");
        File.WriteAllBytes(source, CreatePng());
        File.WriteAllText(source + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        var stable = Path.Combine(CreateStableFolder("volatile-stable"), "keep.txt");
        File.WriteAllText(stable, "keep");
        Assert(ChatAppCompatibility.IsVolatilePath(source) && !ChatAppCompatibility.IsVolatilePath(stable), "临时文件判断不正确");
        Assert(ChatAppCompatibility.IsVolatilePath(@"C:\Users\me\Documents\WeChat Files\wxid\FileStorage\Temp\a.jpg") &&
               !ChatAppCompatibility.IsVolatilePath(@"C:\Users\me\Documents\WeChat Files\wxid\FileStorage\File\a.pdf"), "聊天软件临时目录判断不正确");

        var data = new WpfDataObject();
        data.SetData(System.Windows.DataFormats.FileDrop, new[] { source, stable });
        var read = await DropDataReader.ReadAsync(data);
        Directory.Delete(temporary, true);
        Assert(read.Entries.Count == 2 && read.Entries[0].IsTemporary && read.Entries[0].Value != source, "临时文件没有保存到置物架缓存");
        Assert(File.Exists(read.Entries[0].Value) && Path.GetFileName(read.Entries[0].Value) == "聊天图片.png", "来源程序删除临时文件后内容丢失");
        Assert(File.ReadAllText(read.Entries[0].Value + ":Zone.Identifier").Contains("ZoneId=3"), "临时文件的副本丢失了下载来源标记");
        Assert(!read.Entries[1].IsTemporary && read.Entries[1].Value == stable, "普通文件不应复制");
        foreach (var cache in read.CacheDirectories) await VirtualFileDropService.ReleaseCacheAsync(cache);
    }

    private static void TestRichTextRejectsNetworkPaths()
    {
        var image = Path.Combine(CreateStableFolder("rich-text-local"), "local.png");
        File.WriteAllBytes(image, CreatePng());
        Assert(ChatAppCompatibility.ParseHtmlLocalImages($"<img src=\"{new Uri(image).AbsoluteUri}\">").SequenceEqual([image]), "本机图片路径没有识别");
        Assert(ChatAppCompatibility.ParseHtmlLocalImages("<img src=\"file://server/share/a.png\">").Count == 0, "网页片段中的网络共享路径没有被拒绝");
        Assert(ChatAppCompatibility.ParseQqRichEdit(
            "<QQRichEditFormat><EditElement type=\"1\" filepath=\"\\\\server\\share\\a.png\"></EditElement></QQRichEditFormat>").Count == 0,
            "QQ 富文本中的网络共享路径没有被拒绝");
    }

    private static async Task TestNonWebLinksBecomeText()
    {
        var data = new WpfDataObject();
        data.SetData("UniformResourceLocatorW", new MemoryStream(Encoding.Unicode.GetBytes("file:///C:/Windows/System32/calc.exe\0")));
        var read = await DropDataReader.ReadAsync(data);
        Assert(read.Entries.Single().Kind == ShelfItemKind.Text, "非网页链接没有按文本保存");

        // 旧版本保存的链接项目打开时也要检查协议：只复制文字，不启动程序。
        var viewModel = new ShelfViewModel();
        viewModel.AddEntries([new DroppedEntry(ShelfItemKind.Link, "calc.exe")]);
        viewModel.Open(viewModel.Items[0]);
        Assert(System.Windows.Clipboard.GetText() == "calc.exe", "非网页链接被交给系统打开");
    }

    private static async Task TestChatAppImageFormats()
    {
        var mime = new WpfDataObject();
        mime.SetData("image/png", new MemoryStream(CreatePng()));
        mime.SetData("UniformResourceLocatorW", new MemoryStream(Encoding.Unicode.GetBytes("https://example.com/a.png\0")));
        Assert(DropDataReader.Classify(mime) == DropContentKind.Image, "带有图片数据的拖动被当成了链接");
        var picture = await DropDataReader.ReadAsync(mime);
        Assert(picture.Entries.Single().Value.EndsWith(".png") && File.Exists(picture.Entries[0].Value), "image/png 图片没有保存");
        foreach (var cache in picture.CacheDirectories) await VirtualFileDropService.ReleaseCacheAsync(cache);

        var folder = CreateStableFolder("chat-images");
        var image = Path.Combine(folder, "qq-image.png");
        File.WriteAllBytes(image, CreatePng());
        var qq = new WpfDataObject();
        qq.SetData("QQ_Unicode_RichEdit_Format", new MemoryStream(Encoding.UTF8.GetBytes(
            $"<QQRichEditFormat><Info version=\"1001\"></Info><EditElement type=\"1\" filepath=\"{image}\" shortcut=\"\"></EditElement></QQRichEditFormat>")));
        qq.SetData(System.Windows.DataFormats.UnicodeText, "[图片]");
        Assert(DropDataReader.Classify(qq) == DropContentKind.Files, "旧版 QQ 拖出的图片没有识别");
        var fileName = new WpfDataObject();
        fileName.SetData("FileNameW", new[] { image });
        Assert(DropDataReader.Classify(fileName) == DropContentKind.Files &&
               (await DropDataReader.ReadAsync(fileName)).Entries.Single().Value == image, "只提供 FileNameW 的来源没有识别");
        Assert((await DropDataReader.ReadAsync(qq)).Entries.Single().Value == image, "旧版 QQ 拖出的图片路径不正确");

        var html = new WpfDataObject();
        html.SetData(System.Windows.DataFormats.Html, $"<html><body><!--StartFragment--><img src=\"{new Uri(image).AbsoluteUri}\"><!--EndFragment--></body></html>");
        Assert(DropDataReader.Classify(html) == DropContentKind.Files, "只含本地图片的网页片段没有识别为文件");
        var withText = new WpfDataObject();
        withText.SetData(System.Windows.DataFormats.Html, $"<!--StartFragment-->说明<img src=\"{new Uri(image).AbsoluteUri}\"><!--EndFragment-->");
        withText.SetData(System.Windows.DataFormats.UnicodeText, "说明");
        Assert(DropDataReader.Classify(withText) == DropContentKind.Text, "带文字的网页片段不应只取图片");
    }
}
