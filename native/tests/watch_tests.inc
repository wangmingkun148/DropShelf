// ───────────── 观察文件夹和探出的角 ─────────────

static StrList watch_seen;
static volatile bool watch_got;

static void on_watch(const StrList *paths)
{
    for (int index = 0; index < paths->count; index++) strlist_add(&watch_seen, paths->items[index]);
    watch_got = true;
}

/// 等观察那边交出一批新文件；一直没有时等满 milliseconds。
static void wait_watch(int milliseconds)
{
    watch_got = false;
    pump_until(&watch_got, milliseconds);
}

static bool watch_saw(const wchar_t *path)
{
    return strlist_index_of(&watch_seen, path, true) >= 0;
}

static void test_folder_watch_names(void)
{
    CHECK(folder_watch_is_partial(L"setup.exe.crdownload") && folder_watch_is_partial(L"movie.mkv.part") && folder_watch_is_partial(L"a.!qB") &&
          folder_watch_is_partial(L"x.xltd"), "下载中的临时文件没有认出来");
    CHECK(!folder_watch_is_partial(L"report.pdf") && !folder_watch_is_partial(L"partial.txt") && !folder_watch_is_partial(L"part"),
        "正常文件被当成了临时文件");
    CHECK(folder_watch_is_ignored(L"~$报告.docx") && folder_watch_is_ignored(L"desktop.ini") && folder_watch_is_ignored(L".~lock.a.odt#") &&
          folder_watch_is_ignored(L"x.td.cfg"), "锁文件和系统文件没有忽略");
    CHECK(!folder_watch_is_ignored(L"报告.docx") && !folder_watch_is_ignored(L"~notes.txt"), "正常文件被忽略了");

    // 光标靠哪边就贴哪边，和光标一样高，不超出工作区。
    RECT work = { 0, 0, 1920, 1040 };
    ScreenEdge edge = EDGE_NONE;
    POINT place = shelf_peek_place((POINT){ 300, 500 }, work, 66, 100, &edge);
    CHECK(edge == EDGE_LEFT && place.x == 0 && place.y == 450, "光标在左半边时角应贴左边、和光标一样高");
    place = shelf_peek_place((POINT){ 1500, 10 }, work, 66, 100, &edge);
    CHECK(edge == EDGE_RIGHT && place.x == 1920 - 66 && place.y == 0, "光标在右半边时角应贴右边，不超出上沿");
    RECT second = { 1920, 40, 3840, 1080 };
    place = shelf_peek_place((POINT){ 2500, 1075 }, second, 66, 100, &edge);
    CHECK(edge == EDGE_LEFT && place.x == 1920 && place.y == 980, "第二块屏幕上应贴它自己的边，不超出下沿");
}

static void test_folder_watch(void)
{
    task_initialize();
    wchar_t *folder = test_folder(L"watched");
    strlist_clear(&watch_seen);
    StrList folders = {0};
    strlist_add(&folders, folder);
    folder_watch_set(&folders, on_watch);
    strlist_clear(&folders);
    pump_for(300);

    // 写完的新文件交出来。
    wchar_t *plain = test_file(folder, L"plain.txt", "hello");
    wait_watch(5000);
    CHECK(watch_saw(plain), "写完的新文件没有交出");

    // 浏览器下载：先写 .crdownload，下载完改成正式的名字。
    wchar_t *partial = test_file(folder, L"setup.exe.crdownload", "part");
    wchar_t *final_name = path_join(folder, L"setup.exe");
    pump_for(1500);
    CHECK(!watch_saw(partial), "下载中的临时文件不应交出");
    MoveFileW(partial, final_name);
    wait_watch(5000);
    CHECK(watch_saw(final_name), "从临时名字改成正式名字后没有交出");

    // 还有别的程序在写时不交出，写完关掉以后才交出。
    wchar_t *busy = path_join(folder, L"busy.bin");
    HANDLE writer = CreateFileW(busy, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written = 0;
    WriteFile(writer, "abc", 3, &written, NULL);
    wait_watch(2500);
    CHECK(!watch_saw(busy), "还在写的文件不应交出");
    CloseHandle(writer);
    wait_watch(5000);
    CHECK(watch_saw(busy), "写完关掉后没有交出");

    // 改名、编辑器保存时用临时文件替换原文件，都不算新文件。
    strlist_clear(&watch_seen);
    wchar_t *renamed = path_join(folder, L"renamed.txt");
    MoveFileW(plain, renamed);
    wchar_t *temporary = test_file(folder, L"~WRL0001.tmp", "new content");
    wchar_t *backup = path_join(folder, L"~WRL0002.tmp");
    MoveFileW(final_name, backup);
    MoveFileW(temporary, final_name);
    DeleteFileW(backup);
    wait_watch(3000);
    CHECK(watch_seen.count == 0, "改名或保存时替换原文件被当成了新文件");

    // Firefox：先放一个空的占位文件，内容写在 .part 里，下载完替换占位文件。
    wchar_t *placeholder = test_file(folder, L"movie.mp4", "");
    wchar_t *part = test_file(folder, L"movie.mp4.part", "data");
    wait_watch(2500);
    CHECK(!watch_saw(placeholder), "还有 .part 时占位文件不应交出");
    MoveFileExW(part, placeholder, MOVEFILE_REPLACE_EXISTING);
    wait_watch(5000);
    CHECK(watch_saw(placeholder), "Firefox 下载完成后没有交出");

    // 快速操作期间写完的先留着，结束后去掉它自己写的。
    strlist_clear(&watch_seen);
    folder_watch_hold();
    wchar_t *output = test_file(folder, L"资料.zip", "zip");
    wchar_t *other = test_file(folder, L"other.txt", "other");
    pump_for(3500);
    CHECK(watch_seen.count == 0, "快速操作期间写完的文件应先留着");
    StrList outputs = {0};
    strlist_add(&outputs, output);
    folder_watch_release(&outputs);
    strlist_clear(&outputs);
    CHECK(!watch_saw(output) && watch_saw(other), "快速操作结束后应只去掉它写出的文件");

    // 从置物架拖过去的（资源管理器复制过来的，同名或加了“ (2)”的）不交出，名字只是开头相同的照常交出。
    strlist_clear(&watch_seen);
    StrList dragged = {0};
    strlist_add(&dragged, L"C:\\somewhere\\照片.jpg");
    folder_watch_drag_begin(&dragged);
    strlist_clear(&dragged);
    wchar_t *copy = test_file(folder, L"照片.jpg", "jpg");
    wchar_t *second_copy = test_file(folder, L"照片 (2).jpg", "jpg");
    folder_watch_drag_end();
    wchar_t *unrelated = test_file(folder, L"照片集.zip", "zip");
    wait_watch(5000);
    pump_for(1500);
    CHECK(!watch_saw(copy) && !watch_saw(second_copy) && watch_saw(unrelated), "从置物架拖过去的文件不应交出，别的照常交出");

    // 停下以后不再交出。
    folder_watch_stop();
    strlist_clear(&watch_seen);
    wchar_t *late = test_file(folder, L"late.txt", "late");
    pump_for(2500);
    CHECK(watch_seen.count == 0, "停止观察后不应再交出");

    free(plain);
    free(partial);
    free(final_name);
    free(busy);
    free(renamed);
    free(temporary);
    free(backup);
    free(placeholder);
    free(part);
    free(output);
    free(other);
    free(copy);
    free(second_copy);
    free(unrelated);
    free(late);
    free(folder);
    strlist_clear(&watch_seen);
}

static void change_peek_seconds(AppSettings *settings, void *context)
{
    settings->watch_peek_seconds = *(int *)context;
}

static void test_shelf_peek(void)
{
    task_initialize();
    language_set_mode(LANGUAGE_CHINESE);
    shelf_peek_ignore_busy(true);
    // 光标放到屏幕中间：角不会刚好出现在光标下面（那样算悬停，不会按时缩回）；之前的置物架都关掉，不会被拿来复用。
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
    shelves_close_all();
    pump_for(300);
    wchar_t *folder = test_folder(L"peek");
    wchar_t *one = test_file(folder, L"one.txt", "1");
    wchar_t *two = test_file(folder, L"two.txt", "22");
    StrList first = {0}, second = {0};
    strlist_add(&first, one);
    strlist_add(&second, two);
    strlist_add(&second, one);

    shelf_peek_add(&first);
    pump_for(400);
    CHECK(shelf_peek_is_shown() && shelf_peek_count() == 1, "新文件出现后没有探出角");
    shelf_peek_add(&second);
    CHECK(shelf_peek_count() == 2, "连着出现的新文件应合在同一个角里，不重复");

    // 点开：角收起，置物架从角所在的那条边伸出来，里面是这两个文件。
    int before = shelves_open_count();
    ScreenEdge edge = shelf_peek_edge();
    CHECK(shelf_peek_open() && !shelf_peek_is_shown(), "点开后角应收起");
    ShelfWindow *shelf = shelves_first();
    for (int wait = 0; wait < 30 && shelf && shelf->model->count < 2; wait++) pump_for(100);
    CHECK(shelves_open_count() == before + 1 && shelf && shelf->model->count == 2, "点开角后没有伸出放着新文件的置物架");
    CHECK(shelf && shelf->entrance == ENTRANCE_EDGE && shelf->edge == edge, "置物架应从角所在的那条边滑出");
    CHECK(!shelf_peek_open(), "角收起后不能再点开");

    // 右键收起：不打开置物架。
    shelf_peek_add(&first);
    pump_for(100);
    shelf_peek_dismiss();
    pump_for(600);
    CHECK(!shelf_peek_is_shown() && shelves_open_count() == before + 1, "收起角时不应打开置物架");

    // 过了停留时间自动缩回。
    int seconds = 1;
    app_update_settings(change_peek_seconds, &seconds);
    shelf_peek_add(&first);
    pump_for(300);
    CHECK(shelf_peek_is_shown(), "角没有探出");
    pump_for(1800);
    CHECK(!shelf_peek_is_shown(), "过了停留时间没有缩回");
    seconds = WATCH_PEEK_DEFAULT_SECONDS;
    app_update_settings(change_peek_seconds, &seconds);

    shelf_peek_close();
    shelf_peek_ignore_busy(false);
    shelves_close_all();
    pump_for(300);
    strlist_clear(&first);
    strlist_clear(&second);
    free(one);
    free(two);
    free(folder);
}
