// 临时附件的缓存：%TEMP%\DropShelf\Attachments 下每次接收一个子文件夹，和 C# 版的 VirtualFileDropService 相同。
// 临时附件可以从一个置物架拖到另一个，每个缓存文件夹记录有几个使用者（置物架、读取结果、正在进行的操作），
// 最后一个使用者释放时在后台删除。持有、释放和删除都在同一把锁内判断，删除开始后不会再有人取得这个缓存。
#pragma once

#include "base.h"

/// 缓存的根文件夹（末尾没有分隔符）。不要 free。
const wchar_t *attachment_cache_root(void);

/// 为一次接收新建一个缓存文件夹，创建者持有一次。失败时返回 NULL。调用方负责 free 返回的路径。
wchar_t *attachment_cache_create(void);

void attachment_cache_retain(const wchar_t *directory);

/// 释放一次持有；最后一个使用者释放时在后台删除整个文件夹。
void attachment_cache_release(const wchar_t *directory);

/// 过 delay_milliseconds 再释放（交给别的程序的临时文件多留一会儿）。
void attachment_cache_release_later(const wchar_t *directory, unsigned delay_milliseconds);

/// 文件所在的缓存文件夹；不在缓存里时返回 NULL。调用方负责 free。
wchar_t *attachment_cache_directory_of(const wchar_t *path);

/// 是不是 DropShelf 自己的缓存文件夹（缓存根下面的一层，不是根本身）。
bool attachment_cache_is_owned(const wchar_t *path);

/// 启动时在后台清理一天前留下的缓存（上次没正常退出时剩下的）。
void attachment_cache_cleanup_stale(void);

/// 把来源程序给的名称变成能用的文件名：去掉路径和不能用的字符，避开保留名，同名时加 (2)、(3)。
/// used 记录这次接收已经用过的名称（不分大小写），新名称会加进去。调用方负责 free。
wchar_t *attachment_safe_name(const wchar_t *supplied, StrList *used);

/// 和文件资源管理器一样，把 Zone.Identifier（下载来源标记）读出来或写上去。读不到时返回 NULL。
char *zone_identifier_read(const wchar_t *path, size_t *length);
void zone_identifier_write(const wchar_t *path, const char *zone, size_t length);

/// 删除整个文件夹（含里面的内容）。
bool directory_delete_tree(const wchar_t *path);
