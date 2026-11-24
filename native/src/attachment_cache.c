#include "attachment_cache.h"
#include "log.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>

typedef struct Holder
{
    wchar_t *key;
    int count;
} Holder;

static SRWLOCK holders_lock = SRWLOCK_INIT;
static Holder *holders;
static int holder_count;

static wchar_t *full_path(const wchar_t *path)
{
    if (!path || !*path) return NULL;
    DWORD length = GetFullPathNameW(path, 0, NULL, NULL);
    if (!length) return NULL;
    wchar_t *full = xalloc((size_t)length * sizeof(wchar_t));
    if (!GetFullPathNameW(path, length, full, NULL))
    {
        free(full);
        return NULL;
    }
    return full;
}

static void trim_separators(wchar_t *path)
{
    size_t length = wcslen(path);
    // 根目录（C:\）保留末尾的分隔符，和 Path.TrimEndingDirectorySeparator 相同。
    while (length > 3 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) path[--length] = 0;
}

static INIT_ONCE root_once = INIT_ONCE_STATIC_INIT;
static wchar_t *cache_root;

/// 第一次用到时算出根目录。启动时后台的清理和界面线程可能同时第一次用到，只能算一次。
static BOOL CALLBACK compute_root(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    wchar_t temp[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    if (!length || length > MAX_PATH) wcscpy_s(temp, MAX_PATH + 1, L"C:\\Windows\\Temp\\");
    wchar_t *drop = path_join(temp, L"DropShelf");
    wchar_t *joined = path_join(drop, L"Attachments");
    free(drop);
    cache_root = full_path(joined);
    if (!cache_root) cache_root = joined;
    else free(joined);
    trim_separators(cache_root);
    return TRUE;
}

const wchar_t *attachment_cache_root(void)
{
    InitOnceExecuteOnce(&root_once, compute_root, NULL, NULL);
    return cache_root;
}

static wchar_t *cache_key(const wchar_t *directory)
{
    wchar_t *key = full_path(directory);
    if (!key) key = wcs_dup(directory ? directory : L"");
    trim_separators(key);
    return key;
}

void attachment_cache_retain(const wchar_t *directory)
{
    if (wcs_is_blank(directory)) return;
    wchar_t *key = cache_key(directory);
    AcquireSRWLockExclusive(&holders_lock);
    for (int index = 0; index < holder_count; index++)
    {
        if (!wcs_equal_ignore_case(holders[index].key, key)) continue;
        holders[index].count++;
        ReleaseSRWLockExclusive(&holders_lock);
        free(key);
        return;
    }
    holders = xrealloc(holders, (size_t)(holder_count + 1) * sizeof(Holder));
    holders[holder_count++] = (Holder){ key, 1 };
    ReleaseSRWLockExclusive(&holders_lock);
}

wchar_t *attachment_cache_create(void)
{
    const wchar_t *root = attachment_cache_root();
    wchar_t *id = new_guid_text();
    wchar_t *directory = path_join(root, id);
    free(id);
    if (!directory_create(directory))
    {
        log_win32_error(L"无法创建临时附件文件夹", GetLastError());
        free(directory);
        return NULL;
    }
    attachment_cache_retain(directory);
    return directory;
}

bool directory_delete_tree(const wchar_t *path)
{
    DWORD attributes = io_get_attributes(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        if (attributes & FILE_ATTRIBUTE_READONLY) io_set_attributes(path, attributes & ~FILE_ATTRIBUTE_READONLY);
        return io_delete_file(path) != 0;
    }
    // 联接点和符号链接只删除链接本身，不进去删里面的东西。OneDrive 等按需下载的文件夹也带重解析点属性，
    // 但是真正的文件夹：不进去删的话，移动文件夹时复制完了却删不掉原来的（见 find_data_is_link）。
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !path_is_link(path))
    {
        wchar_t *pattern = path_join(path, L"*");
        WIN32_FIND_DATAW found;
        HANDLE search = io_find_first(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        free(pattern);
        if (search != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
                wchar_t *child = path_join(path, found.cFileName);
                directory_delete_tree(child);
                free(child);
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) io_set_attributes(path, attributes & ~FILE_ATTRIBUTE_READONLY);
    return io_remove_directory(path) != 0;
}

bool attachment_cache_is_owned(const wchar_t *path)
{
    if (wcs_is_blank(path)) return false;
    wchar_t *candidate = full_path(path);
    if (!candidate) return false;
    trim_separators(candidate);
    const wchar_t *root = attachment_cache_root();
    size_t root_length = wcslen(root);
    bool owned = wcslen(candidate) > root_length + 1 && candidate[root_length] == PATH_SEPARATOR &&
                 CompareStringOrdinal(candidate, (int)root_length, root, (int)root_length, TRUE) == CSTR_EQUAL;
    free(candidate);
    return owned;
}

static bool is_held(const wchar_t *key)
{
    for (int index = 0; index < holder_count; index++)
        if (wcs_equal_ignore_case(holders[index].key, key)) return true;
    return false;
}

/// 删不掉时（多半是杀毒软件或资源管理器的预览正占着刚写的文件）隔这么久（毫秒）再试，都不行就留给下次启动时清理。
static const unsigned retry_delays[] = { 500, 2000, 5000 };

typedef struct Deletion
{
    wchar_t *directory;
    int retries;                // 已经重试了几次
    DWORD error;                // 这次没删掉的原因；删掉了或者又有人持有时为 0
} Deletion;

/// 后台删除：删之前在锁里重新确认没有人再次持有，删的过程中也拿着锁。
static void delete_unheld(void *data)
{
    Deletion *deletion = data;
    wchar_t *key = cache_key(deletion->directory);
    deletion->error = 0;
    AcquireSRWLockExclusive(&holders_lock);
    if (!is_held(key) && !directory_delete_tree(deletion->directory))
    {
        DWORD error = GetLastError();
        if (path_exists(deletion->directory)) deletion->error = error ? error : ERROR_SHARING_VIOLATION;
    }
    ReleaseSRWLockExclusive(&holders_lock);
    free(key);
}

static void deletion_finished(void *data);

static void start_deletion(void *data)
{
    task_run(delete_unheld, deletion_finished, data);
}

static void deletion_finished(void *data)
{
    Deletion *deletion = data;
    if (deletion->error && deletion->retries < (int)(sizeof(retry_delays) / sizeof(retry_delays[0])))
    {
        task_delay(retry_delays[deletion->retries++], start_deletion, deletion);
        return;
    }
    if (deletion->error) log_win32_error(L"清理临时附件失败", deletion->error);
    free(deletion->directory);
    free(deletion);
}

void attachment_cache_release(const wchar_t *directory)
{
    if (wcs_is_blank(directory)) return;
    wchar_t *key = cache_key(directory);
    bool last = true;
    AcquireSRWLockExclusive(&holders_lock);
    for (int index = 0; index < holder_count; index++)
    {
        if (!wcs_equal_ignore_case(holders[index].key, key)) continue;
        if (--holders[index].count > 0)
        {
            last = false;
            break;
        }
        free(holders[index].key);
        holders[index] = holders[--holder_count];
        break;
    }
    ReleaseSRWLockExclusive(&holders_lock);
    free(key);
    if (!last || !attachment_cache_is_owned(directory)) return;
    Deletion *deletion = xcalloc(1, sizeof(Deletion));
    deletion->directory = wcs_dup(directory);
    start_deletion(deletion);
}

static void release_now(void *data)
{
    attachment_cache_release(data);
    free(data);
}

void attachment_cache_release_later(const wchar_t *directory, unsigned delay_milliseconds)
{
    if (wcs_is_blank(directory)) return;
    if (delay_milliseconds == 0)
    {
        attachment_cache_release(directory);
        return;
    }
    task_delay(delay_milliseconds, release_now, wcs_dup(directory));
}

wchar_t *attachment_cache_directory_of(const wchar_t *path)
{
    wchar_t *full = full_path(path);
    if (!full) return NULL;
    const wchar_t *root = attachment_cache_root();
    size_t root_length = wcslen(root);
    wchar_t *result = NULL;
    if (wcslen(full) > root_length + 1 && full[root_length] == PATH_SEPARATOR &&
        CompareStringOrdinal(full, (int)root_length, root, (int)root_length, TRUE) == CSTR_EQUAL)
    {
        const wchar_t *name = full + root_length + 1;
        const wchar_t *end = wcschr(name, PATH_SEPARATOR);
        size_t name_length = end ? (size_t)(end - name) : wcslen(name);
        if (name_length > 0)
        {
            WStr directory = {0};
            wstr_append(&directory, root);
            wstr_append_char(&directory, PATH_SEPARATOR);
            wstr_append_n(&directory, name, name_length);
            result = wstr_take(&directory);
        }
    }
    free(full);
    return result;
}

static void cleanup_stale(void *data)
{
    (void)data;
    const wchar_t *root = attachment_cache_root();
    wchar_t *pattern = path_join(root, L"*");
    WIN32_FIND_DATAW found;
    HANDLE search = io_find_first(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cutoff = { .LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime };
    cutoff.QuadPart -= 24ull * 3600 * 10000000;
    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)
            continue;
        ULARGE_INTEGER written = { .LowPart = found.ftLastWriteTime.dwLowDateTime, .HighPart = found.ftLastWriteTime.dwHighDateTime };
        if (written.QuadPart >= cutoff.QuadPart) continue;
        wchar_t *directory = path_join(root, found.cFileName);
        // 单个旧缓存清理不了时不影响别的。
        if (attachment_cache_is_owned(directory)) directory_delete_tree(directory);
        free(directory);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

void attachment_cache_cleanup_stale(void)
{
    task_run(cleanup_stale, NULL, NULL);
}

// ───────────── 文件名 ─────────────

static bool is_invalid_name_character(wchar_t c)
{
    return c < 32 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L':' || c == L'*' || c == L'?' || c == L'\\' || c == L'/';
}

/// 扩展名的起点（最后一个点）；没有时指向末尾。
static const wchar_t *extension_of(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    return dot ? dot : name + wcslen(name);
}

wchar_t *attachment_safe_name(const wchar_t *supplied, StrList *used)
{
    const wchar_t *source = supplied ? supplied : L"";
    // 只取最后一段：来源程序给的名称里可能带着路径。
    const wchar_t *start = source;
    for (const wchar_t *c = source; *c; c++)
        if (*c == L'\\' || *c == L'/') start = c + 1;
    wchar_t *name = wcs_dup(start);
    for (wchar_t *c = name; *c; c++)
        if (is_invalid_name_character(*c)) *c = L'_';
    wchar_t *trimmed = wcs_trim_dup(name);
    free(name);
    name = trimmed;
    size_t length = wcslen(name);
    while (length > 0 && name[length - 1] == L'.') name[--length] = 0;
    if (wcs_is_blank(name))
    {
        free(name);
        name = wcs_dup(L"attachment");
        length = wcslen(name);
    }
    if (length > 140)
    {
        const wchar_t *extension = extension_of(name);
        size_t extension_length = wcslen(extension);
        size_t keep = 140 > extension_length ? 140 - extension_length : 1;
        if (keep < 1) keep = 1;
        // 别截在一个占两个 UTF-16 单元的字符中间。
        if (keep > 1 && IS_HIGH_SURROGATE(name[keep - 1])) keep--;
        WStr shortened = {0};
        wstr_append_n(&shortened, name, keep);
        wstr_append(&shortened, extension);
        free(name);
        name = wstr_take(&shortened);
    }

    const wchar_t *extension = extension_of(name);
    size_t stem_length = (size_t)(extension - name);
    WStr stem = {0};
    if (file_name_is_reserved(name)) wstr_append_char(&stem, L'_');
    wstr_append_n(&stem, name, stem_length);
    wchar_t *extension_copy = wcs_dup(extension);
    free(name);
    wchar_t *base = wstr_take(&stem);

    WStr candidate = {0};
    wstr_appendf(&candidate, L"%ls%ls", base, extension_copy);
    wchar_t *result = wstr_take(&candidate);
    for (int copy = 2; strlist_index_of(used, result, true) >= 0; copy++)
    {
        free(result);
        WStr next = {0};
        wstr_appendf(&next, L"%ls (%d)%ls", base, copy, extension_copy);
        result = wstr_take(&next);
    }
    strlist_add(used, result);
    free(base);
    free(extension_copy);
    return result;
}

// ───────────── 下载来源标记 ─────────────

#ifdef _WIN32

char *zone_identifier_read(const wchar_t *path, size_t *length)
{
    *length = 0;
    WStr stream = {0};
    wstr_appendf(&stream, L"%ls:Zone.Identifier", path);
    HANDLE file = io_create_file(stream.data, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    wstr_free(&stream);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    char *bytes = NULL;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024)
    {
        bytes = xalloc((size_t)size.QuadPart + 1);
        DWORD read = 0;
        if (!ReadFile(file, bytes, (DWORD)size.QuadPart, &read, NULL) || read == 0)
        {
            free(bytes);
            bytes = NULL;
        }
        else
        {
            bytes[read] = 0;
            *length = read;
        }
    }
    CloseHandle(file);
    return bytes;
}

void zone_identifier_write(const wchar_t *path, const char *zone, size_t length)
{
    if (!zone || length == 0) return;
    WStr stream = {0};
    wstr_appendf(&stream, L"%ls:Zone.Identifier", path);
    HANDLE file = io_create_file(stream.data, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    wstr_free(&stream);
    if (file == INVALID_HANDLE_VALUE)
    {
        // FAT、网络位置等不支持备用数据流时忽略。
        log_error(L"无法写入下载来源标记：%ls", path);
        return;
    }
    DWORD written;
    WriteFile(file, zone, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

#else

// Linux 没有备用数据流；下载来源标记是 Windows 专有的提示，读不到也不影响别的功能。
char *zone_identifier_read(const wchar_t *path, size_t *length)
{
    (void)path;
    *length = 0;
    return NULL;
}

void zone_identifier_write(const wchar_t *path, const char *zone, size_t length)
{
    (void)path;
    (void)zone;
    (void)length;
}

#endif
