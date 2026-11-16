// 界面语言：跟随 Windows 显示语言，或固定为中文、英文；还可以选用语言包。
// 每条界面文字同时写出中文和英文，按当前语言返回；选了语言包时按原文模板找译文，没有译文的显示原文。
#pragma once

#include "base.h"

// ───────────── 语言包 ─────────────
// 语言包是一个 JSON 文件，把界面上的原文对照成另一种语言的译文，不用改程序就能增加语言。
// 它对照中文或英文（from 为 zh 或 en）逐条给出译文，带参数的文字按模板对照，
// 例如 "已添加 {0} 项": "{0} 件追加しました"。

/// 超过这么大的文件不读：正常的语言包只有几十 KB。
#define LANGUAGE_PACK_MAXIMUM_SIZE (2 * 1024 * 1024)
#define LANGUAGE_PACK_FOLDER L"Languages"

typedef struct LanguagePack LanguagePack;

/// 解析语言包的内容；不是语言包或一条译文都没有时返回 NULL。id 是文件名（不含扩展名）。
LanguagePack *language_pack_parse(const wchar_t *id, const wchar_t *json);

/// 读取语言包文件；太大、读不了或不是语言包时返回 NULL。
LanguagePack *language_pack_load(const wchar_t *path);
void language_pack_free(LanguagePack *pack);

const wchar_t *language_pack_id(const LanguagePack *pack);      // 保存在设置里，例如 ja、zh-TW
const wchar_t *language_pack_name(const LanguagePack *pack);    // 显示在设置里的名称，例如 日本語
bool language_pack_from_chinese(const LanguagePack *pack);      // 译文对照的是中文原文，否则对照英文
int language_pack_count(const LanguagePack *pack);

/// 原文对应的译文；没有时返回 NULL。
const wchar_t *language_pack_find(const LanguagePack *pack, const wchar_t *source);

/// 重新查找语言包：程序所在文件夹和 user_directory（设置所在文件夹里的 Languages 文件夹），后者的同名文件优先。
/// 之前选用的语言包随之失效，调用后要重新 language_set_pack。
void language_packs_reload(const wchar_t *user_directory);
int language_packs_count(void);
LanguagePack *language_packs_at(int index);                     // 按名称排序
LanguagePack *language_packs_find(const wchar_t *id);           // 不区分大小写；没有时返回 NULL

// ───────────── 当前语言 ─────────────

void language_set_mode(int mode);                               // AppLanguage
bool language_is_chinese(void);

/// 跟随系统时，Windows 显示语言为中文（包括繁体）使用中文，其他语言使用英文。
bool language_resolve_is_chinese(int mode, LANGID ui_language);

/// 选用语言包；NULL 表示不使用。
void language_set_pack(LanguagePack *pack);
LanguagePack *language_current_pack(void);

/// 语言或语言包变化后调用 handler，已显示的文字需要重新读取。
typedef void (*LanguageChangedHandler)(void *context);
void language_add_listener(LanguageChangedHandler handler, void *context);
void language_remove_listener(LanguageChangedHandler handler, void *context);

// ───────────── 文字 ─────────────

/// 没有参数的文字。返回的指针在语言包重新加载之前有效，不要 free。
const wchar_t *tr(const wchar_t *chinese, const wchar_t *english);

/// 带参数的文字里的一个参数：整数或字符串。
typedef struct LocArg
{
    bool is_text;
    long long number;
    const wchar_t *text;
} LocArg;

#define ARG_INT(value) ((LocArg){ false, (long long)(value), NULL })
#define ARG_TEXT(value) ((LocArg){ true, 0, (value) })
#define LOC_MAXIMUM_ARGUMENTS 6

/// 一条带参数的文字：模板里参数的位置写成 {0}、{1}……，花括号本身写成 {{ 和 }}。语言包用模板查找译文。
typedef struct LocFormat
{
    const wchar_t *template_text;
    int count;
    LocArg arguments[LOC_MAXIMUM_ARGUMENTS];
} LocFormat;

/// 把参数填进模板。模板里参数位置写错（超出个数、花括号不成对）时返回 false，out 不变。
bool loc_format(WStr *out, const wchar_t *template_text, const LocArg *arguments, int count);

/// 带参数的文字。有译文时把参数填进译文；没有、或译文里参数位置写错时显示原文。调用方负责 free。
wchar_t *trf(const LocFormat *chinese, const LocFormat *english);

/// 英文里的 "1 item" / "3 items"。调用方负责 free。
wchar_t *english_count(long long count, const wchar_t *one, const wchar_t *many);

/// 导出翻译模板时用：设置之后每取一条文字都把它的中英文原文（带参数的是模板）交给 collector。NULL 取消。
typedef void (*LanguageCollector)(const wchar_t *chinese, const wchar_t *english, void *context);
void language_set_collector(LanguageCollector collector, void *context);

/// 把全部界面文字的原文写成一个空白的语言包（JSON），交给翻译的人去填；from_chinese 决定对照中文还是英文。调用方 free。
wchar_t *language_template_build(bool from_chinese);
