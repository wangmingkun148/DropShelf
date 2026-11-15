#include "file_ops.h"
#include "attachment_cache.h"
#include "log.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

// 自己用的两个错误：用 Windows 留给程序自定义的范围（第 29 位）。
#define ERROR_INTO_ITSELF 0x20000001u
#define ERROR_CONTAINS_LINKS 0x20000002u

static bool is_separator(wchar_t character)
{
    return character == L'\\' || character == L'/';
}

// ───────────── 路径 ─────────────

bool path_is_available(const wchar_t *path)
{
    return !wcs_is_blank(path) && path_exists(path);
}

void path_split_name(const wchar_t *name, wchar_t **stem, wchar_t **extension)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot)
    {
        *stem = wcs_dup(name);
        *extension = wcs_dup(L"");
        return;
    }
    *stem = wcs_dup_n(name, (size_t)(dot - name));
    *extension = wcs_dup(dot[1] ? dot : L"");
}

wchar_t *path_available(const wchar_t *directory, const wchar_t *base_name, const wchar_t *extension)
{
    WStr name = {0};
    wstr_appendf(&name, L"%ls%ls", base_name, extension ? extension : L"");
    wchar_t *candidate = path_join(directory, name.data);
    wstr_free(&name);
    for (int index = 2; path_exists(candidate); index++)
    {
        free(candidate);
        if (index >= 10000) return NULL;
        wstr_appendf(&name, L"%ls (%d)%ls", base_name, index, extension ? extension : L"");
        candidate = path_join(directory, name.data);
        wstr_free(&name);
    }
    return candidate;
}

wchar_t *path_full(const wchar_t *path)
{
    DWORD length = GetFullPathNameW(path, 0, NULL, NULL);
    if (length == 0) return wcs_dup(path);
    wchar_t *full = xalloc((size_t)length * sizeof(wchar_t));
    if (!GetFullPathNameW(path, length, full, NULL))
    {
        free(full);
        return wcs_dup(path);
    }
    return full;
}

wchar_t *path_trim_end_separator(const wchar_t *path)
{
    wchar_t *trimmed = wcs_dup(path);
    size_t length = wcslen(trimmed);
    // 根目录（C:\、\\server\share\ 之外的单个分隔符）不去掉，和 Path.TrimEndingDirectorySeparator 相同。
    while (length > 1 && is_separator(trimmed[length - 1]) && !(length == 3 && trimmed[1] == L':')) trimmed[--length] = 0;
    return trimmed;
}

wchar_t *path_folder_name(const wchar_t *folder)
{
    wchar_t *trimmed = path_trim_end_separator(folder);
    const wchar_t *name = path_file_name(trimmed);
    wchar_t *result = wcs_dup(name && *name ? name : trimmed);
    free(trimmed);
    return result;
}

static wchar_t *full_trimmed(const wchar_t *path)
{
    wchar_t *full = path_full(path);
    wchar_t *trimmed = path_trim_end_separator(full);
    free(full);
    return trimmed;
}

static bool same_or_inside(const wchar_t *candidate, const wchar_t *folder)
{
    size_t length = wcslen(folder);
    if (_wcsicmp(candidate, folder) == 0) return true;
    return _wcsnicmp(candidate, folder, length) == 0 && (is_separator(candidate[length]) || (length > 0 && is_separator(folder[length - 1])));
}

bool path_is_same_or_inside(const wchar_t *path, const wchar_t *folder)
{
    wchar_t *candidate = full_trimmed(path), *base = full_trimmed(folder);
    bool inside = same_or_inside(candidate, base);
    free(candidate);
    free(base);
    return inside;
}

bool path_is_direct_child(const wchar_t *path, const wchar_t *folder)
{
    wchar_t *full = full_trimmed(path);
    wchar_t *parent = path_directory(full);
    bool child = false;
    if (parent)
    {
        wchar_t *trimmed_parent = path_trim_end_separator(parent);
        wchar_t *base = full_trimmed(folder);
        child = _wcsicmp(trimmed_parent, base) == 0;
        free(trimmed_parent);
        free(base);
        free(parent);
    }
    free(full);
    return child;
}

/// 把联接点和目录符号链接换成实际指向的位置：取最长的、存在的上级，交给系统解析，再接上后面还不存在的部分。
static wchar_t *resolve_links(const wchar_t *path)
{
    wchar_t *existing = full_trimmed(path);
    WStr tail = {0};
    for (;;)
    {
        HANDLE handle = CreateFileW(existing, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (handle != INVALID_HANDLE_VALUE)
        {
            wchar_t final_path[1024];
            DWORD length = GetFinalPathNameByHandleW(handle, final_path, 1024, VOLUME_NAME_DOS);
            CloseHandle(handle);
            if (length > 0 && length < 1024)
            {
                const wchar_t *start = final_path;
                WStr resolved = {0};
                if (wcsncmp(start, L"\\\\?\\UNC\\", 8) == 0)
                {
                    wstr_append(&resolved, L"\\\\");
                    start += 8;
                }
                else if (wcsncmp(start, L"\\\\?\\", 4) == 0) start += 4;
                wstr_append(&resolved, start);
                if (tail.length > 0) wstr_append(&resolved, tail.data);
                free(existing);
                wstr_free(&tail);
                wchar_t *result = wstr_take(&resolved);
                wchar_t *trimmed = path_trim_end_separator(result);
                free(result);
                return trimmed;
            }
            break;
        }
        wchar_t *parent = path_directory(existing);
        if (!parent || wcslen(parent) >= wcslen(existing))
        {
            free(parent);
            break;
        }
        WStr next = {0};
        wstr_append(&next, existing + wcslen(parent) - (is_separator(parent[wcslen(parent) - 1]) ? 1 : 0));
        if (tail.length > 0) wstr_append(&next, tail.data);
        wstr_free(&tail);
        tail = next;
        free(existing);
        existing = parent;
    }
    // 解析不了时按原样比较。
    wstr_free(&tail);
    free(existing);
    return full_trimmed(path);
}

long long disk_available(const wchar_t *directory)
{
    ULARGE_INTEGER available;
    wchar_t *full = path_full(directory);
    bool ok = GetDiskFreeSpaceExW(full, &available, NULL, NULL);
    free(full);
    return ok ? (long long)available.QuadPart : -1;
}

wchar_t *persistent_output_directory(void)
{
    wchar_t *directory = NULL;
    PWSTR known = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, KF_FLAG_CREATE, NULL, &known)) && known && *known) directory = wcs_dup(known);
    CoTaskMemFree(known);
    if (!directory)
    {
        wchar_t *profile = env_get(L"USERPROFILE");
        if (profile) directory = path_join(profile, L"Downloads");
        else if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &known)) && known) directory = wcs_dup(known);
        if (!profile) CoTaskMemFree(known);
        free(profile);
    }
    if (directory && !directory_create(directory))
    {
        free(directory);
        directory = NULL;
    }
    return directory;
}

// ───────────── 复制和移动 ─────────────

static StringId message_for(DWORD error)
{
    switch (error)
    {
        case ERROR_ACCESS_DENIED: return STR_AccessDenied;
        case ERROR_FILE_NOT_FOUND: return STR_ItemNotFound;
        case ERROR_PATH_NOT_FOUND: return STR_FolderNotFound;
        case ERROR_FILENAME_EXCED_RANGE: return STR_PathTooLong;
        case ERROR_INTO_ITSELF: return STR_FolderIntoItself;
        case ERROR_CONTAINS_LINKS: return STR_FolderContainsLinks;
        case ERROR_SUCCESS: return STR_OperationIncomplete;
        default: return STR_ItemBusy;
    }
}

/// 由系统复制，连同下载来源标记（Zone.Identifier 等备用数据流）、文件属性和修改时间一起保留。
static DWORD copy_file(const wchar_t *source, const wchar_t *target)
{
    return CopyFileExW(source, target, NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS) ? ERROR_SUCCESS : GetLastError();
}

/// 联接点和目录符号链接无法原样复制，跳过又会让移动后删掉源文件夹时丢失它们；扫描时一并检查，发现就在复制之前报错。
static DWORD copy_directory(const wchar_t *source, const wchar_t *destination)
{
    StrList directories = {0}, files = {0}, pending = {0};
    strlist_add(&pending, source);
    DWORD error = ERROR_SUCCESS;
    while (pending.count > 0 && error == ERROR_SUCCESS)
    {
        wchar_t *current = pending.items[--pending.count];
        wchar_t *pattern = path_join(current, L"*");
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        free(pattern);
        if (find == INVALID_HANDLE_VALUE)
        {
            DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) error = code;
            free(current);
            continue;
        }
        do
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            wchar_t *full = path_join(current, data.cFileName);
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) strlist_add_owned(&files, full);
            else if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                free(full);
                error = ERROR_CONTAINS_LINKS;
                break;
            }
            else
            {
                strlist_add(&directories, full);
                strlist_add_owned(&pending, full);
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        free(current);
    }
    strlist_clear(&pending);

    // 里面各项相对源文件夹的部分从这里开始（path_join 在没有分隔符时补了一个）。
    size_t root = wcslen(source);
    if (root > 0 && !is_separator(source[root - 1])) root++;
    if (error == ERROR_SUCCESS && !directory_create(destination)) error = GetLastError();
    for (int index = 0; index < directories.count && error == ERROR_SUCCESS; index++)
    {
        wchar_t *target = path_join(destination, directories.items[index] + root);
        if (!directory_create(target)) error = GetLastError();
        free(target);
    }
    for (int index = 0; index < files.count && error == ERROR_SUCCESS; index++)
    {
        wchar_t *target = path_join(destination, files.items[index] + root);
        error = copy_file(files.items[index], target);
        free(target);
    }
    strlist_clear(&directories);
    strlist_clear(&files);
    return error;
}

static DWORD copy_one(const wchar_t *source, const wchar_t *target)
{
    return path_is_directory(source) ? copy_directory(source, target) : copy_file(source, target);
}

static DWORD move_one(const wchar_t *source, const wchar_t *target)
{
    bool folder = path_is_directory(source);
    if (MoveFileExW(source, target, folder ? 0 : MOVEFILE_COPY_ALLOWED)) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    // 没有权限时复制也不会成功；其余情况（跨磁盘、被占用）先复制再删除原来的。
    if (error == ERROR_ACCESS_DENIED) return error;
    error = copy_one(source, target);
    if (error != ERROR_SUCCESS) return error;
    if (folder) return directory_delete_tree(source) ? ERROR_SUCCESS : ERROR_SHARING_VIOLATION;
    return DeleteFileW(source) ? ERROR_SUCCESS : GetLastError();
}

typedef DWORD (*Operation)(const wchar_t *source, const wchar_t *target);

static FileOpResults run_each(const StrList *sources, const wchar_t *destination, const wchar_t *verb, Operation operation)
{
    FileOpResults results = {0};
    results.items = xcalloc((size_t)(sources->count ? sources->count : 1), sizeof(FileOpResult));
    bool ready = !wcs_is_blank(destination) && directory_create(destination);
    DWORD destination_error = ready ? ERROR_SUCCESS : (wcs_is_blank(destination) ? ERROR_PATH_NOT_FOUND : GetLastError());
    for (int index = 0; index < sources->count; index++)
    {
        const wchar_t *source = sources->items[index];
        FileOpResult *result = &results.items[results.count++];
        result->source = wcs_dup(source);
        DWORD error = destination_error;
        if (error == ERROR_SUCCESS && !path_exists(source)) error = ERROR_FILE_NOT_FOUND;
        // 目标就是源文件夹本身也不行：复制时会把新建的子文件夹一起遍历进去，无限嵌套。
        // 路径本身或它的上级可能是指向源文件夹的联接点、符号链接，按实际位置再比较一次。
        if (error == ERROR_SUCCESS && path_is_directory(source))
        {
            bool inside = path_is_same_or_inside(destination, source);
            if (!inside)
            {
                wchar_t *real_destination = resolve_links(destination), *real_source = resolve_links(source);
                inside = same_or_inside(real_destination, real_source);
                free(real_destination);
                free(real_source);
            }
            if (inside) error = ERROR_INTO_ITSELF;
        }
        if (error == ERROR_SUCCESS)
        {
            wchar_t *trimmed = full_trimmed(source);
            const wchar_t *name = path_file_name(trimmed);
            if (path_is_directory(source)) result->destination = path_available(destination, name, L"");
            else
            {
                wchar_t *stem, *extension;
                path_split_name(name, &stem, &extension);
                result->destination = path_available(destination, stem, extension);
                free(stem);
                free(extension);
            }
            free(trimmed);
            error = result->destination ? operation(source, result->destination) : ERROR_FILE_EXISTS;
        }
        result->succeeded = error == ERROR_SUCCESS;
        result->error = result->succeeded ? STR_OperationIncomplete : message_for(error);
        if (result->succeeded) log_info(L"%ls完成：%ls -> %ls", verb, source, result->destination);
        else log_error(L"%ls失败：%ls\r\n错误 %lu", verb, source, (unsigned long)error);
    }
    return results;
}

FileOpResults file_ops_copy(const StrList *sources, const wchar_t *destination)
{
    return run_each(sources, destination, L"复制", copy_one);
}

FileOpResults file_ops_move(const StrList *sources, const wchar_t *destination)
{
    return run_each(sources, destination, L"移动", move_one);
}

void file_op_results_free(FileOpResults *results)
{
    for (int index = 0; index < results->count; index++)
    {
        free(results->items[index].source);
        free(results->items[index].destination);
    }
    free(results->items);
    *results = (FileOpResults){0};
}
