// 快捷方式的图标跟着风格换：开始菜单、桌面和固定到任务栏的快捷方式里指向本程序的那几个，图标改成程序里对应风格的图标
// （简洁是第一个，纸牌是第二个）。exe 文件本身的图标总是第一个，改不了。在后台做，找不到或改不了的就算了。
#pragma once

#include "theme.h"

void shortcut_icons_sync(Look look);
