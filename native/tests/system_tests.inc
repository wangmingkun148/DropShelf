// 第 8 步：C# 版 SettingsTests、SystemTests、WindowTests 里还没有对应的部分——置物架的尺寸和位置、程序的版本号和图标、
// 系统缩略图、图片处理线程、转交命令行、探针窗口。

static void test_shelf_sizes(void)
{
    float widths[3];
    for (int size = 0; size < 3; size++) widths[size] = shelf_width_for(size);
    CHECK(widths[SHELF_SIZE_SMALL] < widths[SHELF_SIZE_MEDIUM] && widths[SHELF_SIZE_MEDIUM] < widths[SHELF_SIZE_LARGE], "三档尺寸没有依次增大");

    // 窗口大小只取决于尺寸、项目数和按钮数：用一个空的置物架结构量各档的大小，不用真的建窗口。
    for (int buttons = 0; buttons <= 4; buttons += 4)
    {
        ShelfModel model = {0};
        ShelfWindow shelf = {0};
        shelf.model = &model;
        shelf.actions.count = buttons;
        bool ratio = true, wide = true;
        float expanded[3];
        for (int size = 0; size < 3; size++)
        {
            shelf.size = size;
            float width, height, list_width, list_height;
            shelf_target_size(&shelf, false, &width, &height);
            shelf_target_size(&shelf, true, &list_width, &list_height);
            expanded[size] = list_width;
            // 堆叠区域的高度是宽度的 0.66：总高减去标题栏、堆叠下的两行文字和按钮栏。
            float stack = height - SHELF_HEADER_HEIGHT - SHELF_CAPTION_HEIGHT - (buttons > 0 ? SHELF_ACTION_BAR_HEIGHT : 10);
            if (fabsf(stack / width - 0.66f) > 0.01f) ratio = false;
            if (list_width < width + 60) wide = false;
        }
        CHECK(ratio, "堆叠区域比例不一致");
        CHECK(wide, "展开列表过窄");
        CHECK(expanded[0] < expanded[1] && expanded[1] < expanded[2] && expanded[1] == 300, "展开后的宽度没有随三档尺寸变化");
    }

    // 小窗以鼠标为中心并保持在屏幕内（和探针窗口用同一个算法）。
    POINT placed = probe_place((POINT){ 500, 400 }, (RECT){ 0, 0, 1000, 800 }, 300, 300);
    CHECK(placed.x == 350 && placed.y == 250, "小窗没有以鼠标位置为中心");
    placed = probe_place((POINT){ 10, 10 }, (RECT){ 0, 0, 1000, 800 }, 300, 300);
    CHECK(placed.x == 0 && placed.y == 0, "左上角位置超出屏幕");
    placed = probe_place((POINT){ 990, 790 }, (RECT){ 0, 0, 1000, 800 }, 300, 300);
    CHECK(placed.x == 700 && placed.y == 500, "右下角位置超出屏幕");
    placed = probe_place((POINT){ -500, 300 }, (RECT){ -1920, 0, 0, 1080 }, 300, 300);
    CHECK(placed.x == -650 && placed.y == 150, "左侧副屏位置不正确");
}

/// 测试程序旁边的 DropShelf.exe（两个在同一个编译目录里）。调用方负责 free。
static wchar_t *app_executable(void)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t *directory = path_directory(self);
    wchar_t *path = path_join(directory, L"DropShelf.exe");
    free(directory);
    return path;
}

static wchar_t *repository_text(const wchar_t *relative)
{
    wchar_t *path = path_join(DROPSHELF_REPOSITORY_DIR, relative);
    wchar_t *text = file_read_text(path, 1 << 20);
    free(path);
    return text;
}

static void test_program_resources(void)
{
    int major = 0, minor = 0, patch = 0;
    CHECK(swscanf(DROPSHELF_VERSION, L"%d.%d.%d", &major, &minor, &patch) == 3 && major >= 2, "程序里的版本号不正确");

    wchar_t *exe = app_executable();
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(exe, &ignored);
    void *block = size ? xalloc(size) : NULL;
    VS_FIXEDFILEINFO *fixed = NULL;
    UINT length = 0;
    wchar_t *product = NULL;
    bool read = block && GetFileVersionInfoW(exe, 0, size, block) && VerQueryValueW(block, L"\\", (void **)&fixed, &length) && fixed &&
                VerQueryValueW(block, L"\\StringFileInfo\\080404B0\\ProductVersion", (void **)&product, &length);
    CHECK(read && HIWORD(fixed->dwFileVersionMS) == (WORD)major && LOWORD(fixed->dwFileVersionMS) == (WORD)minor &&
              HIWORD(fixed->dwFileVersionLS) == (WORD)patch && LOWORD(fixed->dwFileVersionLS) == 0 && product && wcscmp(product, DROPSHELF_VERSION) == 0,
        "程序文件的版本信息与版本号不一致");
    free(block);

    // 托盘和窗口用的图标、清单（版本号与 csproj 一致）都在程序里。
    HMODULE module = LoadLibraryExW(exe, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    HICON icon = module ? LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR) : NULL;
    CHECK(icon != NULL, "没有从图标资源创建托盘图标");
    if (icon) DestroyIcon(icon);
    HRSRC resource = module ? FindResourceW(module, MAKEINTRESOURCEW(1), RT_MANIFEST) : NULL;
    HGLOBAL loaded = resource ? LoadResource(module, resource) : NULL;
    const char *manifest = loaded ? LockResource(loaded) : NULL;
    DWORD manifest_size = resource ? SizeofResource(module, resource) : 0;
    char expected[64];
    snprintf(expected, sizeof(expected), "version=\"%d.%d.%d.0\"", major, minor, patch);
    bool found = false;
    for (DWORD offset = 0; manifest && offset + strlen(expected) <= manifest_size && !found; offset++)
        found = memcmp(manifest + offset, expected, strlen(expected)) == 0;
    CHECK(found, "程序里的清单版本号与版本号不一致");
    if (module) FreeLibrary(module);
    free(exe);

    // 版本号写在四处（见仓库根目录的 CLAUDE.md），csproj 之外的三处要一致。
    wchar_t version_text[64], define[96], info[96];
    swprintf(version_text, 64, L"version=\"%d.%d.%d.0\"", major, minor, patch);
    swprintf(define, 96, L"#define AppVersion \"%ls\"", DROPSHELF_VERSION);
    swprintf(info, 96, L"VersionInfoVersion=%ls.0", DROPSHELF_VERSION);
    wchar_t *app_manifest = repository_text(L"src/DropShelf/app.manifest");
    wchar_t *installer = repository_text(L"installer/DropShelf.iss");
    wchar_t *uninstaller = repository_text(L"installer/UninstallLauncher.iss");
    CHECK(app_manifest && wcsstr(app_manifest, version_text), "app.manifest 的版本号与 DropShelf.csproj 不一致");
    CHECK(installer && wcsstr(installer, define) && wcsstr(installer, info), "DropShelf.iss 的版本号与 DropShelf.csproj 不一致");
    CHECK(uninstaller && wcsstr(uninstaller, define) && wcsstr(uninstaller, info), "UninstallLauncher.iss 的版本号与 DropShelf.csproj 不一致");
    free(app_manifest);
    free(installer);
    free(uninstaller);
}

typedef struct ThumbnailWait
{
    volatile bool done;
    GfxImage *image;
} ThumbnailWait;

static void on_test_thumbnail(GfxImage *image, void *context)
{
    ThumbnailWait *wait = context;
    wait->image = image;
    wait->done = true;
}

typedef struct ThreadProbe
{
    volatile bool done;
    DWORD thread;
    bool sta;
} ThreadProbe;

static void probe_thread_work(void *data)
{
    ThreadProbe *probe = data;
    probe->thread = GetCurrentThreadId();
    APTTYPE type;
    APTTYPEQUALIFIER qualifier;
    probe->sta = SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
}

static void probe_thread_done(void *data)
{
    ((ThreadProbe *)data)->done = true;
}

static void test_shell_thumbnail(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *folder = test_folder(L"shell-thumbnail");
    wchar_t *path = test_file(folder, L"preview.txt", "thumbnail preview");
    ThumbnailWait wait = {0};
    thumbnail_load(path, 96, on_test_thumbnail, &wait);
    pump_until(&wait.done, 10000);
    CHECK(wait.done && wait.image && gfx_image_width(wait.image) > 0, "没有获取到缩略图");
    CHECK(wait.image && gfx_image_width(wait.image) <= 96 && gfx_image_height(wait.image) <= 96, "缩略图大于需要的尺寸");
    gfx_image_release(wait.image);
    wchar_t *type = file_type_name(path);
    CHECK(type && !wcs_is_blank(type), "没有获取到文件类型名称");
    free(type);
    free(path);
    free(folder);

    // 图片处理集中在同一个单线程套间的常驻线程上，不占用界面线程；整批转换另用一个线程。
    ThreadProbe first = {0}, second = {0}, long_work = {0};
    task_run_imaging(probe_thread_work, probe_thread_done, &first);
    pump_until(&first.done, 5000);
    task_run_imaging(probe_thread_work, probe_thread_done, &second);
    pump_until(&second.done, 5000);
    task_run_long_imaging(probe_thread_work, probe_thread_done, &long_work);
    pump_until(&long_work.done, 5000);
    CHECK(first.done && second.done && first.thread == second.thread && first.sta, "图片处理没有在同一个单线程套间线程上运行");
    CHECK(first.thread != GetCurrentThreadId(), "图片处理占用了界面线程");
    CHECK(long_work.done && long_work.thread != first.thread && long_work.thread != GetCurrentThreadId() && long_work.sta,
        "整批压缩图片和缩略图共用了同一个线程");
    if (SUCCEEDED(ole)) OleUninitialize();
}

static StrList forwarded_arguments;
static bool forwarded;

static LRESULT CALLBACK forward_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_COPYDATA)
    {
        strlist_clear(&forwarded_arguments);
        forwarded = single_instance_read_arguments((const COPYDATASTRUCT *)lparam, &forwarded_arguments);
        return TRUE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void test_argument_forwarding(void)
{
    wchar_t *folder = test_folder(L"forward");
    wchar_t *file = test_file(folder, L"send to.txt", "x");
    WNDCLASSEXW window_class = { .cbSize = sizeof(window_class), .lpfnWndProc = forward_window_procedure, .hInstance = GetModuleHandleW(NULL),
                                 .lpszClassName = L"DropShelf.Tests.ForwardWindow" };
    RegisterClassExW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"DropShelf.Tests.Forward", WS_POPUP, 0, 0, 0, 0, NULL, NULL,
        window_class.hInstance, NULL);
    StrList arguments = {0};
    strlist_add(&arguments, L"--shelf");
    strlist_add(&arguments, file);
    forwarded = false;
    CHECK(window && single_instance_send_arguments(L"DropShelf.Tests.Forward", &arguments), "没有找到正在运行的实例");
    CHECK(forwarded && list_is(&forwarded_arguments, 2, L"--shelf", file), "转交的参数不正确");
    StrList other = {0};
    strlist_add(&other, L"x");
    CHECK(!single_instance_send_arguments(L"DropShelf.Tests.NoSuchWindow", &other), "不存在的实例被误认为已转交");
    strlist_clear(&other);
    strlist_clear(&arguments);
    strlist_clear(&forwarded_arguments);
    if (window) DestroyWindow(window);
    free(file);
    free(folder);
}

/// 在窗口里 (x, y)（DIP）处点一下：和真的鼠标一样经过窗口的消息处理。
static void click_at(UiWindow *window, float x, float y)
{
    HWND handle = ui_window_handle(window);
    float scale = ui_window_scale(window);
    LPARAM point = MAKELPARAM((int)lroundf(x * scale), (int)lroundf(y * scale));
    SendMessageW(handle, WM_MOUSEMOVE, 0, point);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(handle, WM_LBUTTONUP, 0, point);
}

static void click_rect(UiWindow *window, GfxRect rect)
{
    click_at(window, (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
}

static bool rect_is_empty(GfxRect rect)
{
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

static bool window_title_is(HWND window, const wchar_t *expected)
{
    wchar_t title[256] = {0};
    GetWindowTextW(window, title, 256);
    return wcscmp(title, expected) == 0;
}

static void change_shelf_window_settings(AppSettings *settings, void *context)
{
    const int *values = context;
    settings->quick_action_layout = values[0];
    settings->drag_out_mode = values[1];
    settings->quick_shelf_size = SHELF_SIZE_MEDIUM;
    settings->close_when_empty = false;
}

static void set_shelf_window_settings(int layout, int drag_out_mode)
{
    int values[] = { layout, drag_out_mode };
    app_update_settings(change_shelf_window_settings, values);
    pump_for(50);
}

static int window_width(HWND window)
{
    RECT rect = {0};
    GetWindowRect(window, &rect);
    return rect.right - rect.left;
}

static int window_height(HWND window)
{
    RECT rect = {0};
    GetWindowRect(window, &rect);
    return rect.bottom - rect.top;
}

/// 命令行或“发送到”传来文件时（app_handle_arguments）：已有一个放了东西的置物架，新开的那个用的是预先建好、隐藏着待命的窗口，
/// 文件要放进这个新的置物架。
static void test_spare_shelf_receives_paths(void)
{
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *folder = test_folder(L"spare-shelf");
    wchar_t *one = test_file(folder, L"one.txt", "1");
    wchar_t *two = test_file(folder, L"two.txt", "2");
    StrList paths = {0};
    ShelfWindow *first = shelves_open_at_cursor(true);
    strlist_add(&paths, one);
    if (first) shelf_window_add_paths(first, &paths);
    strlist_clear(&paths);
    // 预先建好待命的窗口要等 0.6 秒。
    pump_for(1200);
    CHECK(first && first->model->count == 1, "第一个置物架没有放进文件");
    ShelfWindow *second = shelves_open_at_cursor(true);
    strlist_add(&paths, two);
    if (second) shelf_window_add_paths(second, &paths);
    strlist_clear(&paths);
    pump_for(1200);
    CHECK(second && second != first && shelves_open_count() == 2 && second->model->count == 1 && first->model->count == 1,
        "第二个置物架没有放进传来的文件");
    shelves_close_all();
    pump_for(300);
    free(one);
    free(two);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
}

/// C# 版 TestShelfWindowActions：按钮数量、标题、拖出方式、切换语言、按钮布局、置顶、展开收起、关闭后记入最近关闭并能恢复。
static void test_shelf_window_actions(void)
{
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *history_file = path_join(temporary_directory, L"history-window.json");
    DeleteFileW(history_file);
    shelf_history_load(history_file);
    set_shelf_window_settings(LAYOUT_FOUR, TRANSFER_MOVE);
    ShelfWindow *shelf = shelves_open_at((POINT){ 600, 400 }, false, false, EDGE_NONE);
    CHECK(shelf != NULL, "没有打开置物架");
    if (!shelf)
    {
        free(history_file);
        if (SUCCEEDED(ole)) OleUninitialize();
        return;
    }
    pump_for(600);
    HWND handle = shelf->handle;
    CHECK(shelf->actions.count == LAYOUT_FOUR, "快速操作按钮数量不正确");
    CHECK(shelf_window_is_reusable(shelf), "空置物架应可复用");
    CHECK(window_title_is(handle, L"DropShelf 置物架 - 置物架"), "置物架窗口标题不正确");
    CHECK(shelf->move_mode && wcscmp(S(STR_TransferMove), L"移动") == 0, "默认拖出方式应显示为移动");

    language_set_mode(LANGUAGE_ENGLISH);
    pump_for(50);
    CHECK(shelf->action_tooltips[0] && wcsncmp(shelf->action_tooltips[0], L"Compress: ", 10) == 0, "切换语言后快速操作提示没有更新");
    CHECK(window_title_is(handle, L"DropShelf shelf - Shelf") && wcscmp(S(STR_TransferMove), L"Move") == 0, "切换语言后置物架文字没有更新");
    language_set_mode(LANGUAGE_CHINESE);
    pump_for(50);

    set_shelf_window_settings(LAYOUT_TWO, TRANSFER_COPY);
    CHECK(!shelf->move_mode, "拖出方式没有更新");
    CHECK(shelf->actions.count == 2, "两个按钮时应显示两个操作");
    int two_height = window_height(handle);
    set_shelf_window_settings(LAYOUT_FOUR, TRANSFER_COPY);
    CHECK(abs(window_height(handle) - two_height) <= 1, "两个和四个按钮的置物架高度应相同");
    shelf_layout(shelf);
    bool one_row = shelf->actions.count == 4;
    for (int index = 1; index < shelf->actions.count; index++)
        if (shelf->layout.action_buttons[index].top != shelf->layout.action_buttons[0].top) one_row = false;
    CHECK(one_row, "四个操作时按钮应排成一行");
    set_shelf_window_settings(LAYOUT_OFF, TRANSFER_COPY);
    CHECK(shelf->actions.count == 0, "关闭快速操作后仍显示按钮");
    set_shelf_window_settings(LAYOUT_FOUR, TRANSFER_MOVE);

    wchar_t *folder = test_folder(L"shelf-window-actions");
    wchar_t *file = test_file(folder, L"a.txt", "x");
    shelf_layout(shelf);
    CHECK(rect_is_empty(shelf->layout.view), "空置物架不应显示文件堆/列表切换按钮");
    StrList paths = {0};
    strlist_add(&paths, file);
    shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
    pump_for(500);
    shelf_layout(shelf);
    CHECK(shelf->model->count == 1 && !rect_is_empty(shelf->layout.view), "只有一项时也应显示文件堆/列表切换按钮");
    strlist_add(&paths, folder);
    shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
    pump_for(500);
    CHECK(shelf->model->count == 2 && !shelf_window_is_reusable(shelf), "有内容的置物架不应被复用");
    CHECK(shelf->model->pinned && (GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST), "置物架默认应置顶");
    shelf_layout(shelf);
    click_rect(shelf->ui, shelf->layout.pin);
    CHECK(!shelf->model->pinned && !(GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST), "取消钉子后置物架不应再置顶");
    click_rect(shelf->ui, shelf->layout.pin);
    CHECK(shelf->model->pinned && (GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST), "再点钉子后置物架应恢复置顶");

    // 切换时窗口大小平滑过渡、牌飞到各行，等动画播完再检查。
    float scale = shelf_scale(shelf);
    int collapsed_width = window_width(handle);
    click_rect(shelf->ui, shelf->layout.view);
    pump_for(800);
    CHECK(shelf->model->expanded, "点击切换按钮没有展开列表");
    CHECK(window_width(handle) >= (int)lroundf(shelf_width_for(SHELF_SIZE_MEDIUM) * scale) + (int)(60 * scale), "展开后宽度没有变化");

    // 展开时继续放入项目：窗口跟着变高。
    int two_rows_height = window_height(handle);
    wchar_t *extra = test_file(folder, L"b.txt", "y");
    strlist_add(&paths, extra);
    shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
    pump_for(800);
    CHECK(shelf->model->count == 3 && window_height(handle) > two_rows_height, "展开时放入项目后列表或高度没有更新");
    ShelfItem *last = shelf->model->items[shelf->model->count - 1];
    shelf_remove_items(shelf, &last, 1);
    pump_for(600);
    shelf_layout(shelf);
    click_rect(shelf->ui, shelf->layout.view);
    pump_for(800);
    CHECK(!shelf->model->expanded && abs(window_width(handle) - collapsed_width) <= 1, "收起列表的动画结束后应回到堆叠，窗口恢复收起时的宽度");

    // 关掉以后记入最近关闭，恢复时项目原样回来。
    shelf_window_close_animated(shelf);
    CHECK(pump_until_done(no_shelves_open, 3000), "置物架没有关掉");
    ShelfHistoryEntry *latest = shelf_history_latest();
    CHECK(latest && latest->count == 2 && wcs_equal(latest->items[0].path, file) &&
              wcs_equal(latest->items[1].path, folder), "关闭的置物架没有记入最近关闭");
    if (latest)
    {
        shelves_restore(latest, NULL);
        pump_for(800);
        int count = 0;
        shelf_history_entries(&count);
        CHECK(shelves_open_count() == 1 && count == 0, "最近关闭的置物架没有恢复");
    }
    shelves_close_all();
    pump_for(300);
    shelf_history_clear();
    shelf_history_flush();
    DeleteFileW(history_file);
    free(history_file);
    free(extra);
    free(file);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
}

static void test_probe_window(void)
{
    HRESULT ole = OleInitialize(NULL);
    CHECK(probe_create(GetModuleHandleW(NULL), NULL), "探针窗口没有建好");
    HWND window = FindWindowW(NULL, L"DropShelf Probe");
    probe_show_at((POINT){ -10000, -10000 });
    pump_for(50);
    CHECK(window && IsWindowVisible(window), "探针窗口没有显示");
    probe_hide();
    CHECK(window && !IsWindowVisible(window), "探针窗口没有隐藏");
    probe_destroy();
    CHECK(!IsWindow(window), "探针窗口没有销毁");
    if (SUCCEEDED(ole)) OleUninitialize();
}

// ───────────── 纸牌风格的置物架 ─────────────

static void test_shelf_sizes_cards(void)
{
    // 纸牌风格下再跑一遍（和 style 分支原来的测试相同），跑完换回简洁。
    theme_set_look(LOOK_CARDS);
    float widths[3];
    for (int size = 0; size < 3; size++) widths[size] = shelf_width_for(size);
    CHECK(widths[SHELF_SIZE_SMALL] < widths[SHELF_SIZE_MEDIUM] && widths[SHELF_SIZE_MEDIUM] < widths[SHELF_SIZE_LARGE], "三档尺寸没有依次增大");

    // 窗口大小只取决于尺寸、项目数和按钮数：用一个空的置物架结构量各档的大小，不用真的建窗口。
    for (int buttons = 0; buttons <= 4; buttons += 4)
    {
        ShelfModel model = {0};
        ShelfWindow shelf = {0};
        shelf.model = &model;
        shelf.actions.count = buttons;
        bool ratio = true, wide = true;
        float expanded[3];
        for (int size = 0; size < 3; size++)
        {
            shelf.size = size;
            float width, height, list_width, list_height;
            shelf_target_size(&shelf, false, &width, &height);
            shelf_target_size(&shelf, true, &list_width, &list_height);
            expanded[size] = list_width;
            // 堆叠区域的高度是宽度的 0.66：总高减去标题栏、堆叠下的两行文字和按钮栏。
            float stack = height - SHELF_HEADER_HEIGHT - SHELF_CAPTION_HEIGHT - (buttons > 0 ? SHELF_ACTION_BAR_HEIGHT : 10);
            if (fabsf(stack / width - 0.66f) > 0.01f) ratio = false;
            if (list_width < width + 60) wide = false;
        }
        CHECK(ratio, "堆叠区域比例不一致");
        CHECK(wide, "平铺开后过窄");
        // 平铺开时一行放 4、5、6 张牌：中档 5 × 70 + 22。
        CHECK(expanded[0] < expanded[1] && expanded[1] < expanded[2] && expanded[1] == 372, "展开后的宽度没有随三档尺寸变化");
    }

    // 小窗以鼠标为中心并保持在屏幕内（和探针窗口用同一个算法）。
    POINT placed = probe_place((POINT){ 500, 400 }, (RECT){ 0, 0, 1000, 800 }, 300, 300);
    CHECK(placed.x == 350 && placed.y == 250, "小窗没有以鼠标位置为中心");
    placed = probe_place((POINT){ 10, 10 }, (RECT){ 0, 0, 1000, 800 }, 300, 300);
    CHECK(placed.x == 0 && placed.y == 0, "左上角位置超出屏幕");
    placed = probe_place((POINT){ 990, 790 }, (RECT){ 0, 0, 1000, 800 }, 300, 300);
    CHECK(placed.x == 700 && placed.y == 500, "右下角位置超出屏幕");
    placed = probe_place((POINT){ -500, 300 }, (RECT){ -1920, 0, 0, 1080 }, 300, 300);
    CHECK(placed.x == -650 && placed.y == 150, "左侧副屏位置不正确");
    theme_set_look(LOOK_SIMPLE);
}

/// C# 版 TestShelfWindowActions：按钮数量、标题、拖出方式、切换语言、按钮布局、置顶、展开收起、关闭后记入最近关闭并能恢复。
static void test_shelf_window_actions_cards(void)
{
    // 纸牌风格下再跑一遍（和 style 分支原来的测试相同），跑完换回简洁。
    theme_set_look(LOOK_CARDS);
    language_set_mode(LANGUAGE_CHINESE);
    HRESULT ole = OleInitialize(NULL);
    task_initialize();
    wchar_t *history_file = path_join(temporary_directory, L"history-window.json");
    DeleteFileW(history_file);
    shelf_history_load(history_file);
    set_shelf_window_settings(LAYOUT_FOUR, TRANSFER_MOVE);
    ShelfWindow *shelf = shelves_open_at((POINT){ 600, 400 }, false, false, EDGE_NONE);
    CHECK(shelf != NULL, "没有打开置物架");
    if (!shelf)
    {
        free(history_file);
        if (SUCCEEDED(ole)) OleUninitialize();
        theme_set_look(LOOK_SIMPLE);
        return;
    }
    pump_for(600);
    HWND handle = shelf->handle;
    CHECK(shelf->actions.count == LAYOUT_FOUR, "快速操作按钮数量不正确");
    CHECK(shelf_window_is_reusable(shelf), "空置物架应可复用");
    CHECK(window_title_is(handle, L"DropShelf 置物架 - 置物架"), "置物架窗口标题不正确");
    CHECK(shelf->move_mode && wcscmp(S(STR_TransferMove), L"移动") == 0, "默认拖出方式应显示为移动");

    language_set_mode(LANGUAGE_ENGLISH);
    pump_for(50);
    CHECK(shelf->action_tooltips[0] && wcsncmp(shelf->action_tooltips[0], L"Compress: ", 10) == 0, "切换语言后快速操作提示没有更新");
    CHECK(window_title_is(handle, L"DropShelf shelf - Shelf") && wcscmp(S(STR_TransferMove), L"Move") == 0, "切换语言后置物架文字没有更新");
    language_set_mode(LANGUAGE_CHINESE);
    pump_for(50);

    set_shelf_window_settings(LAYOUT_TWO, TRANSFER_COPY);
    CHECK(!shelf->move_mode, "拖出方式没有更新");
    CHECK(shelf->actions.count == 2, "两个按钮时应显示两个操作");
    int two_height = window_height(handle);
    set_shelf_window_settings(LAYOUT_FOUR, TRANSFER_COPY);
    CHECK(abs(window_height(handle) - two_height) <= 1, "两个和四个按钮的置物架高度应相同");
    shelf_layout(shelf);
    bool one_row = shelf->actions.count == 4;
    for (int index = 1; index < shelf->actions.count; index++)
        if (shelf->layout.action_buttons[index].top != shelf->layout.action_buttons[0].top) one_row = false;
    CHECK(one_row, "四个操作时按钮应排成一行");
    set_shelf_window_settings(LAYOUT_OFF, TRANSFER_COPY);
    CHECK(shelf->actions.count == 0, "关闭快速操作后仍显示按钮");
    set_shelf_window_settings(LAYOUT_FOUR, TRANSFER_MOVE);

    wchar_t *folder = test_folder(L"shelf-window-actions");
    wchar_t *file = test_file(folder, L"a.txt", "x");
    shelf_layout(shelf);
    CHECK(rect_is_empty(shelf->layout.view), "空置物架不应显示文件堆/列表切换按钮");
    StrList paths = {0};
    strlist_add(&paths, file);
    shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
    pump_for(500);
    shelf_layout(shelf);
    CHECK(shelf->model->count == 1 && !rect_is_empty(shelf->layout.view), "只有一项时也应显示文件堆/列表切换按钮");
    strlist_add(&paths, folder);
    shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
    pump_for(500);
    CHECK(shelf->model->count == 2 && !shelf_window_is_reusable(shelf), "有内容的置物架不应被复用");
    CHECK(shelf->model->pinned && (GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST), "置物架默认应置顶");
    shelf_layout(shelf);
    click_rect(shelf->ui, shelf->layout.pin);
    CHECK(!shelf->model->pinned && !(GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST), "取消钉子后置物架不应再置顶");
    click_rect(shelf->ui, shelf->layout.pin);
    CHECK(shelf->model->pinned && (GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST), "再点钉子后置物架应恢复置顶");

    // 切换时窗口先平滑变大、牌再一张张发到格子，等动画播完再检查。
    float scale = shelf_scale(shelf);
    int collapsed_width = window_width(handle);
    click_rect(shelf->ui, shelf->layout.view);
    pump_for(900);
    CHECK(shelf->model->expanded, "点击切换按钮没有平铺开");
    CHECK(window_width(handle) >= (int)lroundf(shelf_width_for(SHELF_SIZE_MEDIUM) * scale) + (int)(60 * scale), "展开后宽度没有变化");

    // 平铺时继续放入项目：一行放满之后再放一张时窗口变高。
    int columns = cards_shelf_grid_columns(shelf->size);
    shelf_layout(shelf);
    CHECK(shelf->layout.columns == columns, "平铺一行放的牌数不对");
    int one_row_height = window_height(handle);
    wchar_t *extras[8] = {0};
    for (int index = 0; index < columns - 1 && index < 8; index++)
    {
        wchar_t name[32];
        swprintf(name, ARRAYSIZE(name), L"extra%d.txt", index);
        extras[index] = test_file(folder, name, "y");
        strlist_add(&paths, extras[index]);
    }
    shelf_window_add_paths(shelf, &paths);
    strlist_clear(&paths);
    pump_for(800);
    CHECK(shelf->model->count == columns + 1 && window_height(handle) > one_row_height, "平铺时放入项目后牌或高度没有更新");
    int later_count = shelf->model->count > 2 ? shelf->model->count - 2 : 0;
    ShelfItem *later[8];
    for (int index = 0; index < later_count && index < 8; index++) later[index] = shelf->model->items[2 + index];
    shelf_remove_items(shelf, later, later_count < 8 ? later_count : 8);
    pump_for(600);
    shelf_layout(shelf);
    click_rect(shelf->ui, shelf->layout.view);
    pump_for(900);
    CHECK(!shelf->model->expanded && abs(window_width(handle) - collapsed_width) <= 1 && !shelf->switching && !shelf->card_posed[0],
        "叠起来的动画结束后应回到堆叠，窗口恢复收起时的宽度");

    // 关掉以后记入最近关闭，恢复时项目原样回来。
    shelf_window_close_animated(shelf);
    CHECK(pump_until_done(no_shelves_open, 3000), "置物架没有关掉");
    ShelfHistoryEntry *latest = shelf_history_latest();
    CHECK(latest && latest->count == 2 && wcs_equal(latest->items[0].path, file) &&
              wcs_equal(latest->items[1].path, folder), "关闭的置物架没有记入最近关闭");
    if (latest)
    {
        shelves_restore(latest, NULL);
        pump_for(800);
        int count = 0;
        shelf_history_entries(&count);
        CHECK(shelves_open_count() == 1 && count == 0, "最近关闭的置物架没有恢复");
    }
    shelves_close_all();
    pump_for(300);
    shelf_history_clear();
    shelf_history_flush();
    DeleteFileW(history_file);
    free(history_file);
    for (int index = 0; index < 8; index++) free(extras[index]);
    free(file);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
    theme_set_look(LOOK_SIMPLE);
}
