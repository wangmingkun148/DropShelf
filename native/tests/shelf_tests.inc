// 第 4 步（置物架窗口）的测试，对应 C# 版 ShelfTests 和 DropDataTests 里的各项。由 tests.c 引入。

// ───────────── 辅助 ─────────────

/// 处理消息直到 *flag 变为 true 或超时：后台任务做完要经消息回到这个线程。
static void pump_until(volatile bool *flag, int milliseconds)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)milliseconds;
    MSG message;
    while (!*flag && GetTickCount64() < end)
    {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!*flag) MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
}

static void pump_for(int milliseconds)
{
    volatile bool never = false;
    pump_until(&never, milliseconds);
}

/// 处理消息直到 done() 成立或超时，返回 done() 是否成立。机器忙时动画会慢一些：等的是结果，不是固定的时间。
static bool pump_until_done(bool (*done)(void), int milliseconds)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)milliseconds;
    MSG message;
    while (!done() && GetTickCount64() < end)
    {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!done()) MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
    return done();
}

static bool no_shelves_open(void)
{
    return shelves_open_count() == 0;
}

/// 等到文件或文件夹不在了（后台删除）；超时返回 false。
static bool wait_gone(const wchar_t *path, int milliseconds)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)milliseconds;
    while (path_exists(path))
    {
        if (GetTickCount64() > end) return false;
        pump_for(20);
    }
    return true;
}

static wchar_t *test_folder(const wchar_t *name)
{
    wchar_t *folder = path_join(temporary_directory, name);
    directory_create(folder);
    return folder;
}

static wchar_t *test_file(const wchar_t *folder, const wchar_t *name, const char *content)
{
    wchar_t *path = path_join(folder, name);
    write_bytes(path, content);
    return path;
}

typedef struct ReadWait
{
    volatile bool done;
    DropReadResult *result;
} ReadWait;

static void on_read(DropReadResult *result, void *context)
{
    ReadWait *wait = context;
    wait->result = result;
    wait->done = true;
}

static DropReadResult *read_data(IDataObject *data, bool files_only)
{
    ReadWait wait = {0};
    drop_read(data, files_only, true, NULL, on_read, &wait);
    pump_until(&wait.done, 5000);
    return wait.result;
}

static void set_file_list(IDataObject *data, const wchar_t *const *paths, int count)
{
    WStr list = {0};
    for (int index = 0; index < count; index++)
    {
        wstr_append(&list, paths[index]);
        wstr_append_char(&list, 0);
    }
    wstr_append_char(&list, 0);
    size_t size = sizeof(DROPFILES) + list.length * sizeof(wchar_t);
    unsigned char *bytes = xcalloc(1, size);
    ((DROPFILES *)bytes)->pFiles = sizeof(DROPFILES);
    ((DROPFILES *)bytes)->fWide = TRUE;
    memcpy(bytes + sizeof(DROPFILES), list.data, list.length * sizeof(wchar_t));
    set_bytes(data, NULL, CF_HDROP, bytes, size);
    free(bytes);
    wstr_free(&list);
}

/// 一份只有文件路径的读取结果（不带后台读好的文件信息，放入时当场读）。
static DropReadResult *paths_result(const wchar_t *const *paths, int count)
{
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc((size_t)count, sizeof(DroppedEntry));
    for (int index = 0; index < count; index++) result->entries[index] = (DroppedEntry){ ITEM_FILE, wcs_dup(paths[index]) };
    result->count = count;
    return result;
}

static ShelfAddSummary add_paths(ShelfModel *model, const wchar_t *const *paths, int count)
{
    DropReadResult *result = paths_result(paths, count);
    ShelfAddSummary summary = shelf_model_add(model, result);
    drop_result_free(result);
    return summary;
}

static ShelfAddSummary add_entry(ShelfModel *model, ShelfItemKind kind, const wchar_t *value, const wchar_t *title)
{
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->entries = xcalloc(1, sizeof(DroppedEntry));
    result->entries[0] = (DroppedEntry){ kind, wcs_dup(value), wcs_dup(title) };
    result->count = 1;
    ShelfAddSummary summary = shelf_model_add(model, result);
    drop_result_free(result);
    return summary;
}

static bool owned_text_is(wchar_t *text, const wchar_t *expected)
{
    bool same = text && wcscmp(text, expected) == 0;
    if (!same) wprintf(L"      实际：%ls\n", text ? text : L"(null)");
    free(text);
    return same;
}

// ───────────── 项目的文字 ─────────────

static void test_shelf_item_text(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    shelf_items_language_changed();
    CHECK(owned_text_is(format_size(0), L"0 B") && owned_text_is(format_size(1023), L"1023 B") && owned_text_is(format_size(1024), L"1 KB"), "大小的写法不正确");
    CHECK(owned_text_is(format_size(1536), L"1.5 KB") && owned_text_is(format_size(5505024), L"5.3 MB") && owned_text_is(format_size(3LL << 30), L"3 GB"), "大小的写法不正确");
    CHECK(owned_text_is(text_summarize(L"  第一行  内容\n第二行", 60), L"第一行 内容"), "文本摘要没有只取第一行、合并空白");
    CHECK(owned_text_is(text_summarize(L"\n\n  abcdef", 3), L"abc…"), "长文本没有截断");
    CHECK(text_is_single_url(L"https://example.com/page") && text_is_single_url(L"HTTP://Example.com") && !text_is_single_url(L"file:///C:/x") &&
          !text_is_single_url(L"https://a b") && !text_is_single_url(L"ftp://example.com") && !text_is_single_url(L"https://a\nb"), "网址判断不正确");
    CHECK(owned_text_is(url_host(L"https://user@Example.COM:8080/x?y"), L"example.com") && url_host(L"mailto:a@b") == NULL, "主机名不正确");
    CHECK(wcscmp(path_extension(L"C:\\a\\b.txt"), L".txt") == 0 && wcscmp(path_extension(L"a."), L"") == 0 &&
          wcscmp(path_extension(L"C:\\dir.x\\file"), L"") == 0, "扩展名不正确");

    ShelfItem *file = shelf_item_new(ITEM_FILE, L"C:\\docs\\报告.pdf", NULL, false);
    file->has_size = true;
    file->size_bytes = 2048;
    free(file->type_name);
    file->type_name = wcs_dup(L"PDF 文件");
    CHECK(wcscmp(shelf_item_display_name(file), L"报告.pdf") == 0 && wcscmp(shelf_item_detail(file), L"2 KB · PDF 文件") == 0, "文件的名称或说明不正确");
    file->available = false;
    shelf_item_text_changed(file);
    CHECK(wcscmp(shelf_item_detail(file), L"已删除或移动") == 0, "不在了的文件说明不正确");
    shelf_item_release(file);

    ShelfItem *folder = shelf_item_new(ITEM_FOLDER, L"D:\\资料\\", NULL, false);
    CHECK(wcscmp(shelf_item_display_name(folder), L"资料") == 0 && wcscmp(shelf_item_detail(folder), L"文件夹") == 0, "文件夹的名称或说明不正确");
    folder->child_count = 3;
    folder->has_size = true;
    folder->size_bytes = 3072;
    shelf_item_text_changed(folder);
    CHECK(wcscmp(shelf_item_detail(folder), L"3 项 · 3 KB") == 0, "文件夹的说明不正确");
    folder->has_size = false;
    shelf_item_text_changed(folder);
    CHECK(wcscmp(shelf_item_detail(folder), L"3 项") == 0, "不知道大小时文件夹的说明不正确");
    folder->child_count = 0;
    shelf_item_text_changed(folder);
    CHECK(wcscmp(shelf_item_detail(folder), L"空文件夹") == 0, "空文件夹的说明不正确");
    shelf_item_release(folder);
    ShelfItem *root = shelf_item_new(ITEM_FOLDER, L"C:\\", NULL, false);
    CHECK(wcscmp(shelf_item_display_name(root), L"C:\\") == 0, "根目录的名称不正确");
    shelf_item_release(root);

    ShelfItem *link = shelf_item_new(ITEM_LINK, L"https://Example.com/a/b/", NULL, false);
    CHECK(wcscmp(shelf_item_display_name(link), L"example.com/a/b") == 0 && wcscmp(shelf_item_detail(link), L"example.com") == 0, "链接的名称或说明不正确");
    shelf_item_release(link);
    ShelfItem *text = shelf_item_new(ITEM_TEXT, L"hello", NULL, false);
    CHECK(wcscmp(shelf_item_detail(text), L"文本 · 5 个字符") == 0 && wcscmp(shelf_item_glyph(text), L"\xE8D2") == 0, "文本的说明或图标不正确");
    language_set_mode(LANGUAGE_ENGLISH);
    shelf_items_language_changed();
    CHECK(wcscmp(shelf_item_detail(text), L"Text · 5 characters") == 0, "切换语言后说明没有跟着变");
    language_set_mode(LANGUAGE_CHINESE);
    shelf_items_language_changed();
    shelf_item_release(text);

    // 文本和链接写成文件时的文件名。
    ShelfItem *notes = shelf_item_new(ITEM_TEXT, L"会议纪要：a/b?\n第二行", NULL, false);
    ShelfItem *titled = shelf_item_new(ITEM_LINK, L"https://example.com/路径?q=1", L"示例: 首页", false);
    ShelfItem *bare = shelf_item_new(ITEM_LINK, L"https://example.com/a", NULL, false);
    ShelfItem *odd = shelf_item_new(ITEM_TEXT, L"???", NULL, false);
    ShelfItem *reserved = shelf_item_new(ITEM_TEXT, L"con", NULL, false);
    CHECK(owned_text_is(shelf_item_file_name(notes), L"会议纪要：a b.txt"), "文本的文件名不正确");
    CHECK(owned_text_is(shelf_item_file_name(titled), L"示例 首页.url") && owned_text_is(shelf_item_file_name(bare), L"example.com.url"), "链接的文件名不正确");
    CHECK(owned_text_is(shelf_item_file_name(odd), L"文本.txt") && owned_text_is(shelf_item_file_name(reserved), L"_con.txt"), "无法使用的文件名没有换掉");
    shelf_item_release(notes);
    shelf_item_release(titled);
    shelf_item_release(bare);
    shelf_item_release(odd);
    shelf_item_release(reserved);
}

// ───────────── 置物架的内容 ─────────────

static int count_of(const ShelfModel *model, ShelfItemKind kind)
{
    int count = 0;
    for (int index = 0; index < model->count; index++) count += model->items[index]->kind == kind;
    return count;
}

static void test_shelf_model(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    shelf_items_language_changed();
    task_initialize();
    wchar_t *folder = test_folder(L"model");
    wchar_t *paths[100];
    for (int index = 0; index < 100; index++)
    {
        wchar_t name[32];
        swprintf(name, 32, L"sample-%03d.txt", index + 1);
        paths[index] = test_file(folder, name, "x");
    }

    // 一百个文件、重复和找不到的文件。
    ShelfModel *model = shelf_model_new(NULL);
    ShelfAddSummary first = add_paths(model, (const wchar_t *const *)paths, 100);
    ShelfAddSummary second = add_paths(model, (const wchar_t *const *)paths, 100);
    CHECK(model->count == 100 && first.added == 100 && second.duplicates == 100, "添加结果统计不正确");
    CHECK(owned_text_is(shelf_add_summary_message(second), L"这些项目已在置物架中") && owned_text_is(shelf_model_stack_caption(model), L"100 个文件"), "重复时的提示或汇总不正确");
    wchar_t *missing = path_join(folder, L"missing.txt");
    ShelfAddSummary gone = add_paths(model, (const wchar_t *const *)&missing, 1);
    CHECK(gone.unavailable == 1 && model->count == 100 && owned_text_is(shelf_add_summary_message(gone), L"项目无法访问，未能添加"), "不存在的文件被添加");
    CHECK(owned_text_is(shelf_add_summary_message((ShelfAddSummary){ 3, 1, 2 }), L"已添加 3 项，忽略 1 个重复项，2 项无法访问"), "添加结果的说明不正确");
    ShelfItem *sixty[60];
    memcpy(sixty, model->items, sizeof(sixty));
    shelf_removal_free(shelf_model_remove(model, sixty, 60));
    CHECK(model->count == 40 && owned_text_is(shelf_model_stack_caption(model), L"40 个文件") && owned_text_is(shelf_model_title(model), L"40 个项目"), "批量移除后汇总不正确");
    CHECK(path_exists(paths[0]), "移除置物架上的项目删除了磁盘文件");
    shelf_model_release(model);

    // 移除和撤销：按原来的位置放回，已经重新放上来的不重复放。
    model = shelf_model_new(NULL);
    add_paths(model, (const wchar_t *const *)paths, 5);
    ShelfItem *order[5];
    memcpy(order, model->items, sizeof(order));
    ShelfItem *pick[2] = { order[3], order[1] };
    ShelfRemoval *removal = shelf_model_remove(model, pick, 2);
    CHECK(removal && removal->count == 2 && removal->indices[0] == 1 && removal->indices[1] == 3 && model->count == 3, "移除记录里的位置不是从前到后");
    CHECK(shelf_model_restore(model, removal, NULL) == 2 && memcmp(model->items, order, sizeof(order)) == 0, "撤销后项目没有回到原来的位置");
    shelf_removal_free(removal);
    ShelfItem *again = order[0];
    removal = shelf_model_remove(model, &again, 1);
    CHECK(removal && !shelf_model_remove(model, &again, 1), "移除不在置物架上的项目也返回了记录");
    shelf_model_restore(model, removal, NULL);
    shelf_removal_free(removal);
    ShelfItem *all[5];
    memcpy(all, model->items, sizeof(all));
    removal = shelf_model_remove(model, all, 5);
    add_paths(model, (const wchar_t *const *)paths, 1);
    CHECK(shelf_model_restore(model, removal, NULL) == 4 && model->count == 5, "撤销时重复放回了已经在置物架上的文件");
    bool in_order = true;
    for (int index = 0; index < 5; index++) in_order &= wcs_equal_ignore_case(model->items[index]->value, paths[index]);
    CHECK(in_order, "清空后撤销的顺序不对");
    shelf_removal_free(removal);
    shelf_model_release(model);

    // 文本和链接：相同的不重复放，链接的标题留着。
    model = shelf_model_new(NULL);
    ShelfAddSummary text = add_entry(model, ITEM_TEXT, L"一段文本", NULL);
    add_entry(model, ITEM_LINK, L"https://example.com", L"示例");
    ShelfAddSummary twice = add_entry(model, ITEM_TEXT, L"一段文本", NULL);
    CHECK(text.added == 1 && twice.duplicates == 1 && model->count == 2 && wcs_equal(model->items[1]->title, L"示例"), "文本或链接去重不正确");

    // 和文件一起拖出时替文本写好文件；转成文件时位置不变；关闭后清理。
    StrList carried;
    shelf_model_write_for_drag(model, model->items, model->count, &carried);
    CHECK(carried.count == 2 && path_exists(carried.items[0]) && path_exists(carried.items[1]) && count_of(model, ITEM_TEXT) == 1,
        "和文件一起拖出时没有替文本写好文件，或改动了置物架上的项目");
    StrList reused;
    shelf_model_write_for_drag(model, model->items, model->count, &reused);
    CHECK(strlist_equal(&carried, &reused, false), "再次拖出时没有接着用写好的文件");
    strlist_clear(&reused);
    wchar_t *ids[2] = { wcs_dup(model->items[0]->id), wcs_dup(model->items[1]->id) };
    CHECK(shelf_model_convert_to_files(model, model->items, model->count) == 2, "文本和链接没有转成文件");
    CHECK(count_of(model, ITEM_FILE) == 2 && model->items[0]->temporary && wcs_equal(model->items[0]->id, ids[0]) && wcs_equal(model->items[1]->id, ids[1]) &&
          wcscmp(path_extension(model->items[1]->value), L".url") == 0, "转成文件后位置或类型不对");
    wchar_t *shortcut_text = file_read_text(model->items[1]->value, 4096);
    CHECK(shortcut_text && wcsstr(shortcut_text, L"[InternetShortcut]\r\nURL=https://example.com\r\n"), "快捷方式的内容不正确");
    free(shortcut_text);
    wchar_t *cache = wcs_dup(model->caches.items[0]);
    shelf_model_release(model);
    CHECK(wait_gone(cache, 8000), "关闭置物架后没有清理文本转成的文件");
    free(cache);
    free(ids[0]);
    free(ids[1]);
    strlist_clear(&carried);

    // 中文的快捷方式地址按 UTF-8 转义；同名时不覆盖。
    model = shelf_model_new(NULL);
    add_entry(model, ITEM_LINK, L"https://example.com/路径?q=1", L"示例: 首页");
    add_entry(model, ITEM_TEXT, L"会议纪要：a/b?\n第二行", NULL);
    StrList written;
    shelf_model_write_for_drag(model, model->items, model->count, &written);
    wchar_t *content = written.count == 2 ? file_read_text(written.items[0], 4096) : NULL;
    CHECK(content && wcsstr(content, L"URL=https://example.com/%E8%B7%AF%E5%BE%84?q=1"), "快捷方式里的中文没有转义");
    free(content);
    content = written.count == 2 ? file_read_text(written.items[1], 4096) : NULL;
    CHECK(content && wcscmp(content, L"会议纪要：a/b?\n第二行") == 0 && wcscmp(path_file_name(written.items[1]), L"会议纪要：a b.txt") == 0, "文本文件的内容或名称不正确");
    free(content);
    strlist_clear(&written);
    shelf_model_release(model);

    // 堆叠下面的汇总。
    wchar_t *summary_folder = test_folder(L"stack-summary");
    wchar_t *a = path_join(summary_folder, L"a.txt"), *b = path_join(summary_folder, L"b.txt");
    char kilobytes[2049];
    memset(kilobytes, 'x', sizeof(kilobytes));
    kilobytes[1024] = 0;
    write_bytes(a, kilobytes);
    kilobytes[1024] = 'x';
    kilobytes[2048] = 0;
    write_bytes(b, kilobytes);
    model = shelf_model_new(NULL);
    add_paths(model, (const wchar_t *const *)&a, 1);
    // 第二行是“大小 · 类型”，类型名称由 Windows 给出，各台电脑不一样，只看开头。
    wchar_t *single_detail = shelf_model_stack_detail(model);
    CHECK(owned_text_is(shelf_model_stack_caption(model), L"a.txt") && single_detail && wcsncmp(single_detail, L"1 KB", 4) == 0, "单个项目的堆叠说明不正确");
    free(single_detail);
    const wchar_t *more[2] = { b, summary_folder };
    add_paths(model, more, 2);
    CHECK(shelf_model_from_end(model, 1) == model->items[2] && shelf_model_from_end(model, 3) == model->items[0] && !shelf_model_from_end(model, 4),
        "堆叠最上面不是最新项目");
    CHECK(owned_text_is(shelf_model_stack_caption(model), L"2 个文件 · 1 个文件夹") && owned_text_is(shelf_model_stack_detail(model), L"3 KB") &&
          owned_text_is(shelf_model_title(model), L"3 个项目"), "汇总不正确");
    shelf_model_release(model);

    // 拖出之后：取消、复制时按设置决定、移走了就移除。
    model = shelf_model_new(NULL);
    add_paths(model, (const wchar_t *const *)paths, 3);
    ShelfItem *head = model->items[0];
    shelf_model_apply_drag_out(model, &head, 1, DRAG_OUT_CANCELLED, true);
    shelf_model_apply_drag_out(model, &head, 1, DRAG_OUT_COPIED, false);
    CHECK(model->count == 3, "取消拖动或设置为保留时移除了项目");
    shelf_model_apply_drag_out(model, &head, 1, DRAG_OUT_COPIED, true);
    CHECK(model->count == 2 && path_exists(paths[0]), "复制拖出后没有移除项目，或删除了磁盘文件");
    ShelfItem *moved = model->items[0];
    wchar_t *moved_to = path_join(folder, L"moved-away.txt");
    MoveFileW(moved->value, moved_to);
    shelf_model_apply_drag_out(model, &moved, 1, DRAG_OUT_MOVED, false);
    CHECK(model->count == 1 && !shelf_model_contains(model, moved), "源文件已移动但项目仍留在置物架");
    shelf_model_release(model);
    free(moved_to);

    // 文件夹的统计。
    wchar_t *measured = test_folder(L"folder-contents");
    wchar_t *inner = path_join(measured, L"inner");
    directory_create(inner);
    wchar_t *empty = path_join(measured, L"empty");
    directory_create(empty);
    wchar_t *one = test_file(measured, L"a.bin", "1234");
    wchar_t *two = test_file(inner, L"b.bin", "12345678");
    int children;
    bool has_size;
    long long size;
    CHECK(folder_measure(measured, &children, &has_size, &size, NULL) && children == 3 && has_size && size == 12, "文件夹的项目数或大小不正确");
    CHECK(folder_measure(empty, &children, &has_size, &size, NULL) && children == 0 && has_size && size == 0, "空文件夹的统计不正确");
    wchar_t *nowhere = path_join(measured, L"missing");
    CHECK(!folder_measure(nowhere, &children, &has_size, &size, NULL), "不存在的文件夹没有返回失败");

    // 放进来的文件夹在后台统计，统计完说明跟着变。
    model = shelf_model_new(NULL);
    add_paths(model, (const wchar_t *const *)&measured, 1);
    ShelfItem *counted = model->items[0];
    ULONGLONG until = GetTickCount64() + 3000;
    while (counted->child_count < 0 && GetTickCount64() < until) pump_for(20);
    CHECK(counted->kind == ITEM_FOLDER && counted->child_count == 3 && wcscmp(shelf_item_detail(counted), L"3 项 · 12 B") == 0, "放进来的文件夹没有统计");
    shelf_model_release(model);

    free(nowhere);
    free(one);
    free(two);
    free(inner);
    free(empty);
    free(measured);
    free(a);
    free(b);
    free(summary_folder);
    free(missing);
    for (int index = 0; index < 100; index++) free(paths[index]);
    free(folder);
}

// ───────────── 拖出的规则 ─────────────

static void test_drag_out_rules(void)
{
    CHECK(drag_resolve(DRAGDROP_S_CANCEL, 0, -1, -1, false) == DRAG_OUT_CANCELLED, "取消没有识别");
    CHECK(drag_resolve(DRAGDROP_S_DROP, 1, -1, -1, false) == DRAG_OUT_COPIED, "复制没有识别");
    CHECK(drag_resolve(DRAGDROP_S_DROP, 2, -1, -1, false) == DRAG_OUT_MOVED, "移动没有识别");
    CHECK(drag_resolve(DRAGDROP_S_DROP, 4, -1, -1, false) == DRAG_OUT_LINKED, "创建快捷方式没有识别");
    CHECK(drag_resolve(DRAGDROP_S_DROP, 0, 0, 2, false) == DRAG_OUT_MOVED, "资源管理器的优化移动没有识别");
    CHECK(drag_resolve(DRAGDROP_S_DROP, 0, -1, -1, true) == DRAG_OUT_MOVED, "源文件消失时没有判定为移动");
    CHECK(drag_resolve(DRAGDROP_S_DROP, 0, -1, -1, false) == DRAG_OUT_CANCELLED, "目标拒绝放下时被误判为成功");
    CHECK(drag_allowed_effects(true, false) == 7 && drag_allowed_effects(false, false) == 5, "移动模式应同时允许复制，复制模式不应允许移动");
    CHECK(drag_allowed_effects(true, true) == 3 && drag_allowed_effects(false, true) == 1, "临时附件不应提供快捷方式");
}

// ───────────── 临时附件的缓存 ─────────────

static void test_attachment_cache(void)
{
    task_initialize();
    StrList used = {0};
    CHECK(owned_text_is(attachment_safe_name(L"a/b\\c.txt", &used), L"c.txt"), "名称里的路径没有去掉");
    CHECK(owned_text_is(attachment_safe_name(L"  bad:name?.txt ", &used), L"bad_name_.txt"), "不能用的字符没有换掉");
    CHECK(owned_text_is(attachment_safe_name(L"con.txt", &used), L"_con.txt") && owned_text_is(attachment_safe_name(L"...", &used), L"attachment"), "保留名或空名称没有换掉");
    CHECK(owned_text_is(attachment_safe_name(L"C.TXT", &used), L"C (2).TXT") && owned_text_is(attachment_safe_name(L"c.txt", &used), L"c (3).txt"), "同名时没有加序号");
    wchar_t longer[220];
    for (int index = 0; index < 200; index++) longer[index] = L'a';
    wcscpy_s(longer + 200, 20, L".docx");
    wchar_t *shortened = attachment_safe_name(longer, &used);
    CHECK(wcslen(shortened) == 140 && wcscmp(path_extension(shortened), L".docx") == 0, "太长的名称没有截短");
    free(shortened);
    strlist_clear(&used);

    wchar_t *cache = attachment_cache_create();
    CHECK(cache && path_is_directory(cache) && attachment_cache_is_owned(cache) && !attachment_cache_is_owned(attachment_cache_root()), "缓存文件夹没有建好");
    wchar_t *file = path_join(cache, L"附件.txt");
    write_bytes(file, "shared");
    wchar_t *owner = attachment_cache_directory_of(file);
    CHECK(owner && wcs_equal_ignore_case(owner, cache) && !attachment_cache_directory_of(L"C:\\Windows\\notepad.exe"), "找不到文件所在的缓存");
    free(owner);
    attachment_cache_retain(cache);
    attachment_cache_release(cache);
    pump_for(300);
    CHECK(path_exists(file), "还有人持有时缓存被删除");
    attachment_cache_release(cache);
    CHECK(wait_gone(cache, 8000), "最后一个持有者释放后缓存没有清理");

    // 释放后、后台删除执行前又被持有：不能删除。
    wchar_t *raced = attachment_cache_create();
    wchar_t *keep = path_join(raced, L"keep.txt");
    write_bytes(keep, "keep");
    attachment_cache_release(raced);
    attachment_cache_retain(raced);
    pump_for(300);
    CHECK(path_exists(keep), "重新持有后缓存仍被删除");
    attachment_cache_release(raced);
    CHECK(wait_gone(raced, 8000), "释放后缓存没有清理");

    // 别的程序暂时占着里面的文件：先删不掉，放开以后过一会儿再试时删掉。
    wchar_t *busy = attachment_cache_create();
    wchar_t *opened = path_join(busy, L"busy.txt");
    write_bytes(opened, "busy");
    HANDLE holder = CreateFileW(opened, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(holder != INVALID_HANDLE_VALUE, "打不开缓存里的文件");
    attachment_cache_release(busy);
    pump_for(300);
    CHECK(path_exists(opened), "文件被占着时缓存却删掉了");
    if (holder != INVALID_HANDLE_VALUE) CloseHandle(holder);
    CHECK(wait_gone(busy, 8000), "文件放开以后没有再试着清理缓存");
    free(opened);
    free(busy);
    free(keep);
    free(raced);
    free(file);
    free(cache);
}

// ───────────── 读取拖入的内容 ─────────────

static bool starts_with_png(const wchar_t *path)
{
    unsigned char header[4] = {0};
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD read = 0;
    if (file != INVALID_HANDLE_VALUE)
    {
        ReadFile(file, header, 4, &read, NULL);
        CloseHandle(file);
    }
    return read == 4 && memcmp(header, "\x89PNG", 4) == 0;
}

static void test_drop_read(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *folder = test_folder(L"read-drop");
    wchar_t *file = test_file(folder, L"a.txt", "x");

    // 文件和文件夹。
    IDataObject *data = new_data();
    const wchar_t *both[2] = { file, folder };
    set_file_list(data, both, 2);
    DropReadResult *read = read_data(data, false);
    CHECK(read && read->count == 2 && read->entries[0].kind == ITEM_FILE && read->entries[1].kind == ITEM_FOLDER && drop_result_file_count(read) == 2,
        "文件读取不正确");
    CHECK(read && read->entries[0].has_facts && read->entries[0].facts.size_bytes == 1 && read->entries[0].type_name, "没有在后台读好文件信息");
    ShelfModel *model = shelf_model_new(NULL);
    if (read) shelf_model_add(model, read);
    CHECK(model->count == 2 && model->items[1]->kind == ITEM_FOLDER && model->items[0]->size_bytes == 1, "读好的文件没有放进置物架");
    shelf_model_release(model);
    drop_result_free(read);
    IDataObject_Release(data);

    // 网址文本、带标题的链接、多行文本，只要文件时不接受文本。
    data = new_data();
    set_unicode(data, NULL, CF_UNICODETEXT, L"  https://example.com/page  ");
    read = read_data(data, false);
    CHECK(read && read->count == 1 && read->entries[0].kind == ITEM_LINK && wcscmp(read->entries[0].value, L"https://example.com/page") == 0,
        "网址文本没有识别为链接");
    drop_result_free(read);
    IDataObject_Release(data);
    data = new_data();
    set_unicode(data, L"text/x-moz-url", 0, L"https://example.org\n示例网站");
    read = read_data(data, false);
    CHECK(read && read->count == 1 && wcs_equal(read->entries[0].title, L"示例网站"), "链接标题没有读取");
    drop_result_free(read);
    IDataObject_Release(data);
    data = new_data();
    set_unicode(data, NULL, CF_UNICODETEXT, L"第一行\n第二行");
    read = read_data(data, false);
    CHECK(read && read->count == 1 && read->entries[0].kind == ITEM_TEXT, "多行文本识别不正确");
    drop_result_free(read);
    read = read_data(data, true);
    CHECK(read && read->count == 0 && wcs_equal(read->error, L"这个操作只支持文件或文件夹"), "快速操作错误地接受了文本");
    drop_result_free(read);
    IDataObject_Release(data);
    data = new_data();
    set_unicode(data, L"UniformResourceLocatorW", 0, L"file:///C:/Windows/System32/calc.exe");
    read = read_data(data, false);
    CHECK(read && read->count == 1 && read->entries[0].kind == ITEM_TEXT, "非网页链接没有按文本保存");
    drop_result_free(read);
    IDataObject_Release(data);

    // 编码好的图片直接存下来；只有位图时存成 PNG。
    data = new_data();
    set_bytes(data, L"PNG", 0, "\x89PNG\r\n\x1a\n", 8);
    set_unicode(data, L"UniformResourceLocatorW", 0, L"https://example.com/a.png");
    read = read_data(data, true);
    CHECK(read && read->count == 1 && read->entries[0].temporary && starts_with_png(read->entries[0].value) &&
          wcscmp(path_extension(read->entries[0].value), L".png") == 0, "图片没有保存为临时文件");
    if (read && read->count == 1)
    {
        wchar_t *saved = wcs_dup(read->entries[0].value);
        drop_result_release_caches(read);
        CHECK(wait_gone(saved, 8000), "图片临时目录没有清理");
        free(saved);
    }
    drop_result_free(read);
    IDataObject_Release(data);
    struct { BITMAPINFOHEADER header; DWORD pixels[4]; } dib = { { sizeof(BITMAPINFOHEADER), 2, 2, 1, 32, BI_RGB }, { 0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF } };
    data = new_data();
    set_bytes(data, NULL, CF_DIB, &dib, sizeof(dib));
    read = read_data(data, false);
    CHECK(read && read->count == 1 && starts_with_png(read->entries[0].value), "位图没有存成 PNG");
    if (read) drop_result_release_caches(read);
    drop_result_free(read);
    IDataObject_Release(data);

    // 未落盘的附件。
    data = new_data();
    set_descriptor(data, L"chat-attachment.txt");
    set_bytes(data, CFSTR_FILECONTENTS, 0, "virtual attachment content", 26);
    read = read_data(data, false);
    CHECK(read && read->count == 1 && read->failed == 0 && read->entries[0].temporary, read && read->error ? "虚拟附件没有成功接收" : "虚拟附件没有成功接收");
    if (read && read->count == 1)
    {
        wchar_t *content = file_read_text(read->entries[0].value, 1024);
        CHECK(wcscmp(path_file_name(read->entries[0].value), L"chat-attachment.txt") == 0 && content && wcscmp(content, L"virtual attachment content") == 0,
            "虚拟附件内容不正确");
        free(content);
        model = shelf_model_new(NULL);
        wchar_t *cache = wcs_dup(read->caches.items[0]);
        shelf_model_add(model, read);
        CHECK(model->count == 1 && model->items[0]->temporary && model->caches.count == 1, "临时附件没有登记");
        shelf_model_release(model);
        CHECK(wait_gone(cache, 8000), "关闭置物架后临时附件没有清理");
        free(cache);
    }
    drop_result_free(read);
    IDataObject_Release(data);

    // 从另一个置物架拖来的临时附件：读取时就持有缓存，原置物架关闭也不会删掉。
    wchar_t *shared = attachment_cache_create();
    wchar_t *attachment = test_file(shared, L"附件.txt", "shared");
    model = shelf_model_new(NULL);
    DropReadResult *first = paths_result((const wchar_t *const *)&attachment, 1);
    first->entries[0].temporary = true;
    strlist_add(&first->caches, shared);
    shelf_model_add(model, first);
    drop_result_free(first);
    data = new_data();
    set_file_list(data, (const wchar_t *const *)&attachment, 1);
    read = read_data(data, false);
    shelf_model_release(model);
    pump_for(300);
    CHECK(path_exists(attachment), "读取后原置物架关闭，临时附件被删除");
    model = shelf_model_new(NULL);
    if (read) shelf_model_add(model, read);
    CHECK(model->count == 1 && model->items[0]->temporary && wcs_equal(model->items[0]->value, attachment), "从其他置物架拖来的临时附件没有标记为临时附件");
    shelf_model_release(model);
    CHECK(wait_gone(shared, 8000), "所有置物架关闭后临时附件没有清理");
    drop_result_free(read);
    IDataObject_Release(data);
    free(attachment);
    free(shared);

    // 来源程序的临时文件：当场打开、复制到缓存，下载来源标记跟着走；普通文件不复制。
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    wchar_t *chat_root = path_join(temp, L"DropShelf.CTests.Chat");
    wchar_t *id = new_guid_text();
    wchar_t *chat = path_join(chat_root, id);
    directory_create(chat);
    wchar_t *picture = test_file(chat, L"聊天图片.png", "\x89PNG");
    const char *zone_mark = "[ZoneTransfer]\r\nZoneId=3\r\n";
    zone_identifier_write(picture, zone_mark, strlen(zone_mark));
    wchar_t *local = local_app_data_directory();
    wchar_t *stable_folder = path_join(local, L"DropShelfCTests-stable");
    directory_create(stable_folder);
    wchar_t *stable = test_file(stable_folder, L"keep.txt", "keep");
    CHECK(chat_is_volatile_path(picture) && !chat_is_volatile_path(stable), "临时文件判断不正确");
    CHECK(chat_is_volatile_path(L"C:\\Users\\me\\Documents\\WeChat Files\\wxid\\FileStorage\\Temp\\a.jpg") &&
          !chat_is_volatile_path(L"C:\\Users\\me\\Documents\\WeChat Files\\wxid\\FileStorage\\File\\a.pdf"), "聊天软件临时目录判断不正确");
    data = new_data();
    const wchar_t *mixed[2] = { picture, stable };
    set_file_list(data, mixed, 2);
    ReadWait wait = {0};
    drop_read(data, false, true, NULL, on_read, &wait);
    // 松手的回调返回后来源程序马上删掉了临时文件。
    directory_delete_tree(chat);
    pump_until(&wait.done, 5000);
    read = wait.result;
    CHECK(read && read->count == 2 && read->entries[0].temporary && !wcs_equal(read->entries[0].value, picture), "临时文件没有保存到置物架缓存");
    if (read && read->count == 2)
    {
        CHECK(path_exists(read->entries[0].value) && wcscmp(path_file_name(read->entries[0].value), L"聊天图片.png") == 0, "来源程序删除临时文件后内容丢失");
        size_t zone_length;
        char *zone = zone_identifier_read(read->entries[0].value, &zone_length);
        CHECK(zone && strstr(zone, "ZoneId=3"), "临时文件的副本丢失了下载来源标记");
        free(zone);
        CHECK(!read->entries[1].temporary && wcs_equal(read->entries[1].value, stable), "普通文件不应复制");
        drop_result_release_caches(read);
    }
    drop_result_free(read);
    IDataObject_Release(data);
    directory_delete_tree(chat_root);
    directory_delete_tree(stable_folder);
    free(stable);
    free(stable_folder);
    free(local);
    free(picture);
    free(chat);
    free(id);
    free(chat_root);

    // 只提供 FileNameW 的来源。
    data = new_data();
    set_unicode(data, L"FileNameW", 0, file);
    read = read_data(data, false);
    // 测试文件在系统临时文件夹里，会被当作来源程序的临时文件另存一份，名称不变。
    CHECK(read && read->count == 1 && wcscmp(path_file_name(read->entries[0].value), L"a.txt") == 0, "只提供 FileNameW 的来源没有识别");
    if (read) drop_result_release_caches(read);
    drop_result_free(read);
    IDataObject_Release(data);

    free(file);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
}

// ───────────── 置物架窗口 ─────────────

/// C# 版 TestFileSuits：牌角的花色按文件类型。
static void test_file_suits(void)
{
    CHECK(file_suit_of_path(L"C:\\a\\photo.JPG") == SUIT_IMAGE && file_suit_of_path(L"C:\\a\\report.pdf") == SUIT_DOCUMENT, "图片或文档的花色不正确");
    CHECK(file_suit_of_path(L"C:\\a\\Downloads") == SUIT_FOLDER && file_suit_of_path(L"C:\\a\\Downloads\\") == SUIT_FOLDER, "没有扩展名的应按文件夹算");
    CHECK(file_suit_of_path(L"C:\\a\\setup.exe") == SUIT_OTHER, "其他文件的花色不正确");
    FileSuit suits[] = { SUIT_IMAGE, SUIT_DOCUMENT, SUIT_IMAGE };
    CHECK(file_suit_dominant(suits, 3) == SUIT_IMAGE, "应取数量最多的花色");
    CHECK(file_suit_dominant(suits, 2) == SUIT_DOCUMENT, "数量相同时应取最后放入的");
    CHECK(file_suit_dominant(NULL, 0) == SUIT_OTHER, "没有项目时应为其他");
    ShelfItem *link = shelf_item_new(ITEM_LINK, L"https://example.com", NULL, false);
    ShelfItem *photo = shelf_item_new(ITEM_FILE, L"C:\\a\\photo.jpeg", NULL, false);
    ShelfItem *folder = shelf_item_new(ITEM_FOLDER, L"C:\\a\\Downloads", NULL, false);
    ShelfItem *plain = shelf_item_new(ITEM_FILE, L"C:\\a\\README", NULL, false);
    CHECK(shelf_item_suit(link) == SUIT_OTHER, "链接的花色不正确");
    wchar_t *photo_mark = shelf_item_card_mark(photo), *link_mark = shelf_item_card_mark(link);
    wchar_t *folder_mark = shelf_item_card_mark(folder), *plain_mark = shelf_item_card_mark(plain);
    CHECK(wcscmp(photo_mark, L"JPEG") == 0 && shelf_item_suit(photo) == SUIT_IMAGE && wcscmp(link_mark, L"URL") == 0, "牌角应印扩展名或链接");
    CHECK(!*folder_mark && !*plain_mark && shelf_item_suit(folder) == SUIT_FOLDER, "文件夹和没有扩展名的文件牌角不印文字");
    free(photo_mark);
    free(link_mark);
    free(folder_mark);
    free(plain_mark);
    shelf_item_release(link);
    shelf_item_release(photo);
    shelf_item_release(folder);
    shelf_item_release(plain);
}

static bool card_at_home(CardKey pose, bool with_turn)
{
    return pose.x == 0 && pose.y == 0 && pose.scale == 1 && pose.angle == 0 && (!with_turn || pose.turn == 0) && pose.opacity == 1;
}

/// C# 版 TestShelfMotion：出场离场的牌落点、版本轮换和平铺的行列。
static void test_shelf_motion(void)
{
    RECT target = { 1500, 300, 1720, 573 }, work = { 0, 0, 1920, 1040 };
    double half = (target.right - target.left) / 2.0;
    bool alternates = true, hidden = true, outside = true, lands = true;
    double last_shake = NAN, last_edge = NAN;
    for (int round = 0; round < 20; round++)
    {
        CardMotion shake = card_motion_shake_in(30, 40);
        CardMotion right = card_motion_edge_in(EDGE_RIGHT, target, work, 1);
        if (shake.keys[0].angle == last_shake || right.keys[0].angle == last_edge) alternates = false;
        last_shake = shake.keys[0].angle;
        last_edge = right.keys[0].angle;
        CardKey start = card_motion_at(&shake, 0);
        if (start.turn != 180 || start.opacity != 0) hidden = false;
        if (!(target.left + half + card_motion_at(&right, 0).x - half > work.right)) outside = false;
        if (!card_at_home(card_motion_at(&shake, card_motion_duration(&shake) + 1), true) ||
            !card_at_home(card_motion_at(&right, card_motion_duration(&right) + 1), true))
            lands = false;
    }
    CHECK(alternates, "相邻两次呼出不应用同一个版本");
    CHECK(hidden, "晃动呼出应从还看不见的牌背开始");
    CHECK(outside, "从右边缘呼出时牌应从屏幕外开始");
    CHECK(lands, "出场的牌最后应正好落在置物架的位置");
    bool sides = true;
    for (int round = 0; round < 8; round++)
    {
        CardMotion left = card_motion_edge_in(EDGE_LEFT, target, work, 1), top = card_motion_edge_in(EDGE_TOP, target, work, 1);
        if (!card_at_home(card_motion_at(&left, card_motion_duration(&left) + 1), false) ||
            !card_at_home(card_motion_at(&top, card_motion_duration(&top) + 1), false))
            sides = false;
    }
    CHECK(sides, "从左边和顶边呼出的牌最后应正好落在置物架的位置");

    CardMotion toss = card_motion_exit(EDGE_LEFT, false, target, work, 1);
    CardMotion flip = card_motion_exit(EDGE_NONE, true, target, work, 1);
    CardKey toss_start = card_motion_at(&toss, 0), toss_end = card_motion_at(&toss, card_motion_duration(&toss));
    CHECK(toss_start.x == 0 && toss_start.scale == 1 && toss_end.x < -target.left, "边缘呼出的置物架关闭时应甩回那条边");
    CardKey flip_end = card_motion_at(&flip, card_motion_duration(&flip));
    CHECK(flip.horizontal_axis && flip_end.turn == 180 && flip_end.opacity == 0, "其余的置物架关闭时应按出场的方向翻回背面淡出");

    CardMotion line = { .count = 2, .fade_in_ms = 50 };
    line.keys[0] = (CardKey){ 0, 0, 0, 1, 0, 0, 1, false, EASE_OF(EASE_LINEAR) };
    line.keys[1] = (CardKey){ 100, 10, 0, 1, 0, 0, 1, false, EASE_OF(EASE_LINEAR) };
    CHECK(fabs(card_motion_at(&line, 50).x - 5) < 1e-9 && fabs(card_motion_at(&line, 25).opacity - 0.5) < 1e-9, "关键帧之间应按时间插值，开头按时间淡入");

    CHECK(shelf_grid_columns_for(280) == 4 && shelf_grid_rows_for(5, 280) == 2 && shelf_grid_rows_for(4, 280) == 1, "平铺应一行四张");
    CHECK(fabsf(shelf_grid_height(2) - 266) < 1e-4f && shelf_grid_height(0) == 0, "平铺的高度不正确");
}

static void test_shelf_window(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *folder = test_folder(L"shelf-window");
    wchar_t *one = test_file(folder, L"one.txt", "1");
    wchar_t *two = test_file(folder, L"two.txt", "22");
    ShelfWindow *shelf = shelves_open_at((POINT){ 400, 400 }, false, false, EDGE_NONE);
    CHECK(shelf != NULL && shelves_open_count() == 1, "没有打开置物架");
    if (shelf)
    {
        StrList paths = {0};
        strlist_add(&paths, one);
        strlist_add(&paths, two);
        strlist_add(&paths, one);
        shelf_window_add_paths(shelf, &paths);
        strlist_clear(&paths);
        pump_for(800);
        HWND handle = FindWindowExW(NULL, NULL, L"DropShelfWindow", L"DropShelf 置物架 - 2 个项目");
        CHECK(handle && IsWindowVisible(handle), "放进文件后标题没有跟着变，或窗口没有显示");
        wchar_t *image = path_join(folder, L"shelf.png");
        ui_snapshot_windows(folder);
        wchar_t *first = path_join(folder, L"window-1.png");
        CHECK(path_exists(first), "置物架的画面没有画出来");
        free(first);
        free(image);
        // 手势打开的空置物架被复用，不会越开越多；松开鼠标后没放东西的会自动关闭。
        ShelfWindow *gesture = shelves_open_at((POINT){ 900, 400 }, true, false, EDGE_LEFT);
        ShelfWindow *reused = shelves_open_at((POINT){ 1000, 400 }, true, false, EDGE_NONE);
        CHECK(gesture && gesture == reused && gesture != shelf && shelves_open_count() == 2, "晃动打开的空置物架没有复用");
        shelves_drag_released();
        pump_for(1200);
        CHECK(shelves_open_count() == 1, "松开鼠标后空的置物架没有自动关闭");
        if (handle) SendMessageW(handle, WM_KEYDOWN, VK_ESCAPE, 0);
        CHECK(pump_until_done(no_shelves_open, 3000), "按 Esc 没有关闭置物架");
    }
    shelves_close_all();
    pump_for(100);
    free(one);
    free(two);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
}
