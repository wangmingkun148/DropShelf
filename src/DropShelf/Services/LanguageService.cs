using System.Globalization;
using DropShelf.Models;

namespace DropShelf.Services;

/// <summary>界面语言：跟随 Windows 显示语言，或固定为中文、英文。切换后已打开的界面立即更新。</summary>
public static class LanguageService
{
    private static AppLanguage _mode = AppLanguage.System;

    public static event EventHandler? LanguageChanged;
    public static AppLanguage Mode => _mode;
    public static bool IsChinese { get; private set; } = ResolveIsChinese(AppLanguage.System, CultureInfo.CurrentUICulture);

    public static void SetMode(AppLanguage mode)
    {
        _mode = mode;
        var chinese = ResolveIsChinese(mode, CultureInfo.CurrentUICulture);
        if (chinese == IsChinese) return;
        IsChinese = chinese;
        LanguageChanged?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>跟随系统时，Windows 显示语言为中文（包括繁体）使用中文，其他语言使用英文。</summary>
    public static bool ResolveIsChinese(AppLanguage mode, CultureInfo uiCulture) => mode switch
    {
        AppLanguage.Chinese => true,
        AppLanguage.English => false,
        _ => uiCulture.TwoLetterISOLanguageName == "zh"
    };
}
