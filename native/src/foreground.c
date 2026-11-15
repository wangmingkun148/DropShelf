#include "foreground.h"
#include "log.h"

#include <dwmapi.h>
#include <tlhelp32.h>
#include <stdlib.h>

static StrList blocked;
static HWINEVENTHOOK hook;
static bool is_blocked;
static void (*listener)(bool blocked);

/// 带反作弊等保护的进程不允许打开，改从系统的进程列表读取名称。
/// 与 C# 版的 Process.ProcessName + ".exe" 相同：名称本来以 .exe 结尾时不重复加。
static wchar_t *protected_process_file_name(DWORD process_id)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return NULL;
    wchar_t *name = NULL;
    PROCESSENTRY32W entry = { .dwSize = sizeof(entry) };
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !name; more = Process32NextW(snapshot, &entry))
    {
        if (entry.th32ProcessID != process_id) continue;
        size_t length = wcslen(entry.szExeFile);
        if (length >= 4 && CompareStringOrdinal(entry.szExeFile + length - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL) length -= 4;
        WStr text = {0};
        wstr_append_n(&text, entry.szExeFile, length);
        wstr_append(&text, L".exe");
        name = wstr_take(&text);
    }
    CloseHandle(snapshot);
    return name;
}

wchar_t *process_file_name(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return protected_process_file_name(process_id);
    wchar_t path[1024];
    DWORD size = (DWORD)(sizeof(path) / sizeof(path[0]));
    wchar_t *name = QueryFullProcessImageNameW(process, 0, path, &size) ? wcs_dup(path_file_name(path)) : NULL;
    CloseHandle(process);
    return name;
}

static bool is_blocked_window(HWND window)
{
    if (!window) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t *name = process_file_name(process);
    bool result = name && strlist_index_of(&blocked, name, true) >= 0;
    free(name);
    return result;
}

static void update(HWND window)
{
    bool now_blocked = blocked.count > 0 && is_blocked_window(window);
    if (now_blocked == is_blocked) return;
    is_blocked = now_blocked;
    log_trace(now_blocked ? L"屏蔽的应用切到前台，暂停弹出置物架" : L"屏蔽的应用离开前台，恢复弹出置物架");
    if (listener) listener(now_blocked);
}

// 通知可能晚到、一次切换也可能连续收到几条，每次都以当前真正的前台窗口为准。
static void CALLBACK on_foreground_changed(HWINEVENTHOOK handle, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    (void)handle, (void)event, (void)window, (void)object, (void)child, (void)thread, (void)time;
    update(GetForegroundWindow());
}

static void unhook(void)
{
    if (!hook) return;
    UnhookWinEvent(hook);
    hook = NULL;
}

void foreground_watcher_set_listener(void (*changed)(bool blocked))
{
    listener = changed;
}

void foreground_watcher_set_blocked_apps(const StrList *names)
{
    strlist_clear(&blocked);
    // 和 C# 版的 HashSet 一样去掉重复的名称。
    for (int index = 0; index < names->count; index++)
        if (strlist_index_of(&blocked, names->items[index], true) < 0) strlist_add(&blocked, names->items[index]);
    if (blocked.count > 0 && !hook)
        hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_foreground_changed, 0, 0, WINEVENT_OUTOFCONTEXT);
    else if (blocked.count == 0) unhook();
    update(GetForegroundWindow());
}

bool foreground_watcher_is_blocked(void)
{
    return is_blocked;
}

bool foreground_watcher_check(void)
{
    update(GetForegroundWindow());
    return is_blocked;
}

bool foreground_watcher_is_blocked_at(POINT point)
{
    return foreground_watcher_check() ||
           (blocked.count > 0 && is_blocked_window(GetAncestor(WindowFromPoint(point), GA_ROOT)));
}

void foreground_watcher_stop(void)
{
    unhook();
    strlist_clear(&blocked);
    is_blocked = false;
}

// ───────────── 有窗口的程序 ─────────────

typedef struct ProcessSet
{
    DWORD *items;
    int count;
    int capacity;
} ProcessSet;

static BOOL CALLBACK collect_window(HWND window, LPARAM parameter)
{
    ProcessSet *set = (ProcessSet *)parameter;
    // 与任务栏相同的筛选：可见、没有所有者、有标题，且没有被系统隐藏（挂起的应用商店应用）。
    int cloaked = 0;
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || GetWindowTextLengthW(window) <= 0) return TRUE;
    if (DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK && cloaked != 0) return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    for (int index = 0; index < set->count; index++)
        if (set->items[index] == process) return TRUE;
    if (set->count == set->capacity)
    {
        set->capacity = set->capacity ? set->capacity * 2 : 32;
        set->items = xrealloc(set->items, (size_t)set->capacity * sizeof(DWORD));
    }
    set->items[set->count++] = process;
    return TRUE;
}

static int compare_names(const void *first, const void *second)
{
    const wchar_t *a = *(const wchar_t *const *)first, *b = *(const wchar_t *const *)second;
    return CompareStringOrdinal(a, -1, b, -1, TRUE) - CSTR_EQUAL;
}

void foreground_windowed_apps(StrList *names)
{
    *names = (StrList){0};
    ProcessSet set = {0};
    EnumWindows(collect_window, (LPARAM)&set);
    DWORD self = GetCurrentProcessId();
    for (int index = 0; index < set.count; index++)
    {
        if (set.items[index] == self) continue;
        wchar_t *name = process_file_name(set.items[index]);
        if (name && strlist_index_of(names, name, true) < 0) strlist_add_owned(names, name);
        else free(name);
    }
    free(set.items);
    if (names->count > 1) qsort(names->items, (size_t)names->count, sizeof(wchar_t *), compare_names);
}
