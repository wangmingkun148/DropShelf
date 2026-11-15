// 界面底座内部用的函数：窗口和元素之间互相调用，别的模块不用。
#pragma once

#include "ui.h"

/// 元素被销毁前调用：窗口不再记着它（光标下的、按下的、根元素）。
void ui_window_forget(UiWindow *window, UiElement *element);

/// 把元素和它的子元素都记到 window 名下。
void ui_element_attach(UiElement *element, UiWindow *window);

/// 控件里存的文字：文字表里的一条，或自己留的一份副本。
typedef struct StoredText
{
    int string;
    wchar_t *literal;
} StoredText;

StoredText ui_text_store(UiText text);
void ui_text_free(StoredText *text);
/// 现在该显示的文字：文字表里的按当前语言现取。
const wchar_t *ui_text_get(const StoredText *text);
