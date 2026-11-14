// C 版的测试：对应 C# 版测试里设置、快捷键、语言和系统几部分的逻辑。
// 设置和日志都写到临时文件夹（通过环境变量），不碰真实的设置。
#include "base.h"
#include "hotkey.h"
#include "json.h"
#include "lang.h"
#include "log.h"
#include "settings.h"
#include "single_instance.h"
#include "startup.h"
#include "strings.h"
#include "theme.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const char *current_test;

#define CHECK(condition, message) \
    do { if (!(condition)) { failures++; printf("  FAILED %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, message); } } while (0)

static wchar_t *temporary_directory;

static void write_text(const wchar_t *path, const wchar_t *text)
{
    size_t length = 0;
    char *bytes = utf8_from_wide(text, &length);
    file_write_atomic(path, bytes, length);
    free(bytes);
}

static bool list_is(const StrList *list, int count, ...)
{
    if (list->count != count) return false;
    va_list expected;
    va_start(expected, count);
    bool same = true;
    for (int index = 0; index < count; index++)
        if (wcscmp(list->items[index], va_arg(expected, const wchar_t *)) != 0) same = false;
    va_end(expected);
    return same;
}

// ───────────── JSON ─────────────

static void test_json(void)
{
    JsonValue *value = json_parse(L"{ \"a\": [1, 2.5, -3e2, true, null], \"b\": \"x\\u4e2d\\n\\\"\", \"a\": \"last\" }", 0);
    CHECK(value && value->type == JSON_OBJECT && value->count == 3, "对象没有读对");
    CHECK(value && json_get(value, L"a")->type == JSON_STRING, "同名属性应取最后一个");
    CHECK(value && wcscmp(json_get(value, L"b")->string, L"x中\n\"") == 0, "字符串里的转义没有读对");
    const JsonValue *array = value ? value->items[0] : NULL;
    CHECK(array && array->count == 5 && array->items[0]->is_integer && !array->items[1]->is_integer && array->items[2]->number == -300, "数字没有读对");
    json_free(value);

    CHECK(!json_parse(L"{ \"a\": 1, }", 0) && !json_parse(L"{ // x\n \"a\": 1 }", 0), "设置文件不允许多余的逗号和注释");
    value = json_parse(L"{ /* x */ \"a\": [1, 2, ], // y\n }", JSON_ALLOW_COMMENTS | JSON_ALLOW_TRAILING_COMMAS);
    CHECK(value && json_get(value, L"a")->count == 2, "语言包允许注释和多余的逗号");
    json_free(value);
    CHECK(!json_parse(L"", 0) && !json_parse(L"{", 0) && !json_parse(L"[1] 2", 0) && !json_parse(L"\"abc", 0) && !json_parse(L"01", 0),
        "格式不对的内容应读取失败");

    JsonWriter writer = {0};
    json_write_begin_object(&writer, NULL);
    json_write_int(&writer, L"Version", 3);
    json_write_double(&writer, L"Left", NAN);
    json_write_double(&writer, L"Width", 860);
    json_write_double(&writer, L"Top", 160.5);
    json_write_begin_array(&writer, L"Empty");
    json_write_end_array(&writer);
    json_write_begin_array(&writer, L"List");
    json_write_string(&writer, NULL, L"a\"b");
    json_write_end_array(&writer);
    json_write_string(&writer, L"None", NULL);
    json_write_end_object(&writer);
    wchar_t *text = json_write_finish(&writer);
    CHECK(wcscmp(text, L"{\r\n  \"Version\": 3,\r\n  \"Left\": \"NaN\",\r\n  \"Width\": 860,\r\n  \"Top\": 160.5,\r\n  \"Empty\": [],\r\n"
        L"  \"List\": [\r\n    \"a\\\"b\"\r\n  ],\r\n  \"None\": null\r\n}") == 0, "写出的 JSON 格式不正确");
    free(text);
}

// ───────────── 设置 ─────────────

static void test_settings_round_trip(void)
{
    AppSettings defaults;
    settings_load(&defaults);
    CHECK(defaults.close_when_empty && defaults.show_getting_started && defaults.performance_mode == PERFORMANCE_AUTO, "默认开关不正确");
    CHECK(defaults.accent_color == NULL, "默认强调色应跟随 Windows");
    CHECK(defaults.quick_shelf_size == SHELF_SIZE_MEDIUM, "默认尺寸不是中档");
    CHECK(defaults.quick_action_layout == LAYOUT_FOUR &&
          list_is(&defaults.quick_action_slots, 4, L"CompressZip", L"CopyFullPath", L"MoveTo", L"Properties"), "默认快速操作不正确");
    CHECK(defaults.custom_action_count == 1 && defaults.custom_actions[0].kind == CUSTOM_ACTION_COMPRESS_IMAGE &&
          defaults.custom_actions[0].image_size_kb == 500 && wcslen(defaults.custom_actions[0].id) == 32, "没有提供压缩图片的示例操作");
    CHECK(defaults.auto_open_trigger == TRIGGER_BOTH && defaults.edge_dwell == DWELL_MEDIUM, "默认呼出方式不正确");
    CHECK(defaults.drag_out_mode == TRANSFER_MOVE, "默认拖出方式应为移动");
    CHECK(wcscmp(defaults.hotkey, L"Ctrl+Shift+Space") == 0, "默认快捷键不正确");
    CHECK(defaults.language == LANGUAGE_SYSTEM && isnan(defaults.window_left) && defaults.window_width == 860, "默认语言或窗口位置不正确");

    defaults.shake_sensitivity = 20;
    defaults.auto_open_trigger = TRIGGER_EDGE;
    defaults.edge_dwell = DWELL_LONG;
    defaults.drag_out_mode = TRANSFER_COPY;
    defaults.theme = THEME_DARK;
    defaults.language = LANGUAGE_ENGLISH;
    defaults.quick_shelf_size = SHELF_SIZE_LARGE;
    defaults.quick_action_layout = LAYOUT_TWO;
    defaults.custom_actions = xrealloc(defaults.custom_actions, 2 * sizeof(CustomAction));
    CustomAction *custom = &defaults.custom_actions[1];
    custom_action_init(custom);
    custom->kind = CUSTOM_ACTION_FOLDER;
    custom->folder = wcs_dup(temporary_directory);
    custom->folder_mode = TRANSFER_COPY;
    custom->name = wcs_dup(L"素材");
    defaults.custom_action_count = 2;
    wchar_t *custom_id = wcs_dup(custom->id);
    strlist_clear(&defaults.quick_action_slots);
    strlist_add(&defaults.quick_action_slots, L"Reveal");
    strlist_add(&defaults.quick_action_slots, custom_id);
    strlist_add(&defaults.quick_action_slots, L"CopyFullPath");
    strlist_add(&defaults.quick_action_slots, L"MoveTo");
    defaults.close_when_empty = false;
    defaults.show_getting_started = false;
    defaults.performance_mode = PERFORMANCE_NORMAL;
    defaults.accent_color = wcs_dup(L"#8764B8");
    free(defaults.hotkey);
    defaults.hotkey = wcs_dup(L"Ctrl+Alt+D");
    defaults.has_completed_first_run = true;
    defaults.window_left = 624;
    defaults.window_top = 160.5;
    strlist_add(&defaults.blocked_apps, L"Photoshop.exe");
    strlist_add(&defaults.blocked_apps, L"game.exe");
    settings_save(&defaults);

    AppSettings loaded;
    settings_load(&loaded);
    CHECK(loaded.shake_sensitivity == 20, "灵敏度未恢复");
    CHECK(loaded.auto_open_trigger == TRIGGER_EDGE && loaded.edge_dwell == DWELL_LONG, "呼出方式未恢复");
    CHECK(loaded.drag_out_mode == TRANSFER_COPY && loaded.theme == THEME_DARK && loaded.language == LANGUAGE_ENGLISH, "拖出方式、主题或语言未恢复");
    CHECK(loaded.quick_shelf_size == SHELF_SIZE_LARGE && loaded.quick_action_layout == LAYOUT_TWO, "尺寸或按钮数量未恢复");
    CHECK(list_is(&loaded.quick_action_slots, 4, L"Reveal", custom_id, L"CopyFullPath", L"MoveTo"), "快速操作位置没有按原顺序恢复");
    CHECK(list_is(&loaded.blocked_apps, 2, L"Photoshop.exe", L"game.exe"), "屏蔽的应用未恢复");
    CHECK(loaded.custom_action_count == 2 && wcscmp(loaded.custom_actions[1].id, custom_id) == 0 &&
          wcscmp(loaded.custom_actions[1].folder, temporary_directory) == 0 && loaded.custom_actions[1].folder_mode == TRANSFER_COPY &&
          wcscmp(loaded.custom_actions[1].name, L"素材") == 0, "自定义操作未恢复");
    CHECK(!loaded.close_when_empty && !loaded.show_getting_started && loaded.performance_mode == PERFORMANCE_NORMAL, "开关未恢复");
    CHECK(wcs_equal(loaded.accent_color, L"#8764B8") && wcscmp(loaded.hotkey, L"Ctrl+Alt+D") == 0 && loaded.has_completed_first_run, "强调色、快捷键或首次运行状态未恢复");
    CHECK(loaded.window_left == 624 && loaded.window_top == 160.5, "窗口位置未恢复");

    wchar_t *path = settings_path();
    wchar_t *text = file_read_text(path, 1 << 20);
    CHECK(text && !wcsstr(text, L"\"QuickActions\"") && !wcsstr(text, L"MouseGestureSensitivity") && !wcsstr(text, L"LowPerformanceMode") &&
          !wcsstr(text, L"\"QuickAction\""), "已迁移的旧版设置仍写入了设置文件");
    CHECK(text && wcsstr(text, L"\"WindowLeft\": 624,\r\n") && wcsstr(text, L"\"WindowTop\": 160.5,\r\n") && wcsstr(text, L"\"WindowWidth\": 860,\r\n") && wcsstr(text, L"\"AutoOpenTrigger\": \"Edge\"") && wcsstr(text, L"\"LanguagePack\": null"),
        "设置文件的写法与 C# 版不一致");
    free(text);

    AppSettings clone;
    settings_clone(&clone, &loaded);
    strlist_clear(&clone.quick_action_slots);
    free(clone.custom_actions[0].name);
    clone.custom_actions[0].name = wcs_dup(L"changed");
    CHECK(loaded.quick_action_slots.count == 4 && loaded.custom_actions[0].name == NULL, "复制设置时共享了快速操作");

    settings_free(&clone);
    settings_free(&loaded);
    settings_free(&defaults);
    free(custom_id);
    DeleteFileW(path);
    free(path);
}

static void load_from_text(AppSettings *settings, const wchar_t *text)
{
    wchar_t *path = settings_path();
    write_text(path, text);
    settings_load(settings);
    DeleteFileW(path);
    free(path);
}

static void test_settings_migration(void)
{
    AppSettings migrated;
    load_from_text(&migrated,
        L"{\n  \"AlwaysOnTop\": false,\n  \"WindowLeft\": 624,\n  \"WindowTop\": 160,\n  \"MouseGestureSensitivity\": \"Responsive\",\n"
        L"  \"Theme\": \"System\",\n  \"StartWithWindows\": false,\n  \"QuickShelfSize\": \"Small\",\n  \"QuickAction\": \"CompressZip\",\n"
        L"  \"LowPerformanceMode\": true,\n  \"HasCompletedFirstRun\": true\n}");
    CHECK(migrated.version == SETTINGS_CURRENT_VERSION, "版本号没有更新");
    CHECK(migrated.shake_sensitivity == SHAKE_RESPONSIVE && !migrated.has_legacy_sensitivity && migrated.quick_shelf_size == SHELF_SIZE_SMALL, "旧设置没有保留");
    CHECK(migrated.quick_action_layout == LAYOUT_FOUR &&
          list_is(&migrated.quick_action_slots, 4, L"CompressZip", L"CopyFullPath", L"MoveTo", L"Properties"), "旧的单个快速操作没有迁移为默认组合");
    CHECK(migrated.window_left == 624 && migrated.has_completed_first_run, "窗口位置或首次运行状态丢失");
    CHECK(migrated.performance_mode == PERFORMANCE_LOW && !migrated.legacy_low_performance, "旧的低性能模式开关打开时应迁移为始终启用");
    CHECK(migrated.auto_open_trigger == TRIGGER_BOTH && migrated.drag_out_mode == TRANSFER_MOVE, "新增设置没有使用默认值");
    settings_free(&migrated);

    load_from_text(&migrated, L"{ \"QuickAction\": \"None\" }");
    CHECK(migrated.quick_action_layout == LAYOUT_OFF, "关闭快速操作的旧设置没有保持关闭");
    settings_free(&migrated);

    load_from_text(&migrated, L"{ \"QuickAction\": \"ConvertPng\" }");
    CHECK(list_is(&migrated.quick_action_slots, 4, L"CompressZip", L"CopyFullPath", L"MoveTo", L"Properties"), "已移除的图片转换设置没有恢复默认");
    settings_free(&migrated);

    // 2.0：选了两个操作时改为显示两个大按钮，原来的操作排在前面。
    load_from_text(&migrated, L"{ \"Version\": 2, \"QuickActions\": [\"Reveal\", \"CopyFullPath\"] }");
    CHECK(migrated.quick_action_layout == LAYOUT_TWO && migrated.quick_action_slots.count == 4 &&
          wcscmp(migrated.quick_action_slots.items[0], L"CopyFullPath") == 0 && wcscmp(migrated.quick_action_slots.items[1], L"Reveal") == 0,
        "2.0 版的快速操作没有正确迁移");
    settings_free(&migrated);

    load_from_text(&migrated, L"{ \"Version\": 2, \"QuickActions\": [] }");
    CHECK(migrated.quick_action_layout == LAYOUT_OFF, "2.0 版关闭的快速操作没有保持关闭");
    settings_free(&migrated);

    // 格式不对或类型不对的文件整个作废，使用默认值。
    load_from_text(&migrated, L"{ \"Theme\": \"Dark\", \"ShakeSensitivity\": \"高\" }");
    CHECK(migrated.theme == THEME_SYSTEM && migrated.shake_sensitivity == SHAKE_BALANCED, "类型不对的设置文件没有改用默认值");
    settings_free(&migrated);
    load_from_text(&migrated, L"{ \"Theme\": \"Dark\"");
    CHECK(migrated.theme == THEME_SYSTEM, "格式不对的设置文件没有改用默认值");
    settings_free(&migrated);

    // C# 版写出的文件：中文写成 \uXXXX，取值不区分大小写，自定义操作里多两个只读属性。
    load_from_text(&migrated,
        L"{ \"Version\": 3, \"WindowLeft\": \"NaN\", \"Theme\": \"dark\", \"EdgeDwell\": 2, \"QuickActionLayout\": \"Two\",\r\n"
        L"  \"QuickActionSlots\": [\"Reveal\", \"abc\", \"MoveTo\", \"Properties\"],\r\n"
        L"  \"CustomActions\": [ { \"Id\": \"abc\", \"Kind\": \"Folder\", \"Name\": \"\\u7D20\\u6750\", \"Folder\": \"C:\\\\X\", \"FolderMode\": \"Copy\",\r\n"
        L"    \"ImageSizeKb\": 500, \"ImageFormat\": \"Jpeg\", \"ProgramPath\": null, \"Arguments\": null, \"IsComplete\": true, \"Key\": \"abc\\u001FFolder\" } ],\r\n"
        L"  \"Hotkey\": \"Ctrl\\u002BAlt\\u002BD\" }");
    CHECK(isnan(migrated.window_left) && migrated.theme == THEME_DARK && migrated.edge_dwell == DWELL_LONG && migrated.quick_action_layout == LAYOUT_TWO,
        "C# 版写出的设置没有读对");
    CHECK(migrated.custom_action_count == 1 && wcscmp(migrated.custom_actions[0].name, L"素材") == 0 &&
          wcscmp(migrated.custom_actions[0].folder, L"C:\\X") == 0 && migrated.custom_actions[0].folder_mode == TRANSFER_COPY &&
          list_is(&migrated.quick_action_slots, 4, L"Reveal", L"abc", L"MoveTo", L"Properties") && wcscmp(migrated.hotkey, L"Ctrl+Alt+D") == 0,
        "C# 版写出的自定义操作或快捷键没有读对");
    settings_free(&migrated);
}

static void add_custom(AppSettings *settings, int kind, int image_size, const wchar_t *name)
{
    settings->custom_actions = xrealloc(settings->custom_actions, (size_t)(settings->custom_action_count + 1) * sizeof(CustomAction));
    CustomAction *action = &settings->custom_actions[settings->custom_action_count++];
    custom_action_init(action);
    action->kind = kind;
    action->image_size_kb = image_size;
    action->name = wcs_dup(name);
    settings->has_custom_actions = true;
}

static void test_settings_normalization(void)
{
    AppSettings invalid;
    settings_init(&invalid);
    invalid.has_legacy_sensitivity = true;
    invalid.legacy_sensitivity = 999;
    invalid.auto_open_trigger = 999;
    invalid.edge_dwell = 999;
    invalid.drag_out_mode = 999;
    invalid.theme = 999;
    invalid.accent_color = wcs_dup(L"blue");
    invalid.language = 999;
    invalid.quick_shelf_size = 999;
    invalid.quick_action_layout = 3;
    invalid.has_quick_action_slots = true;
    strlist_add(&invalid.quick_action_slots, L"Properties");
    strlist_add(&invalid.quick_action_slots, L"Properties");
    strlist_add(&invalid.quick_action_slots, L"ConvertJpeg");
    strlist_add(&invalid.quick_action_slots, L"nope");
    add_custom(&invalid, CUSTOM_ACTION_FOLDER, 500, NULL);
    add_custom(&invalid, CUSTOM_ACTION_COMPRESS_IMAGE, 1, L"   ");
    add_custom(&invalid, 999, 500, NULL);
    free(invalid.hotkey);
    invalid.hotkey = wcs_dup(L"Shift+A");
    strlist_add(&invalid.blocked_apps, L"  game.exe ");
    strlist_add(&invalid.blocked_apps, L"GAME.EXE");
    strlist_add(&invalid.blocked_apps, L"");
    strlist_add(&invalid.blocked_apps, L"C:\\Tools\\paint.exe");
    invalid.window_width = 10;
    invalid.window_height = NAN;

    AppSettings normalized;
    settings_clone(&normalized, &invalid);
    settings_normalize(&normalized);
    CHECK(normalized.shake_sensitivity == SHAKE_BALANCED, "异常灵敏度没有恢复默认值");
    CHECK(normalized.auto_open_trigger == TRIGGER_BOTH && normalized.edge_dwell == DWELL_MEDIUM, "异常呼出方式没有恢复默认值");
    CHECK(normalized.drag_out_mode == TRANSFER_MOVE, "异常拖出方式没有恢复默认值");
    CHECK(normalized.theme == THEME_SYSTEM && normalized.accent_color == NULL, "异常主题或强调色没有恢复默认值");
    CHECK(normalized.language == LANGUAGE_SYSTEM && normalized.quick_shelf_size == SHELF_SIZE_MEDIUM, "异常语言或尺寸没有恢复默认值");
    CHECK(normalized.quick_action_layout == LAYOUT_FOUR, "异常按钮数量没有恢复默认值");
    CHECK(list_is(&normalized.blocked_apps, 2, L"game.exe", L"paint.exe"), "屏蔽的应用没有去掉空白、路径和重复");
    CHECK(list_is(&normalized.quick_action_slots, 4, L"Properties", L"CompressZip", L"CopyFullPath", L"MoveTo"), "快速操作位置没有规范化");
    CHECK(normalized.custom_action_count == 1 && normalized.custom_actions[0].kind == CUSTOM_ACTION_COMPRESS_IMAGE &&
          normalized.custom_actions[0].image_size_kb == CUSTOM_ACTION_MINIMUM_IMAGE_KB && normalized.custom_actions[0].name == NULL,
        "自定义操作没有规范化");
    CHECK(wcscmp(normalized.hotkey, L"Ctrl+Shift+Space") == 0, "无效快捷键没有恢复默认值");
    CHECK(normalized.window_width >= 640 && normalized.window_height >= 520, "异常窗口尺寸没有恢复默认值");
    CHECK(invalid.theme == 999, "规范化时修改了原设置对象");
    settings_free(&normalized);
    settings_free(&invalid);

    AppSettings range;
    settings_init(&range);
    range.shake_sensitivity = 250;
    range.language_pack = wcs_dup(L" ja ");
    settings_normalize(&range);
    CHECK(range.shake_sensitivity == SHAKE_RESPONSIVE && wcs_equal(range.language_pack, L"ja"), "超出范围的灵敏度或语言包名称没有整理");
    range.shake_sensitivity = -5;
    free(range.language_pack);
    range.language_pack = wcs_dup(L"  ");
    settings_normalize(&range);
    CHECK(range.shake_sensitivity == SHAKE_STEADY && range.language_pack == NULL, "超出范围的灵敏度或空白的语言包名称没有整理");
    settings_free(&range);

    COLORREF color = 0;
    CHECK(accent_color_parse(L"#8764B8", &color) && color == RGB(0x87, 0x64, 0xB8) && !accent_color_parse(L"#12345", NULL) &&
          !accent_color_parse(L"#12345G", NULL) && !accent_color_parse(NULL, NULL), "强调色解析不正确");
}

static void test_quick_action_slots(void)
{
    CustomAction custom;
    custom_action_init(&custom);
    custom.kind = CUSTOM_ACTION_COMPRESS_IMAGE;
    StrList slots = {0};
    strlist_add(&slots, L"Reveal");
    strlist_add(&slots, L"Reveal");
    strlist_add(&slots, L"nope");
    strlist_add(&slots, custom.id);
    quick_action_normalize_slots(&slots, &custom, 1);
    CHECK(list_is(&slots, 4, L"Reveal", custom.id, L"CompressZip", L"CopyFullPath"), "快速操作位置没有去重或补齐");
    CHECK(quick_action_is_builtin_id(L"MoveTo") && !quick_action_is_builtin_id(L"moveto") && !quick_action_is_builtin_id(L"ConvertPng"), "内置操作的标识不正确");
    strlist_clear(&slots);
    custom_action_free(&custom);
}

// ───────────── 快捷键 ─────────────

static bool text_is(HotkeyGesture gesture, const wchar_t *expected)
{
    wchar_t *text = hotkey_to_text(gesture);
    bool same = wcscmp(text, expected) == 0;
    free(text);
    return same;
}

static void test_hotkey_gesture(void)
{
    HotkeyGesture gesture;
    CHECK(hotkey_parse(L"Ctrl+Shift+Space", &gesture) && gesture.modifiers == (MOD_CONTROL | MOD_SHIFT) && gesture.key == VK_SPACE, "默认快捷键无法解析");
    CHECK(text_is(gesture, L"Ctrl+Shift+Space"), "快捷键显示不正确");
    CHECK(hotkey_parse(L"alt+win+1", &gesture) && gesture.key == '1' && text_is(gesture, L"Alt+Win+1"), "数字快捷键不正确");
    CHECK(hotkey_parse(L"F9", &gesture) && hotkey_is_valid(gesture) && text_is(gesture, L"F9"), "功能键应允许不带修饰键");
    CHECK(!hotkey_parse(L"Shift+A", &gesture), "只有 Shift 的快捷键不应有效");
    CHECK(!hotkey_parse(L"Ctrl+Nope", &gesture) && !hotkey_parse(L"Ctrl+A+B", &gesture) && !hotkey_parse(L"Ctrl+LeftShift", &gesture), "未知或无效的按键不应有效");
    CHECK(hotkey_parse(L"", &gesture) && hotkey_is_empty(gesture) && text_is(gesture, L"") && hotkey_parse(NULL, &gesture), "空快捷键应表示关闭");
    CHECK(hotkey_parse(L"Ctrl + Alt + d", &gesture) && text_is(gesture, L"Ctrl+Alt+D"), "字母键或空格没有处理");
    CHECK(hotkey_parse(L"Ctrl+Num5", &gesture) && gesture.key == VK_NUMPAD5 && text_is(gesture, L"Ctrl+Num5") &&
          hotkey_parse(L"Ctrl+NumPad5", &gesture) && gesture.key == VK_NUMPAD5, "小键盘数字不正确");
    CHECK(hotkey_parse(L"Win+`", &gesture) && gesture.key == VK_OEM_3 && text_is(gesture, L"Win+`") &&
          hotkey_parse(L"Ctrl+OemTilde", &gesture) && gesture.key == VK_OEM_3, "符号键不正确");
    CHECK(hotkey_parse(L"Ctrl+Enter", &gesture) && gesture.key == VK_RETURN && text_is(gesture, L"Ctrl+Enter") &&
          hotkey_parse(L"Ctrl+Return", &gesture) && gesture.key == VK_RETURN && hotkey_parse(L"Alt+Esc", &gesture) && text_is(gesture, L"Alt+Esc"),
        "回车或 Esc 不正确");
    CHECK(hotkey_parse(L"Ctrl+Alt+Delete", &gesture) && text_is(gesture, L"Ctrl+Alt+Delete") && hotkey_parse(L"Ctrl+F24", &gesture) &&
          gesture.key == VK_F24 && !hotkey_parse(L"Ctrl+F25", &gesture), "其他按键名称不正确");
}

static int hotkey_presses;
static void on_test_hotkey(void) { hotkey_presses++; }

static void test_hotkey_registration(void)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"DropShelf.Tests.Hotkey", WS_POPUP, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    hotkey_service_start(window, on_test_hotkey);
    HotkeyGesture gesture = { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23 };
    CHECK(hotkey_service_register(gesture), "快捷键注册失败");
    CHECK(!RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23), "快捷键没有真的注册上");
    CHECK(hotkey_service_handle_message(WM_HOTKEY, 0x4453) && hotkey_presses == 1 && !hotkey_service_handle_message(WM_HOTKEY, 1), "快捷键消息没有处理");
    hotkey_service_set_paused(true);
    CHECK(RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23), "暂停时没有注销快捷键");
    UnregisterHotKey(window, 1);
    hotkey_service_set_paused(false);
    CHECK(!RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23), "恢复时没有重新注册快捷键");
    CHECK(hotkey_service_register(HOTKEY_NONE), "关闭快捷键应视为成功");
    CHECK(RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F23), "关闭快捷键后没有注销");
    UnregisterHotKey(window, 1);
    hotkey_service_stop();
    DestroyWindow(window);
}

// ───────────── 语言 ─────────────

static int language_changes;
static void on_language_changed(void *context) { (void)context; language_changes++; }

static void test_language_packs(void)
{
    LocArg arguments[] = { ARG_INT(3), ARG_TEXT(L"X") };
    WStr text = {0};
    CHECK(loc_format(&text, L"已添加 {0} 项，{1}，{{原样}}", arguments, 2) && wcscmp(text.data, L"已添加 3 项，X，{原样}") == 0, "把参数填进模板的结果不正确");
    CHECK(!loc_format(&text, L"{7}", arguments, 2) && !loc_format(&text, L"{0", arguments, 2) && !loc_format(&text, L"a } b", arguments, 2) &&
          wcscmp(text.data, L"已添加 3 项，X，{原样}") == 0, "参数位置写错的模板应失败，并且不改动已有内容");
    wstr_free(&text);

    language_set_mode(LANGUAGE_CHINESE);
    LanguagePack *pack = language_pack_parse(L"ja",
        L"{\n  // 允许写注释\n  \"name\": \"日本語\", \"from\": \"zh\",\n"
        L"  \"strings\": { \"置物架\": \"シェルフ\", \"已添加 {0} 项\": \"{0} 件追加しました\", \"{0} 个项目\": \"{3} 件\", \"新建置物架\": \"\" },\n"
        L"  \"reference\": { \"置物架\": \"Shelf\" },\n}");
    CHECK(pack && wcscmp(language_pack_id(pack), L"ja") == 0 && wcscmp(language_pack_name(pack), L"日本語") == 0 &&
          language_pack_from_chinese(pack) && language_pack_count(pack) == 3, "语言包没有读对");
    CHECK(!language_pack_parse(L"x", L"[]") && !language_pack_parse(L"x", L"{ \"strings\": { \"a\": \"\" } }"), "不是语言包或没有译文的文件被当成了语言包");

    language_changes = 0;
    language_add_listener(on_language_changed, NULL);
    language_set_pack(pack);
    language_set_pack(pack);
    wchar_t *added = str_added(3);
    CHECK(language_changes == 1 && wcscmp(S(STR_Shelf), L"シェルフ") == 0 && wcscmp(added, L"3 件追加しました") == 0, "选用语言包后没有显示译文");
    free(added);
    // 没有译文、译文留空或参数位置写错的条目显示原文。
    wchar_t *count = str_item_count(5);
    CHECK(wcscmp(S(STR_OpenShelf), L"打开置物架") == 0 && wcscmp(S(STR_NewShelf), L"新建置物架") == 0 && wcscmp(count, L"5 个项目") == 0, "没有译文的条目没有显示原文");
    free(count);

    // 对照英文翻译的语言包：没有译文的条目显示英文，不管界面语言设成了什么。
    LanguagePack *german = language_pack_parse(L"de", L"{ \"from\": \"en\", \"strings\": { \"Shelf\": \"Regal\", \"Added {0}\": \"{0} hinzugefügt\" } }");
    language_set_pack(german);
    added = str_added(1);
    CHECK(german && wcscmp(language_pack_name(german), L"de") == 0 && !language_pack_from_chinese(german) && wcscmp(S(STR_Shelf), L"Regal") == 0 &&
          wcscmp(S(STR_OpenShelf), L"Open shelf") == 0 && wcscmp(added, L"1 item hinzugefügt") == 0, "对照英文的语言包不正确");
    free(added);
    language_set_pack(NULL);
    CHECK(language_changes == 3 && wcscmp(S(STR_Shelf), L"置物架") == 0, "去掉语言包后没有恢复");
    language_pack_free(pack);
    language_pack_free(german);

    // 从文件夹里查找语言包：按名称排序，按文件名（不区分大小写）查找。
    wchar_t *directory = path_join(temporary_directory, LANGUAGE_PACK_FOLDER);
    directory_create(directory);
    wchar_t *first = path_join(directory, L"zz.json");
    wchar_t *second = path_join(directory, L"ja.json");
    wchar_t *ignored = path_join(directory, L"notes.txt");
    write_text(first, L"{ \"name\": \"Alpha\", \"strings\": { \"置物架\": \"A\" } }");
    write_text(second, L"{ \"name\": \"日本語\", \"strings\": { \"置物架\": \"シェルフ\" } }");
    write_text(ignored, L"{ \"strings\": { \"置物架\": \"B\" } }");
    language_packs_reload(directory);
    CHECK(language_packs_count() == 2 && wcscmp(language_pack_id(language_packs_at(0)), L"zz") == 0 && language_packs_find(L"JA") &&
          !language_packs_find(L"notes") && !language_packs_find(L" ") && !language_packs_find(NULL), "没有从文件夹里找到语言包");
    language_set_pack(language_packs_find(L"ja"));
    CHECK(wcscmp(S(STR_Shelf), L"シェルフ") == 0, "从文件读到的语言包没有用上");
    language_packs_reload(NULL);
    CHECK(language_packs_count() == 0 && language_current_pack() == NULL && wcscmp(S(STR_Shelf), L"置物架") == 0, "重新查找后旧的语言包应失效");
    language_remove_listener(on_language_changed, NULL);
    free(first);
    free(second);
    free(ignored);
    free(directory);
}

static void test_language_switch(void)
{
    CHECK(language_resolve_is_chinese(LANGUAGE_SYSTEM, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)) &&
          language_resolve_is_chinese(LANGUAGE_SYSTEM, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)) &&
          !language_resolve_is_chinese(LANGUAGE_SYSTEM, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) &&
          language_resolve_is_chinese(LANGUAGE_CHINESE, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) &&
          !language_resolve_is_chinese(LANGUAGE_ENGLISH, MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)), "跟随系统时的语言判断不正确");

    language_set_mode(LANGUAGE_CHINESE);
    language_changes = 0;
    language_add_listener(on_language_changed, NULL);
    language_set_mode(LANGUAGE_ENGLISH);
    language_set_mode(LANGUAGE_ENGLISH);
    CHECK(language_changes == 1 && !language_is_chinese(), "切换到英文时没有通知或重复通知");
    wchar_t *one = str_added(1), *many = str_added(3);
    CHECK(wcscmp(S(STR_Overview), L"Overview") == 0 && wcscmp(one, L"Added 1 item") == 0 && wcscmp(many, L"Added 3 items") == 0, "英文文字不正确");
    free(one);
    free(many);
    language_set_mode(LANGUAGE_CHINESE);
    CHECK(language_changes == 2 && wcscmp(S(STR_Overview), L"概览") == 0, "切换回中文不正确");
    language_remove_listener(on_language_changed, NULL);

    // 文字表完整：每一条都有中文和英文。
    int empty = 0;
    for (int id = 0; id < STR_COUNT; id++)
    {
        const wchar_t *chinese, *english;
        string_sources((StringId)id, &chinese, &english);
        if (!chinese || !*chinese || !english || !*english) empty++;
    }
    CHECK(STR_COUNT > 300 && empty == 0, "文字表不完整");
}

// ───────────── 系统 ─────────────

static void test_startup_command(void)
{
    wchar_t *command = startup_build_command(L"C:\\Program Files\\Drop Shelf\\DropShelf.exe");
    CHECK(wcscmp(command, L"\"C:\\Program Files\\Drop Shelf\\DropShelf.exe\" --background") == 0, "开机启动命令没有正确处理空格路径");
    free(command);
}

static void test_single_instance(void)
{
    wchar_t *file = path_join(temporary_directory, L"a file.txt");
    write_text(file, L"x");
    StrList arguments = {0};
    strlist_add(&arguments, L"--background");
    CHECK(!single_instance_is_shelf_request(&arguments), "--background 不是打开置物架的请求");
    strlist_add(&arguments, L"--SHELF");
    CHECK(single_instance_is_shelf_request(&arguments), "--shelf 应不区分大小写");
    strlist_clear(&arguments);
    strlist_add(&arguments, file);
    strlist_add(&arguments, L"C:\\不存在\\nothing.txt");
    strlist_add(&arguments, temporary_directory);
    StrList paths;
    single_instance_path_arguments(&arguments, &paths);
    CHECK(single_instance_is_shelf_request(&arguments) && list_is(&paths, 2, file, temporary_directory), "命令行里的文件和文件夹没有认出来");
    strlist_clear(&paths);

    // 转交给已有实例的格式：UTF-16，用换行分隔，末尾带一个 0。
    WStr joined = {0};
    wstr_appendf(&joined, L"--shelf\n%ls", file);
    COPYDATASTRUCT data = { 0x44534831, (DWORD)((joined.length + 1) * sizeof(wchar_t)), joined.data };
    StrList received;
    CHECK(single_instance_read_arguments(&data, &received) && list_is(&received, 2, L"--shelf", file), "转交过来的命令行没有读对");
    strlist_clear(&received);
    data.dwData = 1;
    CHECK(!single_instance_read_arguments(&data, &received), "不是本程序发来的内容应忽略");
    wstr_free(&joined);

    wchar_t *mutex = single_instance_mutex_name(), *event = single_instance_show_event_name(), *title = single_instance_window_title();
    CHECK(wcscmp(mutex, L"Local\\DropShelf.SingleInstance.v1.ctest") == 0 && wcscmp(event, L"Local\\DropShelf.ShowMainWindow.v1.ctest") == 0 &&
          wcscmp(title, L"DropShelfMessageWindow.ctest") == 0, "单实例的名称不正确");
    CHECK(!single_instance_send_arguments(title, &arguments), "没有运行的实例时转交应失败");
    free(mutex);
    free(event);
    free(title);
    strlist_clear(&arguments);
    DeleteFileW(file);
    free(file);
}

static void test_log(void)
{
    log_info(L"测试 %d %ls", 7, L"日志");
    log_win32_error(L"出错了", ERROR_ACCESS_DENIED);
    wchar_t *directory = log_directory();
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[40];
    swprintf(name, 40, L"dropshelf-%04d-%02d-%02d.log", now.wYear, now.wMonth, now.wDay);
    wchar_t *path = path_join(directory, name);
    wchar_t *text = file_read_text(path, 1 << 20);
    // 例如 2026-11-14T10:23:45.1234567+08:00 [INFO] 测试 7 日志
    CHECK(text && wcslen(text) > 40 && text[4] == L'-' && text[10] == L'T' && text[19] == L'.' && (text[27] == L'+' || text[27] == L'-') &&
          text[30] == L':' && text[33] == L' ' && text[34] == L'[' && wcsstr(text, L" [INFO] 测试 7 日志\r\n"), "日志的格式不正确");
    CHECK(text && wcsstr(text, L" [ERROR] 出错了\r\nWin32 错误 5 (0x00000005): "), "错误日志的格式不正确");
    free(text);

    // 14 天前的日志会被清理，新的保留。
    wchar_t *old = path_join(directory, L"dropshelf-2020-01-01.log");
    write_text(old, L"old");
    HANDLE file = CreateFileW(old, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    FILETIME past = { 0, 0x01D50000 };
    SetFileTime(file, NULL, NULL, &past);
    CloseHandle(file);
    log_cleanup_old();
    CHECK(!path_exists(old) && path_exists(path), "旧日志没有清理，或清理了新的日志");
    free(old);
    free(path);
    free(directory);
}

static void test_paths(void)
{
    wchar_t *joined = path_join(L"C:\\a", L"b.txt");
    wchar_t *directory = path_directory(L"C:\\a\\b.txt");
    wchar_t *root = path_directory(L"C:\\b.txt");
    wchar_t *stem = path_file_stem(L"C:\\a\\zh-TW.json");
    CHECK(wcscmp(joined, L"C:\\a\\b.txt") == 0 && wcscmp(directory, L"C:\\a") == 0 && wcscmp(root, L"C:\\") == 0 && wcscmp(stem, L"zh-TW") == 0 &&
          wcscmp(path_file_name(L"C:/x/y.exe"), L"y.exe") == 0 && path_directory(L"file.txt") == NULL, "路径处理不正确");
    free(joined);
    free(directory);
    free(root);
    free(stem);
    wchar_t *first = new_guid_text(), *second = new_guid_text();
    CHECK(wcslen(first) == 32 && wcscmp(first, second) != 0, "新的标识不正确");
    free(first);
    free(second);
}

// ───────────── 主题 ─────────────

static void test_theme(void)
{
    CHECK(theme_resolve_is_dark(THEME_SYSTEM, true) && !theme_resolve_is_dark(THEME_SYSTEM, false), "跟随系统不正确");
    CHECK(theme_resolve_is_dark(THEME_DARK, false) && !theme_resolve_is_dark(THEME_LIGHT, true), "手动主题被系统覆盖");

    static const uint8_t bytes[] = {
        0x7B, 0x71, 0x71, 0xFF, 0x73, 0x69, 0x69, 0xFF, 0x6D, 0x64, 0x64, 0xFF, 0x68, 0x5F, 0x5F, 0xFF,
        0x62, 0x59, 0x59, 0xFF, 0x5C, 0x54, 0x54, 0xFF, 0x54, 0x4C, 0x4C, 0xFF, 0x88, 0x17, 0x98, 0x00
    };
    AccentPalette palette;
    CHECK(accent_palette_parse(bytes, sizeof(bytes), &palette) && color_equal(palette.base, COLOR_RGB(0x68, 0x5F, 0x5F)) &&
          color_equal(palette.light2, COLOR_RGB(0x73, 0x69, 0x69)), "强调色调色板解析错误");
    CHECK(!accent_palette_parse(bytes, 4, &palette), "不完整的调色板应被忽略");

    Color dark[THEME_COLOR_COUNT], light[THEME_COLOR_COUNT];
    theme_build_palette(true, palette, dark);
    theme_build_palette(false, accent_palette_default(), light);
    CHECK(color_equal(dark[THEME_TEXT_ON_ACCENT], COLOR_WHITE), "深色强调色上应使用白字");
    CHECK(color_contrast_ratio(dark[THEME_ACCENT_TEXT], COLOR_RGB(0x20, 0x20, 0x20)) >= 4.5, "强调色文字对比度不足");
    CHECK(color_equal(light[THEME_ACCENT], accent_palette_default().dark1) && color_equal(light[THEME_TEXT_ON_ACCENT], COLOR_WHITE), "默认浅色强调色不正确");
    CHECK(color_prefers_dark_text(COLOR_RGB(0x99, 0xEB, 0xFF)), "浅色强调色上应使用黑字");
    CHECK(light[THEME_WINDOW_BACKGROUND].r > light[THEME_WINDOW_BACKGROUND].b && light[THEME_SHELF_SOLID].r > light[THEME_SHELF_SOLID].b, "浅色背景应为象牙白");
    CHECK(color_equal(light[THEME_ACCENT_HOVER], COLOR_ARGB(0xE6, 0x00, 0x67, 0xC0)) && color_equal(light[THEME_STACK_EDGE], COLOR_ARGB(0x0D, 0x00, 0x67, 0xC0)) &&
          color_equal(light[THEME_SHEET_GLOW], COLOR_ARGB(0x19, 0x00, 0x67, 0xC0)) && color_equal(light[THEME_TEXT_PRIMARY], COLOR_ARGB(0xE4, 0, 0, 0)),
        "浅色调色板与 C# 版算出来的不一致");

    AccentPalette custom = accent_palette_from_color(COLOR_RGB(0x87, 0x64, 0xB8));
    CHECK(color_equal(custom.base, COLOR_RGB(0x87, 0x64, 0xB8)) && custom.light1.r > custom.base.r && custom.dark1.r < custom.base.r, "由一个颜色生成的深浅不正确");
    CHECK(color_equal(color_lerp(COLOR_BLACK, COLOR_WHITE, 0), COLOR_BLACK) && color_equal(color_lerp(COLOR_BLACK, COLOR_WHITE, 1), COLOR_WHITE) &&
          color_lerp(COLOR_BLACK, COLOR_WHITE, 0.5).r == 128 && color_equal(color_lerp(COLOR_TRANSPARENT, COLOR_RGB(10, 20, 30), 0.5), COLOR_ARGB(128, 10, 20, 30)),
        "颜色过渡不正确");

    theme_set_mode(THEME_DARK, L"#8764B8");
    CHECK(theme_is_dark() && color_equal(theme_accent().base, COLOR_RGB(0x87, 0x64, 0xB8)) && color_equal(theme_color(THEME_FLYOUT), COLOR_RGB(0x2C, 0x2C, 0x2C)),
        "切换到深色和自选强调色后颜色不正确");
    theme_set_mode(THEME_LIGHT, NULL);
    CHECK(!theme_is_dark() && color_equal(theme_color(THEME_FLYOUT), COLOR_RGB(0xFD, 0xFC, 0xF8)), "切换回浅色后颜色不正确");
}

// ───────────── 动画 ─────────────

static bool close_to(double value, double expected)
{
    return fabs(value - expected) < 1e-6;
}

static void test_animation(void)
{
    // 帧率正常时按真实时间走；一帧耽搁太久时只走两帧的时间，整段动画最多拖长 0.1 秒。
    double lag = 0;
    CHECK(close_to(frame_clock_step(0.016, &lag), 0.016) && lag == 0, "正常的一帧应按真实时间推进");
    CHECK(close_to(frame_clock_step(0.084, &lag), 0.034) && close_to(lag, 0.05), "耽搁的一帧应只前进两帧的时间");
    CHECK(close_to(frame_clock_step(0.134, &lag), 0.084) && close_to(lag, 0.1), "拖长的时间不应超过上限");
    CHECK(close_to(frame_clock_step(0.5, &lag), 0.5), "拖长的时间用完后应按真实时间推进");

    Ease standard = EASE_STANDARD;
    CHECK(ease_apply(standard, 0) == 0 && ease_apply(standard, 1) == 1 && ease_apply(standard, 0.5) > 0.8, "贝塞尔缓动不正确");
    CHECK(close_to(ease_bezier_inverse(standard, ease_apply(standard, 0.3)), 0.3), "贝塞尔缓动反过来求进度不正确");
    CHECK(close_to(ease_apply(EASE_OF(EASE_CUBIC_OUT), 0.5), 0.875) && close_to(ease_apply(EASE_OF(EASE_QUINTIC_OUT), 0.5), 0.96875) &&
          close_to(ease_apply(EASE_OF(EASE_LINEAR), 0.25), 0.25), "缓动不正确");
    double overshoot = 0;
    for (int step = 1; step < 100; step++)
    {
        double value = ease_apply(EASE_BACK_OF(0.28), step / 100.0);
        if (value > overshoot) overshoot = value;
    }
    CHECK(overshoot > 1.0 && overshoot < 1.2, "回弹的缓动应略微超过终点");

    Anim anim = {0};
    anim_set(&anim, 10);
    CHECK(!anim.running && anim.value == 10 && !anim_update(&anim, 1.0), "直接到位的数值不应在动");
    if (anim_enabled())
    {
        anim_start_delayed(&anim, 20, 0.2, 0.1, EASE_OF(EASE_LINEAR));
        CHECK(anim.running && anim_update(&anim, 5.0) && anim.value == 10, "第一帧应从起点开始");
        CHECK(anim_update(&anim, 5.0) && anim.value == 10, "同一帧重复更新不应多走");
        anim_update(&anim, 5.03);
        anim_update(&anim, 5.06);
        anim_update(&anim, 5.09);
        CHECK(anim.value == 10, "延迟期间应停在起点");
        anim_update(&anim, 5.12);
        anim_update(&anim, 5.15);
        anim_update(&anim, 5.18);
        anim_update(&anim, 5.20);
        CHECK(close_to(anim.value, 15), "动画走到一半时的数值不正确");
        anim_update(&anim, 5.23);
        anim_update(&anim, 5.26);
        anim_update(&anim, 5.29);
        CHECK(!anim_update(&anim, 5.31) && anim.value == 20 && !anim.running, "动画没有按时结束在终点");
    }

    low_performance_set_mode(PERFORMANCE_NORMAL);
    CHECK(!low_performance_enabled() && !low_performance_skip(1.0), "关闭低性能模式时不应跳帧");
    low_performance_set_mode(PERFORMANCE_LOW);
    CHECK(low_performance_enabled(), "开启低性能模式没有生效");
    low_performance_set_mode(PERFORMANCE_AUTO);
}

// ───────────── 布局和控件 ─────────────

static int clicks, selected_item = -1;
static bool toggled;
static void on_click(void *context) { (void)context; clicks++; }
static void on_select(int index, void *context) { (void)context; selected_item = index; }
static void on_toggle(bool checked, void *context) { (void)context; toggled = checked; }

static void test_layout(void)
{
    if (!gfx_initialize())
    {
        printf("  skipped: 这台机器上创建不了显卡设备\n");
        return;
    }
    language_set_mode(LANGUAGE_CHINESE);
    GfxTextStyle style = { GFX_FONT_TEXT, 14, 400, false, false, GFX_ALIGN_START, GFX_ALIGN_START };
    GfxSize single = gfx_text_measure(L"置物架 Shelf", &style, 0);
    style.wrap = true;
    GfxSize wrapped = gfx_text_measure(L"置物架 Shelf 置物架 Shelf 置物架 Shelf 置物架 Shelf", &style, 100);
    CHECK(single.width > 40 && single.height > 14 && single.height < 30 && wrapped.width <= 100 && wrapped.height > single.height * 2, "文字的大小量得不对");

    // 一行：左边固定的按钮，中间伸展的文字，右边分段选择；外面再套一列，底下一个占满剩余高度的占位。
    UiElement *root = ui_stack(false, 10);
    ui_stack_set_padding(root, 20, 20, 20, 20);
    UiElement *row = ui_stack(true, 8);
    UiElement *button = ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Quit), on_click, NULL);
    UiElement *text = ui_label(UI_STR(STR_StartupDescription), UI_TEXT_BODY);
    text->grow = 1;
    const UiText items[] = { UI_LIT(L"A"), UI_LIT(L"BB"), UI_STR(STR_ThemeDark) };
    UiElement *segmented = ui_segmented(items, 3, 0, on_select, NULL);
    UiElement *toggle = ui_toggle(false, on_toggle, NULL);
    ui_add(row, button);
    ui_add(row, text);
    ui_add(row, segmented);
    ui_add(row, toggle);
    ui_add(root, row);
    UiElement *filler = ui_spacer();
    ui_add(root, filler);
    UiElement *footer = ui_label(UI_LIT(L"footer"), UI_TEXT_CAPTION);
    ui_add(root, footer);

    ui_measure(root, 600);
    ui_arrange(root, (GfxRect){ 0, 0, 600, 400 });
    CHECK(row->bounds.left == 20 && row->bounds.right == 580 && row->bounds.top == 20, "一行的位置不对");
    CHECK(button->bounds.left == 20 && button->bounds.bottom - button->bounds.top == 32, "按钮应靠左、高 32");
    CHECK(toggle->bounds.right == 580 && segmented->bounds.right == toggle->bounds.left - 8, "右边的控件应贴着右边排列");
    CHECK(text->bounds.left == button->bounds.right + 8 && text->bounds.right == segmented->bounds.left - 8, "伸展的文字应占满中间");
    CHECK(footer->bounds.bottom == 380 && filler->bounds.bottom - filler->bounds.top > 200, "占位应把最后一行推到底部");
    float button_middle = (button->bounds.top + button->bounds.bottom) / 2, row_middle = (row->bounds.top + row->bounds.bottom) / 2;
    CHECK(fabs(button_middle - row_middle) < 0.01, "按钮应在这一行里垂直居中");

    // 窄了以后中间的文字折行，这一行变高。
    float wide_height = row->bounds.bottom - row->bounds.top;
    ui_measure(root, 380);
    ui_arrange(root, (GfxRect){ 0, 0, 380, 400 });
    CHECK(row->bounds.bottom - row->bounds.top > wide_height, "变窄后文字应折行");

    // 换成英文后控件上的文字跟着变，不用重建。
    float chinese_width = button->desired.width;
    language_set_mode(LANGUAGE_ENGLISH);
    ui_measure(root, 600);
    CHECK(button->desired.width != chinese_width, "切换语言后按钮的文字没有跟着变");
    language_set_mode(LANGUAGE_CHINESE);
    ui_measure(root, 600);
    ui_arrange(root, (GfxRect){ 0, 0, 600, 400 });

    // 找到光标下的控件；不接收鼠标的文字和容器不算。
    CHECK(ui_hit_test(root, button->bounds.left + 5, button_middle) == button && ui_hit_test(root, text->bounds.left + 5, row_middle) == NULL &&
          ui_hit_test(root, 5, 5) == NULL && ui_hit_test(root, segmented->bounds.left + 5, row_middle) == segmented, "光标下的控件找得不对");

    // 点击：在控件上松开才算。
    UiMouseEvent up = { .kind = UI_MOUSE_UP, .x = button->bounds.left + 5, .y = button_middle, .inside = true };
    button->type->mouse(button, &up);
    up.inside = false;
    button->type->mouse(button, &up);
    CHECK(clicks == 1, "按钮的点击不正确");
    up.inside = true;
    up.x = segmented->bounds.right - 8;
    segmented->type->mouse(segmented, &up);
    CHECK(selected_item == 2, "分段选择没有选中点到的那一项");
    toggle->type->mouse(toggle, &up);
    CHECK(toggled, "开关没有切换");
    ui_set_disabled(toggle, true);
    toggle->type->mouse(toggle, &up);
    CHECK(toggled, "禁用的开关不应响应点击");

    // 画进一张图片：能画出来，文件不是空的。
    wchar_t *image = path_join(temporary_directory, L"layout.png");
    bool drawn = gfx_begin_image(600, 400, 96);
    if (drawn)
    {
        gfx_clear(theme_color(THEME_WINDOW_BACKGROUND));
        gfx_shadow((GfxRect){ 300, 200, 500, 300 }, 8, 20, 0, 5, COLOR_ARGB(51, 0, 0, 0));
        gfx_push_opacity(0.5f);
        gfx_push_transform(gfx_matrix_scale(0.9f, 0.9f, 300, 200));
        ui_paint(root, 0);
        gfx_pop_transform();
        gfx_pop_layer();
        drawn = gfx_end_image(image);
    }
    WIN32_FILE_ATTRIBUTE_DATA data = {0};
    CHECK(drawn && GetFileAttributesExW(image, GetFileExInfoStandard, &data) && data.nFileSizeLow > 1000, "界面没有画进图片");
    free(image);
    ui_element_destroy(root);

    GfxMatrix scale = gfx_matrix_scale(2, 2, 10, 10);
    GfxMatrix moved = gfx_matrix_multiply(scale, gfx_matrix_translate(5, 0));
    CHECK(scale.dx == -10 && scale.dy == -10 && moved.dx == -5 && moved.m11 == 2, "变换矩阵不正确");
    gfx_shutdown();
}

static void run(const char *name, void (*test)(void))
{
    current_test = name;
    int before = failures;
    test();
    printf("%s %s\n", failures == before ? "ok    " : "FAILED", name);
}

int main(void)
{
    // 设置、日志和实例名称都换成测试专用的，不碰正在使用的 DropShelf。
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t folder[64];
    swprintf(folder, 64, L"DropShelfCTests-%lu", GetCurrentProcessId());
    temporary_directory = path_join(base, folder);
    directory_create(temporary_directory);
    wchar_t *settings_file = path_join(temporary_directory, L"settings.json");
    wchar_t *logs = path_join(temporary_directory, L"Logs");
    SetEnvironmentVariableW(L"DROPSHELF_SETTINGS_PATH", settings_file);
    SetEnvironmentVariableW(L"DROPSHELF_LOG_DIRECTORY", logs);
    SetEnvironmentVariableW(L"DROPSHELF_INSTANCE", L"ctest");

    run("json", test_json);
    run("paths", test_paths);
    run("settings round trip", test_settings_round_trip);
    run("settings migration", test_settings_migration);
    run("settings normalization", test_settings_normalization);
    run("quick action slots", test_quick_action_slots);
    run("hotkey gesture", test_hotkey_gesture);
    run("hotkey registration", test_hotkey_registration);
    run("language packs", test_language_packs);
    run("language switch", test_language_switch);
    run("startup command", test_startup_command);
    run("single instance", test_single_instance);
    run("log", test_log);
    run("theme", test_theme);
    run("animation", test_animation);
    run("layout and controls", test_layout);

    // 清理临时文件夹。
    settings_flush();
    wchar_t command[MAX_PATH * 2];
    swprintf(command, MAX_PATH * 2, L"cmd /c rmdir /s /q \"%ls\"", temporary_directory);
    _wsystem(command);
    free(settings_file);
    free(logs);

    printf(failures == 0 ? "\nAll tests passed.\n" : "\n%d check(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
