// 界面文字。没有参数的文字列在 strings.def 里，用 S(STR_名称) 读取；带参数的文字是下面的函数。
// 都按当前语言（或语言包）返回，见 lang.h。
#pragma once

#include "lang.h"

typedef enum StringId
{
#define STR(name, chinese, english) STR_##name,
#include "strings.def"
#undef STR
    STR_COUNT
} StringId;

/// 当前语言下的文字。返回的指针在语言包重新加载之前有效，不要 free。
const wchar_t *S(StringId id);

/// 一条文字的中文和英文原文；导出翻译模板时用。
void string_sources(StringId id, const wchar_t **chinese, const wchar_t **english);

// ───────────── 带参数的文字 ─────────────
// 返回的字符串由调用方 free。其余带参数的文字在移植到用它们的界面时再加进来。

wchar_t *str_item_count(int count);     // 3 个项目 / 3 items
wchar_t *str_added(int count);          // 已添加 3 项 / Added 3 items
wchar_t *str_file_count(int count);     // 3 个文件
wchar_t *str_folder_count(int count);   // 3 个文件夹
wchar_t *str_text_count(int count);     // 3 段文本
wchar_t *str_link_count(int count);     // 3 个链接
wchar_t *str_folder_items(int count);   // 3 项
wchar_t *str_text_detail(int characters);               // 文本 · 12 个字符
wchar_t *str_file_type(const wchar_t *extension);       // PDF 文件
wchar_t *str_shelf_window_title(const wchar_t *title);  // DropShelf 置物架 - 3 个项目
wchar_t *str_skipped_duplicates(int count);             // ，忽略 2 个重复项
wchar_t *str_unavailable_items(int count);              // ，2 项无法访问
wchar_t *str_removed_items(int count);                  // 已移除 3 项
wchar_t *str_converted_to_files(int count);             // 已转成 2 个文件
wchar_t *str_copied_files(int count);                   // 已复制 3 个文件
wchar_t *str_copied_paths(int count);                   // 已复制 3 个路径
wchar_t *str_reading_files(int count);                  // 正在读取 30 个文件…
wchar_t *str_with_failed_attachments(const wchar_t *message, int failed);
