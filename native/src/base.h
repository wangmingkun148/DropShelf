// 基础工具：内存、字符串、路径和文件。程序内部一律用 UTF-16（wchar_t），只在读写文件时转成 UTF-8。
#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <wchar.h>

// ───────────── 内存 ─────────────
// 分配失败时直接结束程序：托盘程序用到的内存很少，分配不出来时也没有可行的补救。

void *xalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *pointer, size_t size);

// ───────────── 字符串 ─────────────

wchar_t *wcs_dup(const wchar_t *text);                  // text 为 NULL 时返回 NULL
wchar_t *wcs_dup_n(const wchar_t *text, size_t length);
wchar_t *wcs_trim_dup(const wchar_t *text);             // 去掉首尾空白后的副本
bool wcs_is_blank(const wchar_t *text);                 // NULL、空或全是空白
bool wchar_is_white_space(wchar_t character);           // 与 C# 的 char.IsWhiteSpace 相同
/// 不分大小写（与 C# 的 OrdinalIgnoreCase 相同）查找 part，找不到时返回 NULL。
const wchar_t *wcs_find_ignore_case(const wchar_t *text, const wchar_t *part);
bool wcs_equal(const wchar_t *first, const wchar_t *second);        // 两个都是 NULL 也算相同
bool wcs_equal_ignore_case(const wchar_t *first, const wchar_t *second);

/// 可以不断追加的字符串。
typedef struct WStr
{
    wchar_t *data;
    size_t length;
    size_t capacity;
} WStr;

void wstr_append(WStr *string, const wchar_t *text);
void wstr_append_n(WStr *string, const wchar_t *text, size_t length);
void wstr_append_char(WStr *string, wchar_t character);
void wstr_appendf(WStr *string, const wchar_t *format, ...);
void wstr_appendv(WStr *string, const wchar_t *format, va_list arguments);
wchar_t *wstr_take(WStr *string);                       // 取走内容（不会是 NULL），string 清空
void wstr_free(WStr *string);

/// 字符串列表，列表拥有其中的字符串。
typedef struct StrList
{
    wchar_t **items;
    int count;
    int capacity;
} StrList;

void strlist_add(StrList *list, const wchar_t *text);   // 加入一份副本
void strlist_add_owned(StrList *list, wchar_t *text);   // 直接收下 text
void strlist_clear(StrList *list);
void strlist_clone(StrList *target, const StrList *source);
int strlist_index_of(const StrList *list, const wchar_t *text, bool ignore_case);
bool strlist_equal(const StrList *first, const StrList *second, bool ignore_case);

/// 排好序的一组文字，用来在一大批里查重：逐个比较要比 n² 次，排好序以后每次只比十几次。
/// 不拥有这些文字，它们要比集合活得久。ignore_case 时两段文字算不算相同和 wcs_equal_ignore_case 一致。用 {0} 初始化。
typedef struct TextSet
{
    const wchar_t **items;
    int count;
    int capacity;
    bool ignore_case;
} TextSet;

bool text_set_add(TextSet *set, const wchar_t *text);   // 已经有相同的就不加，返回 false
bool text_set_contains(const TextSet *set, const wchar_t *text);
void text_set_clear(TextSet *set);

char *utf8_from_wide(const wchar_t *text, size_t *length);
wchar_t *wide_from_utf8(const char *text, size_t length);

// ───────────── 路径和文件 ─────────────

wchar_t *path_join(const wchar_t *directory, const wchar_t *name);
wchar_t *path_directory(const wchar_t *path);           // 没有文件夹部分时返回 NULL
const wchar_t *path_file_name(const wchar_t *path);     // 指向 path 里最后一段
wchar_t *path_file_stem(const wchar_t *path);           // 文件名去掉扩展名
bool path_exists(const wchar_t *path);
bool path_is_directory(const wchar_t *path);
bool directory_create(const wchar_t *path);             // 连同上级文件夹一起创建

/// 目录项是符号链接、联接点这类“指向别处”的重解析点：复制、压缩、统计大小时不能当成里面的内容，也不能进去。
/// OneDrive 等按需下载的占位文件和文件夹、重复数据删除和系统压缩的文件也带“重解析点”属性，
/// 但它们是真正的内容，不算链接。认不得的重解析点按链接处理（和只看属性时一样保守）。
bool find_data_is_link(const WIN32_FIND_DATAW *data);

/// 路径本身是不是上面说的链接；读不到信息时按链接处理，不去动它。
bool path_is_link(const wchar_t *path);

/// 文件名会被 Windows 当成设备（CON、NUL、COM1 这些）。Windows 10 及更早只看第一个点之前、去掉末尾空格的部分，
/// 所以 NUL.tar.gz、“CON .txt” 也是设备：写进去的内容会丢掉，或者根本打不开。
bool file_name_is_reserved(const wchar_t *name);

/// 读取文本文件（UTF-8，认得 UTF-8 和 UTF-16 的 BOM）。文件不存在、读不了或超过 maximum 字节时返回 NULL。
wchar_t *file_read_text(const wchar_t *path, size_t maximum);

/// 读不懂的设置或记录文件被默认值覆盖之前，复制一份到旁边的 .bad（已有的换成这次的），手改坏了还能找回来。
void file_keep_unreadable(const wchar_t *path);

/// 先写到旁边的 .tmp 再替换原文件，中途出错时原文件不受影响。失败时返回 false，错误码在 GetLastError()。
bool file_write_atomic(const wchar_t *path, const void *data, size_t length);

wchar_t *env_get(const wchar_t *name);                  // 没有设置或为空时返回 NULL
wchar_t *local_app_data_directory(void);                // %LOCALAPPDATA%
wchar_t *executable_path(void);
wchar_t *executable_directory(void);

/// 新的标识：32 位小写十六进制，和 C# 的 Guid.NewGuid().ToString("N") 一样。
wchar_t *new_guid_text(void);

// ───────────── 后台保存 ─────────────

/// 在后台线程按顺序写文件，界面不用等（杀毒软件检查写入时可能要几十毫秒）。
/// 连续保存时只写最新的内容：还没开始写的旧内容直接丢掉。
typedef struct SaveQueue
{
    INIT_ONCE once;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE changed;
    HANDLE thread;
    wchar_t *path;
    char *data;
    size_t length;
    bool busy;
    const wchar_t *failure_message;
} SaveQueue;

#define SAVE_QUEUE_INIT(message) { INIT_ONCE_STATIC_INIT, {0}, {0}, NULL, NULL, NULL, 0, false, (message) }

void save_queue_post(SaveQueue *queue, const wchar_t *path, char *data, size_t length);   // 收下 data
void save_queue_flush(SaveQueue *queue);                // 等还没写完的保存写完
