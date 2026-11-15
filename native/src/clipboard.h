// 剪贴板。剪贴板常被其他程序短暂占用，写入都重试几次再放弃，和 C# 版的 ClipboardService 相同。
#pragma once

#include "base.h"

#include <ole2.h>

bool clipboard_set_text(const wchar_t *text);

/// 以“复制文件”的方式写入，可直接在资源管理器或聊天软件中粘贴；同时附上路径文字。
bool clipboard_set_files(const StrList *paths);

/// 剪贴板上的数据；取不到时返回 NULL。调用方负责 Release。
IDataObject *clipboard_get_data(void);
