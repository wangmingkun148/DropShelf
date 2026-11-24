using DropShelf.Models;
using WpfPoint = System.Windows.Point;

namespace DropShelf.Views;

/// <summary>
/// 正在从某个置物架拖出的项目。放到另一个置物架或“新置物架”上时，对方不去读拖放数据，只在这里记下自己；
/// 拖动结束后由来源置物架把项目原样交过去：文本、链接、缩略图和临时附件都跟着走，是移动而不是再复制一份。
/// </summary>
internal sealed class ShelfDragSession(ShelfWindow source, IReadOnlyList<ShelfItem> items)
{
    /// <summary>进行中的那一次拖出；同一时刻只有一次。</summary>
    public static ShelfDragSession? Current { get; set; }

    public ShelfWindow Source { get; } = source;
    public IReadOnlyList<ShelfItem> Items { get; } = items;

    /// <summary>放到了这个置物架上，以及松手的位置（相对那个窗口）。</summary>
    public ShelfWindow? Target { get; private set; }
    public WpfPoint DropPoint { get; private set; }

    /// <summary>放到了“新置物架”上：拆成一个新的置物架。</summary>
    public bool Split { get; set; }

    public void DropOn(ShelfWindow target, WpfPoint point)
    {
        Target = target;
        DropPoint = point;
    }

    /// <summary>拖动数据是不是这一次拖出的（按来源置物架的标识判断）。</summary>
    public static ShelfDragSession? From(System.Windows.IDataObject data) =>
        Current is { } session && Services.DropDataReader.GetSourceShelfId(data) == session.Source.ViewModel.Id ? session : null;
}
