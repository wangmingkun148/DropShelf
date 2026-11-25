#include "settings.h"
#include "json.h"
#include "log.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define COUNT_OF(array) ((int)(sizeof(array) / sizeof((array)[0])))

// ───────────── 取值的名称 ─────────────

typedef struct EnumName
{
    const wchar_t *name;
    int value;
} EnumName;

typedef struct EnumTable
{
    const EnumName *names;
    int count;
} EnumTable;

#define ENUM_TABLE(table) { table, COUNT_OF(table) }

static const EnumName trigger_names[] = { { L"Shake", TRIGGER_SHAKE }, { L"Edge", TRIGGER_EDGE }, { L"Both", TRIGGER_BOTH } };
static const EnumName dwell_names[] = { { L"Short", DWELL_SHORT }, { L"Medium", DWELL_MEDIUM }, { L"Long", DWELL_LONG } };
static const EnumName theme_names[] = { { L"System", THEME_SYSTEM }, { L"Light", THEME_LIGHT }, { L"Dark", THEME_DARK } };
static const EnumName look_names[] = { { L"Simple", LOOK_SIMPLE }, { L"Cards", LOOK_CARDS } };
static const EnumName language_names[] = { { L"System", LANGUAGE_SYSTEM }, { L"Chinese", LANGUAGE_CHINESE }, { L"English", LANGUAGE_ENGLISH } };
static const EnumName size_names[] = { { L"Small", SHELF_SIZE_SMALL }, { L"Medium", SHELF_SIZE_MEDIUM }, { L"Large", SHELF_SIZE_LARGE } };
static const EnumName transfer_names[] = { { L"Move", TRANSFER_MOVE }, { L"Copy", TRANSFER_COPY } };
static const EnumName mixed_drag_names[] = { { L"Smart", MIXED_DRAG_SMART }, { L"Batch", MIXED_DRAG_BATCH }, { L"Files", MIXED_DRAG_FILES }, { L"Text", MIXED_DRAG_TEXT } };
static const EnumName layout_names[] = { { L"Off", LAYOUT_OFF }, { L"Two", LAYOUT_TWO }, { L"Four", LAYOUT_FOUR } };
static const EnumName performance_names[] = { { L"Auto", PERFORMANCE_AUTO }, { L"Normal", PERFORMANCE_NORMAL }, { L"Low", PERFORMANCE_LOW } };
static const EnumName sensitivity_names[] = {
    { L"Responsive", SENSITIVITY_RESPONSIVE }, { L"Balanced", SENSITIVITY_BALANCED }, { L"Steady", SENSITIVITY_STEADY }
};
static const EnumName action_type_names[] = {
    { L"None", QUICK_ACTION_NONE }, { L"Properties", QUICK_ACTION_PROPERTIES }, { L"CompressZip", QUICK_ACTION_COMPRESS_ZIP },
    { L"CopyFullPath", QUICK_ACTION_COPY_FULL_PATH }, { L"ConvertPng", QUICK_ACTION_CONVERT_PNG }, { L"ConvertJpeg", QUICK_ACTION_CONVERT_JPEG },
    { L"Reveal", QUICK_ACTION_REVEAL }, { L"MoveTo", QUICK_ACTION_MOVE_TO }, { L"Custom", QUICK_ACTION_CUSTOM }
};
static const EnumName kind_names[] = {
    { L"Folder", CUSTOM_ACTION_FOLDER }, { L"CompressImage", CUSTOM_ACTION_COMPRESS_IMAGE },
    { L"OpenWith", CUSTOM_ACTION_OPEN_WITH }, { L"ConvertImage", CUSTOM_ACTION_CONVERT_IMAGE }
};
static const EnumName format_names[] = { { L"Jpeg", IMAGE_FORMAT_JPEG }, { L"Png", IMAGE_FORMAT_PNG } };

static const EnumTable trigger_table = ENUM_TABLE(trigger_names);
static const EnumTable dwell_table = ENUM_TABLE(dwell_names);
static const EnumTable theme_table = ENUM_TABLE(theme_names);
static const EnumTable look_table = ENUM_TABLE(look_names);
static const EnumTable language_table = ENUM_TABLE(language_names);
static const EnumTable size_table = ENUM_TABLE(size_names);
static const EnumTable transfer_table = ENUM_TABLE(transfer_names);
static const EnumTable mixed_drag_table = ENUM_TABLE(mixed_drag_names);
static const EnumTable layout_table = ENUM_TABLE(layout_names);
static const EnumTable performance_table = ENUM_TABLE(performance_names);
static const EnumTable sensitivity_table = ENUM_TABLE(sensitivity_names);
static const EnumTable action_type_table = ENUM_TABLE(action_type_names);
static const EnumTable kind_table = ENUM_TABLE(kind_names);
static const EnumTable format_table = ENUM_TABLE(format_names);

static bool enum_is_defined(const EnumTable *table, int value)
{
    for (int index = 0; index < table->count; index++)
        if (table->names[index].value == value) return true;
    return false;
}

static const wchar_t *enum_name(const EnumTable *table, int value)
{
    for (int index = 0; index < table->count; index++)
        if (table->names[index].value == value) return table->names[index].name;
    return NULL;
}

// ───────────── 快速操作 ─────────────

/// 内置操作，按设置页中的排列顺序。
static const QuickActionType builtin_actions[] = {
    QUICK_ACTION_COMPRESS_ZIP, QUICK_ACTION_COPY_FULL_PATH, QUICK_ACTION_MOVE_TO, QUICK_ACTION_PROPERTIES, QUICK_ACTION_REVEAL
};

static const QuickActionType default_actions[] = {
    QUICK_ACTION_COMPRESS_ZIP, QUICK_ACTION_COPY_FULL_PATH, QUICK_ACTION_MOVE_TO, QUICK_ACTION_PROPERTIES
};

int quick_action_builtin_count(void)
{
    return COUNT_OF(builtin_actions);
}

QuickActionType quick_action_builtin_type(int index)
{
    return builtin_actions[index];
}

const wchar_t *quick_action_builtin_id(int index)
{
    return enum_name(&action_type_table, builtin_actions[index]);
}

bool quick_action_is_builtin_id(const wchar_t *id)
{
    if (!id) return false;
    for (int index = 0; index < COUNT_OF(builtin_actions); index++)
        if (wcscmp(quick_action_builtin_id(index), id) == 0) return true;
    return false;
}

bool quick_action_exists(const wchar_t *id, const CustomAction *customs, int custom_count)
{
    if (wcs_is_blank(id)) return false;
    if (quick_action_is_builtin_id(id)) return true;
    for (int index = 0; index < custom_count; index++)
        if (wcs_equal(customs[index].id, id)) return true;
    return false;
}

void quick_action_normalize_slots(StrList *slots, const CustomAction *customs, int custom_count)
{
    StrList result = {0};
    for (int index = 0; index < slots->count && result.count < QUICK_ACTION_SLOTS; index++)
    {
        const wchar_t *id = slots->items[index];
        if (quick_action_exists(id, customs, custom_count) && strlist_index_of(&result, id, false) < 0) strlist_add(&result, id);
    }
    // 缺少的位置依次用默认操作、其余内置操作、自定义操作补齐。
    for (int index = 0; index < COUNT_OF(default_actions) && result.count < QUICK_ACTION_SLOTS; index++)
    {
        const wchar_t *id = enum_name(&action_type_table, default_actions[index]);
        if (strlist_index_of(&result, id, false) < 0) strlist_add(&result, id);
    }
    for (int index = 0; index < COUNT_OF(builtin_actions) && result.count < QUICK_ACTION_SLOTS; index++)
    {
        const wchar_t *id = quick_action_builtin_id(index);
        if (strlist_index_of(&result, id, false) < 0) strlist_add(&result, id);
    }
    for (int index = 0; index < custom_count && result.count < QUICK_ACTION_SLOTS; index++)
    {
        const wchar_t *id = customs[index].id;
        if (id && strlist_index_of(&result, id, false) < 0) strlist_add(&result, id);
    }
    strlist_clear(slots);
    *slots = result;
}

// ───────────── 自定义操作 ─────────────

void custom_action_init(CustomAction *action)
{
    memset(action, 0, sizeof(*action));
    action->id = new_guid_text();
    action->kind = CUSTOM_ACTION_FOLDER;
    action->folder_mode = TRANSFER_MOVE;
    action->image_size_kb = CUSTOM_ACTION_DEFAULT_IMAGE_KB;
    action->image_format = IMAGE_FORMAT_JPEG;
}

void custom_action_free(CustomAction *action)
{
    free(action->id);
    free(action->name);
    free(action->folder);
    free(action->program_path);
    free(action->arguments);
    memset(action, 0, sizeof(*action));
}

void custom_action_clone(CustomAction *target, const CustomAction *source)
{
    *target = *source;
    target->id = wcs_dup(source->id);
    target->name = wcs_dup(source->name);
    target->folder = wcs_dup(source->folder);
    target->program_path = wcs_dup(source->program_path);
    target->arguments = wcs_dup(source->arguments);
}

bool custom_action_same(const CustomAction *first, const CustomAction *second)
{
    return wcs_equal(first->id, second->id) && first->kind == second->kind && wcs_equal(first->name, second->name) &&
           wcs_equal(first->folder, second->folder) && first->folder_mode == second->folder_mode &&
           first->image_size_kb == second->image_size_kb && first->image_format == second->image_format &&
           wcs_equal(first->program_path, second->program_path) && wcs_equal(first->arguments, second->arguments);
}

bool custom_action_is_complete(const CustomAction *action)
{
    switch (action->kind)
    {
        case CUSTOM_ACTION_FOLDER: return !wcs_is_blank(action->folder);
        case CUSTOM_ACTION_OPEN_WITH: return !wcs_is_blank(action->program_path);
        default: return true;
    }
}

/// 去掉首尾空白，原来是 NULL 的仍是 NULL。
static void trim_in_place(wchar_t **text)
{
    if (!*text) return;
    wchar_t *trimmed = wcs_trim_dup(*text);
    free(*text);
    *text = trimmed;
}

static void normalize_custom_actions(AppSettings *settings)
{
    if (!settings->has_custom_actions)
    {
        // 首次运行时提供一个压缩图片的示例操作，用户可以直接放到置物架上。
        settings->custom_actions = xcalloc(1, sizeof(CustomAction));
        custom_action_init(&settings->custom_actions[0]);
        settings->custom_actions[0].kind = CUSTOM_ACTION_COMPRESS_IMAGE;
        settings->custom_action_count = 1;
        settings->has_custom_actions = true;
        return;
    }

    int kept = 0;
    for (int index = 0; index < settings->custom_action_count; index++)
    {
        CustomAction *action = &settings->custom_actions[index];
        if (kept == QUICK_ACTION_MAXIMUM_CUSTOM || !enum_is_defined(&kind_table, action->kind) || !custom_action_is_complete(action))
        {
            custom_action_free(action);
            continue;
        }

        bool duplicate = false;
        for (int earlier = 0; earlier < kept && !duplicate; earlier++)
            duplicate = wcs_equal(settings->custom_actions[earlier].id, action->id);
        if (wcs_is_blank(action->id) || quick_action_is_builtin_id(action->id) || duplicate)
        {
            free(action->id);
            action->id = new_guid_text();
        }

        trim_in_place(&action->name);
        if (action->name && !*action->name)
        {
            free(action->name);
            action->name = NULL;
        }
        if (action->name && wcslen(action->name) > CUSTOM_ACTION_MAXIMUM_NAME) action->name[CUSTOM_ACTION_MAXIMUM_NAME] = 0;
        if (!enum_is_defined(&transfer_table, action->folder_mode)) action->folder_mode = TRANSFER_MOVE;
        if (action->image_size_kb < CUSTOM_ACTION_MINIMUM_IMAGE_KB) action->image_size_kb = CUSTOM_ACTION_MINIMUM_IMAGE_KB;
        if (action->image_size_kb > CUSTOM_ACTION_MAXIMUM_IMAGE_KB) action->image_size_kb = CUSTOM_ACTION_MAXIMUM_IMAGE_KB;
        if (!enum_is_defined(&format_table, action->image_format)) action->image_format = IMAGE_FORMAT_JPEG;
        trim_in_place(&action->folder);

        // 程序路径两头的引号也去掉：从资源管理器“复制为路径”得到的带引号。
        trim_in_place(&action->program_path);
        if (action->program_path)
        {
            wchar_t *start = action->program_path;
            while (*start == L'"') start++;
            size_t length = wcslen(start);
            while (length > 0 && start[length - 1] == L'"') length--;
            wchar_t *unquoted = wcs_dup_n(start, length);
            free(action->program_path);
            action->program_path = unquoted;
        }

        if (wcs_is_blank(action->arguments))
        {
            free(action->arguments);
            action->arguments = NULL;
        }
        else
        {
            trim_in_place(&action->arguments);
        }

        if (kept != index)
        {
            settings->custom_actions[kept] = *action;
            memset(action, 0, sizeof(*action));
        }
        kept++;
    }
    settings->custom_action_count = kept;
}

// ───────────── 默认值、复制和释放 ─────────────

void settings_init(AppSettings *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->version = SETTINGS_CURRENT_VERSION;
    settings->window_left = NAN;
    settings->window_top = NAN;
    settings->window_width = 860;
    settings->window_height = 640;
    settings->auto_open_trigger = TRIGGER_BOTH;
    settings->shake_sensitivity = SHAKE_BALANCED;
    settings->edge_dwell = DWELL_MEDIUM;
    settings->theme = THEME_SYSTEM;
    settings->language = LANGUAGE_SYSTEM;
    settings->quick_shelf_size = SHELF_SIZE_MEDIUM;
    settings->drag_out_mode = TRANSFER_MOVE;
    settings->mixed_drag = MIXED_DRAG_BATCH;
    settings->quick_action_layout = LAYOUT_FOUR;
    settings->close_when_empty = true;
    settings->watch_peek_seconds = WATCH_PEEK_DEFAULT_SECONDS;
    settings->performance_mode = PERFORMANCE_AUTO;
    settings->hotkey = hotkey_to_text(HOTKEY_DEFAULT);
    settings->show_getting_started = true;
    settings->legacy_action = QUICK_ACTION_PROPERTIES;
}

void settings_free(AppSettings *settings)
{
    strlist_clear(&settings->blocked_apps);
    strlist_clear(&settings->quick_action_slots);
    strlist_clear(&settings->watched_folders);
    for (int index = 0; index < settings->custom_action_count; index++) custom_action_free(&settings->custom_actions[index]);
    free(settings->custom_actions);
    free(settings->accent_color);
    free(settings->language_pack);
    free(settings->hotkey);
    free(settings->legacy_actions);
    memset(settings, 0, sizeof(*settings));
}

void settings_clone(AppSettings *target, const AppSettings *source)
{
    *target = *source;
    strlist_clone(&target->blocked_apps, &source->blocked_apps);
    strlist_clone(&target->quick_action_slots, &source->quick_action_slots);
    strlist_clone(&target->watched_folders, &source->watched_folders);
    target->custom_actions = xcalloc((size_t)source->custom_action_count, sizeof(CustomAction));
    for (int index = 0; index < source->custom_action_count; index++)
        custom_action_clone(&target->custom_actions[index], &source->custom_actions[index]);
    target->accent_color = wcs_dup(source->accent_color);
    target->language_pack = wcs_dup(source->language_pack);
    target->hotkey = wcs_dup(source->hotkey);
    target->legacy_actions = NULL;
    if (source->legacy_action_count > 0)
    {
        target->legacy_actions = xalloc((size_t)source->legacy_action_count * sizeof(int));
        memcpy(target->legacy_actions, source->legacy_actions, (size_t)source->legacy_action_count * sizeof(int));
    }
}

// ───────────── 整理 ─────────────

bool accent_color_parse(const wchar_t *text, COLORREF *color)
{
    if (!text || wcslen(text) != 7 || text[0] != L'#') return false;
    unsigned value = 0;
    for (int index = 1; index < 7; index++)
    {
        wchar_t character = text[index];
        unsigned digit;
        if (character >= L'0' && character <= L'9') digit = (unsigned)(character - L'0');
        else if (character >= L'a' && character <= L'f') digit = (unsigned)(character - L'a') + 10;
        else if (character >= L'A' && character <= L'F') digit = (unsigned)(character - L'A') + 10;
        else return false;
        value = value * 16 + digit;
    }
    if (color) *color = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

int shake_sensitivity_from_legacy(int legacy)
{
    switch (legacy)
    {
        case SENSITIVITY_RESPONSIVE: return SHAKE_RESPONSIVE;
        // 旧版的稳健档比现在的最稳健容易，约等于 30（2.8.2 把最稳健调得更难之前是 20）。
        case SENSITIVITY_STEADY: return 30;
        default: return SHAKE_BALANCED;
    }
}

bool auto_open_uses_shake(int trigger)
{
    return trigger == TRIGGER_SHAKE || trigger == TRIGGER_BOTH;
}

bool auto_open_uses_edge(int trigger)
{
    return trigger == TRIGGER_EDGE || trigger == TRIGGER_BOTH;
}

int edge_dwell_milliseconds(int dwell)
{
    switch (dwell)
    {
        case DWELL_SHORT: return 300;
        case DWELL_LONG: return 1000;
        default: return 600;
    }
}

/// 旧版本的快速操作迁移为四个位置：1.0 只有一个操作，关闭时保持关闭，其余情况使用默认组合；
/// 2.0 选择的操作按固定顺序放在前面，选了一两个时改为显示两个大按钮。
static void migrate_quick_actions(AppSettings *settings)
{
    StrList legacy = {0};
    if (settings->has_legacy_actions)
    {
        // 去重、去掉已移除的旧操作，并按内置操作的固定顺序最多保留四个。
        for (int index = 0; index < COUNT_OF(builtin_actions) && legacy.count < QUICK_ACTION_SLOTS; index++)
        {
            bool chosen = false;
            for (int item = 0; item < settings->legacy_action_count && !chosen; item++)
                chosen = settings->legacy_actions[item] == (int)builtin_actions[index];
            if (chosen) strlist_add(&legacy, quick_action_builtin_id(index));
        }
    }
    else if (settings->legacy_action != QUICK_ACTION_NONE)
    {
        for (int index = 0; index < COUNT_OF(default_actions); index++)
            strlist_add(&legacy, enum_name(&action_type_table, default_actions[index]));
    }
    settings->quick_action_layout = legacy.count == 0 ? LAYOUT_OFF : legacy.count <= 2 ? LAYOUT_TWO : LAYOUT_FOUR;
    strlist_clear(&settings->quick_action_slots);
    settings->quick_action_slots = legacy;
    settings->has_quick_action_slots = true;
}

void settings_normalize(AppSettings *settings)
{
    if (settings->has_legacy_sensitivity) settings->shake_sensitivity = shake_sensitivity_from_legacy(settings->legacy_sensitivity);
    settings->has_legacy_sensitivity = false;
    if (settings->shake_sensitivity < SHAKE_STEADY) settings->shake_sensitivity = SHAKE_STEADY;
    if (settings->shake_sensitivity > SHAKE_RESPONSIVE) settings->shake_sensitivity = SHAKE_RESPONSIVE;

    if (wcs_is_blank(settings->language_pack))
    {
        free(settings->language_pack);
        settings->language_pack = NULL;
    }
    else
    {
        trim_in_place(&settings->language_pack);
    }

    if (!enum_is_defined(&trigger_table, settings->auto_open_trigger)) settings->auto_open_trigger = TRIGGER_BOTH;
    if (!enum_is_defined(&dwell_table, settings->edge_dwell)) settings->edge_dwell = DWELL_MEDIUM;
    if (!enum_is_defined(&theme_table, settings->theme)) settings->theme = THEME_SYSTEM;
    if (!enum_is_defined(&look_table, settings->look)) settings->look = LOOK_SIMPLE;
    // 跟随 Windows 存为 "system"，和 C# 版的 ThemeService.SystemAccent 相同。
    if (!wcs_equal(settings->accent_color, THEME_SYSTEM_ACCENT_VALUE) && !accent_color_parse(settings->accent_color, NULL))
    {
        free(settings->accent_color);
        settings->accent_color = NULL;
    }
    if (!enum_is_defined(&language_table, settings->language)) settings->language = LANGUAGE_SYSTEM;
    if (!enum_is_defined(&size_table, settings->quick_shelf_size)) settings->quick_shelf_size = SHELF_SIZE_MEDIUM;
    if (!enum_is_defined(&transfer_table, settings->drag_out_mode)) settings->drag_out_mode = TRANSFER_MOVE;
    if (!enum_is_defined(&mixed_drag_table, settings->mixed_drag)) settings->mixed_drag = MIXED_DRAG_BATCH;
    if (!enum_is_defined(&layout_table, settings->quick_action_layout)) settings->quick_action_layout = LAYOUT_FOUR;
    if (settings->legacy_low_performance) settings->performance_mode = PERFORMANCE_LOW;
    settings->legacy_low_performance = false;
    if (!enum_is_defined(&performance_table, settings->performance_mode)) settings->performance_mode = PERFORMANCE_AUTO;

    // 屏蔽的应用只留文件名，去掉空的和重复的。
    StrList apps = {0};
    for (int index = 0; index < settings->blocked_apps.count; index++)
    {
        wchar_t *trimmed = wcs_trim_dup(settings->blocked_apps.items[index]);
        const wchar_t *name = path_file_name(trimmed);
        if (*name && strlist_index_of(&apps, name, true) < 0) strlist_add(&apps, name);
        free(trimmed);
    }
    strlist_clear(&settings->blocked_apps);
    settings->blocked_apps = apps;

    // 观察的文件夹去掉空的、重复的和末尾的分隔符（盘符根目录保留，如 D:\）。
    StrList folders = {0};
    for (int index = 0; index < settings->watched_folders.count && folders.count < WATCH_FOLDERS_MAXIMUM; index++)
    {
        wchar_t *folder = wcs_trim_dup(settings->watched_folders.items[index]);
        size_t length = wcslen(folder);
        while (length > 3 && (folder[length - 1] == L'\\' || folder[length - 1] == L'/')) folder[--length] = 0;
        if (*folder && strlist_index_of(&folders, folder, true) < 0) strlist_add(&folders, folder);
        free(folder);
    }
    strlist_clear(&settings->watched_folders);
    settings->watched_folders = folders;
    if (settings->watch_peek_seconds < WATCH_PEEK_MINIMUM_SECONDS || settings->watch_peek_seconds > WATCH_PEEK_MAXIMUM_SECONDS)
        settings->watch_peek_seconds = WATCH_PEEK_DEFAULT_SECONDS;

    normalize_custom_actions(settings);
    if (!settings->has_quick_action_slots) migrate_quick_actions(settings);
    quick_action_normalize_slots(&settings->quick_action_slots, settings->custom_actions, settings->custom_action_count);
    free(settings->legacy_actions);
    settings->legacy_actions = NULL;
    settings->legacy_action_count = 0;
    settings->has_legacy_actions = false;
    settings->legacy_action = QUICK_ACTION_NONE;

    HotkeyGesture hotkey;
    if (!hotkey_parse(settings->hotkey, &hotkey)) hotkey = HOTKEY_DEFAULT;
    free(settings->hotkey);
    settings->hotkey = hotkey_to_text(hotkey);

    if (!isfinite(settings->window_width) || settings->window_width < 640) settings->window_width = 860;
    if (!isfinite(settings->window_height) || settings->window_height < 520) settings->window_height = 640;
    settings->version = SETTINGS_CURRENT_VERSION;
}

HotkeyGesture settings_hotkey(const AppSettings *settings)
{
    HotkeyGesture gesture;
    return hotkey_parse(settings->hotkey, &gesture) ? gesture : HOTKEY_NONE;
}

// ───────────── 读取 ─────────────
// 与 C# 版的 System.Text.Json 一致：属性名区分大小写，不认识的属性忽略，同名属性取最后一个，
// 类型不对时整个文件作废（由调用方改用默认值）。

static bool read_int(const JsonValue *value, int *out)
{
    if (value->type != JSON_NUMBER || !value->is_integer || value->number < -2147483648.0 || value->number > 2147483647.0) return false;
    *out = (int)value->number;
    return true;
}

static bool read_bool(const JsonValue *value, bool *out)
{
    if (value->type != JSON_BOOL) return false;
    *out = value->boolean;
    return true;
}

static bool read_double(const JsonValue *value, double *out)
{
    if (value->type == JSON_NUMBER)
    {
        *out = value->number;
        return true;
    }
    if (value->type != JSON_STRING) return false;
    if (wcscmp(value->string, L"NaN") == 0) *out = NAN;
    else if (wcscmp(value->string, L"Infinity") == 0) *out = INFINITY;
    else if (wcscmp(value->string, L"-Infinity") == 0) *out = -INFINITY;
    else return false;
    return true;
}

static bool read_string(const JsonValue *value, wchar_t **out)
{
    if (value->type != JSON_STRING && value->type != JSON_NULL) return false;
    free(*out);
    *out = wcs_dup(value->string);
    return true;
}

/// 取值可以写名称（不区分大小写），也可以写数字；数字不在范围内时由 settings_normalize 改回默认值。
static bool read_enum(const JsonValue *value, const EnumTable *table, int *out)
{
    if (value->type == JSON_NUMBER) return read_int(value, out);
    if (value->type != JSON_STRING) return false;
    for (int index = 0; index < table->count; index++)
    {
        if (_wcsicmp(table->names[index].name, value->string) != 0) continue;
        *out = table->names[index].value;
        return true;
    }
    return false;
}

/// 字符串数组；null 当作没有写。数组里的 null 当作空字符串，之后整理时会被去掉。
static bool read_string_list(const JsonValue *value, StrList *out, bool *present)
{
    strlist_clear(out);
    if (present) *present = false;
    if (value->type == JSON_NULL) return true;
    if (value->type != JSON_ARRAY) return false;
    for (int index = 0; index < value->count; index++)
    {
        const JsonValue *item = value->items[index];
        if (item->type != JSON_STRING && item->type != JSON_NULL) return false;
        strlist_add(out, item->string);
    }
    if (present) *present = true;
    return true;
}

static bool read_custom_action(const JsonValue *value, CustomAction *action)
{
    for (int index = 0; index < value->count; index++)
    {
        const wchar_t *key = value->keys[index];
        const JsonValue *item = value->items[index];
        bool ok = true;
        if (wcscmp(key, L"Id") == 0) ok = read_string(item, &action->id);
        else if (wcscmp(key, L"Kind") == 0) ok = read_enum(item, &kind_table, &action->kind);
        else if (wcscmp(key, L"Name") == 0) ok = read_string(item, &action->name);
        else if (wcscmp(key, L"Folder") == 0) ok = read_string(item, &action->folder);
        else if (wcscmp(key, L"FolderMode") == 0) ok = read_enum(item, &transfer_table, &action->folder_mode);
        else if (wcscmp(key, L"ImageSizeKb") == 0) ok = read_int(item, &action->image_size_kb);
        else if (wcscmp(key, L"ImageFormat") == 0) ok = read_enum(item, &format_table, &action->image_format);
        else if (wcscmp(key, L"ProgramPath") == 0) ok = read_string(item, &action->program_path);
        else if (wcscmp(key, L"Arguments") == 0) ok = read_string(item, &action->arguments);
        if (!ok) return false;
    }
    return true;
}

static bool read_custom_actions(const JsonValue *value, AppSettings *settings)
{
    for (int index = 0; index < settings->custom_action_count; index++) custom_action_free(&settings->custom_actions[index]);
    free(settings->custom_actions);
    settings->custom_actions = NULL;
    settings->custom_action_count = 0;
    settings->has_custom_actions = false;
    if (value->type == JSON_NULL) return true;
    if (value->type != JSON_ARRAY) return false;

    settings->custom_actions = xcalloc((size_t)value->count, sizeof(CustomAction));
    settings->has_custom_actions = true;
    for (int index = 0; index < value->count; index++)
    {
        const JsonValue *item = value->items[index];
        if (item->type == JSON_NULL) continue;
        if (item->type != JSON_OBJECT) return false;
        CustomAction *action = &settings->custom_actions[settings->custom_action_count++];
        custom_action_init(action);
        if (!read_custom_action(item, action)) return false;
    }
    return true;
}

static bool read_legacy_actions(const JsonValue *value, AppSettings *settings)
{
    free(settings->legacy_actions);
    settings->legacy_actions = NULL;
    settings->legacy_action_count = 0;
    settings->has_legacy_actions = false;
    if (value->type == JSON_NULL) return true;
    if (value->type != JSON_ARRAY) return false;
    settings->legacy_actions = xcalloc((size_t)value->count, sizeof(int));
    settings->has_legacy_actions = true;
    for (int index = 0; index < value->count; index++)
        if (!read_enum(value->items[index], &action_type_table, &settings->legacy_actions[settings->legacy_action_count++])) return false;
    return true;
}

bool settings_parse(const wchar_t *json, AppSettings *settings)
{
    JsonValue *root = json_parse(json, 0);
    if (!root) return false;
    bool ok = root->type == JSON_OBJECT || root->type == JSON_NULL;
    for (int index = 0; ok && index < root->count; index++)
    {
        const wchar_t *key = root->keys[index];
        const JsonValue *value = root->items[index];
        if (wcscmp(key, L"Version") == 0) ok = read_int(value, &settings->version);
        else if (wcscmp(key, L"WindowLeft") == 0) ok = read_double(value, &settings->window_left);
        else if (wcscmp(key, L"WindowTop") == 0) ok = read_double(value, &settings->window_top);
        else if (wcscmp(key, L"WindowWidth") == 0) ok = read_double(value, &settings->window_width);
        else if (wcscmp(key, L"WindowHeight") == 0) ok = read_double(value, &settings->window_height);
        else if (wcscmp(key, L"AutoOpenTrigger") == 0) ok = read_enum(value, &trigger_table, &settings->auto_open_trigger);
        else if (wcscmp(key, L"ShakeSensitivity") == 0) ok = read_int(value, &settings->shake_sensitivity);
        else if (wcscmp(key, L"MouseGestureSensitivity") == 0)
        {
            settings->has_legacy_sensitivity = value->type != JSON_NULL;
            if (settings->has_legacy_sensitivity) ok = read_enum(value, &sensitivity_table, &settings->legacy_sensitivity);
        }
        else if (wcscmp(key, L"EdgeDwell") == 0) ok = read_enum(value, &dwell_table, &settings->edge_dwell);
        else if (wcscmp(key, L"BlockedApps") == 0) ok = read_string_list(value, &settings->blocked_apps, NULL);
        else if (wcscmp(key, L"RespondInFullscreen") == 0) ok = read_bool(value, &settings->respond_when_fullscreen);
        else if (wcscmp(key, L"Theme") == 0) ok = read_enum(value, &theme_table, &settings->theme);
        else if (wcscmp(key, L"AccentColor") == 0) ok = read_string(value, &settings->accent_color);
        else if (wcscmp(key, L"Look") == 0) ok = read_enum(value, &look_table, &settings->look);
        else if (wcscmp(key, L"Language") == 0) ok = read_enum(value, &language_table, &settings->language);
        else if (wcscmp(key, L"LanguagePack") == 0) ok = read_string(value, &settings->language_pack);
        else if (wcscmp(key, L"StartWithWindows") == 0) ok = read_bool(value, &settings->start_with_windows);
        else if (wcscmp(key, L"QuickShelfSize") == 0) ok = read_enum(value, &size_table, &settings->quick_shelf_size);
        else if (wcscmp(key, L"DragOutMode") == 0) ok = read_enum(value, &transfer_table, &settings->drag_out_mode);
        else if (wcscmp(key, L"MixedDragMode") == 0) ok = read_enum(value, &mixed_drag_table, &settings->mixed_drag);
        else if (wcscmp(key, L"QuickActionLayout") == 0) ok = read_enum(value, &layout_table, &settings->quick_action_layout);
        else if (wcscmp(key, L"QuickActionSlots") == 0) ok = read_string_list(value, &settings->quick_action_slots, &settings->has_quick_action_slots);
        else if (wcscmp(key, L"CustomActions") == 0) ok = read_custom_actions(value, settings);
        else if (wcscmp(key, L"QuickActions") == 0) ok = read_legacy_actions(value, settings);
        else if (wcscmp(key, L"QuickAction") == 0) ok = read_enum(value, &action_type_table, &settings->legacy_action);
        else if (wcscmp(key, L"CloseWhenEmpty") == 0) ok = read_bool(value, &settings->close_when_empty);
        else if (wcscmp(key, L"WatchedFolders") == 0) ok = read_string_list(value, &settings->watched_folders, NULL);
        else if (wcscmp(key, L"WatchPeekSeconds") == 0) ok = read_int(value, &settings->watch_peek_seconds);
        else if (wcscmp(key, L"PerformanceMode") == 0) ok = read_enum(value, &performance_table, &settings->performance_mode);
        else if (wcscmp(key, L"LowPerformanceMode") == 0)
        {
            settings->legacy_low_performance = false;
            if (value->type != JSON_NULL) ok = read_bool(value, &settings->legacy_low_performance);
        }
        else if (wcscmp(key, L"Hotkey") == 0) ok = read_string(value, &settings->hotkey);
        else if (wcscmp(key, L"HasCompletedFirstRun") == 0) ok = read_bool(value, &settings->has_completed_first_run);
        else if (wcscmp(key, L"ShowGettingStarted") == 0) ok = read_bool(value, &settings->show_getting_started);
        else if (wcscmp(key, L"HasAskedFolderWatch") == 0) ok = read_bool(value, &settings->has_asked_folder_watch);
        else if (wcscmp(key, L"HasAskedLook") == 0) ok = read_bool(value, &settings->has_asked_look);
    }
    json_free(root);
    return ok;
}

// ───────────── 写出 ─────────────

static void write_enum(JsonWriter *writer, const wchar_t *key, const EnumTable *table, int value)
{
    const wchar_t *name = enum_name(table, value);
    if (name) json_write_string(writer, key, name);
    else json_write_int(writer, key, value);
}

static void write_string_list(JsonWriter *writer, const wchar_t *key, const StrList *list)
{
    json_write_begin_array(writer, key);
    for (int index = 0; index < list->count; index++) json_write_string(writer, NULL, list->items[index]);
    json_write_end_array(writer);
}

wchar_t *settings_serialize(const AppSettings *settings)
{
    // 属性的顺序与 C# 版写出的相同。只用于迁移的旧属性不再写出。
    JsonWriter writer = {0};
    json_write_begin_object(&writer, NULL);
    json_write_int(&writer, L"Version", settings->version);
    json_write_double(&writer, L"WindowLeft", settings->window_left);
    json_write_double(&writer, L"WindowTop", settings->window_top);
    json_write_double(&writer, L"WindowWidth", settings->window_width);
    json_write_double(&writer, L"WindowHeight", settings->window_height);
    write_enum(&writer, L"AutoOpenTrigger", &trigger_table, settings->auto_open_trigger);
    json_write_int(&writer, L"ShakeSensitivity", settings->shake_sensitivity);
    write_enum(&writer, L"EdgeDwell", &dwell_table, settings->edge_dwell);
    write_string_list(&writer, L"BlockedApps", &settings->blocked_apps);
    json_write_bool(&writer, L"RespondInFullscreen", settings->respond_when_fullscreen);
    write_enum(&writer, L"Theme", &theme_table, settings->theme);
    json_write_string(&writer, L"AccentColor", settings->accent_color);
    write_enum(&writer, L"Look", &look_table, settings->look);
    write_enum(&writer, L"Language", &language_table, settings->language);
    json_write_string(&writer, L"LanguagePack", settings->language_pack);
    json_write_bool(&writer, L"StartWithWindows", settings->start_with_windows);
    write_enum(&writer, L"QuickShelfSize", &size_table, settings->quick_shelf_size);
    write_enum(&writer, L"DragOutMode", &transfer_table, settings->drag_out_mode);
    write_enum(&writer, L"MixedDragMode", &mixed_drag_table, settings->mixed_drag);
    write_enum(&writer, L"QuickActionLayout", &layout_table, settings->quick_action_layout);
    write_string_list(&writer, L"QuickActionSlots", &settings->quick_action_slots);

    json_write_begin_array(&writer, L"CustomActions");
    for (int index = 0; index < settings->custom_action_count; index++)
    {
        const CustomAction *action = &settings->custom_actions[index];
        json_write_begin_object(&writer, NULL);
        json_write_string(&writer, L"Id", action->id);
        write_enum(&writer, L"Kind", &kind_table, action->kind);
        json_write_string(&writer, L"Name", action->name);
        json_write_string(&writer, L"Folder", action->folder);
        write_enum(&writer, L"FolderMode", &transfer_table, action->folder_mode);
        json_write_int(&writer, L"ImageSizeKb", action->image_size_kb);
        write_enum(&writer, L"ImageFormat", &format_table, action->image_format);
        json_write_string(&writer, L"ProgramPath", action->program_path);
        json_write_string(&writer, L"Arguments", action->arguments);
        json_write_end_object(&writer);
    }
    json_write_end_array(&writer);

    json_write_bool(&writer, L"CloseWhenEmpty", settings->close_when_empty);
    write_string_list(&writer, L"WatchedFolders", &settings->watched_folders);
    json_write_int(&writer, L"WatchPeekSeconds", settings->watch_peek_seconds);
    write_enum(&writer, L"PerformanceMode", &performance_table, settings->performance_mode);
    json_write_string(&writer, L"Hotkey", settings->hotkey);
    json_write_bool(&writer, L"HasCompletedFirstRun", settings->has_completed_first_run);
    json_write_bool(&writer, L"ShowGettingStarted", settings->show_getting_started);
    json_write_bool(&writer, L"HasAskedFolderWatch", settings->has_asked_folder_watch);
    json_write_bool(&writer, L"HasAskedLook", settings->has_asked_look);
    json_write_end_object(&writer);
    return json_write_finish(&writer);
}

// ───────────── 文件 ─────────────

/// 整个程序共用一条保存队列：重新读取设置前可以先等还没写完的保存。
static SaveQueue settings_queue = SAVE_QUEUE_INIT(L"保存设置失败");

wchar_t *settings_path(void)
{
    wchar_t *custom = env_get(L"DROPSHELF_SETTINGS_PATH");
    if (custom) return custom;
#ifdef _WIN32
    wchar_t *base = local_app_data_directory();
    wchar_t *path = path_join(base, L"DropShelf\\settings.json");
#else
    wchar_t *base = config_directory();
    wchar_t *path = path_join(base, L"settings.json");
#endif
    free(base);
    return path;
}

wchar_t *settings_data_directory(void)
{
#ifndef _WIN32
    return data_directory();
#else
    wchar_t *path = settings_path();
    wchar_t *directory = path_directory(path);
    free(path);
    return directory ? directory : wcs_dup(L"");
#endif
}

void settings_load(AppSettings *settings)
{
    settings_flush();
    settings_init(settings);
    wchar_t *path = settings_path();
    if (path_exists(path))
    {
        wchar_t *text = file_read_text(path, 16 * 1024 * 1024);
        if (!text || !settings_parse(text, settings))
        {
            log_error(L"读取设置失败，已使用默认值\r\n%ls", text ? L"设置文件的格式不正确" : L"无法读取设置文件");
            // 下次保存时会用默认值覆盖它；先留一份，手改坏了的自定义操作还能找回来。
            file_keep_unreadable(path);
            settings_free(settings);
            settings_init(settings);
        }
        free(text);
    }
    free(path);
    settings_normalize(settings);
}

void settings_save_in_background(const AppSettings *settings)
{
    AppSettings snapshot;
    settings_clone(&snapshot, settings);
    settings_normalize(&snapshot);
    wchar_t *json = settings_serialize(&snapshot);
    settings_free(&snapshot);

    size_t length = 0;
    char *bytes = utf8_from_wide(json, &length);
    free(json);
    wchar_t *path = settings_path();
    save_queue_post(&settings_queue, path, bytes, length);
    free(path);
}

void settings_save(const AppSettings *settings)
{
    settings_save_in_background(settings);
    settings_flush();
}

void settings_flush(void)
{
    save_queue_flush(&settings_queue);
}
