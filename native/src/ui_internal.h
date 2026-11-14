// 界面底座内部用的函数：窗口和元素之间互相调用，别的模块不用。
#pragma once

#include "ui.h"

/// 元素被销毁前调用：窗口不再记着它（光标下的、按下的、根元素）。
void ui_window_forget(UiWindow *window, UiElement *element);

/// 只重画，不重新排版（悬停、按下这类外观变化）。
void ui_window_repaint(UiWindow *window);

/// 把元素和它的子元素都记到 window 名下。
void ui_element_attach(UiElement *element, UiWindow *window);
