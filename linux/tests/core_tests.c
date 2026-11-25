// Linux 共享核心的冒烟测试：字符串、路径、文件、JSON、手势、设置。不需要窗口和 Wayland。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "attachment_cache.h"
#include "base.h"
#include "drop_read.h"
#include "file_identity.h"
#include "gesture.h"
#include "gesture_linux.h"
#include "foreground_linux.h"
#include "hyprland_linux.h"
#include "image_quality.h"
#include "json.h"
#include "log.h"
#include "settings.h"
#include "shelf_model.h"
#include "task.h"

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                                 \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

static bool wcs_is(const wchar_t *text, const wchar_t *expected)
{
    return text && wcscmp(text, expected) == 0;
}

static void test_wide_strings(void)
{
    WStr text = {0};
    wstr_appendf(&text, L"%s|%ls|%c|%hs|%S|%I64d|%.1f", L"wide", L"wide2", L'w', "narrow", "narrow2",
        (long long)9000000000LL, 1.25);
    CHECK(wcs_is(text.data, L"wide|wide2|w|narrow|narrow2|9000000000|1.2"));
    wstr_free(&text);

    wchar_t *trimmed = wcs_trim_dup(L"  \t你好　 ");
    CHECK(wcs_is(trimmed, L"你好"));
    free(trimmed);

    CHECK(wcs_equal_ignore_case(L"DrOpShElF", L"dropshelf"));
    CHECK(wcs_find_ignore_case(L"ABCDEF", L"xyz") == NULL);
    const wchar_t *found = wcs_find_ignore_case(L"Hello World", L"world");
    CHECK(found && wcs_is(found, L"World"));

    TextSet set = { .ignore_case = true };
    CHECK(text_set_add(&set, L"Alpha"));
    CHECK(!text_set_add(&set, L"alpha"));
    CHECK(text_set_contains(&set, L"ALPHA"));
    CHECK(!text_set_contains(&set, L"Beta"));
    text_set_clear(&set);
}

static void test_utf8(void)
{
    const char *source = "你好 😀 /tmp";
    wchar_t *wide = wide_from_utf8(source, strlen(source));
    CHECK(wcslen(wide) == 9); // 你 好 空格 😀 空格 / t m p
    CHECK(wide[0] == L'你');
    size_t length = 0;
    char *back = utf8_from_wide(wide, &length);
    CHECK(length == strlen(source));
    CHECK(strcmp(back, source) == 0);
    free(back);
    free(wide);
}

static void test_paths(void)
{
    wchar_t *joined = path_join(L"/tmp/root", L"a.txt");
    CHECK(wcs_is(joined, L"/tmp/root/a.txt"));
    free(joined);

    joined = path_join(L"/tmp/root/", L"a.txt");
    CHECK(wcs_is(joined, L"/tmp/root/a.txt"));
    free(joined);

    CHECK(wcs_is(path_file_name(L"/tmp/root/a.txt"), L"a.txt"));
    wchar_t *directory = path_directory(L"/tmp/root/a.txt");
    CHECK(wcs_is(directory, L"/tmp/root"));
    free(directory);

    wchar_t *stem = path_file_stem(L"/tmp/root/a.tar.gz");
    CHECK(wcs_is(stem, L"a.tar"));
    free(stem);

    CHECK(file_name_is_reserved(L"CON") == false);
}

static wchar_t *make_temporary_root(void)
{
    char template[] = "/tmp/dropshelf-test-XXXXXX";
    char *directory = mkdtemp(template);
    if (!directory) return NULL;
    return wide_from_utf8(directory, strlen(directory));
}

static void test_files(void)
{
    wchar_t *root = make_temporary_root();
    CHECK(root != NULL);
    if (!root) return;

    wchar_t *nested = path_join(root, L"one/two");
    CHECK(directory_create(nested));
    CHECK(path_is_directory(nested));

    wchar_t *text_path = path_join(nested, L"text.txt");
    const char *content = "你好，DropShelf";
    CHECK(file_write_atomic(text_path, content, strlen(content)));
    wchar_t *read_back = file_read_text(text_path, 1024 * 1024);
    CHECK(wcs_is(read_back, L"你好，DropShelf"));
    free(read_back);

    // UTF-8 BOM
    wchar_t *bom_path = path_join(nested, L"bom8.txt");
    HANDLE file = io_create_file(bom_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    static const unsigned char bom8[] = { 0xEF, 0xBB, 0xBF, 'o', 'k' };
    DWORD written = 0;
    CHECK(WriteFile(file, bom8, sizeof(bom8), &written, NULL));
    CHECK(written == sizeof(bom8));
    CloseHandle(file);
    read_back = file_read_text(bom_path, 1024);
    CHECK(wcs_is(read_back, L"ok"));
    free(read_back);

    // UTF-16 LE BOM（Windows 版写下的设置文件）
    wchar_t *bom16_path = path_join(nested, L"bom16.txt");
    file = io_create_file(bom16_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    static const unsigned char bom16[] = { 0xFF, 0xFE, 'h', 0, 'i', 0 };
    CHECK(WriteFile(file, bom16, sizeof(bom16), &written, NULL));
    CloseHandle(file);
    read_back = file_read_text(bom16_path, 1024);
    CHECK(wcs_is(read_back, L"hi"));
    free(read_back);

    CHECK(io_copy_file(text_path, bom16_path, FALSE));
    wchar_t *moved_path = path_join(nested, L"moved.txt");
    CHECK(io_move_file(text_path, moved_path, MOVEFILE_REPLACE_EXISTING));
    CHECK(!path_exists(text_path));
    free(moved_path);

    ULARGE_INTEGER available, total, free_bytes;
    CHECK(io_disk_free_space(root, &available, &total, &free_bytes));
    CHECK(total.QuadPart > 0);

    free(bom_path);
    free(bom16_path);
    free(text_path);
    free(nested);
    free(root);
}

static void test_json(void)
{
    JsonValue *value = json_parse(L"{ // comment\n \"a\": 1, \"b\": [true, \"x\", null,], }", JSON_ALLOW_COMMENTS | JSON_ALLOW_TRAILING_COMMAS);
    CHECK(value != NULL);
    if (value)
    {
        const JsonValue *a = json_get(value, L"a");
        CHECK(a && a->type == JSON_NUMBER && a->number == 1);
        const JsonValue *b = json_get(value, L"b");
        CHECK(b && b->type == JSON_ARRAY && b->count == 3);
        CHECK(b && b->items[0]->type == JSON_BOOL && b->items[0]->boolean);
        CHECK(b && wcs_is(b->items[1]->string, L"x"));
        json_free(value);
    }

    CHECK(json_parse(L"{\"a\": }", 0) == NULL);
}

static void test_gesture_and_settings(void)
{
    CHECK(shake_sensitivity_clamp(-10) == SHAKE_STEADY);
    CHECK(shake_sensitivity_clamp(150) == SHAKE_RESPONSIVE);
    CHECK(shake_sensitivity_clamp(42) == 42);
    CHECK(edge_dwell_milliseconds(DWELL_SHORT) > 0);
    CHECK(edge_dwell_milliseconds(DWELL_SHORT) < edge_dwell_milliseconds(DWELL_LONG));

    EdgeDwellDetector dwell;
    edge_dwell_init(&dwell, 100);
    CHECK(!edge_dwell_update(&dwell, EDGE_LEFT, 0, 100, 0));
    CHECK(edge_dwell_update(&dwell, EDGE_LEFT, 0, 100, 100));
    CHECK(!edge_dwell_update(&dwell, EDGE_LEFT, 0, 100, 150));
    CHECK(!edge_dwell_update(&dwell, EDGE_NONE, 50, 50, 200));
    CHECK(!edge_dwell_update(&dwell, EDGE_LEFT, 0, 100, 300));
    CHECK(edge_dwell_update(&dwell, EDGE_LEFT, 0, 100, 400));

    AppSettings settings;
    settings_init(&settings);
    CHECK(settings.version == SETTINGS_CURRENT_VERSION);
    CHECK(settings.shake_sensitivity == SHAKE_BALANCED);
    CHECK(settings.hotkey != NULL);
    CHECK(settings.pause_in_fullscreen);
    CHECK(settings.mixed_drag == MIXED_DRAG_BATCH);
    settings.shake_sensitivity = 80;
    settings.pause_in_fullscreen = false;
    settings.mixed_drag = MIXED_DRAG_TEXT;
    strlist_add(&settings.watched_folders, L"/tmp/watched");
    wchar_t *json = settings_serialize(&settings);
    CHECK(json != NULL);

    AppSettings parsed;
    settings_init(&parsed);
    CHECK(settings_parse(json, &parsed));
    CHECK(parsed.shake_sensitivity == 80);
    CHECK(!parsed.pause_in_fullscreen);
    CHECK(parsed.mixed_drag == MIXED_DRAG_TEXT);
    CHECK(parsed.watched_folders.count == 1);
    CHECK(wcs_is(parsed.watched_folders.items[0], L"/tmp/watched"));
    settings_free(&parsed);
    settings_free(&settings);
    free(json);
}

static long long measure_quality(void *context, int quality)
{
    (void)context;
    return (long long)quality * 100;
}

static void test_image_quality(void)
{
    int found = image_quality_search(1, 100, 100, 10000, 500, measure_quality, NULL);
    CHECK(found == 5);
}

static void test_settings_path(void)
{
    wchar_t *root = make_temporary_root();
    if (!root) return;
    char *root_bytes = utf8_from_wide(root, NULL);
    setenv("XDG_CONFIG_HOME", root_bytes, 1);
    setenv("XDG_DATA_HOME", root_bytes, 1);
    setenv("XDG_CACHE_HOME", root_bytes, 1);
    setenv("XDG_STATE_HOME", root_bytes, 1);
    unsetenv("DROPSHELF_SETTINGS_PATH");

    wchar_t *config = config_directory();
    CHECK(path_file_name(config) != NULL);
    free(config);
    wchar_t *path = settings_path();
    const wchar_t *name = path_file_name(path);
    CHECK(wcs_is(name, L"settings.json"));
    free(path);

    // 保存队列：连续写两次只留下最后一次。
    static SaveQueue queue = SAVE_QUEUE_INIT(L"测试保存失败");
    wchar_t *target = path_join(root, L"saved.txt");
    save_queue_post(&queue, target, strdup("one"), 3);
    save_queue_post(&queue, target, strdup("two"), 3);
    save_queue_flush(&queue);
    wchar_t *text = file_read_text(target, 1024);
    CHECK(wcs_is(text, L"two"));
    free(text);
    free(target);

    free(root_bytes);
    free(root);
}

static void test_file_identity(void)
{
    wchar_t *root = path_join(L"/tmp", L"dropshelf-identity-test");
    // 用一个固定的临时目录：清掉上次留下的。
    directory_delete_tree(root);
    CHECK(directory_create(root));
    wchar_t *first = path_join(root, L"first.txt");
    wchar_t *second = path_join(root, L"second.txt");
    CHECK(file_write_atomic(first, "one", 3));
    CHECK(file_write_atomic(second, "two", 3));

    FileIdentity identity = {0};
    CHECK(file_identity_get(first, &identity));
    CHECK(file_identity_valid(&identity));
    FileIdentity other = {0};
    CHECK(file_identity_get(second, &other));
    CHECK(identity.volume_serial != other.volume_serial || identity.file_index != other.file_index);
    FileIdentity again = {0};
    CHECK(file_identity_get(first, &again));
    CHECK(identity.volume_serial == again.volume_serial && identity.file_index == again.file_index);
    CHECK(!file_identity_get(L"/tmp/dropshelf-identity-test/missing.txt", &other));

    free(first);
    free(second);
    CHECK(directory_delete_tree(root));
    free(root);
}

static void test_attachment_names(void)
{
    StrList used = {0};
    wchar_t *name = attachment_safe_name(L"a/b\\\\c:d*e?.txt", &used);
    CHECK(wcs_is(name, L"c_d_e_.txt"));
    free(name);
    wchar_t *duplicate = attachment_safe_name(L"c_d_e_.txt", &used);
    CHECK(wcs_is(duplicate, L"c_d_e_ (2).txt"));
    free(duplicate);
    // 末尾的点和空白会被去掉（Windows 文件名规则），Linux 上这些字符本身是合法的。
    wchar_t *trimmed = attachment_safe_name(L"name.  ", &used);
    CHECK(wcs_is(trimmed, L"name"));
    free(trimmed);
    wchar_t *blank = attachment_safe_name(L"   ", &used);
    CHECK(wcs_is(blank, L"attachment"));
    free(blank);
    strlist_clear(&used);
}

/// 转一会儿主循环：临时附件的删除排在这一轮消息处理之后（见 attachment_cache_release），要等它做完。
static void pump_until_gone(const wchar_t *path)
{
    char *bytes = utf8_from_wide(path, NULL);
    if (!bytes) return;
    for (int waited = 0; waited < 8000 && g_file_test(bytes, G_FILE_TEST_EXISTS); waited += 5)
    {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(5000);
    }
    free(bytes);
}

static void test_attachment_cache(void)
{
    const wchar_t *root = attachment_cache_root();
    CHECK(root && wcslen(root) > 1);
    wchar_t *directory = attachment_cache_create();
    CHECK(directory != NULL);
    CHECK(attachment_cache_is_owned(directory));
    CHECK(!attachment_cache_is_owned(root));
    wchar_t *file = path_join(directory, L"note.txt");
    CHECK(file_write_atomic(file, "hello", 5));
    wchar_t *found = attachment_cache_directory_of(file);
    CHECK(found && wcs_is(found, directory));
    free(found);
    free(file);
    attachment_cache_release(directory);
    // 删除排在后面（消息处理完、线程池删完），转一会儿主循环等它做完。
    pump_until_gone(directory);
    CHECK(!path_exists(directory));
    free(directory);
}

typedef struct ReadResult
{
    bool called;
    int count;
    int failed;
    bool saw_file;
    bool saw_folder;
    bool file_readable;
    bool folder_readable;
} ReadResult;

static void read_done(DropReadResult *result, void *context)
{
    ReadResult *out = context;
    out->called = true;
    out->count = result->count;
    out->failed = result->failed;
    for (int index = 0; index < result->count; index++)
    {
        DroppedEntry *entry = &result->entries[index];
        if (entry->kind == ITEM_FILE)
        {
            out->saw_file = true;
            if (entry->has_facts) out->file_readable = true;
        }
        if (entry->kind == ITEM_FOLDER) out->saw_folder = out->folder_readable = entry->has_facts;
    }
    drop_result_free(result);
}

static void test_drop_read_paths(void)
{
    wchar_t *root = path_join(L"/tmp", L"dropshelf-drop-read-test");
    directory_delete_tree(root);
    CHECK(directory_create(root));
    wchar_t *folder = path_join(root, L"folder");
    CHECK(directory_create(folder));
    wchar_t *file = path_join(root, L"note.txt");
    CHECK(file_write_atomic(file, "hello", 5));
    FileFacts direct = {0};
    CHECK(file_facts_read(file, &direct));
    CHECK(!direct.is_folder && direct.has_size && direct.size_bytes == 5);
    StrList paths = {0};
    strlist_add(&paths, file);
    strlist_add(&paths, folder);
    strlist_add(&paths, L"/tmp/dropshelf-drop-read-test/missing.txt");
    ReadResult read = {0};
    drop_read_paths(&paths, read_done, &read);
    CHECK(read.called);
    CHECK(read.count == 3);
    CHECK(read.failed == 1);
    CHECK(read.saw_file && read.file_readable);
    CHECK(read.saw_folder && read.folder_readable);
    strlist_clear(&paths);
    free(file);
    free(folder);
    directory_delete_tree(root);
    free(root);
}

static void test_shelf_model(void)
{
    wchar_t *root = path_join(L"/tmp", L"dropshelf-model-test");
    directory_delete_tree(root);
    CHECK(directory_create(root));
    wchar_t *file = path_join(root, L"note.txt");
    CHECK(file_write_atomic(file, "hello", 5));

    ShelfModel *model = shelf_model_new(NULL);
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc(2, sizeof(DroppedEntry));
    result->entries[0].kind = ITEM_FILE;
    result->entries[0].value = wcs_dup(file);
    result->entries[1].kind = ITEM_TEXT;
    result->entries[1].value = wcs_dup(L"一段文字");
    result->count = 2;
    ShelfAddSummary summary = shelf_model_add(model, result);
    drop_result_free(result);
    CHECK(summary.added == 2);
    CHECK(model->count == 2);
    CHECK(model->items[0]->kind == ITEM_FILE);

    wchar_t *title = shelf_model_title(model);
    CHECK(title && wcslen(title) > 0);
    free(title);
    wchar_t *caption = shelf_model_stack_caption(model);
    CHECK(caption && wcslen(caption) > 0);
    free(caption);

    // 同一个文件再放一次算重复，内容相同的文字也算重复。
    DropReadResult *duplicate = xcalloc(1, sizeof(DropReadResult));
    duplicate->entries = xcalloc(2, sizeof(DroppedEntry));
    duplicate->entries[0].kind = ITEM_FILE;
    duplicate->entries[0].value = wcs_dup(file);
    duplicate->entries[1].kind = ITEM_TEXT;
    duplicate->entries[1].value = wcs_dup(L"一段文字");
    duplicate->count = 2;
    summary = shelf_model_add(model, duplicate);
    drop_result_free(duplicate);
    CHECK(summary.duplicates == 2);
    CHECK(model->count == 2);

    // Linux 的文件名分大小写：note.txt 和 NOTE.txt、Note.TXT 是不同的文件，都要放进来。
    // 一次放一项走逐个比较，一次放 16 项以上走排好序的集合，两条路都试。
    wchar_t *upper = path_join(root, L"NOTE.txt");
    CHECK(file_write_atomic(upper, "upper", 5));
    DropReadResult *other_case = xcalloc(1, sizeof(DropReadResult));
    other_case->entries = xcalloc(1, sizeof(DroppedEntry));
    other_case->entries[0].kind = ITEM_FILE;
    other_case->entries[0].value = wcs_dup(upper);
    other_case->count = 1;
    summary = shelf_model_add(model, other_case);
    drop_result_free(other_case);
    CHECK(summary.added == 1 && summary.duplicates == 0);
    wchar_t *mixed = path_join(root, L"Note.TXT");
    CHECK(file_write_atomic(mixed, "mixed", 5));
    DropReadResult *many = xcalloc(1, sizeof(DropReadResult));
    many->entries = xcalloc(16, sizeof(DroppedEntry));
    for (int index = 0; index < 15; index++)
    {
        WStr text = {0};
        wstr_appendf(&text, L"第 %d 段", index);
        many->entries[index].kind = ITEM_TEXT;
        many->entries[index].value = wstr_take(&text);
    }
    many->entries[15].kind = ITEM_FILE;
    many->entries[15].value = wcs_dup(mixed);
    many->count = 16;
    summary = shelf_model_add(model, many);
    drop_result_free(many);
    CHECK(summary.added == 16 && summary.duplicates == 0);
    CHECK(model->count == 19);
    free(upper);
    free(mixed);

    shelf_model_release(model);
    free(file);
    CHECK(directory_delete_tree(root));
    free(root);
}


/// 把项目拖回置物架、在某一行松手：按松手位置重排，项目之间的先后不变。
static void test_shelf_model_move(void)
{
    ShelfModel *model = shelf_model_new(NULL);
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc(4, sizeof(DroppedEntry));
    const wchar_t *values[4] = { L"一", L"二", L"三", L"四" };
    for (int index = 0; index < 4; index++)
    {
        result->entries[index].kind = ITEM_TEXT;
        result->entries[index].value = wcs_dup(values[index]);
    }
    result->count = 4;
    CHECK(shelf_model_add(model, result).added == 4);
    drop_result_free(result);
    if (model->count != 4)
    {
        shelf_model_release(model);
        return;
    }
    ShelfItem *first = model->items[0];
    ShelfItem *second = model->items[1];
    ShelfItem *third = model->items[2];
    ShelfItem *fourth = model->items[3];

    // 拖到第四项上松手：它占住第四项原来那一行。
    ShelfItem *one[1] = { first };
    shelf_model_move(model, one, 1, 3);
    CHECK(model->count == 4 && model->items[0] == second && model->items[1] == third && model->items[2] == first &&
        model->items[3] == fourth);

    // 一次拖两项：它们之间的先后不变。
    ShelfItem *pair[2] = { second, third };
    shelf_model_move(model, pair, 2, 4);
    CHECK(model->items[0] == first && model->items[1] == fourth && model->items[2] == second && model->items[3] == third);

    // 拖到列表末尾以外就是放到最上面（堆叠里松手）。
    ShelfItem *last[1] = { first };
    shelf_model_move(model, last, 1, 99);
    CHECK(model->count == 4 && model->items[3] == first);

    // 不在这个置物架上的项目：什么也不做。
    ShelfModel *other = shelf_model_new(NULL);
    DropReadResult *foreign_result = xcalloc(1, sizeof(DropReadResult));
    foreign_result->entries = xcalloc(1, sizeof(DroppedEntry));
    foreign_result->entries[0].kind = ITEM_TEXT;
    foreign_result->entries[0].value = wcs_dup(L"别人的");
    foreign_result->count = 1;
    CHECK(shelf_model_add(other, foreign_result).added == 1);
    drop_result_free(foreign_result);
    if (other->count == 1)
    {
        ShelfItem *foreign[1] = { other->items[0] };
        shelf_model_move(model, foreign, 1, 0);
        CHECK(model->count == 4 && model->items[3] == first && model->items[0] != other->items[0]);
    }
    shelf_model_release(other);
    shelf_model_release(model);
}

static void test_write_for_drag(void)
{
    ShelfModel *model = shelf_model_new(NULL);
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc(2, sizeof(DroppedEntry));
    result->entries[0].kind = ITEM_TEXT;
    result->entries[0].value = wcs_dup(L"拖出的文字");
    result->entries[1].kind = ITEM_LINK;
    result->entries[1].value = wcs_dup(L"https://example.com/page");
    result->count = 2;
    CHECK(shelf_model_add(model, result).added == 2);
    drop_result_free(result);

    // 和文件一起拖出的文字、链接各写成一个文件，内容要完整。
    ShelfItem *others[2] = { model->items[0], model->items[1] };
    StrList paths = {0};
    shelf_model_write_for_drag(model, others, 2, &paths);
    CHECK(paths.count == 2);
    if (paths.count == 2)
    {
        CHECK(path_exists(paths.items[0]) && path_exists(paths.items[1]));
        wchar_t *text = file_read_text(paths.items[0], 64 * 1024);
        CHECK(wcs_is(text, L"拖出的文字"));
        free(text);
        wchar_t *shortcut = file_read_text(paths.items[1], 64 * 1024);
        CHECK(shortcut && wcs_find_ignore_case(shortcut, L"URL=https://example.com/page") != NULL);
        free(shortcut);
        CHECK(attachment_cache_is_owned(paths.items[0]));
    }

    // 同一轮拖动里再取一次时还用已经写好的文件，不重复写。
    StrList again = {0};
    shelf_model_write_for_drag(model, others, 2, &again);
    CHECK(again.count == 2);
    CHECK(again.count == 2 && wcs_equal(again.items[0], paths.items[0]) && wcs_equal(again.items[1], paths.items[1]));
    strlist_clear(&again);
    strlist_clear(&paths);
    shelf_model_release(model);
}

static void test_hyprland_monitors(void)
{
    // 竖屏（transform 3）要按旋转对调宽高，reserved 是逻辑像素。
    const wchar_t *json =
        L"[{\"name\":\"DP-3\",\"x\":2560,\"y\":0,\"width\":2560,\"height\":1440,\"scale\":1.0,\"transform\":3,\"reserved\":[0,0,40,0]},"
        L"{\"name\":\"DP-2\",\"x\":0,\"y\":-1440,\"width\":3840,\"height\":2160,\"scale\":2.0,\"transform\":0,\"reserved\":[0,30,0,0]}]";
    HyprMonitor monitors[2];
    int count = hyprland_parse_monitors(json, monitors, 2);
    CHECK(count == 2);
    CHECK(wcs_is(monitors[0].name, L"DP-3"));
    CHECK(monitors[0].rect.left == 2560 && monitors[0].rect.top == 0);
    CHECK(monitors[0].rect.right == 4000 && monitors[0].rect.bottom == 2560);
    CHECK(monitors[0].reserved_right == 40);
    CHECK(monitors[0].scale == 1);
    CHECK(wcs_is(monitors[1].name, L"DP-2"));
    CHECK(monitors[1].rect.right == 1920 && monitors[1].rect.bottom == -1440 + 1080);
    CHECK(monitors[1].reserved_top == 30);
    CHECK(monitors[1].scale == 2);
    hyprland_monitors_free(monitors, 2);
    CHECK(monitors[0].name == NULL && monitors[1].name == NULL);

    CHECK(hyprland_parse_monitors(L"{}", monitors, 2) == 0);
    CHECK(hyprland_parse_monitors(L"[{\"name\":\"x\"}]", monitors, 2) == 0);
}

static void test_gesture_edges(void)
{
    // 横屏 DP-2（上方有 40 像素的面板），竖屏 DP-3（右侧有 40 像素的停靠栏）。
    HyprMonitor monitors[2] = {0};
    monitors[0].rect = (RECT){ 0, 1120, 2560, 2560 };
    monitors[0].reserved_top = 40;
    monitors[1].rect = (RECT){ 2560, 0, 4000, 2560 };
    monitors[1].reserved_right = 40;
    CHECK(gesture_edge_at(monitors, 2, 0, 2000) == EDGE_LEFT);
    CHECK(gesture_edge_at(monitors, 2, 0, 1140) == EDGE_NONE);       // 面板占着的地方
    CHECK(gesture_edge_at(monitors, 2, 3000, 0) == EDGE_TOP);
    CHECK(gesture_edge_at(monitors, 2, 2560, 0) == EDGE_LEFT);       // 左上角算左边
    CHECK(gesture_edge_at(monitors, 2, 3999, 1000) == EDGE_NONE);    // 停靠栏占着的地方
    CHECK(gesture_edge_at(monitors, 2, 3958, 1000) == EDGE_NONE);    // 离右边不到两像素但不在工作区
    CHECK(gesture_edge_at(monitors, 2, 2559, 2000) == EDGE_NONE);    // 两台显示器中间的分界
    CHECK(gesture_edge_at(monitors, 2, 1000, 2000) == EDGE_NONE);
    CHECK(gesture_edge_at(monitors, 2, -5, 2000) == EDGE_NONE);      // 屏幕外
}

static void test_foreground_parsing(void)
{
    wchar_t *active = foreground_active_class(L"{\"class\":\"firefox\",\"title\":\"首页\"}");
    CHECK(wcs_is(active, L"firefox"));
    free(active);
    CHECK(foreground_active_class(L"{}") == NULL);
    CHECK(foreground_active_fullscreen(L"{\"class\":\"steam_app_123\",\"fullscreen\":1}"));
    CHECK(!foreground_active_fullscreen(L"{\"class\":\"steam_app_123\",\"fullscreen\":0}"));
    CHECK(!foreground_active_fullscreen(L"{}"));

    const wchar_t *clients =
        L"[{\"class\":\"firefox\",\"mapped\":true,\"at\":[0,0],\"size\":[800,600]},"
        L"{\"class\":\"steam_app_123\",\"mapped\":true,\"at\":[800,0],\"size\":[600,600]},"
        L"{\"class\":\"dropshelf\",\"mapped\":true,\"at\":[0,0],\"size\":[10,10]},"
        L"{\"class\":\"firefox\",\"mapped\":true,\"at\":[0,0],\"size\":[800,600]},"
        L"{\"class\":\"hiddenone\",\"mapped\":false,\"at\":[0,0],\"size\":[100,100]}]";
    StrList apps = {0};
    foreground_clients_apps(clients, &apps);
    CHECK(apps.count == 2);
    CHECK(apps.count == 2 && wcs_is(apps.items[0], L"firefox") && wcs_is(apps.items[1], L"steam_app_123"));
    strlist_clear(&apps);

    StrList classes = {0};
    foreground_clients_at(clients, 900, 100, &classes);
    CHECK(classes.count == 1 && wcs_is(classes.items[0], L"steam_app_123"));
    strlist_clear(&classes);
    foreground_clients_at(clients, 5, 5, &classes);
    CHECK(classes.count == 2);   // firefox 和 dropshelf 叠在一起
    strlist_clear(&classes);
    foreground_clients_at(clients, 1500, 100, &classes);
    CHECK(classes.count == 0);
}

int main(void)
{
    test_wide_strings();
    test_utf8();
    test_paths();
    test_files();
    test_json();
    test_gesture_and_settings();
    test_image_quality();
    test_settings_path();
    test_file_identity();
    test_attachment_names();
    test_attachment_cache();
    test_drop_read_paths();
    test_shelf_model();
    test_shelf_model_move();
    test_write_for_drag();
    test_hyprland_monitors();
    test_gesture_edges();
    test_foreground_parsing();

    if (failures)
    {
        fprintf(stderr, "%d 项测试失败\n", failures);
        return 1;
    }
    printf("全部测试通过\n");
    return 0;
}