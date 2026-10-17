using System.IO;

namespace DropShelf.Services;

public static class LogService
{
    private static readonly object Gate = new();

    public static string LogDirectory =>
        Environment.GetEnvironmentVariable("DROPSHELF_LOG_DIRECTORY") is { Length: > 0 } customDirectory
            ? customDirectory
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DropShelf", "Logs");

    public static void Info(string message) => Write("INFO", message, null);

    /// <summary>设置环境变量 DROPSHELF_TRACE=1 时记录手势和拖放的诊断信息。</summary>
    public static bool TraceEnabled { get; } = Environment.GetEnvironmentVariable("DROPSHELF_TRACE") == "1";

    public static void Trace(string message)
    {
        if (TraceEnabled) Write("TRACE", message, null);
    }
    public static void Error(string message, Exception exception) => Write("ERROR", message, exception);

    /// <summary>删除 14 天前的日志，避免长期运行后占用空间。</summary>
    public static void CleanupOldLogs()
    {
        try
        {
            if (!Directory.Exists(LogDirectory)) return;
            var cutoff = DateTime.Now.AddDays(-14);
            foreach (var file in Directory.EnumerateFiles(LogDirectory, "dropshelf-*.log"))
            {
                try
                {
                    if (File.GetLastWriteTime(file) < cutoff) File.Delete(file);
                }
                catch
                {
                }
            }
        }
        catch
        {
        }
    }

    private static void Write(string level, string message, Exception? exception)
    {
        try
        {
            Directory.CreateDirectory(LogDirectory);
            var line = exception is null
                ? $"{DateTimeOffset.Now:O} [{level}] {message}"
                : $"{DateTimeOffset.Now:O} [{level}] {message}{Environment.NewLine}{exception}";
            lock (Gate)
                File.AppendAllText(Path.Combine(LogDirectory, $"dropshelf-{DateTime.Now:yyyy-MM-dd}.log"), line + Environment.NewLine);
        }
        catch
        {
            // 日志写入失败不能影响主程序运行。
        }
    }
}
