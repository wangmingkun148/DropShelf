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
