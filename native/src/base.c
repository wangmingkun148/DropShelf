#include "base.h"
#include "log.h"

#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

// ───────────── 内存 ─────────────

void *xalloc(size_t size)
{
    void *pointer = malloc(size ? size : 1);
    if (!pointer) abort();
    return pointer;
}

void *xcalloc(size_t count, size_t size)
{
    void *pointer = calloc(count ? count : 1, size ? size : 1);
    if (!pointer) abort();
    return pointer;
}

void *xrealloc(void *pointer, size_t size)
{
    void *result = realloc(pointer, size ? size : 1);
    if (!result) abort();
    return result;
}

// ───────────── 字符串 ─────────────

wchar_t *wcs_dup_n(const wchar_t *text, size_t length)
{
    wchar_t *copy = xalloc((length + 1) * sizeof(wchar_t));
    if (length) memcpy(copy, text, length * sizeof(wchar_t));
    copy[length] = 0;
    return copy;
}

wchar_t *wcs_dup(const wchar_t *text)
{
    return text ? wcs_dup_n(text, wcslen(text)) : NULL;
}

bool wchar_is_white_space(wchar_t character)
{
    if (character == L' ' || (character >= 0x09 && character <= 0x0D)) return true;
    if (character < 0x85) return false;
    return character == 0x85 || character == 0xA0 || character == 0x1680 || (character >= 0x2000 && character <= 0x200A) ||
           character == 0x2028 || character == 0x2029 || character == 0x202F || character == 0x205F || character == 0x3000;
}

wchar_t *wcs_trim_dup(const wchar_t *text)
{
    if (!text) return NULL;
    while (*text && wchar_is_white_space(*text)) text++;
    size_t length = wcslen(text);
    while (length > 0 && wchar_is_white_space(text[length - 1])) length--;
    return wcs_dup_n(text, length);
}

bool wcs_is_blank(const wchar_t *text)
{
    if (!text) return true;
    for (; *text; text++)
        if (!wchar_is_white_space(*text)) return false;
    return true;
}

const wchar_t *wcs_find_ignore_case(const wchar_t *text, const wchar_t *part)
{
    int length = (int)wcslen(part);
    for (; *text; text++)
        if (CompareStringOrdinal(text, (int)wcsnlen(text, (size_t)length), part, length, TRUE) == CSTR_EQUAL) return text;
    return length == 0 ? text : NULL;
}

bool wcs_equal(const wchar_t *first, const wchar_t *second)
{
    if (!first || !second) return first == second;
    return wcscmp(first, second) == 0;
}

bool wcs_equal_ignore_case(const wchar_t *first, const wchar_t *second)
{
    if (!first || !second) return first == second;
    return CompareStringOrdinal(first, -1, second, -1, TRUE) == CSTR_EQUAL;
}

static void wstr_reserve(WStr *string, size_t extra)
{
    size_t needed = string->length + extra + 1;
    if (needed <= string->capacity) return;
    size_t capacity = string->capacity ? string->capacity * 2 : 64;
    while (capacity < needed) capacity *= 2;
    string->data = xrealloc(string->data, capacity * sizeof(wchar_t));
    string->capacity = capacity;
}

void wstr_append_n(WStr *string, const wchar_t *text, size_t length)
{
    wstr_reserve(string, length);
    if (length) memcpy(string->data + string->length, text, length * sizeof(wchar_t));
    string->length += length;
    string->data[string->length] = 0;
}

void wstr_append(WStr *string, const wchar_t *text)
{
    if (text) wstr_append_n(string, text, wcslen(text));
}

void wstr_append_char(WStr *string, wchar_t character)
{
    wstr_append_n(string, &character, 1);
}

void wstr_appendv(WStr *string, const wchar_t *format, va_list arguments)
{
    va_list copy;
    va_copy(copy, arguments);
    int length = _vscwprintf(format, copy);
    va_end(copy);
    if (length <= 0) return;
    wstr_reserve(string, (size_t)length);
    vswprintf(string->data + string->length, (size_t)length + 1, format, arguments);
    string->length += (size_t)length;
}

void wstr_appendf(WStr *string, const wchar_t *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    wstr_appendv(string, format, arguments);
    va_end(arguments);
}

wchar_t *wstr_take(WStr *string)
{
    wchar_t *result = string->data ? string->data : wcs_dup(L"");
    string->data = NULL;
    string->length = string->capacity = 0;
    return result;
}

void wstr_free(WStr *string)
{
    free(string->data);
    string->data = NULL;
    string->length = string->capacity = 0;
}

void strlist_add_owned(StrList *list, wchar_t *text)
{
    if (list->count == list->capacity)
    {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = xrealloc(list->items, (size_t)list->capacity * sizeof(wchar_t *));
    }
    list->items[list->count++] = text;
}

void strlist_add(StrList *list, const wchar_t *text)
{
    strlist_add_owned(list, wcs_dup(text ? text : L""));
}

void strlist_clear(StrList *list)
{
    for (int index = 0; index < list->count; index++) free(list->items[index]);
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

void strlist_clone(StrList *target, const StrList *source)
{
    memset(target, 0, sizeof(*target));
    for (int index = 0; index < source->count; index++) strlist_add(target, source->items[index]);
}

int strlist_index_of(const StrList *list, const wchar_t *text, bool ignore_case)
{
    for (int index = 0; index < list->count; index++)
        if (ignore_case ? wcs_equal_ignore_case(list->items[index], text) : wcs_equal(list->items[index], text)) return index;
    return -1;
}

bool strlist_equal(const StrList *first, const StrList *second, bool ignore_case)
{
    if (first->count != second->count) return false;
    for (int index = 0; index < first->count; index++)
    {
        const wchar_t *a = first->items[index], *b = second->items[index];
        if (!(ignore_case ? wcs_equal_ignore_case(a, b) : wcs_equal(a, b))) return false;
    }
    return true;
}

/// text 在集合里的位置；没有时是它该插入的位置。
static int text_set_find(const TextSet *set, const wchar_t *text, bool *found)
{
    int low = 0, high = set->count;
    *found = false;
    while (low < high)
    {
        int middle = low + (high - low) / 2;
        int order = set->ignore_case ? CompareStringOrdinal(set->items[middle], -1, text, -1, TRUE) - CSTR_EQUAL : wcscmp(set->items[middle], text);
        if (order == 0)
        {
            *found = true;
            return middle;
        }
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return low;
}

bool text_set_add(TextSet *set, const wchar_t *text)
{
    bool found;
    int at = text_set_find(set, text, &found);
    if (found) return false;
    if (set->count == set->capacity)
    {
        set->capacity = set->capacity ? set->capacity * 2 : 64;
        set->items = xrealloc((void *)set->items, (size_t)set->capacity * sizeof(const wchar_t *));
    }
    memmove(&set->items[at + 1], &set->items[at], (size_t)(set->count - at) * sizeof(const wchar_t *));
    set->items[at] = text;
    set->count++;
    return true;
}

bool text_set_contains(const TextSet *set, const wchar_t *text)
{
    bool found;
    text_set_find(set, text, &found);
    return found;
}

void text_set_clear(TextSet *set)
{
    free((void *)set->items);
    set->items = NULL;
    set->count = set->capacity = 0;
}

char *utf8_from_wide(const wchar_t *text, size_t *length)
{
    int source = (int)wcslen(text);
    int size = source ? WideCharToMultiByte(CP_UTF8, 0, text, source, NULL, 0, NULL, NULL) : 0;
    char *result = xalloc((size_t)size + 1);
    if (size) WideCharToMultiByte(CP_UTF8, 0, text, source, result, size, NULL, NULL);
    result[size] = 0;
    if (length) *length = (size_t)size;
    return result;
}

wchar_t *wide_from_utf8(const char *text, size_t length)
{
    int size = length ? MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0) : 0;
    wchar_t *result = xalloc(((size_t)size + 1) * sizeof(wchar_t));
    if (size) MultiByteToWideChar(CP_UTF8, 0, text, (int)length, result, size);
    result[size] = 0;
    return result;
}

// ───────────── 路径和文件 ─────────────

static bool is_separator(wchar_t character)
{
    return character == L'\\' || character == L'/';
}

wchar_t *path_join(const wchar_t *directory, const wchar_t *name)
{
    WStr result = {0};
    wstr_append(&result, directory);
    if (result.length > 0 && !is_separator(result.data[result.length - 1])) wstr_append_char(&result, L'\\');
    wstr_append(&result, name);
    return wstr_take(&result);
}

const wchar_t *path_file_name(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *cursor = path; *cursor; cursor++)
        if (is_separator(*cursor) || *cursor == L':') name = cursor + 1;
    return name;
}

wchar_t *path_directory(const wchar_t *path)
{
    const wchar_t *name = path_file_name(path);
    if (name == path) return NULL;
    size_t length = (size_t)(name - path);
    // 去掉末尾的分隔符，但 "C:\" 这样的根目录保留。
    while (length > 1 && is_separator(path[length - 1]) && path[length - 2] != L':') length--;
    return wcs_dup_n(path, length);
}

wchar_t *path_file_stem(const wchar_t *path)
{
    const wchar_t *name = path_file_name(path);
    const wchar_t *dot = wcsrchr(name, L'.');
    return dot ? wcs_dup_n(name, (size_t)(dot - name)) : wcs_dup(name);
}

bool path_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool path_is_directory(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/// 带这些重解析点标记的是真正的文件或文件夹，内容由系统或同步程序按需提供。
static bool reparse_tag_has_content(DWORD tag)
{
    // 云文件（OneDrive 的“按需下载”等）：0x9000001A，以及 0x9000x01A（x 为 1–F）。
    if ((tag & 0xFFFF0FFFu) == 0x9000001Au) return true;
    switch (tag)
    {
        case 0x80000013u:   // 重复数据删除
        case 0x80000017u:   // 系统压缩（WOF）
        case 0x8000001Eu:   // 存储同步
        case 0x80000021u:   // OneDrive（旧版占位文件）
        case 0x9000001Cu:   // 投影文件系统（ProjFS）
            return true;
        default:
            return false;
    }
}

bool find_data_is_link(const WIN32_FIND_DATAW *data)
{
    if (!(data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    // 带重解析点属性时，dwReserved0 里是它的标记；读不出时是 0，按链接处理。
    return !reparse_tag_has_content(data->dwReserved0);
}

bool path_is_link(const wchar_t *path)
{
    // 末尾的分隔符会让 FindFirstFile 去找文件夹里面的东西。
    wchar_t *trimmed = wcs_dup(path);
    size_t length = wcslen(trimmed);
    while (length > 3 && is_separator(trimmed[length - 1])) trimmed[--length] = 0;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(trimmed, &data);
    free(trimmed);
    if (find == INVALID_HANDLE_VALUE) return true;
    FindClose(find);
    return find_data_is_link(&data);
}

bool directory_create(const wchar_t *path)
{
    if (!path || !*path) return false;
    if (path_is_directory(path)) return true;
    wchar_t *parent = path_directory(path);
    if (parent && wcscmp(parent, path) != 0 && !path_is_directory(parent)) directory_create(parent);
    free(parent);
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

wchar_t *file_read_text(const wchar_t *path, size_t maximum)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    wchar_t *text = NULL;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart <= maximum && size.QuadPart < 0x7FFFFFFF)
    {
        size_t length = (size_t)size.QuadPart;
        unsigned char *bytes = xalloc(length + 2);
        DWORD read = 0;
        if (ReadFile(file, bytes, (DWORD)length, &read, NULL) && read == length)
        {
            if (length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
                text = wcs_dup_n((const wchar_t *)(bytes + 2), (length - 2) / sizeof(wchar_t));
            else if (length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
                text = wide_from_utf8((const char *)bytes + 3, length - 3);
            else
                text = wide_from_utf8((const char *)bytes, length);
        }
        free(bytes);
    }
    CloseHandle(file);
    return text;
}

bool file_write_atomic(const wchar_t *path, const void *data, size_t length)
{
    wchar_t *directory = path_directory(path);
    if (directory) directory_create(directory);
    free(directory);

    WStr temporary = {0};
    wstr_append(&temporary, path);
    wstr_append(&temporary, L".tmp");
    bool succeeded = false;
    HANDLE file = CreateFileW(temporary.data, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        succeeded = WriteFile(file, data, (DWORD)length, &written, NULL) && written == length;
        // 先把内容写到磁盘上再换名字：否则突然断电时可能换名已经生效、内容却没写下去，留下一个空文件。
        if (succeeded) succeeded = FlushFileBuffers(file) != 0;
        DWORD error = GetLastError();
        CloseHandle(file);
        if (succeeded)
        {
            succeeded = MoveFileExW(temporary.data, path, MOVEFILE_REPLACE_EXISTING) != 0;
            error = GetLastError();
        }
        if (!succeeded)
        {
            DeleteFileW(temporary.data);
            SetLastError(error);
        }
    }
    wstr_free(&temporary);
    return succeeded;
}

wchar_t *env_get(const wchar_t *name)
{
    DWORD size = GetEnvironmentVariableW(name, NULL, 0);
    if (size <= 1) return NULL;
    wchar_t *value = xalloc(size * sizeof(wchar_t));
    DWORD length = GetEnvironmentVariableW(name, value, size);
    if (length == 0 || length >= size)
    {
        free(value);
        return NULL;
    }
    return value;
}

wchar_t *local_app_data_directory(void)
{
    PWSTR folder = NULL;
    wchar_t *result = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &folder))) result = wcs_dup(folder);
    CoTaskMemFree(folder);
    if (!result) result = env_get(L"LOCALAPPDATA");
    return result ? result : wcs_dup(L".");
}

wchar_t *executable_path(void)
{
    DWORD capacity = MAX_PATH;
    for (;;)
    {
        wchar_t *path = xalloc(capacity * sizeof(wchar_t));
        DWORD length = GetModuleFileNameW(NULL, path, capacity);
        if (length > 0 && length < capacity) return path;
        free(path);
        if (length == 0 || capacity > 32768) return wcs_dup(L"DropShelf.exe");
        capacity *= 2;
    }
}

wchar_t *executable_directory(void)
{
    wchar_t *path = executable_path();
    wchar_t *directory = path_directory(path);
    free(path);
    return directory ? directory : wcs_dup(L".");
}

wchar_t *new_guid_text(void)
{
    GUID guid;
    if (FAILED(CoCreateGuid(&guid)))
    {
        // 极少见：退回到用时间和计数拼一个，仍然足够不重复。
        static LONG counter;
        LARGE_INTEGER ticks;
        QueryPerformanceCounter(&ticks);
        memset(&guid, 0, sizeof(guid));
        guid.Data1 = (unsigned long)ticks.LowPart;
        guid.Data2 = (unsigned short)ticks.HighPart;
        guid.Data3 = (unsigned short)InterlockedIncrement(&counter);
        guid.Data4[0] = (unsigned char)GetCurrentProcessId();
    }
    wchar_t *text = xalloc(33 * sizeof(wchar_t));
    swprintf(text, 33, L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

// ───────────── 后台保存 ─────────────

static DWORD WINAPI save_queue_thread(LPVOID parameter)
{
    SaveQueue *queue = parameter;
    EnterCriticalSection(&queue->lock);
    for (;;)
    {
        while (!queue->data) SleepConditionVariableCS(&queue->changed, &queue->lock, INFINITE);
        wchar_t *path = queue->path;
        char *data = queue->data;
        size_t length = queue->length;
        queue->path = NULL;
        queue->data = NULL;
        queue->busy = true;
        LeaveCriticalSection(&queue->lock);

        if (!file_write_atomic(path, data, length)) log_win32_error(queue->failure_message, GetLastError());
        free(path);
        free(data);

        EnterCriticalSection(&queue->lock);
        queue->busy = false;
        WakeAllConditionVariable(&queue->changed);
    }
}

static BOOL CALLBACK save_queue_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)context;
    SaveQueue *queue = parameter;
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->changed);
    return TRUE;
}

void save_queue_post(SaveQueue *queue, const wchar_t *path, char *data, size_t length)
{
    InitOnceExecuteOnce(&queue->once, save_queue_initialize, queue, NULL);
    EnterCriticalSection(&queue->lock);
    free(queue->path);
    free(queue->data);
    queue->path = wcs_dup(path);
    queue->data = data;
    queue->length = length;
    if (!queue->thread) queue->thread = CreateThread(NULL, 0, save_queue_thread, queue, 0, NULL);
    bool has_thread = queue->thread != NULL;
    if (!has_thread)
    {
        // 开不了线程时就在这里写，不能丢掉这次保存。
        if (!file_write_atomic(queue->path, queue->data, queue->length)) log_win32_error(queue->failure_message, GetLastError());
        free(queue->path);
        free(queue->data);
        queue->path = NULL;
        queue->data = NULL;
    }
    WakeAllConditionVariable(&queue->changed);
    LeaveCriticalSection(&queue->lock);
}

void save_queue_flush(SaveQueue *queue)
{
    InitOnceExecuteOnce(&queue->once, save_queue_initialize, queue, NULL);
    EnterCriticalSection(&queue->lock);
    while (queue->data || queue->busy) SleepConditionVariableCS(&queue->changed, &queue->lock, INFINITE);
    LeaveCriticalSection(&queue->lock);
}
