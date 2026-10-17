using System.ComponentModel;
using System.Reflection;
using System.Windows.Data;
using System.Windows.Markup;
using DropShelf.Services;

namespace DropShelf.Localization;

/// <summary>
/// 在 XAML 中引用界面文字：Text="{l:Loc Overview}"，名称对应 <see cref="Strings"/> 的属性。
/// 生成的是绑定，切换语言后自动刷新；名称写错会在加载界面时报错，不会悄悄显示空白。
/// </summary>
[MarkupExtensionReturnType(typeof(object))]
public sealed class LocExtension : MarkupExtension
{
    public LocExtension()
    {
    }

    public LocExtension(string key) => Key = key;

    [ConstructorArgument("key")]
    public string Key { get; set; } = string.Empty;

    public override object ProvideValue(IServiceProvider serviceProvider)
    {
        if (!LocalizedText.Contains(Key)) throw new ArgumentException($"未知的界面文字：{Key}", nameof(Key));
        var binding = new System.Windows.Data.Binding($"[{Key}]") { Source = LocalizedText.Instance, Mode = BindingMode.OneWay };
        return binding.ProvideValue(serviceProvider);
    }
}

/// <summary>供绑定使用：按名称读取 <see cref="Strings"/> 的当前语言文字，语言切换时通知所有绑定刷新。</summary>
public sealed class LocalizedText : INotifyPropertyChanged
{
    private static readonly Dictionary<string, PropertyInfo> Properties = typeof(Strings)
        .GetProperties(BindingFlags.Public | BindingFlags.Static)
        .Where(property => property.PropertyType == typeof(string))
        .ToDictionary(property => property.Name, StringComparer.Ordinal);

    private LocalizedText()
    {
        LanguageService.LanguageChanged += (_, _) =>
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(System.Windows.Data.Binding.IndexerName));
    }

    public static LocalizedText Instance { get; } = new();

    public static IReadOnlyCollection<string> Keys => Properties.Keys;

    public string this[string key] => Properties.TryGetValue(key, out var property) ? (string)property.GetValue(null)! : key;

    public static bool Contains(string key) => Properties.ContainsKey(key);

    public event PropertyChangedEventHandler? PropertyChanged;
}
