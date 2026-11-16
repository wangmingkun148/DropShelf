#include "lang.h"
#include "json.h"
#include "log.h"
#include "settings.h"

#include <stdlib.h>
#include <string.h>

// ───────────── 语言包 ─────────────

struct LanguagePack
{
    wchar_t *id;
    wchar_t *name;
    bool from_chinese;
    int count;
    int capacity;           // 2 的整数次幂
    wchar_t **keys;         // 开放寻址的哈希表，空位为 NULL
    wchar_t **values;
};

static unsigned hash_text(const wchar_t *text)
{
    unsigned hash = 2166136261u;
    for (; *text; text++) hash = (hash ^ (unsigned)*text) * 16777619u;
    return hash;
}

static int pack_slot(const LanguagePack *pack, const wchar_t *key)
{
    int slot = (int)(hash_text(key) & (unsigned)(pack->capacity - 1));
    while (pack->keys[slot] && wcscmp(pack->keys[slot], key) != 0) slot = (slot + 1) & (pack->capacity - 1);
    return slot;
}

LanguagePack *language_pack_parse(const wchar_t *id, const wchar_t *json)
{
    JsonValue *root = json_parse(json, JSON_ALLOW_COMMENTS | JSON_ALLOW_TRAILING_COMMAS);
    const JsonValue *entries = json_get(root, L"strings");
    if (!entries || entries->type != JSON_OBJECT)
    {
        json_free(root);
        return NULL;
    }

    LanguagePack *pack = xcalloc(1, sizeof(LanguagePack));
    pack->capacity = 16;
    while (pack->capacity < entries->count * 2) pack->capacity *= 2;
    pack->keys = xcalloc((size_t)pack->capacity, sizeof(wchar_t *));
    pack->values = xcalloc((size_t)pack->capacity, sizeof(wchar_t *));
    for (int index = 0; index < entries->count; index++)
    {
        // 译文留空的条目不算，显示原文。
        const JsonValue *value = entries->items[index];
        if (value->type != JSON_STRING || !*value->string) continue;
        int slot = pack_slot(pack, entries->keys[index]);
        if (pack->keys[slot])
        {
            free(pack->values[slot]);
        }
        else
        {
            pack->keys[slot] = wcs_dup(entries->keys[index]);
            pack->count++;
        }
        pack->values[slot] = wcs_dup(value->string);
    }
    if (pack->count == 0)
    {
        language_pack_free(pack);
        json_free(root);
        return NULL;
    }

    const JsonValue *name = json_get(root, L"name");
    const JsonValue *from = json_get(root, L"from");
    pack->id = wcs_dup(id);
    pack->name = name && name->type == JSON_STRING ? wcs_trim_dup(name->string) : NULL;
    if (!pack->name || !*pack->name)
    {
        free(pack->name);
        pack->name = wcs_dup(id);
    }
    pack->from_chinese = !(from && from->type == JSON_STRING && _wcsicmp(from->string, L"en") == 0);
    json_free(root);
    return pack;
}

LanguagePack *language_pack_load(const wchar_t *path)
{
    wchar_t *text = file_read_text(path, LANGUAGE_PACK_MAXIMUM_SIZE);
    if (!text) return NULL;
    wchar_t *id = path_file_stem(path);
    LanguagePack *pack = language_pack_parse(id, text);
    free(id);
    free(text);
    return pack;
}

void language_pack_free(LanguagePack *pack)
{
    if (!pack) return;
    for (int slot = 0; slot < pack->capacity; slot++)
    {
        free(pack->keys[slot]);
        free(pack->values[slot]);
    }
    free(pack->keys);
    free(pack->values);
    free(pack->id);
    free(pack->name);
    free(pack);
}

const wchar_t *language_pack_id(const LanguagePack *pack) { return pack->id; }
const wchar_t *language_pack_name(const LanguagePack *pack) { return pack->name; }
bool language_pack_from_chinese(const LanguagePack *pack) { return pack->from_chinese; }
int language_pack_count(const LanguagePack *pack) { return pack->count; }

const wchar_t *language_pack_find(const LanguagePack *pack, const wchar_t *source)
{
    return pack->values[pack_slot(pack, source)];
}

// ───────────── 当前语言 ─────────────

typedef struct Listener
{
    LanguageChangedHandler handler;
    void *context;
} Listener;

static int current_mode = LANGUAGE_SYSTEM;
static int current_is_chinese = -1;     // -1：还没有判断过
static LanguagePack *current_pack;
static LanguagePack **available_packs;
static int available_count;
static Listener *listeners;
static int listener_count;

static void notify_language_changed(void)
{
    // 处理过程中可能增减监听者，先留一份副本。
    int count = listener_count;
    if (count == 0) return;
    Listener *copy = xalloc((size_t)count * sizeof(Listener));
    memcpy(copy, listeners, (size_t)count * sizeof(Listener));
    for (int index = 0; index < count; index++) copy[index].handler(copy[index].context);
    free(copy);
}

void language_add_listener(LanguageChangedHandler handler, void *context)
{
    listeners = xrealloc(listeners, (size_t)(listener_count + 1) * sizeof(Listener));
    listeners[listener_count++] = (Listener){ handler, context };
}

void language_remove_listener(LanguageChangedHandler handler, void *context)
{
    for (int index = 0; index < listener_count; index++)
    {
        if (listeners[index].handler != handler || listeners[index].context != context) continue;
        memmove(&listeners[index], &listeners[index + 1], (size_t)(listener_count - index - 1) * sizeof(Listener));
        listener_count--;
        return;
    }
}

bool language_resolve_is_chinese(int mode, LANGID ui_language)
{
    if (mode == LANGUAGE_CHINESE) return true;
    if (mode == LANGUAGE_ENGLISH) return false;
    return PRIMARYLANGID(ui_language) == LANG_CHINESE;
}

bool language_is_chinese(void)
{
    if (current_is_chinese < 0) current_is_chinese = language_resolve_is_chinese(current_mode, GetUserDefaultUILanguage());
    return current_is_chinese == 1;
}

void language_set_mode(int mode)
{
    bool before = language_is_chinese();
    current_mode = mode;
    bool chinese = language_resolve_is_chinese(mode, GetUserDefaultUILanguage());
    if (chinese == before) return;
    current_is_chinese = chinese;
    notify_language_changed();
}

void language_set_pack(LanguagePack *pack)
{
    if (pack == current_pack) return;
    current_pack = pack;
    notify_language_changed();
}

LanguagePack *language_current_pack(void)
{
    return current_pack;
}

static int compare_pack_names(const void *first, const void *second)
{
    const LanguagePack *a = *(LanguagePack *const *)first, *b = *(LanguagePack *const *)second;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a->name, -1, b->name, -1, NULL, NULL, 0) - CSTR_EQUAL;
}

static void load_packs_from(const wchar_t *directory)
{
    if (!directory || !path_is_directory(directory)) return;
    wchar_t *pattern = path_join(directory, L"*.json");
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return;
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // FindFirstFile 的 *.json 也会匹配 .jsonx 这样的扩展名，再核对一次。
        const wchar_t *extension = wcsrchr(found.cFileName, L'.');
        if (!extension || _wcsicmp(extension, L".json") != 0) continue;
        wchar_t *path = path_join(directory, found.cFileName);
        LanguagePack *pack = language_pack_load(path);
        free(path);
        if (!pack) continue;

        // 同名的语言包后读到的优先。
        int existing = -1;
        for (int index = 0; index < available_count && existing < 0; index++)
            if (_wcsicmp(available_packs[index]->id, pack->id) == 0) existing = index;
        if (existing >= 0)
        {
            language_pack_free(available_packs[existing]);
            available_packs[existing] = pack;
        }
        else
        {
            available_packs = xrealloc(available_packs, (size_t)(available_count + 1) * sizeof(LanguagePack *));
            available_packs[available_count++] = pack;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

void language_packs_reload(const wchar_t *user_directory)
{
    current_pack = NULL;
    for (int index = 0; index < available_count; index++) language_pack_free(available_packs[index]);
    free(available_packs);
    available_packs = NULL;
    available_count = 0;

    wchar_t *base = executable_directory();
    wchar_t *bundled = path_join(base, LANGUAGE_PACK_FOLDER);
    load_packs_from(bundled);
    load_packs_from(user_directory);
    free(bundled);
    free(base);
    if (available_count > 1) qsort(available_packs, (size_t)available_count, sizeof(LanguagePack *), compare_pack_names);
}

int language_packs_count(void)
{
    return available_count;
}

LanguagePack *language_packs_at(int index)
{
    return index >= 0 && index < available_count ? available_packs[index] : NULL;
}

LanguagePack *language_packs_find(const wchar_t *id)
{
    if (wcs_is_blank(id)) return NULL;
    for (int index = 0; index < available_count; index++)
        if (_wcsicmp(available_packs[index]->id, id) == 0) return available_packs[index];
    return NULL;
}

// ───────────── 文字 ─────────────

static LanguageCollector collector;
static void *collector_context;

void language_set_collector(LanguageCollector handler, void *context)
{
    collector = handler;
    collector_context = context;
}

const wchar_t *tr(const wchar_t *chinese, const wchar_t *english)
{
    if (collector) collector(chinese, english, collector_context);
    if (current_pack)
    {
        const wchar_t *source = current_pack->from_chinese ? chinese : english;
        const wchar_t *translated = language_pack_find(current_pack, source);
        return translated ? translated : source;
    }
    return language_is_chinese() ? chinese : english;
}

bool loc_format(WStr *out, const wchar_t *template_text, const LocArg *arguments, int count)
{
    WStr result = {0};
    for (const wchar_t *cursor = template_text; *cursor; cursor++)
    {
        if (*cursor == L'}')
        {
            if (cursor[1] != L'}') goto failed;
            wstr_append_char(&result, L'}');
            cursor++;
        }
        else if (*cursor != L'{')
        {
            wstr_append_char(&result, *cursor);
        }
        else if (cursor[1] == L'{')
        {
            wstr_append_char(&result, L'{');
            cursor++;
        }
        else
        {
            // 只认 {数字}；对齐和格式（{0,5}、{0:N2}）用不到，遇到时当作写错，由调用方显示原文。
            int index = 0, digits = 0;
            for (cursor++; *cursor >= L'0' && *cursor <= L'9' && digits < 4; cursor++, digits++) index = index * 10 + (*cursor - L'0');
            if (digits == 0 || *cursor != L'}' || index >= count) goto failed;
            if (arguments[index].is_text) wstr_append(&result, arguments[index].text);
            else wstr_appendf(&result, L"%lld", arguments[index].number);
        }
    }
    wstr_append_n(out, result.data ? result.data : L"", result.length);
    wstr_free(&result);
    return true;
failed:
    wstr_free(&result);
    return false;
}

wchar_t *trf(const LocFormat *chinese, const LocFormat *english)
{
    WStr text = {0};
    if (collector) collector(chinese->template_text, english->template_text, collector_context);
    const LocFormat *source = language_is_chinese() ? chinese : english;
    if (current_pack)
    {
        source = current_pack->from_chinese ? chinese : english;
        const wchar_t *translated = language_pack_find(current_pack, source->template_text);
        if (translated && loc_format(&text, translated, source->arguments, source->count)) return wstr_take(&text);
    }
    if (!loc_format(&text, source->template_text, source->arguments, source->count)) wstr_append(&text, source->template_text);
    return wstr_take(&text);
}

wchar_t *english_count(long long count, const wchar_t *one, const wchar_t *many)
{
    WStr text = {0};
    wstr_appendf(&text, L"%lld %ls", count, count == 1 ? one : many);
    return wstr_take(&text);
}
