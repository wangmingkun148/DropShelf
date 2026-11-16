// 系统的打开、保存和选择文件夹对话框。filter 和 WPF 的写法一样："程序|*.exe;*.lnk|所有文件|*.*"。
// 取消时返回 NULL；返回的路径由调用方 free。只在界面线程调用（COM 已初始化）。
#pragma once

#include "base.h"

/// initial_directory 不存在时从系统默认的位置开始。
wchar_t *file_dialog_open(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *initial_directory);
wchar_t *file_dialog_folder(HWND owner, const wchar_t *title, const wchar_t *initial_directory);
/// default_extension 不带点，例如 json。
wchar_t *file_dialog_save(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *file_name, const wchar_t *initial_directory,
    const wchar_t *default_extension);
