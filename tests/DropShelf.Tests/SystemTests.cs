using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DropShelf;
using DropShelf.Interop;
using DropShelf.Models;
using DropShelf.Services;
using DropShelf.ViewModels;
using DropShelf.Views;
using WpfApplication = System.Windows.Application;

namespace DropShelf.Tests;

internal static partial class Program
{
    // ───────────── 系统集成 ─────────────

    private static async Task TestShellThumbnail()
    {
        var folder = CreateFolder("shell-thumbnail");
        var path = Path.Combine(folder, "preview.txt");
        File.WriteAllText(path, "thumbnail preview");
        var thumbnail = await ShellThumbnailService.LoadAsync(path, 96);
        Assert(thumbnail is System.Windows.Media.Imaging.BitmapSource { PixelWidth: > 0 } && thumbnail.IsFrozen, "没有获取到可跨线程使用的缩略图");
        Assert(thumbnail is System.Windows.Media.Imaging.BitmapSource { PixelWidth: <= 96, PixelHeight: <= 96 }, "缩略图大于需要的尺寸");
        Assert(!string.IsNullOrWhiteSpace(ShellFileInfo.GetTypeName(path)), "没有获取到文件类型名称");
        Assert(ShellFileInfo.GetTypeName("noextension") == "文件", "无扩展名文件类型不正确");
    }

    private static async Task TestImagingThread()
    {
        var first = await ImagingThread.RunAsync(() => (Environment.CurrentManagedThreadId, Thread.CurrentThread.GetApartmentState()));
        var second = await ImagingThread.RunAsync(() => Environment.CurrentManagedThreadId);
        Assert(first.Item1 == second && first.Item2 == ApartmentState.STA, "图片处理没有在同一个单线程套间线程上运行");
        Assert(Environment.CurrentManagedThreadId != second, "图片处理占用了界面线程");
        var longWork = await ImagingThread.RunLongAsync(() => (Environment.CurrentManagedThreadId, Thread.CurrentThread.GetApartmentState()));
        Assert(longWork.Item1 != second && longWork.Item2 == ApartmentState.STA, "整批压缩图片和缩略图共用了同一个线程");
        var cancelled = false;
        try { await ImagingThread.RunAsync(() => 1, new CancellationToken(canceled: true)); }
        catch (OperationCanceledException) { cancelled = true; }
        Assert(cancelled, "已取消的图片处理仍然执行");
        var failed = false;
        try { await ImagingThread.RunAsync(() => throw new InvalidOperationException()); }
        catch (InvalidOperationException) { failed = true; }
        Assert(failed, "图片处理中的异常没有传回调用方");
    }

    private static void TestStartupCommand()
    {
        var executablePath = @"C:\Program Files\Drop Shelf\DropShelf.exe";
        Assert(StartupService.BuildCommand(executablePath) == $"\"{executablePath}\" --background", "开机启动命令没有正确处理空格路径");
    }

    private static void TestThemeAndAccent()
    {
        Assert(ThemeService.ResolveIsDark(AppTheme.System, true) && !ThemeService.ResolveIsDark(AppTheme.System, false), "跟随系统不正确");
        Assert(ThemeService.ResolveIsDark(AppTheme.Dark, false) && !ThemeService.ResolveIsDark(AppTheme.Light, true), "手动主题被系统覆盖");

        var bytes = Convert.FromHexString("7B7171FF736969FF6D6464FF685F5FFF625959FF5C5454FF544C4CFF88179800");
        var palette = AccentPalette.Parse(bytes)!.Value;
        Assert(palette.Base == System.Windows.Media.Color.FromRgb(0x68, 0x5F, 0x5F) && palette.Light2 == System.Windows.Media.Color.FromRgb(0x73, 0x69, 0x69),
            "强调色调色板解析错误");
        Assert(AccentPalette.Parse(new byte[4]) is null, "不完整的调色板应被忽略");

        var dark = ThemeService.BuildPalette(true, palette);
        Assert(dark["TextOnAccentBrush"] == System.Windows.Media.Colors.White, "深色强调色上应使用白字");
        var darkBackground = System.Windows.Media.Color.FromRgb(0x20, 0x20, 0x20);
        Assert(ThemeService.ContrastRatio(dark["AccentTextBrush"], darkBackground) >= 4.5, "强调色文字对比度不足");
        var light = ThemeService.BuildPalette(false, AccentPalette.Default);
        Assert(light["AccentBrush"] == AccentPalette.Default.Dark1 && light["TextOnAccentBrush"] == System.Windows.Media.Colors.White, "默认浅色强调色不正确");
        Assert(ThemeService.PrefersDarkText(System.Windows.Media.Color.FromRgb(0x99, 0xEB, 0xFF)), "浅色强调色上应使用黑字");
        Assert(light["WindowBackgroundBrush"].R > light["WindowBackgroundBrush"].B && light["ShelfSolidBrush"].R > light["ShelfSolidBrush"].B, "浅色背景应为象牙白");
    }

    private static void TestTrayIconLoader()
    {
        var resource = WpfApplication.GetResourceStream(new Uri("pack://application:,,,/DropShelf;component/Assets/DropShelf.ico"))!;
        using var memory = new MemoryStream();
        resource.Stream.CopyTo(memory);
        var icon = IconLoader.CreateFromIco(memory.ToArray(), 16);
        Assert(icon != IntPtr.Zero, "没有从图标资源创建托盘图标");
        NativeMethods.DestroyIcon(icon);
        Assert(IconLoader.CreateFromIco([0, 1], 16) == IntPtr.Zero, "无效图标数据没有安全处理");
    }

    private static void TestHotkeyRegistration()
    {
        using var window = new MessageWindow("DropShelf.Tests.Hotkey");
        using var hotkey = new HotkeyService(window);
        Assert(hotkey.Register(new HotkeyGesture(ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Shift, Key.F23)), "快捷键注册失败");
        hotkey.Unregister();
        Assert(hotkey.Register(HotkeyGesture.None), "关闭快捷键应视为成功");
    }

    private static void TestArgumentForwarding()
    {
        var folder = CreateFolder("forward");
        var file = Path.Combine(folder, "send to.txt");
        File.WriteAllText(file, "x");
        Assert(AppController.IsShelfRequest([file]) && AppController.IsShelfRequest(["--shelf"]), "没有识别置物架请求");
        Assert(!AppController.IsShelfRequest(["--background"]) && !AppController.IsShelfRequest([Path.Combine(folder, "missing")]), "错误识别了置物架请求");
        Assert(AppController.GetPathArguments(["--shelf", file]).Single() == file, "没有取出文件参数");

        using var window = new MessageWindow("DropShelf.Tests.Forward");
        string[]? received = null;
        window.ArgumentsReceived += arguments => received = arguments;
        Assert(MessageWindow.SendArguments("DropShelf.Tests.Forward", ["--shelf", file]), "没有找到正在运行的实例");
        Pump(200);
        Assert(received is ["--shelf", var path] && path == file, "转交的参数不正确");
        Assert(!MessageWindow.SendArguments("DropShelf.Tests.NoSuchWindow", ["x"]), "不存在的实例被误认为已转交");
    }

    private static void TestFailureLogExists()
    {
        var log = Path.Combine(_root, "logs", $"dropshelf-{DateTime.Now:yyyy-MM-dd}.log");
        Assert(File.Exists(log) && new FileInfo(log).Length > 0, "错误日志没有生成");
    }
}
