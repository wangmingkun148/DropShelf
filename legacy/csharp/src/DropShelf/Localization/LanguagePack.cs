using System.IO;
using System.Reflection;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using DropShelf.Models;
using DropShelf.Services;

namespace DropShelf.Localization;

/// <summary>
/// 语言包：一个 JSON 文件，把界面上的原文对照成另一种语言的译文，不用改程序就能增加语言。
/// 中文和英文写在 <see cref="Strings"/> 里；语言包对照其中一种（from 为 zh 或 en）逐条给出译文，
/// 带参数的文字按模板对照，例如 "已添加 {0} 项": "{0} 件追加しました"。没有译文的条目显示原文。
/// </summary>
public sealed class LanguagePack
{
    /// <summary>超过这么大的文件不读：正常的语言包只有几十 KB。</summary>
    public const long MaximumFileSize = 2 * 1024 * 1024;

    private readonly Dictionary<string, string> _strings;

    private LanguagePack(string id, string name, bool fromChinese, Dictionary<string, string> strings)
    {
        Id = id;
        Name = name;
        FromChinese = fromChinese;
        _strings = strings;
    }

    /// <summary>文件名（不含扩展名），保存在设置里，例如 ja、zh-TW。</summary>
    public string Id { get; }

    /// <summary>显示在设置里的名称，用这种语言自己书写，例如 日本語。</summary>
    public string Name { get; }

    /// <summary>译文对照的是中文原文；否则对照英文原文。没有译文的条目也显示这种原文。</summary>
    public bool FromChinese { get; }

    public int Count => _strings.Count;

    /// <summary>有译文时把参数填进译文；没有、或译文里参数位置写错时显示原文。</summary>
    public string Translate(LocalizedFormat chinese, LocalizedFormat english)
    {
        var source = FromChinese ? chinese : english;
        return _strings.TryGetValue(source.Template, out var translated) && source.Format(translated) is { } text ? text : source.ToString();
    }

    /// <summary>读取语言包文件；太大、不是语言包或一条译文都没有时返回 null。</summary>
    public static LanguagePack? Load(string path)
    {
        try
        {
            var file = new FileInfo(path);
            if (!file.Exists || file.Length > MaximumFileSize) return null;
            return Parse(Path.GetFileNameWithoutExtension(path), File.ReadAllText(path));
        }
        catch (Exception exception)
        {
            LogService.Error($"读取语言包失败：{path}", exception);
            return null;
        }
    }

    public static LanguagePack? Parse(string id, string json)
    {
        using var document = JsonDocument.Parse(json, new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("strings", out var entries) || entries.ValueKind != JsonValueKind.Object)
            return null;

        var strings = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in entries.EnumerateObject())
            if (entry.Value.ValueKind == JsonValueKind.String && entry.Value.GetString() is { Length: > 0 } translated)
                strings[entry.Name] = translated;
        if (strings.Count == 0) return null;

        var name = root.TryGetProperty("name", out var label) && label.ValueKind == JsonValueKind.String ? label.GetString()?.Trim() : null;
        var from = root.TryGetProperty("from", out var source) && source.ValueKind == JsonValueKind.String ? source.GetString() : null;
        return new LanguagePack(id, string.IsNullOrEmpty(name) ? id : name, !string.Equals(from, "en", StringComparison.OrdinalIgnoreCase), strings);
    }
}

/// <summary>找到的语言包：程序所在文件夹和设置所在文件夹里的 Languages 文件夹，后者的同名文件优先。</summary>
public static class LanguagePacks
{
    public const string FolderName = "Languages";

    public static IReadOnlyList<LanguagePack> Available { get; private set; } = [];

    /// <summary>重新查找语言包。userDirectory 是设置所在文件夹里的 Languages 文件夹。</summary>
    public static void Reload(string userDirectory)
    {
        var packs = new Dictionary<string, LanguagePack>(StringComparer.OrdinalIgnoreCase);
        foreach (var directory in new[] { Path.Combine(AppContext.BaseDirectory, FolderName), userDirectory })
        {
            try
            {
                if (!Directory.Exists(directory)) continue;
                foreach (var path in Directory.EnumerateFiles(directory, "*.json"))
                    if (LanguagePack.Load(path) is { } pack) packs[pack.Id] = pack;
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                LogService.Error($"查找语言包失败：{directory}", exception);
            }
        }
        Available = packs.Values.OrderBy(pack => pack.Name, StringComparer.CurrentCultureIgnoreCase).ToArray();
    }

    public static LanguagePack? Find(string? id) =>
        string.IsNullOrWhiteSpace(id) ? null : Available.FirstOrDefault(pack => string.Equals(pack.Id, id, StringComparison.OrdinalIgnoreCase));
}

/// <summary>
/// 给翻译的人用的模板：把 <see cref="Strings"/> 里的文字都过一遍，收集中文和英文的原文模板，写成一个空白的语言包。
/// 带参数的方法用几组有代表性的参数各调用一次，不同分支里的文字都能收集到。
/// </summary>
public static class LanguageTemplate
{
    private const int MaximumCombinations = 400;

    /// <summary>全部原文，中文和英文成对，按在 <see cref="Strings"/> 里出现的顺序。</summary>
    public static IReadOnlyList<(string Chinese, string English)> Collect()
    {
        var pairs = new List<(string, string)>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        Strings.Collector = (chinese, english) =>
        {
            if (seen.Add(chinese.Template + '\u0001' + english.Template)) pairs.Add((chinese.Template, english.Template));
        };
        try
        {
            foreach (var member in typeof(Strings).GetMembers(BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly)
                         .OrderBy(member => member.MetadataToken))
            {
                switch (member)
                {
                    case PropertyInfo { PropertyType: var type } property when type == typeof(string):
                        Try(() => property.GetValue(null));
                        break;
                    case MethodInfo { IsSpecialName: false } method when method.ReturnType == typeof(string):
                        foreach (var arguments in Combinations(method.GetParameters().Select(parameter => Samples(parameter.ParameterType)).ToArray()))
                            Try(() => method.Invoke(null, arguments));
                        break;
                }
            }
        }
        finally
        {
            Strings.Collector = null;
        }
        return pairs;
    }

    /// <summary>
    /// 空白语言包的内容：strings 里每条原文对应一个待填的译文；另一种语言的原文列在 reference 里供对照（读取时忽略）。
    /// fromChinese 决定对照中文还是英文来翻译。
    /// </summary>
    public static string Build(bool fromChinese = true)
    {
        // 同一条原文在几处出现时只列一次。
        var keys = new HashSet<string>(StringComparer.Ordinal);
        var pairs = Collect().Where(pair => keys.Add(fromChinese ? pair.Chinese : pair.English)).ToArray();
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
        {
            writer.WriteStartObject();
            writer.WriteString("name", string.Empty);
            writer.WriteString("from", fromChinese ? "zh" : "en");
            writer.WriteStartObject("strings");
            foreach (var (chinese, english) in pairs) writer.WriteString(fromChinese ? chinese : english, string.Empty);
            writer.WriteEndObject();
            writer.WriteStartObject("reference");
            foreach (var (chinese, english) in pairs) writer.WriteString(fromChinese ? chinese : english, fromChinese ? english : chinese);
            writer.WriteEndObject();
            writer.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private static void Try(Action call)
    {
        try
        {
            call();
        }
        catch
        {
            // 个别参数组合不合理（比如空的路径）时会出错，不影响收集其余的文字。
        }
    }

    /// <summary>每种参数类型的几个有代表性的值：数量取 0、1、2（单复数和“没有”的分支），开关和枚举取全部。</summary>
    private static object?[] Samples(Type type)
    {
        if (type == typeof(int)) return [0, 1, 2];
        if (type == typeof(long)) return [0L, 1L, 2L];
        if (type == typeof(bool)) return [false, true];
        if (type == typeof(string)) return ["X", null];
        if (type == typeof(DateTime)) return [DateTime.Now, DateTime.Today.AddDays(-1).AddHours(9), DateTime.Today.AddDays(-40)];
        if (type == typeof(IEnumerable<string>)) return [new[] { "X" }];
        if (type == typeof(HotkeyGesture)) return [HotkeyGesture.Default, HotkeyGesture.None];
        if (type == typeof(ShelfHistoryEntry))
            return
            [
                new ShelfHistoryEntry { ClosedAt = DateTime.Now, Items = [new ShelfHistoryItem { Path = @"C:\X", Name = "X" }] },
                new ShelfHistoryEntry
                {
                    ClosedAt = DateTime.Now,
                    Items = [new ShelfHistoryItem { Path = @"C:\X", Name = "X" }, new ShelfHistoryItem { Path = @"C:\Y", Name = "Y" }]
                }
            ];
        if (type == typeof(CustomQuickAction))
            return Enum.GetValues<CustomActionKind>().SelectMany(kind => Enum.GetValues<TransferMode>().Select(mode => (object?)new CustomQuickAction
            {
                Kind = kind, FolderMode = mode, Folder = @"C:\X", ProgramPath = @"C:\X.exe"
            })).ToArray();
        if (type.IsEnum) return Enum.GetValues(type).Cast<object?>().ToArray();
        if (Nullable.GetUnderlyingType(type) is { } inner) return [.. Samples(inner), null];
        return [null];
    }

    /// <summary>各个参数取值的全部组合；组合太多时只让每个参数轮流变化，其余取第一个值。</summary>
    private static IEnumerable<object?[]> Combinations(object?[][] samples)
    {
        if (samples.Length == 0)
        {
            yield return [];
            yield break;
        }
        var total = samples.Aggregate(1L, (product, values) => product * Math.Max(values.Length, 1));
        if (total <= MaximumCombinations)
        {
            var indexes = new int[samples.Length];
            while (true)
            {
                yield return samples.Select((values, position) => values[indexes[position]]).ToArray();
                var position = samples.Length - 1;
                while (position >= 0 && ++indexes[position] == samples[position].Length) indexes[position--] = 0;
                if (position < 0) yield break;
            }
        }
        for (var varying = 0; varying < samples.Length; varying++)
            foreach (var value in samples[varying])
                yield return samples.Select((values, position) => position == varying ? value : values[0]).ToArray();
    }
}
