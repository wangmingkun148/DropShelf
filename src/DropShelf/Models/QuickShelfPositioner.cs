namespace DropShelf.Models;

public readonly record struct QuickShelfPlacement(double Left, double Top);

public static class QuickShelfPositioner
{
    public static QuickShelfPlacement Calculate(
        double cursorX,
        double cursorY,
        double workingLeft,
        double workingTop,
        double workingRight,
        double workingBottom,
        double windowWidth,
        double windowHeight)
    {
        var maximumLeft = Math.Max(workingLeft, workingRight - windowWidth);
        var maximumTop = Math.Max(workingTop, workingBottom - windowHeight);
        return new QuickShelfPlacement(
            Math.Clamp(cursorX - windowWidth / 2, workingLeft, maximumLeft),
            Math.Clamp(cursorY - windowHeight / 2, workingTop, maximumTop));
    }
}
