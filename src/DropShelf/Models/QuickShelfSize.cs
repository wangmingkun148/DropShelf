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

    /// <summary>展开列表时的宽度，保证文件名有足够空间。</summary>
    public static double ResolveExpandedWidth(QuickShelfSize size) => Math.Max(ResolveWidth(size), 300);
}
