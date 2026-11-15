// 置物架窗口和它们的管理，和 C# 版的 ShelfWindow、ShelfManager 对应。
// 同时可以开着多个置物架；晃动呼出的空置物架会被复用，避免越开越多。
#pragma once

#include "base.h"
#include "gesture.h"

typedef struct ShelfWindow ShelfWindow;

/// 在 center（屏幕像素）打开置物架。by_gesture：由拖动手势呼出，edge 不是 EDGE_NONE 时从那条屏幕边缘滑入。
/// activate：成为当前窗口（快捷键、托盘菜单打开时）。返回打开的置物架；创建失败时返回 NULL。
ShelfWindow *shelves_open_at(POINT center, bool by_gesture, bool activate, ScreenEdge edge);
ShelfWindow *shelves_open_at_cursor(bool activate);

/// 把这些文件放进置物架（命令行传来的路径）；有重复或读不到的会提示。
void shelf_window_add_paths(ShelfWindow *shelf, const StrList *paths);

/// 拖动手势之后松开了鼠标：由手势打开、什么都没放进来的置物架稍后自动关闭。
void shelves_drag_released(void);

/// 尺寸、拖出方式等设置变了：已打开的置物架立即更新。
void shelves_apply_settings(void);

/// 空闲时准备一个隐藏的置物架窗口，手势呼出时不用现建。
void shelves_prewarm(void);

/// 退出时关掉全部置物架（不播放动画）。
void shelves_close_all(void);

/// 打开着的置物架有几个（测试和诊断用）。
int shelves_open_count(void);
