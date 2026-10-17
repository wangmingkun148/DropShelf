using DropShelf.Localization;

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
    MoveTo
}

/// <summary>快速操作的图标；名称和说明按当前界面语言读取。</summary>
public sealed record QuickActionInfo(QuickActionType Type, string Glyph)
{
    public string Label => Strings.QuickActionLabel(Type);
    public string Description => Strings.QuickActionDescription(Type);
    public string BusyText => Strings.QuickActionBusyText(Type);
}

public static class QuickActions
{
    public const int MaximumVisible = 4;

    public static IReadOnlyList<QuickActionInfo> All { get; } =
    [
        new(QuickActionType.CompressZip, ""),
        new(QuickActionType.CopyFullPath, ""),
        new(QuickActionType.MoveTo, ""),
        new(QuickActionType.Properties, ""),
        new(QuickActionType.Reveal, "")
    ];

    public static IReadOnlyList<QuickActionType> Defaults { get; } =
        [QuickActionType.CompressZip, QuickActionType.CopyFullPath, QuickActionType.MoveTo, QuickActionType.Properties];

    public static bool IsSupported(QuickActionType type) => All.Any(info => info.Type == type);

    public static QuickActionInfo Get(QuickActionType type) =>
        All.FirstOrDefault(info => info.Type == type) ?? All[0];

    /// <summary>去重、去掉已移除的旧操作，并按固定顺序最多保留四个。</summary>
    public static List<QuickActionType> Normalize(IEnumerable<QuickActionType>? actions) =>
        All.Select(info => info.Type)
            .Where(type => actions?.Contains(type) == true)
            .Take(MaximumVisible)
            .ToList();
}
