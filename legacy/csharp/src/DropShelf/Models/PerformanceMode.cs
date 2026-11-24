namespace DropShelf.Models;

/// <summary>低性能模式什么时候启用，见 LowPerformance。</summary>
public enum PerformanceMode
{
    /// <summary>使用电池或系统开启节能模式时启用。</summary>
    Auto,
    Normal,
    Low
}
