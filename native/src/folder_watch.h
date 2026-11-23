// 观察文件夹：用户选的文件夹里出现新文件（下载完成、从别处挪进来）时交给界面线程，屏幕边缘随之探出置物架的一角。
// 只看文件夹的第一层，用系统的变化通知（ReadDirectoryChangesW），没有变化时不占处理器。
// 下载中的临时文件不算；改成正式的名字、写完以后才交出。保存文档时替换原文件、改名都不算新文件。
// DropShelf 自己写进去的（快速操作的结果、从置物架拖过去的）也不交出。
#pragma once

#include "base.h"

#define FOLDER_WATCH_MAXIMUM 32

/// 在界面线程调用；paths 只在调用期间有效。
typedef void (*FolderWatchHandler)(const StrList *paths);

/// 观察 folders（换掉原来的；空列表时停下后台线程）。在界面线程调用。
void folder_watch_set(const StrList *folders, FolderWatchHandler handler);
void folder_watch_stop(void);

/// 快速操作开始时 hold，结束时 release 并带上它写出的文件：期间写完的新文件先留着，结束后去掉 written 再交出。
void folder_watch_hold(void);
void folder_watch_release(const StrList *written);

/// 从置物架拖出前后调用：这期间出现、名字和拖出的项目相同（或是资源管理器加了“ - 副本”“ (2)”的）的新文件是拖过去的，不交出。
void folder_watch_drag_begin(const StrList *paths);
void folder_watch_drag_end(void);

/// 下载中的临时文件（.crdownload、.part 等）。
bool folder_watch_is_partial(const wchar_t *name);

/// 不当成新文件的名字：临时文件、Office 的锁文件、desktop.ini 这类。
bool folder_watch_is_ignored(const wchar_t *name);
