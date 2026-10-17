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

public sealed record QuickActionInfo(QuickActionType Type, string Label, string Glyph, string Description, string BusyText);

public static class QuickActions
{
    public const int MaximumVisible = 4;

    public static IReadOnlyList<QuickActionInfo> All { get; } =
    [
        new(QuickActionType.CompressZip, "压缩", "", "压缩为 ZIP；单独放入 ZIP 或 7z 时解压到原位置旁。", "正在压缩…"),
        new(QuickActionType.CopyFullPath, "复制路径", "", "把完整路径复制到剪贴板，多个项目按行分隔。", "正在复制路径…"),
        new(QuickActionType.MoveTo, "移动到", "", "选择文件夹后移动项目，同名时自动保留两份。", "正在移动…"),
        new(QuickActionType.Properties, "属性", "", "打开 Windows 文件属性窗口。", "正在打开属性…"),
        new(QuickActionType.Reveal, "显示位置", "", "在文件资源管理器中选中这些项目。", "正在打开文件夹…")
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
