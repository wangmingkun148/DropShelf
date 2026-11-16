// 第 6 步（合并和拆分、最近关闭的置物架、跟着改名）的测试，对应 C# 版 ShelfTests 里的
// TestFollowsRenamedFiles、TestAdoptBetweenShelves 和 TestShelfHistory。由 tests.c 引入。

static ShelfItem *item_named(ShelfModel *model, const wchar_t *name)
{
    for (int index = 0; index < model->count; index++)
        if (wcs_equal(shelf_item_display_name(model->items[index]), name)) return model->items[index];
    return NULL;
}

static bool parent_is(const wchar_t *path, const wchar_t *name)
{
    wchar_t *parent = path_directory(path);
    bool same = parent && wcs_equal(path_file_name(parent), name);
    free(parent);
    return same;
}

static void test_follow_renamed(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    task_initialize();
    wchar_t *folder = test_folder(L"follow-rename");
    wchar_t *path = test_file(folder, L"before.txt", "x");
    FileIdentity identity;
    if (!file_identity_get(path, &identity))
    {
        // 只有 NTFS 上的文件有稳定的标识；测试目录在别的文件系统上时没法验证。
        printf("    （测试目录不在 NTFS 上，跳过）\n");
        free(path);
        free(folder);
        return;
    }
    wchar_t *missing = path_join(folder, L"missing.txt");
    FileIdentity none;
    CHECK(!file_identity_get(missing, &none) && !file_identity_get(L"\\\\server\\share\\a.txt", &none), "不存在的文件或网络路径取到了标识");

    wchar_t *sub = path_join(folder, L"sub");
    directory_create(sub);
    ShelfModel *model = shelf_model_new(NULL);
    const wchar_t *paths[] = { path, sub };
    add_paths(model, paths, 2);
    ShelfItem *file = model->items[0], *directory = model->items[1];
    CHECK(file->identity.file_index == identity.file_index && file_identity_valid(&directory->identity) &&
          directory->identity.file_index != identity.file_index, "放进置物架时没有记下文件标识");

    // 改名并挪进另一个文件夹：原来的路径没了，按标识找到现在的位置；项目还是原来那个，名称和类型跟着变。
    wchar_t *inner = path_join(folder, L"inner");
    directory_create(inner);
    wchar_t *after = path_join(inner, L"after.md");
    wchar_t *renamed = path_join(folder, L"renamed");
    MoveFileW(path, after);
    MoveFileW(sub, renamed);
    ShelfItem *all[] = { file, directory };
    shelf_model_relink(model, all, 2);
    CHECK(model->items[0] == file && wcs_equal(shelf_item_display_name(file), L"after.md") && path_exists(file->value) && file->available,
        "改名并移动后的文件没有跟上");
    CHECK(parent_is(file->value, L"inner"), "文件的新位置不对");
    CHECK(wcs_equal(shelf_item_display_name(directory), L"renamed") && path_is_directory(directory->value), "改名后的文件夹没有跟上");

    // 正在拖出时文件不见了，是被目标程序移走的，不跟过去。
    wchar_t *away = path_join(folder, L"away.md");
    MoveFileW(file->value, away);
    wchar_t *before = wcs_dup(file->value);
    model->dragging_out = true;
    shelf_model_relink(model, &file, 1);
    CHECK(wcs_equal(file->value, before), "拖出期间不应该跟着文件走");
    model->dragging_out = false;
    shelf_model_relink(model, &file, 1);
    CHECK(wcs_equal(shelf_item_display_name(file), L"away.md") && path_exists(file->value), "拖出结束后没有再跟上");

    // 置物架上已经有这个路径时不重复；真的删除了就照旧显示找不到。
    wchar_t *twin = test_file(folder, L"twin.md", "y");
    add_paths(model, (const wchar_t *const *)&twin, 1);
    DeleteFileW(twin);
    MoveFileW(file->value, twin);
    shelf_model_relink(model, &file, 1);
    int twins = 0;
    for (int index = 0; index < model->count; index++) twins += wcs_equal(shelf_item_display_name(model->items[index]), L"twin.md");
    CHECK(wcs_equal(shelf_item_display_name(file), L"away.md") && twins == 1, "跟过去之后和置物架上已有的项目重复了");
    DeleteFileW(twin);
    shelf_model_relink(model, model->items, model->count);
    CHECK(!shelf_item_refresh_availability(file) && wcs_equal(shelf_item_display_name(file), L"away.md"), "已经删除的文件不应该再有路径");
    wchar_t *resolved = file_identity_resolve(&identity, path, false);
    CHECK(resolved == NULL, "已经删除的文件按标识还能找到");
    free(resolved);

    // 后台检查时也会跟上：在别处改了名，点一下置物架就换成新的名称。
    wchar_t *later = test_file(folder, L"later.txt", "z");
    add_paths(model, (const wchar_t *const *)&later, 1);
    ShelfItem *latest = item_named(model, L"later.txt");
    wchar_t *moved = path_join(inner, L"later-renamed.txt");
    MoveFileW(later, moved);
    shelf_model_refresh_availability_in_background(model);
    ULONGLONG end = GetTickCount64() + 3000;
    while (latest && !wcs_equal(latest->value, moved) && GetTickCount64() < end) pump_for(20);
    CHECK(latest && wcs_equal_ignore_case(latest->value, moved) && latest->available, "后台检查时没有跟上改名的文件");

    // 关闭后的置物架不再替项目找新位置。
    shelf_model_release(model);
    free(moved);
    free(later);
    free(twin);
    free(before);
    free(away);
    free(renamed);
    free(after);
    free(inner);
    free(sub);
    free(missing);
    free(path);
    free(folder);
}

static void test_adopt_between_shelves(void)
{
    task_initialize();
    wchar_t *folder = test_folder(L"adopt");
    wchar_t *paths[3];
    for (int index = 0; index < 3; index++)
    {
        wchar_t name[32];
        swprintf(name, 32, L"adopt-%d.txt", index + 1);
        paths[index] = test_file(folder, name, "x");
    }
    ShelfModel *source = shelf_model_new(NULL);
    add_paths(source, (const wchar_t *const *)paths, 3);
    add_entry(source, ITEM_TEXT, L"备注", NULL);
    add_entry(source, ITEM_LINK, L"https://example.com", L"示例");
    ShelfModel *target = shelf_model_new(NULL);
    add_paths(target, (const wchar_t *const *)paths, 1);

    // 缩略图跟着项目走：对方接着用，这边移除时不清掉。
    unsigned pixels[16] = {0};
    GfxImage *thumbnail = gfx_image_create(pixels, 4, 4, 16);
    shelf_item_set_thumbnail(source->items[2], thumbnail);
    source->items[2]->thumbnail_wanted = true;
    source->items[2]->child_count = 7;
    ShelfItem *moving[5];
    for (int index = 0; index < 5; index++) moving[index] = shelf_item_retain(source->items[index]);

    int added = shelf_model_adopt(target, moving, 5, source);
    CHECK(added == 4 && target->count == 5, "交过来的项目数量不对");
    CHECK(wcs_equal(target->items[1]->id, moving[1]->id) && wcs_equal(target->items[4]->id, moving[4]->id) &&
          wcs_equal(target->items[4]->title, L"示例") && target->items[2]->thumbnail == thumbnail,
        "项目没有原样交过来，或顺序不对");
    CHECK(target->has_interacted && source->count == 5, "接收之后对方应该自己移除，这边不替它移除");
    shelf_removal_free(shelf_model_remove(source, moving, 5));
    CHECK(source->count == 0 && target->count == 5 && target->items[2]->thumbnail == thumbnail, "来源移除项目时清掉了已经交出去的缩略图");
    CHECK(shelf_model_adopt(target, target->items, target->count, target) == 0 && target->count == 5, "不应该把项目交给自己");
    shelf_model_release_resources(source);
    ShelfModel *fresh = shelf_model_new(NULL);
    CHECK(shelf_model_adopt(fresh, NULL, 0, source) == 0 && shelf_model_adopt(source, target->items, target->count, target) == 0,
        "已关闭的置物架不应该再接收项目");
    for (int index = 0; index < 5; index++) shelf_item_release(moving[index]);

    // 临时附件：缓存目录由两边共同持有，来源关闭后文件还在，最后一个关闭后才清理。
    wchar_t *cache = attachment_cache_create();
    wchar_t *file = path_join(cache, L"附件.txt");
    write_bytes(file, "shared");
    ShelfModel *first = shelf_model_new(NULL);
    DropReadResult *result = paths_result((const wchar_t *const *)&file, 1);
    result->entries[0].temporary = true;
    strlist_add(&result->caches, cache);
    shelf_model_add(first, result);
    drop_result_free(result);
    ShelfModel *second = shelf_model_new(NULL);
    CHECK(shelf_model_adopt(second, first->items, first->count, first) == 1 && second->caches.count == 1, "接收临时附件时没有一起持有缓存");
    shelf_model_release_resources(first);
    pump_for(300);
    CHECK(path_exists(file), "来源置物架关闭后，交出去的临时附件被删除");
    shelf_model_release_resources(second);
    CHECK(wait_gone(cache, 8000), "两个置物架都关闭后临时附件没有清理");

    shelf_model_release(first);
    shelf_model_release(second);
    shelf_model_release(fresh);
    shelf_model_release(target);
    shelf_model_release(source);
    free(file);
    free(cache);
    for (int index = 0; index < 3; index++) free(paths[index]);
    free(folder);
}

static int history_count(void)
{
    int count;
    shelf_history_entries(&count);
    return count;
}

static void test_shelf_history(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    task_initialize();
    wchar_t *folder = test_folder(L"history");
    wchar_t *files[3];
    for (int index = 0; index < 3; index++)
    {
        wchar_t name[16];
        swprintf(name, 16, L"h%d.txt", index + 1);
        files[index] = test_file(folder, name, "x");
    }
    wchar_t *history_file = path_join(temporary_directory, L"history-test.json");
    DeleteFileW(history_file);
    shelf_history_load(history_file);
    CHECK(history_count() == 0, "没有记录时应该是空的");

    ShelfModel *model = shelf_model_new(NULL);
    add_paths(model, (const wchar_t *const *)files, 3);
    add_entry(model, ITEM_TEXT, L"secret text", NULL);
    add_entry(model, ITEM_LINK, L"https://example.com", NULL);
    WStr gone_path = {0};
    wstr_append(&gone_path, files[0]);
    wstr_append(&gone_path, L".tmp-copy");
    wchar_t *gone = wstr_take(&gone_path);
    add_paths(model, (const wchar_t *const *)&gone, 1);
    wchar_t *temporary = test_file(folder, L"attachment.txt", "x");
    DropReadResult *result = paths_result((const wchar_t *const *)&temporary, 1);
    result->entries[0].temporary = true;
    shelf_model_add(model, result);
    drop_result_free(result);

    CHECK(shelf_history_record(model->items, model->count), "没有记录关闭的置物架");
    ShelfHistoryEntry *entry = shelf_history_latest();
    CHECK(entry && entry->count == 3 && wcs_equal(entry->items[0].path, files[0]) && wcs_equal(entry->items[2].path, files[2]) &&
          wcs_equal(entry->items[0].name, L"h1.txt"), "记录的路径或名称不正确");
    shelf_history_flush();
    char *json = read_all(history_file);
    CHECK(json && !strstr(json, "secret text") && !strstr(json, "example.com") && !strstr(json, "attachment.txt") &&
          strstr(json, "\"ClosedAt\"") && strstr(json, "\"Items\"") && strstr(json, "\"Path\"") && strstr(json, "\"Name\""),
        "记录中包含了文本、链接或临时附件，或格式和 C# 版不同");
    free(json);
    CHECK(!shelf_history_record(NULL, 0), "空置物架不应记录");

    ShelfItem *reversed[16];
    for (int index = 0; index < model->count; index++) reversed[index] = model->items[model->count - 1 - index];
    shelf_history_record(reversed, model->count);
    CHECK(history_count() == 1, "相同内容的置物架重复记录");
    for (int index = 0; index < 12; index++)
    {
        wchar_t name[32];
        swprintf(name, 32, L"single-%d.txt", index);
        wchar_t *path = test_file(folder, name, "x");
        ShelfModel *single = shelf_model_new(NULL);
        add_paths(single, (const wchar_t *const *)&path, 1);
        shelf_history_record(single->items, single->count);
        shelf_model_release(single);
        free(path);
    }
    CHECK(history_count() == SHELF_HISTORY_CAPACITY && wcs_equal(shelf_history_latest()->items[0].name, L"single-11.txt"), "最多保留 10 个");
    shelf_history_flush();

    shelf_history_load(history_file);
    CHECK(history_count() == SHELF_HISTORY_CAPACITY && wcs_equal(path_file_name(shelf_history_latest()->items[0].path), L"single-11.txt"),
        "重新启动后记录丢失");
    int count;
    ShelfHistoryEntry *moved = shelf_history_entry_retain(shelf_history_entries(&count)[0]);
    shelf_history_move(moved, 3);
    shelf_history_flush();
    CHECK(shelf_history_entries(&count)[3] == moved, "调整记录顺序没有生效");
    wchar_t *moved_path = wcs_dup(moved->items[0].path);
    shelf_history_load(history_file);
    CHECK(wcs_equal(shelf_history_entries(&count)[3]->items[0].path, moved_path), "调整记录顺序没有保存");
    shelf_history_entry_release(moved);
    moved = shelf_history_entry_retain(shelf_history_entries(&count)[3]);
    shelf_history_move(moved, 0);
    shelf_history_remove(shelf_history_latest());
    CHECK(history_count() == SHELF_HISTORY_CAPACITY - 1, "恢复后记录没有移除");
    shelf_history_clear();
    shelf_history_flush();
    shelf_history_load(history_file);
    CHECK(history_count() == 0, "清除记录没有保存");
    // 拖动卡片收尾时列表可能已被清空，延迟执行的换序不应出错。
    shelf_history_move(moved, 3);
    CHECK(history_count() == 0, "对已移除的记录换序不应改动列表");
    shelf_history_entry_release(moved);
    free(moved_path);

    // C# 版写的文件：时刻带七位小数和时差，名称可以缺；格式不对的项跳过。
    write_bytes(history_file,
        "[\r\n  {\r\n    \"ClosedAt\": \"2026-11-14T09:30:15.1234567+08:00\",\r\n    \"Items\": [\r\n"
        "      { \"Path\": \"C:\\\\Docs\\\\report.pdf\", \"Name\": \"report.pdf\" },\r\n"
        "      { \"Path\": \"C:\\\\Docs\\\\plan.docx\" },\r\n      { \"Path\": \"\" }\r\n    ]\r\n  },\r\n"
        "  { \"ClosedAt\": \"2026-11-13T01:02:03Z\", \"Items\": [] },\r\n"
        "  { \"ClosedAt\": \"2026-11-12T08:00:00\", \"Items\": [ { \"Path\": \"D:\\\\a.txt\", \"Name\": \"a.txt\" } ] }\r\n]");
    shelf_history_load(history_file);
    ShelfHistoryEntry *const *loaded = shelf_history_entries(&count);
    CHECK(count == 2 && loaded[0]->count == 2 && wcs_equal(loaded[0]->items[1].name, L"plan.docx"), "没有读出 C# 版写的记录");
    // 2026-11-14 09:30:15.1234567 +08:00 = 01:30:15.1234567 UTC。
    SYSTEMTIME utc = { 2026, 11, 0, 14, 1, 30, 15, 0 };
    FILETIME expected;
    SystemTimeToFileTime(&utc, &expected);
    ULONGLONG expected_ticks = (((ULONGLONG)expected.dwHighDateTime << 32) | expected.dwLowDateTime) + 1234567;
    CHECK(count > 0 && loaded[0]->closed_at == expected_ticks, "带时差的时刻读得不对");
    CHECK(count > 0 && owned_text_is(shelf_history_entry_title(loaded[0]), L"report.pdf 等 2 项") &&
          owned_text_is(shelf_history_entry_preview(loaded[0]), L"report.pdf\r\nplan.docx"), "记录的标题或提示不对");
    language_set_mode(LANGUAGE_ENGLISH);
    CHECK(count > 0 && owned_text_is(shelf_history_entry_title(loaded[0]), L"report.pdf and 1 more"), "英文标题不对");
    CHECK(owned_text_is(str_history_date(11, 5), L"Nov 5") && owned_text_is(str_history_yesterday(L"14:05"), L"Yesterday 14:05") &&
          owned_text_is(str_restored(3, 1), L"Reopened 3 items, 1 moved or deleted") && owned_text_is(str_merged_shelves(1), L"Merged 1 item"),
        "英文的时间或提示不对");
    language_set_mode(LANGUAGE_CHINESE);
    CHECK(owned_text_is(str_history_date(11, 5), L"11月5日") && owned_text_is(str_restored(2, 0), L"已恢复 2 项"), "中文的时间或提示不对");

    // 写出去再读回来，时刻不变。
    ULONGLONG closed = loaded[0]->closed_at;
    shelf_history_move(loaded[1], 0);
    shelf_history_flush();
    shelf_history_load(history_file);
    loaded = shelf_history_entries(&count);
    CHECK(count == 2 && loaded[1]->closed_at == closed, "保存后时刻变了");
    // 今天关闭的只写时间。
    ShelfModel *today = shelf_model_new(NULL);
    add_paths(today, (const wchar_t *const *)files, 1);
    shelf_history_record(today->items, today->count);
    wchar_t *time = shelf_history_entry_time(shelf_history_latest());
    CHECK(time && wcslen(time) == 5 && time[2] == L':', "今天关闭的应该只写时间");
    free(time);
    shelf_model_release(today);

    shelf_history_clear();
    shelf_history_flush();
    shelf_model_release(model);
    free(temporary);
    free(gone);
    free(history_file);
    for (int index = 0; index < 3; index++) free(files[index]);
    free(folder);
}

static bool wait_count(ShelfModel *model, int count, int milliseconds)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)milliseconds;
    while (model->count != count && GetTickCount64() < end) pump_for(20);
    return model->count == count;
}

static void test_merge_and_split(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *history_file = path_join(temporary_directory, L"history-merge.json");
    DeleteFileW(history_file);
    shelf_history_load(history_file);
    wchar_t *folder = test_folder(L"merge");
    wchar_t *paths[3];
    for (int index = 0; index < 3; index++)
    {
        wchar_t name[16];
        swprintf(name, 16, L"m%d.txt", index + 1);
        paths[index] = test_file(folder, name, "x");
    }

    // 测试里没有读设置，开关都是关着的：这里按默认值打开“自动关闭空置物架”。
    AppSettings *settings = (AppSettings *)app_settings();
    bool close_when_empty = settings->close_when_empty;
    settings->close_when_empty = true;
    ShelfWindow *source = shelves_open_at((POINT){ 400, 300 }, false, false, EDGE_NONE);
    CHECK(source != NULL, "没有打开置物架");
    if (source)
    {
        shelf_window_retain(source);
        add_paths(source->model, (const wchar_t *const *)paths, 3);
        pump_for(200);

        // 拆分：放在来源下边（放不下时在上边），宽度是收起时的宽度。
        SplitPlace place;
        CHECK(shelf_split_place(source, &place), "算不出新置物架的位置");
        RECT rect;
        GetWindowRect(source->handle, &rect);
        float width, height;
        shelf_target_size(source, false, &width, &height);
        CHECK(place.rect.right - place.rect.left == (int)lroundf(width * shelf_scale(source)) &&
              (place.below ? place.rect.top > rect.bottom : place.rect.bottom < rect.top), "新置物架的位置不对");

        int before = shelves_open_count();
        ShelfItem *split = shelf_item_retain(source->model->items[0]);
        shelf_split_off_from_menu(source, &split, 1);
        CHECK(shelves_open_count() == before + 1, "没有拆出新的置物架");
        ShelfWindow *other = shelves_first();
        CHECK(other && other != source && other->model->count == 1 && wcs_equal(other->model->items[0]->value, paths[0]),
            "拆出的置物架里内容不对");
        CHECK(wait_count(source->model, 2, 3000) && !source->closing, "拆出去的项目没有从原来的置物架移除");
        shelf_item_release(split);

        // 已经有的不重复放，并提示。
        if (other)
        {
            ShelfItem *again = other->model->items[0];
            CHECK(shelf_receive(other, &again, 1, source->model, 20, 20) == 0 && other->toast_text &&
                  wcs_equal(other->toast_text, S(STR_AlreadyOnShelf)), "重复的项目没有提示");

            // 合并：全部交过去，这边直接关闭，不记进最近关闭。
            int count = source->model->count;
            ShelfItem **all = xalloc((size_t)count * sizeof(ShelfItem *));
            for (int index = 0; index < count; index++) all[index] = shelf_item_retain(source->model->items[index]);
            CHECK(shelf_receive(other, all, count, source->model, 40, 40) == 2 && other->model->count == 3, "合并后的数量不对");
            shelf_let_go(source, all, count);
            CHECK(source->merged_away && source->closing, "全部交出去后应该关闭");
            ULONGLONG end = GetTickCount64() + 3000;
            while (!source->closed && GetTickCount64() < end) pump_for(20);
            CHECK(source->closed && history_count() == 0, "合并过去的置物架不应记进最近关闭");
            for (int index = 0; index < count; index++) shelf_item_release(all[index]);
            free(all);

            // 关闭时仍有文件的置物架记入最近关闭；恢复时放进新的置物架，记录随之移除。
            shelf_window_close_now(other);
            pump_for(100);
            CHECK(history_count() == 1 && shelf_history_latest()->count == 3, "关闭的置物架没有记入最近关闭");
            DeleteFileW(paths[2]);
            shelves_restore(shelf_history_latest(), NULL);
            ShelfWindow *restored = shelves_first();
            CHECK(restored && history_count() == 0, "恢复后记录没有移除");
            if (restored)
            {
                CHECK(wait_count(restored->model, 2, 3000) && restored->toast_text && wcsstr(restored->toast_text, L"已恢复 2 项") &&
                      wcsstr(restored->toast_text, L"1 项已删除或移动"), "恢复的项目或提示不对");
            }
        }
        shelf_window_release(source);
    }

    // 带子菜单的菜单能打开和关闭。
    UiMenuItem children[] = { { .id = 11, .text = L"子项", .tooltip = L"提示", .max_width = 260 }, { .separator = true }, { .id = 12, .text = L"清除" } };
    UiMenuItem items[] = { { .id = 1, .text = L"一项" }, { .id = 2, .text = L"子菜单", .children = children, .child_count = 3 } };
    ui_menu_show(items, 2, 300, 300, NULL, NULL);
    CHECK(ui_menu_is_open(), "带子菜单的菜单没有打开");
    ui_menu_dismiss();
    pump_for(50);
    CHECK(!ui_menu_is_open(), "菜单没有关闭");

    shelves_close_all();
    pump_for(100);
    settings->close_when_empty = close_when_empty;
    shelf_history_clear();
    shelf_history_flush();
    free(history_file);
    for (int index = 0; index < 3; index++) free(paths[index]);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
}
