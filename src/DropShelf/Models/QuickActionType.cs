using DropShelf.Localization;
using DropShelf.Services;

namespace DropShelf.Models;

public enum QuickActionType
{
    None,
    Properties,
    CompressZip,
    CopyFullPath,
    ConvertPng,
    ConvertJpeg,
    Reveal,
    MoveTo,
    Custom
}

/// <summary>置物架底部显示几个快速操作。两个时与四个占用相同空间，每个按钮更大。</summary>
public enum QuickActionLayout
{
    Off = 0,
    Two = 2,
    Four = 4
}

/// <summary>移动或复制：用于拖出置物架，也用于“发送到文件夹”的自定义操作。</summary>
public enum TransferMode
{
    Move,
    Copy
}

public enum CustomActionKind
{
    /// <summary>复制或移动到固定的文件夹。</summary>
    Folder,

    /// <summary>把图片压缩为不超过指定大小的 JPG。</summary>
    CompressImage,

    /// <summary>用指定程序打开。</summary>
    OpenWith
}

/// <summary>用户添加的快速操作。只保存操作本身的设置，名称留空时按内容自动生成。</summary>
public sealed class CustomQuickAction
{
    public const int MinimumImageSizeKb = 10;
    public const int MaximumImageSizeKb = 100 * 1024;
    public const int DefaultImageSizeKb = 500;
    public const int MaximumNameLength = 40;

    public string Id { get; set; } = NewId();
    public CustomActionKind Kind { get; set; }
    public string? Name { get; set; }
    public string? Folder { get; set; }
    public TransferMode FolderMode { get; set; } = TransferMode.Move;
    public int ImageSizeKb { get; set; } = DefaultImageSizeKb;
    public string? ProgramPath { get; set; }
    public string? Arguments { get; set; }

    /// <summary>缺少必需内容（文件夹或程序）的操作无法执行。</summary>
    public bool IsComplete => Kind switch
    {
        CustomActionKind.Folder => !string.IsNullOrWhiteSpace(Folder),
        CustomActionKind.OpenWith => !string.IsNullOrWhiteSpace(ProgramPath),
        _ => true
    };

    public CustomQuickAction Clone() => (CustomQuickAction)MemberwiseClone();

    public static string NewId() => Guid.NewGuid().ToString("N");
}

/// <summary>一个可放到置物架上的快速操作：内置操作或自定义操作。名称和说明按当前界面语言读取。</summary>
public sealed class QuickActionInfo
{
    private QuickActionInfo(string id, QuickActionType type, string glyph, CustomQuickAction? custom)
    {
        Id = id;
        Type = type;
        Glyph = glyph;
        Custom = custom;
    }

    /// <summary>保存在设置中的标识：内置操作为类型名称，自定义操作为其 Id。</summary>
    public string Id { get; }
    public QuickActionType Type { get; }
    public string Glyph { get; }
    public CustomQuickAction? Custom { get; }
    public bool IsCustom => Custom is not null;

    /// <summary>置物架按钮上的简短名称。</summary>
    public string Label => Custom is null ? Strings.QuickActionLabel(Type) : Strings.CustomActionLabel(Custom);

    /// <summary>设置页、菜单和提示中的完整名称。</summary>
    public string Title => Custom is null ? Strings.QuickActionTitle(Type) : Strings.CustomActionTitle(Custom);
    public string Description => Custom is null ? Strings.QuickActionDescription(Type) : Strings.CustomActionDescription(Custom);
    public string BusyText => Custom is null ? Strings.QuickActionBusyText(Type) : Strings.CustomActionBusyText(Custom);

    /// <summary>可能耗时较长的操作在处理期间显示进度。</summary>
    public bool ShowsProgress => Type is QuickActionType.CompressZip or QuickActionType.MoveTo ||
                                 Custom?.Kind is CustomActionKind.Folder or CustomActionKind.CompressImage;

    public static QuickActionInfo BuiltIn(QuickActionType type, string glyph) => new(type.ToString(), type, glyph, null);

    public static QuickActionInfo ForCustom(CustomQuickAction custom) =>
        new(custom.Id, QuickActionType.Custom, GlyphFor(custom), custom);

    private static string GlyphFor(CustomQuickAction custom) => custom.Kind switch
    {
        CustomActionKind.Folder => custom.FolderMode == TransferMode.Copy ? "" : "",
        CustomActionKind.CompressImage => "",
        _ => ""
    };
}

public static class QuickActions
{
    /// <summary>设置中始终保存四个位置，切换为两个时保留后两个的选择。</summary>
    public const int SlotCount = 4;
    public const int MaximumVisible = 4;
    public const int MaximumCustomActions = 20;

    /// <summary>内置操作，按设置页中的排列顺序。</summary>
    public static IReadOnlyList<QuickActionInfo> All { get; } =
    [
        QuickActionInfo.BuiltIn(QuickActionType.CompressZip, ""),
        QuickActionInfo.BuiltIn(QuickActionType.CopyFullPath, ""),
        QuickActionInfo.BuiltIn(QuickActionType.MoveTo, ""),
        QuickActionInfo.BuiltIn(QuickActionType.Properties, ""),
        QuickActionInfo.BuiltIn(QuickActionType.Reveal, "")
    ];

    public static IReadOnlyList<QuickActionType> Defaults { get; } =
        [QuickActionType.CompressZip, QuickActionType.CopyFullPath, QuickActionType.MoveTo, QuickActionType.Properties];

    public static bool IsBuiltInId(string? id) => All.Any(info => info.Id == id);

    public static QuickActionInfo Get(QuickActionType type) =>
        All.FirstOrDefault(info => info.Type == type) ?? All[0];

    /// <summary>按标识查找操作；已删除的自定义操作或未知标识返回 null。</summary>
    public static QuickActionInfo? Find(string? id, IReadOnlyList<CustomQuickAction>? customs)
    {
        if (string.IsNullOrWhiteSpace(id)) return null;
        if (All.FirstOrDefault(info => info.Id == id) is { } builtIn) return builtIn;
        return customs?.FirstOrDefault(custom => custom.Id == id) is { } match ? QuickActionInfo.ForCustom(match) : null;
    }

    /// <summary>可放到置物架上的全部操作：内置操作在前，自定义操作在后。</summary>
    public static IReadOnlyList<QuickActionInfo> Available(IReadOnlyList<CustomQuickAction>? customs) =>
        [.. All, .. (customs ?? []).Select(QuickActionInfo.ForCustom)];

    /// <summary>置物架上实际显示的操作，按从左到右的顺序。</summary>
    public static IReadOnlyList<QuickActionInfo> Visible(AppSettings settings) => (settings.QuickActionSlots ?? [])
        .Take((int)settings.QuickActionLayout)
        .Select(id => Find(id, settings.CustomActions))
        .OfType<QuickActionInfo>()
        .ToArray();

    /// <summary>整理为四个互不重复的有效操作；缺少的位置依次用默认操作补齐。</summary>
    public static List<string> NormalizeSlots(IEnumerable<string>? slots, IReadOnlyList<CustomQuickAction>? customs)
    {
        var result = new List<string>(SlotCount);
        foreach (var id in slots ?? [])
        {
            if (result.Count == SlotCount) break;
            if (Find(id, customs) is { } info && !result.Contains(info.Id)) result.Add(info.Id);
        }

        var fallback = Defaults.Select(type => type.ToString())
            .Concat(All.Select(info => info.Id))
            .Concat((customs ?? []).Select(custom => custom.Id));
        foreach (var id in fallback)
        {
            if (result.Count == SlotCount) break;
            if (!result.Contains(id)) result.Add(id);
        }
        return result;
    }

    /// <summary>2.0 版本的快速操作列表：去重、去掉已移除的旧操作，并按固定顺序最多保留四个。仅用于迁移旧设置。</summary>
    public static List<QuickActionType> Normalize(IEnumerable<QuickActionType>? actions) =>
        All.Select(info => info.Type)
            .Where(type => actions?.Contains(type) == true)
            .Take(MaximumVisible)
            .ToList();
}
