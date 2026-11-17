// 缩略图：向 Windows 外壳要文件的缩略图（和资源管理器里看到的一样），在图片处理线程上取。
// 最近用过的 32 张留在缓存里，同一文件放进多个置物架时不用重新生成。与 C# 版的 ShellThumbnailService 相同。
#pragma once

#include "gfx.h"

/// 取到后在界面线程调用 ready；image 可能是 NULL（没有缩略图），不是 NULL 时由 ready 收下这个引用。
typedef void (*ThumbnailReady)(GfxImage *image, void *context);

/// size 是需要的像素大小（正方形，按比例缩进去）。
/// cancelled 可以是 NULL；不是时，轮到这个请求时先看它，已经变为非零就不取了，ready 收到 NULL。
/// 它要一直有效到 ready 被调用（通常放在 context 里）。
void thumbnail_load(const wchar_t *path, int size, volatile LONG *cancelled, ThumbnailReady ready, void *context);
