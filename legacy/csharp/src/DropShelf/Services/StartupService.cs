using System.IO;
using DropShelf.Localization;
using Microsoft.Win32;

namespace DropShelf.Services;

public sealed record StartupRegistrationResult(bool Succeeded, string? ErrorMessage = null);

public sealed class StartupService
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "DropShelf";
    private readonly string _executablePath;

    public StartupService(string? executablePath = null)
    {
        _executablePath = executablePath ?? Environment.ProcessPath
            ?? Path.Combine(AppContext.BaseDirectory, "DropShelf.exe");
    }

    public StartupRegistrationResult SetEnabled(bool enabled)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true);
            if (key is null)
                return new StartupRegistrationResult(false, Strings.StartupSettingsUnavailable);

            if (enabled)
                key.SetValue(ValueName, BuildCommand(_executablePath), RegistryValueKind.String);
            else
                key.DeleteValue(ValueName, throwOnMissingValue: false);

            return new StartupRegistrationResult(true);
        }
        catch (Exception exception)
        {
            LogService.Error("修改开机启动设置失败", exception);
            return new StartupRegistrationResult(false, Strings.StartupRefused);
        }
    }

    public static string BuildCommand(string executablePath) => $"\"{executablePath}\" --background";
}
