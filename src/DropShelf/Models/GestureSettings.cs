namespace DropShelf.Models;

public enum GestureSensitivity
{
    Responsive,
    Balanced,
    Steady
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
