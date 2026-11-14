#include "strings.h"

#include <stdlib.h>

typedef struct StringSource
{
    const wchar_t *chinese;
    const wchar_t *english;
} StringSource;

static const StringSource sources[STR_COUNT] = {
#define STR(name, chinese, english) { chinese, english },
#include "strings.def"
#undef STR
};

const wchar_t *S(StringId id)
{
    if (id < 0 || id >= STR_COUNT) return L"";
    return tr(sources[id].chinese, sources[id].english);
}

void string_sources(StringId id, const wchar_t **chinese, const wchar_t **english)
{
    *chinese = sources[id].chinese;
    *english = sources[id].english;
}

// ───────────── 带参数的文字 ─────────────
// 中文和英文各有自己的模板和参数：英文里的数量连同单复数一起作为一个参数（"3 items"），
// 所以英文模板是 "Added {0}"，语言包对照英文翻译时参数就是 "3 items"。与 C# 版的模板相同。

wchar_t *str_item_count(int count)
{
    wchar_t *counted = english_count(count, L"item", L"items");
    LocFormat chinese = { L"{0} 个项目", 1, { ARG_INT(count) } };
    LocFormat english = { L"{0}", 1, { ARG_TEXT(counted) } };
    wchar_t *text = trf(&chinese, &english);
    free(counted);
    return text;
}

wchar_t *str_added(int count)
{
    wchar_t *counted = english_count(count, L"item", L"items");
    LocFormat chinese = { L"已添加 {0} 项", 1, { ARG_INT(count) } };
    LocFormat english = { L"Added {0}", 1, { ARG_TEXT(counted) } };
    wchar_t *text = trf(&chinese, &english);
    free(counted);
    return text;
}
