namespace DropShelf.Models;

public enum QuickShelfSize
{
    Small,
    Medium,
    Large
}

public static class QuickShelfSizeOptions
{
    /// <summary>收起状态下置物架的宽度（与设备无关像素）。</summary>
    public static double ResolveWidth(QuickShelfSize size) => size switch
    {
        QuickShelfSize.Small => 184,
        QuickShelfSize.Large => 264,
        _ => 220
    };

    /// <summary>缩略图堆叠区域的高度，按宽度等比计算，三档保持相同比例。</summary>
    public static double ResolveStackHeight(QuickShelfSize size) => Math.Round(ResolveWidth(size) * 0.66);

    /// <summary>展开列表时的宽度：比收起时宽 80，给文件名留出空间，并和收起时一样随三档尺寸变化（264、300、344）。</summary>
    public static double ResolveExpandedWidth(QuickShelfSize size) => ResolveWidth(size) + 80;
}
