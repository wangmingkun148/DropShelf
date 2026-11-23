#include "folder_watch.h"
#include "log.h"
#include "power.h"
#include "task.h"

#include <string.h>

#ifndef FILE_FLAG_OPEN_NO_RECALL
#define FILE_FLAG_OPEN_NO_RECALL 0x00100000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

#define BUFFER_SIZE (64 * 1024)
#define CHECK_INTERVAL 1000             // 还没写完的文件每秒看一次
#define RETRY_INTERVAL 30000            // 打不开的文件夹（拔掉的 U 盘、断开的网络位置）过一会儿再试
#define PENDING_LIMIT (30 * 60 * 1000)  // 半小时还没写完的不再等
#define PENDING_MAXIMUM 2000
#define GONE_MEMORY 5000                // 刚删掉或改走的名字记几秒，用来认出“保存时替换原文件”
#define RESCAN_WINDOW 30                // 通知太多来不及收时，把最近这么多秒里建的文件补上
#define OWN_MEMORY (10 * 60 * 1000)     // 快速操作写出的文件记这么久
#define DRAG_MEMORY (30 * 60 * 1000)
#define DRAG_MARGIN 5000                // 拖出结束后资源管理器可能还要一会儿才建出文件

// ───────────── 名字 ─────────────

static bool ends_with(const wchar_t *name, const wchar_t *suffix)
{
    size_t length = wcslen(name), suffix_length = wcslen(suffix);
    return length > suffix_length && _wcsicmp(name + length - suffix_length, suffix) == 0;
}

static bool starts_with(const wchar_t *name, const wchar_t *prefix)
{
    return _wcsnicmp(name, prefix, wcslen(prefix)) == 0;
}

/// 浏览器和下载工具下载中的文件：Chrome、Edge、Firefox、Opera、旧版 IE、WinSCP、迅雷、qBittorrent、µTorrent、BitComet、百度网盘等。
static const wchar_t *const partial_suffixes[] = {
    L".crdownload", L".part", L".partial", L".download", L".opdownload", L".filepart", L".downloading",
    L".xltd", L".td", L".!qb", L".!ut", L".!bt", L".bc!", L".aria2", L".tmp", L".temp",
};

bool folder_watch_is_partial(const wchar_t *name)
{
    for (size_t index = 0; index < ARRAYSIZE(partial_suffixes); index++)
        if (ends_with(name, partial_suffixes[index])) return true;
    return false;
}

bool folder_watch_is_ignored(const wchar_t *name)
{
    if (!name || !*name || wcschr(name, L'\\')) return true;
    // Office 和 LibreOffice 打开文档时旁边的锁文件、保存时的临时文件；迅雷的下载配置。
    static const wchar_t *const prefixes[] = { L"~$", L"~WRL", L".~lock." };
    for (size_t index = 0; index < ARRAYSIZE(prefixes); index++)
        if (starts_with(name, prefixes[index])) return true;
    static const wchar_t *const names[] = { L"desktop.ini", L"thumbs.db", L".DS_Store" };
    for (size_t index = 0; index < ARRAYSIZE(names); index++)
        if (_wcsicmp(name, names[index]) == 0) return true;
    return ends_with(name, L".td.cfg") || ends_with(name, L".xltd.cfg");
}

static bool is_candidate(const wchar_t *name)
{
    return !folder_watch_is_ignored(name) && !folder_watch_is_partial(name);
}

// ───────────── 后台线程 ─────────────

typedef struct Watch
{
    wchar_t *folder;
    HANDLE directory;
    OVERLAPPED overlapped;
    DWORD *buffer;              // ReadDirectoryChangesW 要求按 DWORD 对齐
    bool reading;
    bool reported;              // 打不开只记一次日志
    ULONGLONG retry_at;
    wchar_t *renamed_from;      // 改名时先来旧名字，下一条是新名字
} Watch;

/// 已经出现、等它写完的文件或文件夹。
typedef struct Pending
{
    wchar_t *path;
    ULONGLONG first_seen;
    bool checked;
    long long size;
    FILETIME written;
    int quiet;                  // 连续几次检查都没变
} Pending;

typedef struct Gone
{
    wchar_t *path;
    ULONGLONG at;
    bool was_pending;           // 是我们自己在等的（比如 Firefox 先放的空占位文件）
} Gone;

/// 每次启动一个线程配一份。停下时线程可能卡在打不开的网络位置上一时退不出来，所以各用各的，谁最后放手谁释放。
typedef struct WatchState
{
    LONG references;
    unsigned generation;
    HANDLE wake;
    SRWLOCK lock;
    StrList wanted;             // 以下三项由界面线程改，持锁读写
    bool changed;
    bool stopping;

    Watch **watches;            // 以下只有后台线程用。每个单独分配：读取进行中时系统记着 OVERLAPPED 的地址，不能挪动
    int watch_count;
    Pending *pending;
    int pending_count, pending_capacity;
    Gone *gone;
    int gone_count, gone_capacity;
    ULONGLONG last_check;
    bool overflow_reported;
} WatchState;

typedef struct Batch
{
    unsigned generation;
    StrList paths;
    ULONGLONG *seen;
} Batch;

static void batch_free(Batch *batch)
{
    strlist_clear(&batch->paths);
    free(batch->seen);
    free(batch);
}

static void deliver(void *data);

static void state_release(WatchState *state)
{
    if (InterlockedDecrement(&state->references) > 0) return;
    strlist_clear(&state->wanted);
    if (state->wake) CloseHandle(state->wake);
    free(state);
}

static void close_watch(Watch *watch)
{
    if (watch->directory && watch->directory != INVALID_HANDLE_VALUE)
    {
        if (watch->reading)
        {
            // 等取消真正完成，缓冲区才不会再被写。
            DWORD bytes;
            CancelIoEx(watch->directory, &watch->overlapped);
            GetOverlappedResult(watch->directory, &watch->overlapped, &bytes, TRUE);
        }
        CloseHandle(watch->directory);
    }
    watch->directory = NULL;
    watch->reading = false;
    free(watch->renamed_from);
    watch->renamed_from = NULL;
}

static void free_watch(Watch *watch)
{
    close_watch(watch);
    if (watch->overlapped.hEvent) CloseHandle(watch->overlapped.hEvent);
    free(watch->buffer);
    free(watch->folder);
    free(watch);
}

static void fail_watch(Watch *watch, DWORD error)
{
    if (!watch->reported)
    {
        log_win32_error(L"无法观察文件夹，稍后重试", error);
        log_info(L"无法观察的文件夹：%ls", watch->folder);
        watch->reported = true;
    }
    close_watch(watch);
    watch->retry_at = GetTickCount64() + RETRY_INTERVAL;
}

static void start_read(Watch *watch)
{
    ResetEvent(watch->overlapped.hEvent);
    if (ReadDirectoryChangesW(watch->directory, watch->buffer, BUFFER_SIZE, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
            NULL, &watch->overlapped, NULL))
        watch->reading = true;
    else fail_watch(watch, GetLastError());
}

static void open_watch(Watch *watch)
{
    watch->directory = io_create_file(watch->folder, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (watch->directory == INVALID_HANDLE_VALUE)
    {
        watch->directory = NULL;
        fail_watch(watch, GetLastError());
        return;
    }
    if (watch->reported) log_info(L"开始观察文件夹：%ls", watch->folder);
    watch->reported = false;
    start_read(watch);
}

// ───────────── 等写完 ─────────────

static int pending_index(WatchState *state, const wchar_t *path)
{
    for (int index = 0; index < state->pending_count; index++)
        if (_wcsicmp(state->pending[index].path, path) == 0) return index;
    return -1;
}

static void remove_pending(WatchState *state, int index)
{
    free(state->pending[index].path);
    memmove(&state->pending[index], &state->pending[index + 1], (size_t)(state->pending_count - index - 1) * sizeof(Pending));
    state->pending_count--;
}

static void consider(WatchState *state, const wchar_t *path, ULONGLONG now)
{
    if (!is_candidate(path_file_name(path)) || pending_index(state, path) >= 0) return;
    if (state->pending_count >= PENDING_MAXIMUM)
    {
        if (!state->overflow_reported) log_info(L"观察的文件夹里一次出现的新文件太多，超过 %d 个的不再提示", PENDING_MAXIMUM);
        state->overflow_reported = true;
        return;
    }
    if (state->pending_count == state->pending_capacity)
    {
        state->pending_capacity = state->pending_capacity ? state->pending_capacity * 2 : 16;
        state->pending = xrealloc(state->pending, (size_t)state->pending_capacity * sizeof(Pending));
    }
    state->pending[state->pending_count++] = (Pending){ .path = wcs_dup(path), .first_seen = now };
}

static void remember_gone(WatchState *state, const wchar_t *path, bool was_pending, ULONGLONG now)
{
    // 顺手去掉过期的。
    int kept = 0;
    for (int index = 0; index < state->gone_count; index++)
    {
        if (now - state->gone[index].at <= GONE_MEMORY) state->gone[kept++] = state->gone[index];
        else free(state->gone[index].path);
    }
    state->gone_count = kept;
    if (state->gone_count == state->gone_capacity)
    {
        state->gone_capacity = state->gone_capacity ? state->gone_capacity * 2 : 8;
        state->gone = xrealloc(state->gone, (size_t)state->gone_capacity * sizeof(Gone));
    }
    state->gone[state->gone_count++] = (Gone){ wcs_dup(path), now, was_pending };
}

/// 这个名字刚刚还是一个原有的文件：编辑器保存时先写临时文件，再换掉原文件，不算新文件。
static bool was_replaced(WatchState *state, const wchar_t *path, ULONGLONG now)
{
    for (int index = state->gone_count - 1; index >= 0; index--)
        if (now - state->gone[index].at <= GONE_MEMORY && _wcsicmp(state->gone[index].path, path) == 0) return !state->gone[index].was_pending;
    return false;
}

static bool drop_pending(WatchState *state, const wchar_t *path)
{
    int index = pending_index(state, path);
    if (index < 0) return false;
    remove_pending(state, index);
    return true;
}

static void handle_change(WatchState *state, Watch *watch, DWORD action, const wchar_t *name, ULONGLONG now)
{
    wchar_t *path = path_join(watch->folder, name);
    switch (action)
    {
        case FILE_ACTION_ADDED:
            if (!was_replaced(state, path, now)) consider(state, path, now);
            break;
        case FILE_ACTION_REMOVED:
            remember_gone(state, path, drop_pending(state, path), now);
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            free(watch->renamed_from);
            watch->renamed_from = wcs_dup(name);
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
        {
            const wchar_t *old_name = watch->renamed_from;
            wchar_t *old_path = old_name ? path_join(watch->folder, old_name) : NULL;
            int index = old_path ? pending_index(state, old_path) : -1;
            if (old_path) remember_gone(state, old_path, index >= 0, now);
            if (index >= 0)
            {
                // 还在等的文件改了名（比如“未确认 123.crdownload”改成真名）：接着等新名字。
                if (!is_candidate(name)) remove_pending(state, index);
                else
                {
                    free(state->pending[index].path);
                    state->pending[index].path = wcs_dup(path);
                    state->pending[index].checked = false;
                    state->pending[index].quiet = 0;
                }
            }
            // 从临时的名字改成正式的名字是下载完成；把一个原有的文件改个名不算新文件。
            else if (old_name && !is_candidate(old_name) && !was_replaced(state, path, now)) consider(state, path, now);
            free(old_path);
            free(watch->renamed_from);
            watch->renamed_from = NULL;
            break;
        }
    }
    free(path);
}

/// 通知太多、系统没来得及记下时，把最近建的补上。
static void rescan(WatchState *state, Watch *watch, ULONGLONG now)
{
    if (!path_is_directory(watch->folder))
    {
        fail_watch(watch, ERROR_PATH_NOT_FOUND);
        return;
    }
    FILETIME current;
    GetSystemTimeAsFileTime(&current);
    ULONGLONG limit = (((ULONGLONG)current.dwHighDateTime << 32) | current.dwLowDateTime) - (ULONGLONG)RESCAN_WINDOW * 10000000ULL;
    wchar_t *pattern = path_join(watch->folder, L"*");
    WIN32_FIND_DATAW data;
    HANDLE find = io_find_first(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return;
    do
    {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        ULONGLONG created = ((ULONGLONG)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
        if (created < limit) continue;
        wchar_t *path = path_join(watch->folder, data.cFileName);
        consider(state, path, now);
        free(path);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static void finish_read(WatchState *state, Watch *watch)
{
    DWORD bytes = 0;
    BOOL ok = GetOverlappedResult(watch->directory, &watch->overlapped, &bytes, FALSE);
    watch->reading = false;
    ULONGLONG now = GetTickCount64();
    if (!ok)
    {
        DWORD error = GetLastError();
        if (error != ERROR_NOTIFY_ENUM_DIR)
        {
            fail_watch(watch, error);
            return;
        }
        rescan(state, watch, now);
    }
    else if (bytes == 0) rescan(state, watch, now);
    else
    {
        const BYTE *at = (const BYTE *)watch->buffer;
        for (;;)
        {
            const FILE_NOTIFY_INFORMATION *info = (const FILE_NOTIFY_INFORMATION *)at;
            wchar_t *name = wcs_dup_n(info->FileName, info->FileNameLength / sizeof(wchar_t));
            handle_change(state, watch, info->Action, name, now);
            free(name);
            if (info->NextEntryOffset == 0) break;
            at += info->NextEntryOffset;
        }
    }
    if (watch->directory) start_read(watch);
}

typedef enum Readiness { READY, WAITING, DROPPED } Readiness;

/// Firefox 先放一个空的同名占位文件，真正的内容写在 .part 里；aria2 的进度记在 .aria2 里。
static bool has_partial_sibling(const wchar_t *path)
{
    static const wchar_t *const siblings[] = { L".part", L".aria2" };
    bool found = false;
    for (size_t index = 0; index < ARRAYSIZE(siblings) && !found; index++)
    {
        WStr sibling = {0};
        wstr_append(&sibling, path);
        wstr_append(&sibling, siblings[index]);
        found = io_get_attributes(sibling.data) != INVALID_FILE_ATTRIBUTES;
        wstr_free(&sibling);
    }
    return found;
}

/// 能不能在不允许别人同时写的情况下打开：下载工具、复制还没写完时打不开。
static bool nobody_writing(const wchar_t *path)
{
    HANDLE file = io_create_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_NO_RECALL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
        return true;
    }
    DWORD error = GetLastError();
    return error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION;
}

static Readiness check(Pending *pending, ULONGLONG now)
{
    if (now - pending->first_seen > PENDING_LIMIT) return DROPPED;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!io_get_attributes_ex(pending->path, GetFileExInfoStandard, &data)) return DROPPED;
    if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY)) return DROPPED;
    bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!directory && has_partial_sibling(pending->path))
    {
        pending->checked = false;
        pending->quiet = 0;
        return WAITING;
    }
    long long size = directory ? 0 : (long long)(((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow);
    bool same = pending->checked && size == pending->size && CompareFileTime(&data.ftLastWriteTime, &pending->written) == 0;
    pending->checked = true;
    pending->size = size;
    pending->written = data.ftLastWriteTime;
    if (!same)
    {
        pending->quiet = 0;
        return WAITING;
    }
    // 文件夹（解压出来的、整个挪进来的）里还在往下写时看不出来，多等一会儿没有变化再交出。
    if (directory) return ++pending->quiet >= 2 ? READY : WAITING;
    // 云盘里只有占位的文件不去打开，免得触发下载。
    bool placeholder = (data.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE)) != 0;
    if (!placeholder && !nobody_writing(pending->path))
    {
        pending->quiet = 0;
        return WAITING;
    }
    return READY;
}

static void check_pending(WatchState *state, ULONGLONG now)
{
    if (state->pending_count == 0 || now - state->last_check < CHECK_INTERVAL) return;
    state->last_check = now;
    Batch *batch = NULL;
    for (int index = 0; index < state->pending_count;)
    {
        Readiness readiness = check(&state->pending[index], now);
        if (readiness == WAITING)
        {
            index++;
            continue;
        }
        if (readiness == READY)
        {
            if (!batch)
            {
                batch = xcalloc(1, sizeof(Batch));
                batch->generation = state->generation;
                batch->seen = xalloc((size_t)state->pending_count * sizeof(ULONGLONG));
            }
            batch->seen[batch->paths.count] = state->pending[index].first_seen;
            strlist_add(&batch->paths, state->pending[index].path);
        }
        remove_pending(state, index);
    }
    if (state->pending_count == 0) state->overflow_reported = false;
    if (batch) task_post(deliver, batch);
}

/// 按界面线程给的列表增减要观察的文件夹。返回 false 表示要停下。
static bool apply_wanted(WatchState *state)
{
    AcquireSRWLockExclusive(&state->lock);
    bool stopping = state->stopping;
    StrList wanted = {0};
    bool changed = state->changed;
    if (changed) strlist_clone(&wanted, &state->wanted);
    state->changed = false;
    ReleaseSRWLockExclusive(&state->lock);
    if (stopping) return false;
    if (!changed) return true;
    int kept = 0;
    for (int index = 0; index < state->watch_count; index++)
    {
        if (strlist_index_of(&wanted, state->watches[index]->folder, true) >= 0) state->watches[kept++] = state->watches[index];
        else free_watch(state->watches[index]);
    }
    state->watch_count = kept;
    state->watches = xrealloc(state->watches, (size_t)(wanted.count ? wanted.count : 1) * sizeof(Watch *));
    for (int index = 0; index < wanted.count; index++)
    {
        bool known = false;
        for (int at = 0; at < kept && !known; at++) known = _wcsicmp(state->watches[at]->folder, wanted.items[index]) == 0;
        if (known) continue;
        Watch *watch = xcalloc(1, sizeof(Watch));
        state->watches[state->watch_count++] = watch;
        watch->folder = wcs_dup(wanted.items[index]);
        watch->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        watch->buffer = xalloc(BUFFER_SIZE);
        open_watch(watch);
    }
    strlist_clear(&wanted);
    return true;
}

static DWORD next_timeout(WatchState *state, ULONGLONG now)
{
    ULONGLONG wait = INFINITE;
    if (state->pending_count > 0)
        wait = now - state->last_check >= CHECK_INTERVAL ? 0 : CHECK_INTERVAL - (now - state->last_check);
    for (int index = 0; index < state->watch_count; index++)
    {
        const Watch *watch = state->watches[index];
        if (watch->directory) continue;
        ULONGLONG until = watch->retry_at > now ? watch->retry_at - now : 0;
        if (until < wait) wait = until;
    }
    return (DWORD)wait;
}

static DWORD WINAPI watch_thread(void *data)
{
    WatchState *state = data;
    power_background_thread();
    for (;;)
    {
        HANDLE handles[1 + FOLDER_WATCH_MAXIMUM];
        int owners[1 + FOLDER_WATCH_MAXIMUM];
        DWORD count = 0;
        handles[count++] = state->wake;
        for (int index = 0; index < state->watch_count; index++)
        {
            if (!state->watches[index]->reading) continue;
            owners[count] = index;
            handles[count++] = state->watches[index]->overlapped.hEvent;
        }
        DWORD result = WaitForMultipleObjects(count, handles, FALSE, next_timeout(state, GetTickCount64()));
        if (result == WAIT_OBJECT_0)
        {
            if (!apply_wanted(state)) break;
        }
        else if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) finish_read(state, state->watches[owners[result - WAIT_OBJECT_0]]);
        else if (result == WAIT_FAILED)
        {
            log_win32_error(L"等待文件夹变化失败", GetLastError());
            Sleep(1000);
        }
        ULONGLONG now = GetTickCount64();
        for (int index = 0; index < state->watch_count; index++)
        {
            Watch *watch = state->watches[index];
            if (!watch->directory && now >= watch->retry_at) open_watch(watch);
        }
        check_pending(state, now);
    }
    for (int index = 0; index < state->watch_count; index++) free_watch(state->watches[index]);
    free(state->watches);
    for (int index = 0; index < state->pending_count; index++) free(state->pending[index].path);
    free(state->pending);
    for (int index = 0; index < state->gone_count; index++) free(state->gone[index].path);
    free(state->gone);
    state->watches = NULL;
    state->pending = NULL;
    state->gone = NULL;
    state_release(state);
    return 0;
}

// ───────────── 界面线程 ─────────────

static WatchState *current;
static HANDLE current_thread;
static unsigned generation;
static FolderWatchHandler watch_handler;

/// 快速操作期间留着的。
static int holds;
static StrList held;
static ULONGLONG *held_seen;
static int held_capacity;

typedef struct Expected
{
    wchar_t *path;
    ULONGLONG until;
} Expected;

static Expected *expected;
static int expected_count;

typedef struct DragRecord
{
    StrList stems;
    ULONGLONG start, end;       // end 为 0 表示还在拖
} DragRecord;

#define DRAG_RECORDS 8
static DragRecord drags[DRAG_RECORDS];
static int drag_count;

void folder_watch_stop(void)
{
    generation++;
    if (!current) return;
    AcquireSRWLockExclusive(&current->lock);
    current->stopping = true;
    ReleaseSRWLockExclusive(&current->lock);
    SetEvent(current->wake);
    // 卡在打不开的网络位置上时不等它，线程自己收尾。
    if (current_thread)
    {
        WaitForSingleObject(current_thread, 2000);
        CloseHandle(current_thread);
    }
    current_thread = NULL;
    state_release(current);
    current = NULL;
}

void folder_watch_set(const StrList *folders, FolderWatchHandler handler)
{
    watch_handler = handler;
    StrList list = {0};
    for (int index = 0; folders && index < folders->count && list.count < FOLDER_WATCH_MAXIMUM; index++)
        if (!wcs_is_blank(folders->items[index]) && strlist_index_of(&list, folders->items[index], true) < 0) strlist_add(&list, folders->items[index]);
    if (list.count == 0)
    {
        folder_watch_stop();
        return;
    }
    if (!current)
    {
        WatchState *state = xcalloc(1, sizeof(WatchState));
        state->references = 2;      // 界面线程和后台线程各一份
        state->generation = ++generation;
        state->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        InitializeSRWLock(&state->lock);
        HANDLE thread = state->wake ? CreateThread(NULL, 0, watch_thread, state, 0, NULL) : NULL;
        if (!thread)
        {
            log_win32_error(L"无法启动文件夹观察", GetLastError());
            state->references = 1;
            state_release(state);
            strlist_clear(&list);
            return;
        }
        current = state;
        current_thread = thread;
    }
    AcquireSRWLockExclusive(&current->lock);
    strlist_clear(&current->wanted);
    current->wanted = list;
    current->changed = true;
    ReleaseSRWLockExclusive(&current->lock);
    SetEvent(current->wake);
}

static bool name_matches_stem(const wchar_t *name, const wchar_t *stem)
{
    // 资源管理器遇到同名时会写成“名字 - 副本”“名字 (2)”。
    size_t length = wcslen(stem);
    return length > 0 && _wcsnicmp(name, stem, length) == 0 && (name[length] == L'.' || name[length] == L' ' || name[length] == 0);
}

static bool came_from_drag(const wchar_t *path, ULONGLONG seen)
{
    const wchar_t *name = path_file_name(path);
    for (int index = 0; index < drag_count; index++)
    {
        const DragRecord *record = &drags[index];
        if (seen < record->start || (record->end && seen > record->end + DRAG_MARGIN)) continue;
        for (int at = 0; at < record->stems.count; at++)
            if (name_matches_stem(name, record->stems.items[at])) return true;
    }
    return false;
}

static bool written_by_us(const wchar_t *path, ULONGLONG now)
{
    bool found = false;
    int kept = 0;
    for (int index = 0; index < expected_count; index++)
    {
        if (!found && _wcsicmp(expected[index].path, path) == 0)
        {
            found = true;
            free(expected[index].path);
            continue;
        }
        if (now > expected[index].until)
        {
            free(expected[index].path);
            continue;
        }
        expected[kept++] = expected[index];
    }
    expected_count = kept;
    return found;
}

static void flush_held(void)
{
    if (holds > 0 || held.count == 0) return;
    ULONGLONG now = GetTickCount64();
    StrList ready = {0};
    for (int index = 0; index < held.count; index++)
    {
        const wchar_t *path = held.items[index];
        if (written_by_us(path, now) || came_from_drag(path, held_seen[index])) continue;
        if (strlist_index_of(&ready, path, true) < 0) strlist_add(&ready, path);
    }
    strlist_clear(&held);
    if (ready.count > 0 && watch_handler) watch_handler(&ready);
    strlist_clear(&ready);
}

static void deliver(void *data)
{
    Batch *batch = data;
    if (current && batch->generation == generation)
    {
        for (int index = 0; index < batch->paths.count; index++)
        {
            if (held.count == held_capacity)
            {
                held_capacity = held_capacity ? held_capacity * 2 : 16;
                held_seen = xrealloc(held_seen, (size_t)held_capacity * sizeof(ULONGLONG));
            }
            held_seen[held.count] = batch->seen[index];
            strlist_add(&held, batch->paths.items[index]);
        }
        flush_held();
    }
    batch_free(batch);
}

void folder_watch_hold(void)
{
    holds++;
}

void folder_watch_release(const StrList *written)
{
    ULONGLONG until = GetTickCount64() + OWN_MEMORY;
    for (int index = 0; written && index < written->count; index++)
    {
        expected = xrealloc(expected, (size_t)(expected_count + 1) * sizeof(Expected));
        expected[expected_count++] = (Expected){ wcs_dup(written->items[index]), until };
    }
    if (holds > 0) holds--;
    flush_held();
}

void folder_watch_drag_begin(const StrList *paths)
{
    // 只记最近几次；最早的让位。
    ULONGLONG now = GetTickCount64();
    int kept = 0;
    for (int index = 0; index < drag_count; index++)
    {
        bool expired = drags[index].end && now - drags[index].end > DRAG_MEMORY;
        if (expired || (kept == 0 && drag_count == DRAG_RECORDS && index == 0)) strlist_clear(&drags[index].stems);
        else drags[kept++] = drags[index];
    }
    drag_count = kept;
    DragRecord *record = &drags[drag_count++];
    *record = (DragRecord){ .start = now };
    for (int index = 0; paths && index < paths->count; index++)
    {
        const wchar_t *name = path_file_name(paths->items[index]);
        // 文件夹的名字里可能有点，整个名字和去掉扩展名的都记上。
        strlist_add(&record->stems, name);
        wchar_t *stem = path_file_stem(paths->items[index]);
        if (stem && *stem && _wcsicmp(stem, name) != 0) strlist_add(&record->stems, stem);
        free(stem);
    }
}

void folder_watch_drag_end(void)
{
    if (drag_count > 0 && drags[drag_count - 1].end == 0) drags[drag_count - 1].end = GetTickCount64();
}
