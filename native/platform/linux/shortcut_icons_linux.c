// 快捷方式图标跟着风格换是 Windows 的事（改 .lnk 里指向的图标资源）；Linux 下快捷方式（.desktop）的图标
// 由桌面环境按名字查，两种风格共用同一个图标，切换风格时不用改。
#include "shortcut_icons.h"

void shortcut_icons_sync(Look look)
{
    (void)look;
}