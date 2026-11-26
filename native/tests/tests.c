// Windows 专有行为与核心回归测试（纯逻辑测试由 linux/tests 覆盖，不在此重复）。
// 设置、日志和单实例名称都写到临时文件夹（通过环境变量），不碰真实的设置。
#define COBJMACROS
#include "app.h"
#include "archive.h"
#include "attachment_cache.h"
#include "base.h"
#include "drop_data.h"
#include "drop_read.h"
#include "file_identity.h"
#include "file_ops.h"
#include "folder_watch.h"
#include "foreground.h"
#include "hotkey.h"
#include "image_tools.h"
#include "lang.h"
#include "miniz.h"
#include "quick_action.h"
#include "settings.h"
#include "shelf_model.h"
#include "shelf_peek.h"
#include "single_instance.h"
#include "startup.h"
#include "strings.h"
#include "task.h"
#include "thumbnail.h"

#include <limits.h>
#include <math.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

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

static void write_bytes(const wchar_t *path, const char *bytes)
{
    file_write_atomic(path, bytes, strlen(bytes));
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

static bool owned_text_is(wchar_t *text, const wchar_t *expected)
{
    bool same = text && wcscmp(text, expected) == 0;
    if (!same) wprintf(L"      实际：%ls\n", text ? text : L"(null)");
    free(text);
    return same;
}

// ───────────── 全局快捷键 ─────────────

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

    // 版本号写在四处（见仓库根目录的 CLAUDE.md），CMakeLists.txt 之外的三处要一致。
    wchar_t version_text[64], define[96], info[96];
    swprintf(version_text, 64, L"version=\"%d.%d.%d.0\"", major, minor, patch);
    swprintf(define, 96, L"#define AppVersion \"%ls\"", DROPSHELF_VERSION);
    swprintf(info, 96, L"VersionInfoVersion=%ls.0", DROPSHELF_VERSION);
    wchar_t *app_manifest = repository_text(L"native/res/app.manifest");
    wchar_t *installer = repository_text(L"installer/DropShelf.iss");
    wchar_t *uninstaller = repository_text(L"installer/UninstallLauncher.iss");
    CHECK(app_manifest && wcsstr(app_manifest, version_text), "app.manifest 的版本号与 CMakeLists.txt 不一致");
    CHECK(installer && wcsstr(installer, define) && wcsstr(installer, info), "DropShelf.iss 的版本号与 CMakeLists.txt 不一致");
    CHECK(uninstaller && wcsstr(uninstaller, define) && wcsstr(uninstaller, info), "UninstallLauncher.iss 的版本号与 CMakeLists.txt 不一致");
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

static void test_foreground_watcher(void)
{
    wchar_t *self = process_file_name(GetCurrentProcessId());
    CHECK(self && wcs_equal_ignore_case(self, L"DropShelfTests.exe"), "读不出进程的程序文件名");
    StrList apps;
    foreground_windowed_apps(&apps);
    CHECK(strlist_index_of(&apps, L"DropShelfTests.exe", true) < 0, "有窗口的程序里不应包含自己");
    bool sorted = true;
    for (int index = 1; index < apps.count; index++)
        if (CompareStringOrdinal(apps.items[index - 1], -1, apps.items[index], -1, TRUE) != CSTR_LESS_THAN) sorted = false;
    CHECK(sorted, "有窗口的程序没有按名称排好、去掉重复");
    strlist_clear(&apps);

    // 屏蔽列表为空时从不算屏蔽；屏蔽自己后，光标下是自己的窗口时算屏蔽。
    StrList blocked = {0};
    foreground_watcher_set_blocked_apps(&blocked);
    CHECK(!foreground_watcher_check(), "屏蔽列表为空时不应暂停");
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP | WS_VISIBLE,
        0, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
    RECT rect;
    GetWindowRect(window, &rect);
    strlist_add(&blocked, L"dropshelftests.EXE");
    foreground_watcher_set_blocked_apps(&blocked);
    POINT inside = { rect.left + 100, rect.top + 100 };
    CHECK(GetAncestor(WindowFromPoint(inside), GA_ROOT) != window || foreground_watcher_is_blocked_at(inside),
        "光标下是屏蔽的程序时没有算屏蔽");
    DestroyWindow(window);
    foreground_watcher_stop();
    strlist_clear(&blocked);
    free(self);
}

// ───────────── 拖放内容的判断 ─────────────

static void set_bytes(IDataObject *data, const wchar_t *format_name, CLIPFORMAT format, const void *bytes, size_t size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    void *target = GlobalLock(global);
    if (size) memcpy(target, bytes, size);
    GlobalUnlock(global);
    FORMATETC request = { format_name ? drop_format(format_name) : format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = { TYMED_HGLOBAL };
    medium.hGlobal = global;
    IDataObject_SetData(data, &request, &medium, TRUE);
}

static void set_unicode(IDataObject *data, const wchar_t *format_name, CLIPFORMAT format, const wchar_t *text)
{
    set_bytes(data, format_name, format, text, (wcslen(text) + 1) * sizeof(wchar_t));
}

static void set_utf8(IDataObject *data, const wchar_t *format_name, const wchar_t *text)
{
    size_t length;
    char *bytes = utf8_from_wide(text, &length);
    set_bytes(data, format_name, 0, bytes, length + 1);
    free(bytes);
}

static void set_file_drop(IDataObject *data, const wchar_t *path)
{
    size_t length = wcslen(path);
    size_t size = sizeof(DROPFILES) + (length + 2) * sizeof(wchar_t);
    unsigned char *bytes = xcalloc(1, size);
    DROPFILES *files = (DROPFILES *)bytes;
    files->pFiles = sizeof(DROPFILES);
    files->fWide = TRUE;
    memcpy(bytes + sizeof(DROPFILES), path, length * sizeof(wchar_t));
    set_bytes(data, NULL, CF_HDROP, bytes, size);
    free(bytes);
}

static void set_descriptor(IDataObject *data, const wchar_t *name)
{
    struct { UINT count; FILEDESCRIPTORW descriptor; } group = {0};
    group.count = 1;
    wcscpy_s(group.descriptor.cFileName, MAX_PATH, name);
    set_bytes(data, CFSTR_FILEDESCRIPTORW, 0, &group, sizeof(group));
}

static IDataObject *new_data(void)
{
    IDataObject *data = NULL;
    SHCreateDataObject(NULL, 0, NULL, NULL, &IID_IDataObject, (void **)&data);
    return data;
}

static wchar_t *file_url(const wchar_t *path)
{
    wchar_t url[2048];
    DWORD length = 2048;
    return UrlCreateFromPathW(path, url, &length, 0) == S_OK ? wcs_dup(url) : wcs_dup(L"");
}

static void test_drop_classify(void)
{
    HRESULT ole = OleInitialize(NULL);
    wchar_t *folder = path_join(temporary_directory, L"classify");
    directory_create(folder);
    wchar_t *file = path_join(folder, L"a.txt");
    write_bytes(file, "x");
    wchar_t *image = path_join(folder, L"qq-image.png");
    write_bytes(image, "\x89PNG");
    wchar_t *image_url = file_url(image);

    IDataObject *data = new_data();
    CHECK(data != NULL, "无法创建测试用的数据对象");
    if (!data) return;
    set_file_drop(data, file);
    CHECK(drop_classify(data) == DROP_FILES, "文件没有识别");
    IDataObject_Release(data);

    data = new_data();
    set_unicode(data, NULL, CF_UNICODETEXT, L"hello");
    CHECK(drop_classify(data) == DROP_TEXT, "文本没有识别");
    IDataObject_Release(data);

    data = new_data();
    set_unicode(data, L"UniformResourceLocatorW", 0, L"https://example.com");
    CHECK(drop_classify(data) == DROP_LINK, "链接没有识别");
    IDataObject_Release(data);

    data = new_data();
    set_bytes(data, L"PNG", 0, "\x89PNG", 4);
    CHECK(drop_classify(data) == DROP_IMAGE, "图片没有识别");
    IDataObject_Release(data);

    data = new_data();
    CHECK(drop_classify(data) == DROP_NONE && !drop_source_shelf_id(data), "空数据被误识别");
    set_unicode(data, SHELF_ID_FORMAT, 0, L"abc");
    wchar_t *shelf = drop_source_shelf_id(data);
    CHECK(shelf && wcscmp(shelf, L"abc") == 0, "来源置物架没有读出来");
    free(shelf);
    IDataObject_Release(data);
    CHECK(drop_can_provide_files(DROP_IMAGE) && !drop_can_provide_files(DROP_TEXT), "快速操作可用内容判断不正确");

    // 虚拟附件；全是 .url 又带链接时按链接处理。
    data = new_data();
    set_descriptor(data, L"报告.docx");
    CHECK(drop_classify(data) == DROP_VIRTUAL_FILES, "标准 Windows 虚拟附件未被识别");
    IDataObject_Release(data);
    data = new_data();
    set_descriptor(data, L"Example.URL");
    set_unicode(data, L"UniformResourceLocatorW", 0, L"https://example.com");
    CHECK(drop_classify(data) == DROP_LINK, "浏览器拖出的快捷方式没有按链接处理");
    IDataObject_Release(data);

    // 聊天软件。
    data = new_data();
    set_bytes(data, L"image/png", 0, "\x89PNG", 4);
    set_unicode(data, L"UniformResourceLocatorW", 0, L"https://example.com/a.png");
    CHECK(drop_classify(data) == DROP_IMAGE, "带有图片数据的拖动被当成了链接");
    IDataObject_Release(data);

    WStr xml = {0};
    wstr_appendf(&xml, L"<QQRichEditFormat><Info version=\"1001\"></Info><EditElement type=\"1\" filepath=\"%ls\" shortcut=\"\"></EditElement></QQRichEditFormat>", image);
    data = new_data();
    set_utf8(data, L"QQ_Unicode_RichEdit_Format", xml.data);
    set_unicode(data, NULL, CF_UNICODETEXT, L"[图片]");
    CHECK(drop_classify(data) == DROP_FILES, "旧版 QQ 拖出的图片没有识别");
    IDataObject_Release(data);
    StrList paths;
    chat_parse_qq_rich_edit(xml.data, &paths);
    CHECK(paths.count == 1 && wcscmp(paths.items[0], image) == 0, "旧版 QQ 拖出的图片路径不正确");
    strlist_clear(&paths);
    wstr_free(&xml);

    data = new_data();
    set_unicode(data, L"FileNameW", 0, image);
    CHECK(drop_classify(data) == DROP_FILES, "只提供 FileNameW 的来源没有识别");
    IDataObject_Release(data);

    WStr html = {0};
    wstr_appendf(&html, L"<html><body><!--StartFragment--><img src=\"%ls\"><!--EndFragment--></body></html>", image_url);
    data = new_data();
    set_utf8(data, L"HTML Format", html.data);
    CHECK(drop_classify(data) == DROP_FILES, "只含本地图片的网页片段没有识别为文件");
    IDataObject_Release(data);
    wstr_free(&html);
    wstr_appendf(&html, L"<!--StartFragment-->说明<img src=\"%ls\"><!--EndFragment-->", image_url);
    data = new_data();
    set_utf8(data, L"HTML Format", html.data);
    set_unicode(data, NULL, CF_UNICODETEXT, L"说明");
    CHECK(drop_classify(data) == DROP_TEXT, "带文字的网页片段不应只取图片");
    IDataObject_Release(data);
    wstr_free(&html);

    // 富文本里的网络路径一律不认。
    wstr_appendf(&html, L"<img src=\"%ls\">", image_url);
    chat_parse_html_local_images(html.data, &paths);
    CHECK(paths.count == 1 && wcscmp(paths.items[0], image) == 0, "本机图片路径没有识别");
    strlist_clear(&paths);
    wstr_free(&html);
    chat_parse_html_local_images(L"<img src=\"file://server/share/a.png\">", &paths);
    CHECK(paths.count == 0, "网页片段中的网络共享路径没有被拒绝");
    strlist_clear(&paths);
    chat_parse_qq_rich_edit(L"<QQRichEditFormat><EditElement type=\"1\" filepath=\"\\\\server\\share\\a.png\"></EditElement></QQRichEditFormat>", &paths);
    CHECK(paths.count == 0, "QQ 富文本中的网络共享路径没有被拒绝");
    strlist_clear(&paths);
    chat_parse_qq_rich_edit(L"<QQRichEditFormat><EditElement type=\"0\"><![CDATA[你好]]></EditElement>"
                            L"<EditElement type=\"1\" filepath=\"C:\\a.png\"></EditElement></QQRichEditFormat>", &paths);
    CHECK(paths.count == 0, "QQ 富文本里有文字时不应只取图片");
    strlist_clear(&paths);

    wchar_t *decoded = html_decode(L"a&amp;b&#20013;&#x4E2D;&unknown;&#xD800;&", 41);
    CHECK(wcscmp(decoded, L"a&b中中&unknown;&#xD800;&") == 0, "字符引用没有按 .NET 的方式解开");
    free(decoded);

    free(image_url);
    free(image);
    free(file);
    free(folder);
    if (SUCCEEDED(ole)) OleUninitialize();
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


#include "action_tests.inc"

/// 命令行给了名字时只跑名字里含有其中之一的测试，方便单独排查。
static char **only_tests;
static int only_test_count;

static void run(const char *name, void (*test)(void))
{
    bool selected = only_test_count == 0;
    for (int index = 0; index < only_test_count && !selected; index++) selected = strstr(name, only_tests[index]) != NULL;
    if (!selected) return;
    current_test = name;
    int before = failures;
    test();
    printf("%s %s\n", failures == before ? "ok    " : "FAILED", name);
}

int main(int argc, char **argv)
{
    only_tests = argv + 1;
    only_test_count = argc - 1;
    // 设置、日志和实例名称都换成测试专用的，不碰正在使用的 DropShelf。
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    // 临时文件夹可能是 8.3 短名称（GitHub 的构建机就是 C:\Users\RUNNER~1\...），换成完整名称：
    // 按文件标识找回的路径总是完整名称，拖进来的路径通常也是。
    wchar_t long_base[MAX_PATH];
    DWORD long_length = GetLongPathNameW(base, long_base, MAX_PATH);
    if (long_length > 0 && long_length < MAX_PATH) wcscpy_s(base, MAX_PATH, long_base);
    wchar_t folder[64];
    swprintf(folder, 64, L"DropShelfCTests-%lu", GetCurrentProcessId());
    temporary_directory = path_join(base, folder);
    directory_create(temporary_directory);
    wchar_t *settings_file = path_join(temporary_directory, L"settings.json");
    wchar_t *logs = path_join(temporary_directory, L"Logs");
    SetEnvironmentVariableW(L"DROPSHELF_SETTINGS_PATH", settings_file);
    SetEnvironmentVariableW(L"DROPSHELF_LOG_DIRECTORY", logs);
    SetEnvironmentVariableW(L"DROPSHELF_INSTANCE", L"ctest");
    settings_load((AppSettings *)app_settings());

    run("program resources", test_program_resources);
    run("startup command", test_startup_command);
    run("single instance", test_single_instance);
    run("argument forwarding", test_argument_forwarding);
    run("hotkey registration", test_hotkey_registration);
    run("foreground watcher", test_foreground_watcher);
    run("drop classify", test_drop_classify);
    run("drop read", test_drop_read);
    run("shell thumbnail and imaging thread", test_shell_thumbnail);
    run("file operations", test_file_operations);
    run("archive", test_archive);
    run("archive already compressed", test_archive_already_compressed);
    run("image tools", test_image_tools);
    run("long paths", test_long_paths);
    run("quick actions", test_quick_actions);
    run("follow renamed", test_follow_renamed);
    run("folder watch names", test_folder_watch_names);
    run("folder watch", test_folder_watch);

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
