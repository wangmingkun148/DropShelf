namespace DropShelf.Models;

/// <summary>2.5 及以前的三档晃动灵敏度，仅用于迁移旧设置。</summary>
public enum GestureSensitivity
{
    Responsive,
    Balanced,
    Steady
}

/// <summary>晃动灵敏度 0–100：0 最稳健，100 最灵敏，50 为默认（与旧版的均衡档相同）。</summary>
public static class ShakeSensitivities
{
    public const int Steady = 0;
    public const int Balanced = 50;
    public const int Responsive = 100;

    public static int Clamp(int value) => Math.Clamp(value, Steady, Responsive);

    public static int FromLegacy(GestureSensitivity legacy) => legacy switch
    {
        GestureSensitivity.Responsive => Responsive,
        // 旧版的稳健档比现在的最稳健容易，约等于 20。
        GestureSensitivity.Steady => 20,
        _ => Balanced
    };
}

/// <summary>拖动时用哪种方式自动弹出置物架。</summary>
public enum AutoOpenTrigger
{
    Shake,
    Edge,
    Both
}

/// <summary>拖到屏幕边缘后需要停留多久才弹出置物架。</summary>
public enum EdgeDwellTime
{
    Short,
    Medium,
    Long
}

public enum ScreenEdge
{
    None,
    Left,
    Top,
    Right
}

public static class AutoOpenTriggers
{
    public static bool UsesShake(AutoOpenTrigger trigger) => trigger is AutoOpenTrigger.Shake or AutoOpenTrigger.Both;
    public static bool UsesEdge(AutoOpenTrigger trigger) => trigger is AutoOpenTrigger.Edge or AutoOpenTrigger.Both;

    public static int DwellMilliseconds(EdgeDwellTime dwell) => dwell switch
    {
        EdgeDwellTime.Short => 300,
        EdgeDwellTime.Long => 1000,
        _ => 600
    };
}
