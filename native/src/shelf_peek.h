// 观察的文件夹里出现新文件时，从屏幕边缘探出的置物架一角：贴着光标那一侧（左或右）的边缘，和光标一样高。
// 简洁：一个小置物架斜着藏在屏幕边缘后面，只露出一个圆角；光标停在上面时它转正、往外伸，露出一整条边，计时暂停。
// 纸牌：斜着探出的一张牌，悬停时再伸出一点。两种都随机露出上面或下面的角，看多了也不腻。
// 点一下，置物架从角那里转一整圈飞出来，落在离边缘不远的地方；没点开时过一会儿缩回，右键直接缩回。
// 设置里打开“每个新文件放入独立置物架”时每个新文件各探出一个角，沿着边缘排开（最多 5 个，再多的并进最新的那个）；
// 关闭时角缩回前来的新文件合在同一个角里，角上显示数量。全屏游戏、演示或锁屏时先等着，之后再探出。
#pragma once

#include "base.h"
#include "gesture.h"

/// 同时探出的角最多几个。
#define SHELF_PEEK_MAXIMUM 5

/// 新文件就绪（folder_watch 交来的）。在界面线程调用。
void shelf_peek_add(const StrList *paths);

/// 点开最新的那个角：置物架带着它的文件转出来。没有探出的角时返回 false。
bool shelf_peek_open(void);

/// 不点开，全部直接缩回。
void shelf_peek_dismiss(void);

/// 退出时：关掉窗口，忘掉还没提示的文件。
void shelf_peek_close(void);

/// 有没有探出着的角、探出的角一共有几个、里面一共有几个文件、最新的角贴着哪条边（测试和诊断用）。
bool shelf_peek_is_shown(void);
int shelf_peek_corner_count(void);
int shelf_peek_count(void);
ScreenEdge shelf_peek_edge(void);

/// 测试用：不看用户是不是在全屏或锁屏，有新文件就探出（构建机上可能一直算作没人在用）。
void shelf_peek_ignore_busy(bool ignore);

/// 角的窗口放在哪里（屏幕像素）：光标靠左半边就贴左边，否则贴右边；和光标一样高，不超出工作区。
POINT shelf_peek_place(POINT cursor, RECT work, int width, int height, ScreenEdge *edge);

/// 已经探出的角（或开着的置物架）占着这些竖直范围时，新的放在哪里（窗口的上沿，屏幕像素）：top 本身或紧贴某一段的上面、下面，
/// 取离 top 最近的、不和它们重叠、也不超出工作区的位置；都放不下时还用 top。busy 里每两个数是一段 [上沿, 下沿)。
int shelf_peek_stack(int top, int height, RECT work, const int *busy, int busy_count);

/// 换风格后：探出着的角换成那种风格的大小，贴着同一条边、中心高度不变。
void shelf_peek_apply_look(void);
