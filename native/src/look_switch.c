#include "look_switch.h"
#include "about_window.h"
#include "main_window.h"
#include "onboarding_window.h"
#include "shelf_peek.h"
#include "shelf_window_internal.h"
#include "tray.h"

void look_switch_to(Look look)
{
    if (look == theme_look()) return;
    // 原来的风格先收尾：置物架的动画走到终点，主窗口的内容拆掉（两种风格各有一份代码，各自清掉自己的东西）。
    shelves_leave_look();
    main_window_prepare_look();
    theme_set_look(look);
    // 新风格接手：置物架按新的大小重新排版，主窗口重建并回到原来那一页，其余窗口换上新的材质和图标。
    shelves_adopt_look();
    main_window_apply_look();
    shelf_peek_apply_look();
    onboarding_window_apply_look();
    about_window_apply_look();
    tray_refresh_icon();
}
