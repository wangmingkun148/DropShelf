// GTK 后端额外提供的拖放入口（Windows 后端走 COM 的 IDropTarget，不需要这些）。
// 共享的置物架代码只在 Linux 分支里使用它们。
#pragma once

#include "ui.h"

#include <gdk/gdk.h>

typedef enum UiDropPhase
{
    UI_DROP_ENTER,      // 拖动进入窗口：返回 false 表示不接受
    UI_DROP_MOTION,     // 在窗口上移动：返回 false 表示不接受
    UI_DROP_LEAVE,      // 拖出窗口或拖动结束
    UI_DROP_DROP        // 松开：在这个回调里必须立刻开始读数据（此后来源程序可能不再提供）
} UiDropPhase;

/// 松开时 value 是 GTK 已经读好的数据（GdkFileList、字符串或 GdkTexture）；其余阶段为 NULL。
/// action 进出都用 GdkDragAction；进入和移动时返回 false 表示不接收。
typedef bool (*UiDropHandler)(UiWindow *window, void *context, UiDropPhase phase, GdkDrop *drop, const GValue *value, double x, double y,
    GdkDragAction *action);

void ui_window_set_drop_handler(UiWindow *window, UiDropHandler handler, void *context);

/// 这个拖动是不是从本程序自己的窗口里发起的（置物架拖出）。从别的程序拖进来的返回 false。
bool ui_drag_is_local(GdkDrag *drag);

/// 拖动达到阈值、GTK 即将开始拖出时调用：返回内容；返回 NULL 表示这个位置不拖出。
/// actions 进出都用 GdkDragAction；drag_data 原样交给 drag_end，用来收尾（释放持有的项目等）。
typedef GdkContentProvider *(*UiDragPrepare)(UiWindow *window, void *context, double x, double y, GdkDragAction *actions, void **drag_data);
typedef void (*UiDragEnd)(UiWindow *window, void *context, void *drag_data, GdkDragAction action);

void ui_window_set_drag_handler(UiWindow *window, UiDragPrepare prepare, UiDragEnd end, void *context);

/// 鼠标事件穿透到下面的窗口（Windows 上是 WS_EX_TRANSPARENT）：收尾动画卡片窗口用。
void ui_window_set_click_through(UiWindow *window, bool on);