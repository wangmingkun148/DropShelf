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

/// 中文模板里直接填数量，英文模板里填“数量 + 单复数”。
static wchar_t *counted(const wchar_t *chinese_template, const wchar_t *english_template, int count, const wchar_t *one, const wchar_t *many)
{
    wchar_t *english_value = english_count(count, one, many);
    LocFormat chinese = { chinese_template, 1, { ARG_INT(count) } };
    LocFormat english = { english_template, 1, { ARG_TEXT(english_value) } };
    wchar_t *text = trf(&chinese, &english);
    free(english_value);
    return text;
}

/// 中英文都直接填数量（英文不分单复数的几条）。
static wchar_t *numbered(const wchar_t *chinese_template, const wchar_t *english_template, int count)
{
    LocFormat chinese = { chinese_template, 1, { ARG_INT(count) } };
    LocFormat english = { english_template, 1, { ARG_INT(count) } };
    return trf(&chinese, &english);
}

wchar_t *str_added(int count) { return counted(L"已添加 {0} 项", L"Added {0}", count, L"item", L"items"); }
wchar_t *str_file_count(int count) { return counted(L"{0} 个文件", L"{0}", count, L"file", L"files"); }
wchar_t *str_folder_count(int count) { return counted(L"{0} 个文件夹", L"{0}", count, L"folder", L"folders"); }
wchar_t *str_text_count(int count) { return counted(L"{0} 段文本", L"{0}", count, L"snippet", L"snippets"); }
wchar_t *str_link_count(int count) { return counted(L"{0} 个链接", L"{0}", count, L"link", L"links"); }
wchar_t *str_folder_items(int count) { return counted(L"{0} 项", L"{0}", count, L"item", L"items"); }
wchar_t *str_text_detail(int characters) { return counted(L"文本 · {0} 个字符", L"Text · {0}", characters, L"character", L"characters"); }
wchar_t *str_skipped_duplicates(int count) { return counted(L"，忽略 {0} 个重复项", L", skipped {0}", count, L"duplicate", L"duplicates"); }
wchar_t *str_unavailable_items(int count) { return numbered(L"，{0} 项无法访问", L", {0} couldn't be accessed", count); }
wchar_t *str_removed_items(int count) { return counted(L"已移除 {0} 项", L"Removed {0}", count, L"item", L"items"); }
wchar_t *str_converted_to_files(int count) { return counted(L"已转成 {0} 个文件", L"Turned into {0}", count, L"file", L"files"); }
wchar_t *str_copied_files(int count) { return counted(L"已复制 {0} 个文件", L"Copied {0}", count, L"file", L"files"); }
wchar_t *str_copied_paths(int count) { return numbered(L"已复制 {0} 个路径", L"Copied {0} paths", count); }
wchar_t *str_reading_files(int count) { return numbered(L"正在读取 {0} 个文件…", L"Reading {0} files…", count); }

wchar_t *str_file_type(const wchar_t *extension)
{
    LocFormat chinese = { L"{0} 文件", 1, { ARG_TEXT(extension) } };
    LocFormat english = { L"{0} File", 1, { ARG_TEXT(extension) } };
    return trf(&chinese, &english);
}

wchar_t *str_shelf_window_title(const wchar_t *title)
{
    LocFormat chinese = { L"DropShelf 置物架 - {0}", 1, { ARG_TEXT(title) } };
    LocFormat english = { L"DropShelf shelf - {0}", 1, { ARG_TEXT(title) } };
    return trf(&chinese, &english);
}

wchar_t *str_with_failed_attachments(const wchar_t *message, int failed)
{
    wchar_t *attachments = english_count(failed, L"attachment", L"attachments");
    LocFormat chinese = { L"{0}，{1} 个附件接收失败", 2, { ARG_TEXT(message), ARG_INT(failed) } };
    LocFormat english = { L"{0}; {1} couldn't be received", 2, { ARG_TEXT(message), ARG_TEXT(attachments) } };
    wchar_t *text = trf(&chinese, &english);
    free(attachments);
    return text;
}
