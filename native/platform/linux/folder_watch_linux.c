// 观察文件夹的 Linux 实现：一个后台线程用 inotify 收变化，等文件写完再交给界面线程。
// 判断规则和 Windows 版一致：下载中的临时文件不交，编辑保存替换原文件不算新文件，
// 本程序自己写出和拖出去的文件也不算；文件夹要多安静一会儿才交。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "folder_watch.h"
#include "log.h"
#include "task.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK_INTERVAL 1000             // 还没写完的文件每秒看一次
#define RETRY_INTERVAL 30000            // 打不开的文件夹过一会儿再试
#define PENDING_LIMIT (30 * 60 * 1000)  // 半小时还没写完的不再等
#define PENDING_MAXIMUM 2000
#define GONE_MEMORY 5000                // 刚删掉或改走的名字记几秒，用来认出“保存时替换原文件”
#define RESCAN_WINDOW 30                // 通知太多来不及收时，把最近这么多秒里建的文件补上
#define OWN_MEMORY (10 * 60 * 1000)     // 快速操作写出的文件记这么久
#define DRAG_MEMORY (30 * 60 * 1000)
#define DRAG_MARGIN 5000                // 拖出结束后还要一会儿文件管理器才建出文件
#define EVENT_BUFFER (16 * 1024)

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

/// 浏览器和下载工具下载中的文件。
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
    if (!name || !*name || wcschr(name, L'/')) return true;
    // Linux 上以点开头的是隐藏文件（和 Windows 的隐藏属性对应），不提示。
    if (name[0] == L'.') return true;
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

// ───────────── 状态 ─────────────

typedef struct Watch
{
    wchar_t *folder;
    int watch_descriptor;       // inotify 的 wd；-1 表示还没观察（文件夹不存在）
    bool reported;
    ULONGLONG retry_at;
    wchar_t *renamed_from;
} Watch;

typedef struct Pending
{
    wchar_t *path;
    ULONGLONG first_seen;
    bool checked;
    long long size;
    long long written;          // st_mtim 纳秒
    int quiet;
} Pending;

typedef struct Gone
{
    wchar_t *path;
    ULONGLONG at;
    bool was_pending;
} Gone;

typedef struct WatchState
{
    pthread_mutex_t lock;
    StrList wanted;
    bool changed;
    bool stopping;
    unsigned generation;
    int wake[2];

    Watch *watches;
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

// ───────────── 界面线程的筛选 ─────────────
// 这些列表只在界面线程上动，和 Windows 版一致。

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
    ULONGLONG start, end;
} DragRecord;

#define DRAG_RECORDS 8
static DragRecord drags[DRAG_RECORDS];
static int drag_count;

static unsigned generation;
static FolderWatchHandler watch_handler;
static WatchState *current;
static pthread_t current_thread;
static bool thread_started;

static bool name_matches_stem(const wchar_t *name, const wchar_t *stem)
{
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

static void handle_added(WatchState *state, Watch *watch, const wchar_t *name, ULONGLONG now)
{
    wchar_t *path = path_join(watch->folder, name);
    if (!was_replaced(state, path, now)) consider(state, path, now);
    free(path);
}

static void handle_removed(WatchState *state, Watch *watch, const wchar_t *name, ULONGLONG now)
{
    wchar_t *path = path_join(watch->folder, name);
    remember_gone(state, path, drop_pending(state, path), now);
    free(path);
}

/// 改名：先来旧名字，下一条是新名字。
static void handle_renamed(WatchState *state, Watch *watch, const wchar_t *old_name, const wchar_t *new_name, ULONGLONG now)
{
    wchar_t *old_path = path_join(watch->folder, old_name);
    wchar_t *path = path_join(watch->folder, new_name);
    int index = pending_index(state, old_path);
    remember_gone(state, old_path, index >= 0, now);
    if (index >= 0)
    {
        // 还在等的文件改了名（比如“未确认 123.crdownload”改成真名）：接着等新名字。
        if (!is_candidate(new_name)) remove_pending(state, index);
        else
        {
            free(state->pending[index].path);
            state->pending[index].path = wcs_dup(path);
            state->pending[index].checked = false;
            state->pending[index].quiet = 0;
        }
    }
    // 从临时的名字改成正式的名字是下载完成；把一个原有的文件改个名不算新文件。
    else if (!is_candidate(old_name) && !was_replaced(state, path, now)) consider(state, path, now);
    free(old_path);
    free(path);
}

static long long file_written_ns(const struct stat *data)
{
    return (long long)data->st_mtim.tv_sec * 1000000000LL + (long long)data->st_mtim.tv_nsec;
}

typedef enum Readiness { READY, WAITING, DROPPED } Readiness;

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

static Readiness check(Pending *pending, ULONGLONG now)
{
    if (now - pending->first_seen > PENDING_LIMIT) return DROPPED;
    char *narrow = utf8_from_wide(pending->path, NULL);
    if (!narrow) return DROPPED;
    struct stat data;
    int result = stat(narrow, &data);
    free(narrow);
    if (result != 0) return DROPPED;
    bool directory = S_ISDIR(data.st_mode);
    if (!directory && has_partial_sibling(pending->path))
    {
        pending->checked = false;
        pending->quiet = 0;
        return WAITING;
    }
    long long size = directory ? 0 : (long long)data.st_size;
    long long written = directory ? 0 : file_written_ns(&data);
    bool same = pending->checked && size == pending->size && written == pending->written;
    pending->checked = true;
    pending->size = size;
    pending->written = written;
    if (!same)
    {
        pending->quiet = 0;
        return WAITING;
    }
    // 文件夹（解压出来的、整个挪进来的）里还在往下写时看不出来，多等一会儿没有变化再交出。
    if (directory) return ++pending->quiet >= 2 ? READY : WAITING;
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

// ───────────── 观察 ─────────────

static void close_watch(int inotify, Watch *watch)
{
    if (watch->watch_descriptor >= 0 && inotify >= 0) inotify_rm_watch(inotify, watch->watch_descriptor);
    watch->watch_descriptor = -1;
    free(watch->renamed_from);
    watch->renamed_from = NULL;
}

static void fail_watch(Watch *watch, int error)
{
    if (!watch->reported)
    {
        log_info(L"无法观察文件夹（%hs），稍后重试：%ls", strerror(error), watch->folder);
        watch->reported = true;
    }
    watch->retry_at = GetTickCount64() + RETRY_INTERVAL;
}

static void open_watch(int inotify, Watch *watch)
{
    char *path = utf8_from_wide(watch->folder, NULL);
    if (!path)
    {
        fail_watch(watch, ENOENT);
        return;
    }
    int descriptor = inotify_add_watch(inotify, path,
        IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CLOSE_WRITE | IN_ONLYDIR);
    free(path);
    if (descriptor < 0)
    {
        fail_watch(watch, errno);
        return;
    }
    if (watch->reported) log_info(L"开始观察文件夹：%ls", watch->folder);
    watch->reported = false;
    watch->watch_descriptor = descriptor;
}

/// 通知太多、系统没来得及记下时，把最近建的补上。
static void rescan(WatchState *state, Watch *watch, ULONGLONG now)
{
    (void)now;
    if (!path_is_directory(watch->folder)) return;
    char *folder = utf8_from_wide(watch->folder, NULL);
    if (!folder) return;
    struct timespec current;
    clock_gettime(CLOCK_REALTIME, &current);
    long long limit = (long long)current.tv_sec - RESCAN_WINDOW;
    GDir *directory = g_dir_open(folder, 0, NULL);
    free(folder);
    if (!directory) return;
    const char *name = NULL;
    while ((name = g_dir_read_name(directory)))
    {
        wchar_t *wide = wide_from_utf8(name, strlen(name));
        if (!wide) continue;
        wchar_t *path = path_join(watch->folder, wide);
        free(wide);
        char *narrow = utf8_from_wide(path, NULL);
        struct stat data;
        if (narrow && stat(narrow, &data) == 0 && (long long)data.st_mtim.tv_sec >= limit)
            consider(state, path, GetTickCount64());
        free(narrow);
        free(path);
    }
    g_dir_close(directory);
}

/// 界面线程改了要观察的文件夹列表。
static bool apply_wanted(WatchState *state, int inotify)
{
    pthread_mutex_lock(&state->lock);
    bool stopping = state->stopping;
    bool changed = state->changed;
    StrList wanted = {0};
    if (changed) strlist_clone(&wanted, &state->wanted);
    state->changed = false;
    pthread_mutex_unlock(&state->lock);
    if (stopping) return false;
    if (!changed)
    {
        strlist_clear(&wanted);
        return true;
    }
    int kept = 0;
    for (int index = 0; index < state->watch_count; index++)
    {
        if (strlist_index_of(&wanted, state->watches[index].folder, true) >= 0) state->watches[kept++] = state->watches[index];
        else
        {
            close_watch(inotify, &state->watches[index]);
            free(state->watches[index].folder);
        }
    }
    state->watch_count = kept;
    state->watches = xrealloc(state->watches, (size_t)(wanted.count ? wanted.count : 1) * sizeof(Watch));
    for (int index = 0; index < wanted.count; index++)
    {
        bool known = false;
        for (int at = 0; at < kept && !known; at++) known = _wcsicmp(state->watches[at].folder, wanted.items[index]) == 0;
        if (known) continue;
        Watch *watch = &state->watches[state->watch_count++];
        memset(watch, 0, sizeof *watch);
        watch->folder = wcs_dup(wanted.items[index]);
        watch->watch_descriptor = -1;
        open_watch(inotify, watch);
    }
    strlist_clear(&wanted);
    return true;
}

static int timeout_milliseconds(WatchState *state, ULONGLONG now)
{
    int wait = -1;
    if (state->pending_count > 0)
        wait = now - state->last_check >= CHECK_INTERVAL ? 0 : (int)(CHECK_INTERVAL - (now - state->last_check));
    for (int index = 0; index < state->watch_count; index++)
    {
        const Watch *watch = &state->watches[index];
        if (watch->watch_descriptor >= 0) continue;
        int until = watch->retry_at > now ? (int)(watch->retry_at - now) : 0;
        if (wait < 0 || until < wait) wait = until;
    }
    return wait < 0 ? -1 : wait;
}

static Watch *watch_for(WatchState *state, int descriptor)
{
    for (int index = 0; index < state->watch_count; index++)
        if (state->watches[index].watch_descriptor == descriptor) return &state->watches[index];
    return NULL;
}

/// 改名配对用的 cookie：inotify 先给旧名字（IN_MOVED_FROM），再给新名字（IN_MOVED_TO）。
typedef struct MoveRecord
{
    int watch_descriptor;
    unsigned cookie;
    wchar_t *name;
} MoveRecord;

#define MOVE_RECORDS 16
static MoveRecord moves[MOVE_RECORDS];
static int move_count;

static void remember_move(int descriptor, unsigned cookie, const wchar_t *name)
{
    if (move_count == MOVE_RECORDS)
    {
        free(moves[0].name);
        memmove(&moves[0], &moves[1], (size_t)(MOVE_RECORDS - 1) * sizeof(MoveRecord));
        move_count--;
    }
    moves[move_count++] = (MoveRecord){ descriptor, cookie, wcs_dup(name) };
}

static wchar_t *take_move(int descriptor, unsigned cookie)
{
    for (int index = move_count - 1; index >= 0; index--)
    {
        if (moves[index].watch_descriptor != descriptor || moves[index].cookie != cookie) continue;
        wchar_t *name = moves[index].name;
        memmove(&moves[index], &moves[index + 1], (size_t)(move_count - index - 1) * sizeof(MoveRecord));
        move_count--;
        return name;
    }
    return NULL;
}

static void handle_event(WatchState *state, const struct inotify_event *event, ULONGLONG now)
{
    if (event->mask & IN_Q_OVERFLOW)
    {
        // 事件排不下时 wd 是 -1，把所有观察的文件夹都补扫一遍。
        for (int index = 0; index < state->watch_count; index++) rescan(state, &state->watches[index], now);
        return;
    }
    Watch *watch = watch_for(state, event->wd);
    if (!watch) return;
    if (event->len == 0) return;
    size_t length = strnlen(event->name, event->len);
    wchar_t *name = wide_from_utf8(event->name, length);
    if (!name || !*name) goto done;
    if (event->mask & IN_MOVED_FROM)
    {
        remember_move(event->wd, event->cookie, name);
        // 挪走也是“不见了”；挪到文件夹里的同名文件会在 MOVED_TO 里配回来。
        handle_removed(state, watch, name, now);
        goto done;
    }
    if (event->mask & IN_MOVED_TO)
    {
        wchar_t *old_name = take_move(event->wd, event->cookie);
        if (old_name) handle_renamed(state, watch, old_name, name, now);
        else handle_added(state, watch, name, now);
        free(old_name);
        goto done;
    }
    if (event->mask & IN_DELETE)
    {
        handle_removed(state, watch, name, now);
        goto done;
    }
    if (event->mask & (IN_CREATE | IN_CLOSE_WRITE)) handle_added(state, watch, name, now);
done:
    free(name);
}

static void *watch_thread(void *data)
{
    WatchState *state = data;
    int inotify = inotify_init1(IN_CLOEXEC);
    if (inotify < 0)
    {
        log_info(L"无法启动文件夹观察");
        state->stopping = true;
    }
    while (!state->stopping)
    {
        apply_wanted(state, inotify);
        ULONGLONG now = GetTickCount64();
        struct pollfd waited[2] = {
            { .fd = state->wake[0], .events = POLLIN },
            { .fd = inotify, .events = POLLIN },
        };
        int ready = poll(waited, inotify >= 0 ? 2 : 1, timeout_milliseconds(state, now));
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (waited[0].revents & POLLIN))
        {
            char drain[64];
            ssize_t got = read(state->wake[0], drain, sizeof drain);
            (void)got;
        }
        if (ready > 0 && inotify >= 0 && (waited[1].revents & POLLIN))
        {
            char buffer[EVENT_BUFFER] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t used = read(inotify, buffer, sizeof buffer);
            ULONGLONG at = GetTickCount64();
            for (char *cursor = buffer; used > 0 && cursor < buffer + used;)
            {
                const struct inotify_event *event = (const struct inotify_event *)cursor;
                handle_event(state, event, at);
                cursor += sizeof(struct inotify_event) + event->len;
            }
        }
        now = GetTickCount64();
        for (int index = 0; index < state->watch_count; index++)
        {
            Watch *watch = &state->watches[index];
            if (watch->watch_descriptor < 0 && now >= watch->retry_at) open_watch(inotify, watch);
        }
        check_pending(state, now);
    }
    if (inotify >= 0)
    {
        for (int index = 0; index < state->watch_count; index++) close_watch(inotify, &state->watches[index]);
        close(inotify);
    }
    for (int index = 0; index < state->watch_count; index++) free(state->watches[index].folder);
    free(state->watches);
    for (int index = 0; index < state->pending_count; index++) free(state->pending[index].path);
    free(state->pending);
    for (int index = 0; index < state->gone_count; index++) free(state->gone[index].path);
    free(state->gone);
    close(state->wake[0]);
    close(state->wake[1]);
    pthread_mutex_destroy(&state->lock);
    free(state);
    return NULL;
}

// ───────────── 对外的接口 ─────────────

void folder_watch_stop(void)
{
    generation++;
    if (!current) return;
    pthread_mutex_lock(&current->lock);
    current->stopping = true;
    pthread_mutex_unlock(&current->lock);
    char wake = 1;
    ssize_t written = write(current->wake[1], &wake, 1);
    (void)written;
    if (thread_started)
    {
        pthread_join(current_thread, NULL);
        thread_started = false;
    }
    current = NULL;
    strlist_clear(&held);
    for (int index = 0; index < expected_count; index++) free(expected[index].path);
    expected_count = 0;
    free(expected);
    expected = NULL;
    for (int index = 0; index < drag_count; index++) strlist_clear(&drags[index].stems);
    drag_count = 0;
}

void folder_watch_set(const StrList *folders, FolderWatchHandler handler)
{
    watch_handler = handler;
    StrList list = {0};
    for (int index = 0; folders && index < folders->count && list.count < FOLDER_WATCH_MAXIMUM; index++)
        if (!wcs_is_blank(folders->items[index]) && strlist_index_of(&list, folders->items[index], true) < 0)
            strlist_add(&list, folders->items[index]);
    if (list.count == 0)
    {
        folder_watch_stop();
        return;
    }
    if (!current)
    {
        WatchState *state = xcalloc(1, sizeof(WatchState));
        pthread_mutex_init(&state->lock, NULL);
        state->generation = ++generation;
        if (pipe2(state->wake, O_CLOEXEC | O_NONBLOCK) != 0)
        {
            free(state);
            strlist_clear(&list);
            log_info(L"无法启动文件夹观察");
            return;
        }
        current = state;
        if (pthread_create(&current_thread, NULL, watch_thread, state) != 0)
        {
            current = NULL;
            close(state->wake[0]);
            close(state->wake[1]);
            pthread_mutex_destroy(&state->lock);
            free(state);
            strlist_clear(&list);
            log_info(L"无法启动文件夹观察");
            return;
        }
        pthread_setname_np(current_thread, "dropshelf-watch");
        thread_started = true;
    }
    pthread_mutex_lock(&current->lock);
    strlist_clear(&current->wanted);
    current->wanted = list;
    current->changed = true;
    pthread_mutex_unlock(&current->lock);
    char wake = 1;
    ssize_t written = write(current->wake[1], &wake, 1);
    (void)written;
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