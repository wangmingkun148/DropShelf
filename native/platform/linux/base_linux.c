// base.h 的 POSIX 实现。
//
// 共享代码是按 Win32 的形状写的：HANDLE、GetLastError、WIN32_FIND_DATAW、io_*。这里用 POSIX 实现同一套接口，
// 让文件逻辑（复制、移动、压缩、附件缓存……）在 Linux 上原样复用。路径在边界上转成 UTF-8 字节。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "base.h"
#include "file_ops.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

// ───────────── 错误码 ─────────────

static _Thread_local DWORD thread_error;

DWORD GetLastError(void)
{
    return thread_error;
}

void SetLastError(DWORD error)
{
    thread_error = error;
}

static DWORD error_from_errno(int error)
{
    switch (error)
    {
        case 0: return ERROR_SUCCESS;
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:
        case EROFS: return ERROR_ACCESS_DENIED;
        case EBADF: return ERROR_INVALID_HANDLE;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case EEXIST: return ERROR_FILE_EXISTS;
        case ENOTEMPTY:
        case EISDIR: return ERROR_DIR_NOT_EMPTY;
        case ENOSPC:
        case EDQUOT: return ERROR_DISK_FULL;
        case EXDEV: return ERROR_NOT_SAME_DEVICE;
        case EINVAL: return ERROR_INVALID_PARAMETER;
        case ENOSYS: return ERROR_NOT_SUPPORTED;
        case EINTR: return ERROR_CANCELLED;
        default: return (DWORD)error;
    }
}

static void set_error_from_errno(void)
{
    SetLastError(error_from_errno(errno));
}

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

// ───────────── UTF-8 / UTF-32 ─────────────

/// 把一个码位写成 UTF-8；返回写出的字节数。不合法的码位写成 U+FFFD。
static size_t encode_utf8(uint32_t code, char *out)
{
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
    if (code < 0x80)
    {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800)
    {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000)
    {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (code >> 18));
    out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

char *utf8_from_wide(const wchar_t *text, size_t *length)
{
    size_t source = text ? wcslen(text) : 0;
    char *result = xalloc(source * 4 + 1);
    size_t written = 0;
    for (size_t index = 0; index < source; index++)
        written += encode_utf8((uint32_t)text[index], result + written);
    result[written] = 0;
    if (length) *length = written;
    return result;
}

/// 读一个 UTF-8 字符；不合法时返回 U+FFFD 并只前进一个字节。
static uint32_t decode_utf8(const unsigned char *text, size_t length, size_t *used)
{
    unsigned char first = text[0];
    if (first < 0x80)
    {
        *used = 1;
        return first;
    }
    size_t count = 0;
    uint32_t code = 0;
    if ((first & 0xE0) == 0xC0)
    {
        count = 2;
        code = first & 0x1F;
    }
    else if ((first & 0xF0) == 0xE0)
    {
        count = 3;
        code = first & 0x0F;
    }
    else if ((first & 0xF8) == 0xF0)
    {
        count = 4;
        code = first & 0x07;
    }
    else
    {
        *used = 1;
        return 0xFFFD;
    }
    if (count > length)
    {
        *used = 1;
        return 0xFFFD;
    }
    for (size_t index = 1; index < count; index++)
    {
        if ((text[index] & 0xC0) != 0x80)
        {
            *used = 1;
            return 0xFFFD;
        }
        code = (code << 6) | (text[index] & 0x3F);
    }
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) || (count == 2 && code < 0x80) || (count == 3 && code < 0x800) ||
        (count == 4 && code < 0x10000))
    {
        *used = 1;
        return 0xFFFD;
    }
    *used = count;
    return code;
}

wchar_t *wide_from_utf8(const char *text, size_t length)
{
    wchar_t *result = xalloc((length + 1) * sizeof(wchar_t));
    size_t used = 0, written = 0;
    while (used < length)
    {
        size_t step = 0;
        uint32_t code = decode_utf8((const unsigned char *)text + used, length - used, &step);
        result[written++] = (wchar_t)code;
        used += step;
    }
    result[written] = 0;
    return result;
}

/// UTF-16 LE 字节 → UTF-32，处理代理对。file_read_text 读 Windows 写下的 UTF-16 设置文件时用。
static wchar_t *wide_from_utf16le(const unsigned char *bytes, size_t length)
{
    size_t units = length / 2;
    wchar_t *result = xalloc((units + 1) * sizeof(wchar_t));
    size_t written = 0;
    for (size_t index = 0; index < units; index++)
    {
        uint32_t unit = (uint32_t)bytes[index * 2] | ((uint32_t)bytes[index * 2 + 1] << 8);
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units)
        {
            uint32_t next = (uint32_t)bytes[index * 2 + 2] | ((uint32_t)bytes[index * 2 + 3] << 8);
            if (next >= 0xDC00 && next <= 0xDFFF)
            {
                result[written++] = (wchar_t)(0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                index++;
                continue;
            }
        }
        result[written++] = (wchar_t)((unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
    }
    result[written] = 0;
    return result;
}

static wchar_t *wide_from_utf16be(const unsigned char *bytes, size_t length)
{
    size_t units = length / 2;
    wchar_t *result = xalloc((units + 1) * sizeof(wchar_t));
    size_t written = 0;
    for (size_t index = 0; index < units; index++)
    {
        uint32_t unit = ((uint32_t)bytes[index * 2] << 8) | (uint32_t)bytes[index * 2 + 1];
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < units)
        {
            uint32_t next = ((uint32_t)bytes[index * 2 + 2] << 8) | (uint32_t)bytes[index * 2 + 3];
            if (next >= 0xDC00 && next <= 0xDFFF)
            {
                result[written++] = (wchar_t)(0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                index++;
                continue;
            }
        }
        result[written++] = (wchar_t)((unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
    }
    result[written] = 0;
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

static wchar_t ordinal_fold(wchar_t character)
{
    return (wchar_t)towlower((wint_t)character);
}

/// 与 CompareStringOrdinal(ignore_case=TRUE) 的大小关系一致：只比字符的序数，不加语言规则。
static int ordinal_compare_n(const wchar_t *first, size_t first_length, const wchar_t *second, size_t second_length, bool ignore_case)
{
    size_t length = first_length < second_length ? first_length : second_length;
    for (size_t index = 0; index < length; index++)
    {
        wchar_t a = ignore_case ? ordinal_fold(first[index]) : first[index];
        wchar_t b = ignore_case ? ordinal_fold(second[index]) : second[index];
        if (a != b) return a < b ? -1 : 1;
    }
    if (first_length == second_length) return 0;
    return first_length < second_length ? -1 : 1;
}

const wchar_t *wcs_find_ignore_case(const wchar_t *text, const wchar_t *part)
{
    size_t part_length = wcslen(part);
    if (part_length == 0) return text;
    for (const wchar_t *cursor = text; *cursor; cursor++)
    {
        size_t left = wcslen(cursor);
        if (left < part_length) break;
        if (ordinal_compare_n(cursor, part_length, part, part_length, true) == 0) return cursor;
    }
    return NULL;
}

bool wcs_equal(const wchar_t *first, const wchar_t *second)
{
    if (!first || !second) return first == second;
    return wcscmp(first, second) == 0;
}

bool wcs_equal_ignore_case(const wchar_t *first, const wchar_t *second)
{
    if (!first || !second) return first == second;
    return ordinal_compare_n(first, wcslen(first), second, wcslen(second), true) == 0;
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
    wchar_t *translated = wide_format_translate(format);
    const wchar_t *fmt = translated ? translated : format;
    size_t capacity = 256;
    for (;;)
    {
        wchar_t *buffer = xalloc(capacity * sizeof(wchar_t));
        va_list copy;
        va_copy(copy, arguments);
        int written = vswprintf(buffer, capacity, fmt, copy);
        va_end(copy);
        if (written >= 0)
        {
            wstr_append_n(string, buffer, (size_t)written);
            free(buffer);
            break;
        }
        // glibc 在缓冲不够时返回 -1，不告诉我们实际要多长；成倍加大再试。
        free(buffer);
        if (capacity >= (size_t)16 * 1024 * 1024) break;
        capacity *= 2;
    }
    free(translated);
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

static int text_set_find(const TextSet *set, const wchar_t *text, bool *found)
{
    int low = 0, high = set->count;
    *found = false;
    while (low < high)
    {
        int middle = low + (high - low) / 2;
        int order = ordinal_compare_n(set->items[middle], wcslen(set->items[middle]), text, wcslen(text), set->ignore_case);
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

// ───────────── 路径 ─────────────

static bool is_separator(wchar_t character)
{
    return character == L'/';
}

wchar_t *path_join(const wchar_t *directory, const wchar_t *name)
{
    WStr result = {0};
    wstr_append(&result, directory);
    if (result.length > 0 && !is_separator(result.data[result.length - 1])) wstr_append_char(&result, L'/');
    wstr_append(&result, name);
    return wstr_take(&result);
}

const wchar_t *path_file_name(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *cursor = path; *cursor; cursor++)
        if (is_separator(*cursor)) name = cursor + 1;
    return name;
}

wchar_t *path_directory(const wchar_t *path)
{
    const wchar_t *name = path_file_name(path);
    if (name == path) return NULL;
    size_t length = (size_t)(name - path);
    // 去掉末尾的分隔符，但根目录 “/” 保留。
    while (length > 1 && is_separator(path[length - 1])) length--;
    return wcs_dup_n(path, length);
}

wchar_t *path_file_stem(const wchar_t *path)
{
    const wchar_t *name = path_file_name(path);
    const wchar_t *dot = wcsrchr(name, L'.');
    return dot ? wcs_dup_n(name, (size_t)(dot - name)) : wcs_dup(name);
}

/// 路径 → 给 POSIX 用的 UTF-8 字节。调用方 free。
static char *path_bytes(const wchar_t *path)
{
    return utf8_from_wide(path, NULL);
}

wchar_t *path_for_io(const wchar_t *path)
{
    // Linux 没有 260 个字符的限制，也不需要 \\?\ 前缀。
    return path ? wcs_dup(path) : NULL;
}

// ───────────── 句柄 ─────────────

typedef struct FindState
{
    DIR *directory;
    char *pattern;
    char *directory_path;
    WIN32_FIND_DATAW data;
} FindState;

static HANDLE handle_from_fd(int fd)
{
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)(fd + 1);
}

static int fd_from_handle(HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) return -1;
    return (int)(intptr_t)handle - 1;
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *security, DWORD disposition, DWORD flags, HANDLE template_file)
{
    (void)share;
    (void)security;
    (void)template_file;
    if (!path)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    char *bytes = path_bytes(path);
    int open_flags = O_CLOEXEC;
    bool read = (access & (GENERIC_READ | FILE_READ_DATA)) != 0;
    bool write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    if (access == 0) read = true;
    if (read && write) open_flags |= O_RDWR;
    else if (write) open_flags |= O_WRONLY;
    else open_flags |= O_RDONLY;
    if (access & FILE_APPEND_DATA) open_flags |= O_APPEND;
    if (flags & FILE_FLAG_OPEN_REPARSE_POINT) open_flags |= O_NOFOLLOW;

    switch (disposition)
    {
        case CREATE_NEW: open_flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: open_flags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: open_flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: open_flags |= O_TRUNC; break;
        default: break;
    }

    int fd = open(bytes, open_flags, 0666);
    if (fd < 0)
    {
        free(bytes);
        set_error_from_errno();
        return INVALID_HANDLE_VALUE;
    }
    // 打开以后马上删掉名字：句柄关闭时文件自然消失。
    if (flags & FILE_FLAG_DELETE_ON_CLOSE) unlink(bytes);
    free(bytes);
    SetLastError(ERROR_SUCCESS);
    return handle_from_fd(fd);
}

BOOL CloseHandle(HANDLE handle)
{
    int fd = fd_from_handle(handle);
    if (fd < 0)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (close(fd) != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    return TRUE;
}

BOOL ReadFile(HANDLE handle, LPVOID buffer, DWORD bytes, DWORD *read_count, void *overlapped)
{
    (void)overlapped;
    int fd = fd_from_handle(handle);
    if (fd < 0)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    size_t done = 0;
    while (done < bytes)
    {
        ssize_t got = read(fd, (char *)buffer + done, bytes - done);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            set_error_from_errno();
            return FALSE;
        }
        if (got == 0) break;
        done += (size_t)got;
    }
    if (read_count) *read_count = (DWORD)done;
    return TRUE;
}

BOOL WriteFile(HANDLE handle, LPCVOID buffer, DWORD bytes, DWORD *written, void *overlapped)
{
    (void)overlapped;
    int fd = fd_from_handle(handle);
    if (fd < 0)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    size_t done = 0;
    while (done < bytes)
    {
        ssize_t put = write(fd, (const char *)buffer + done, bytes - done);
        if (put < 0)
        {
            if (errno == EINTR) continue;
            set_error_from_errno();
            return FALSE;
        }
        done += (size_t)put;
    }
    if (written) *written = (DWORD)done;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER *size)
{
    int fd = fd_from_handle(handle);
    struct stat information;
    if (fd < 0 || !size || fstat(fd, &information) != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    size->QuadPart = (LONGLONG)information.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE handle)
{
    int fd = fd_from_handle(handle);
    if (fd < 0 || fsync(fd) != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    return TRUE;
}

// ───────────── 文件属性 ─────────────

static void filetime_from_timespec(const struct timespec *time, FILETIME *file_time)
{
    uint64_t ticks = (uint64_t)(time->tv_sec + 11644473600LL) * 10000000ULL + (uint64_t)time->tv_nsec / 100ULL;
    file_time->dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFu);
    file_time->dwHighDateTime = (DWORD)(ticks >> 32);
}

static DWORD attributes_from_stat(const struct stat *information)
{
    DWORD attributes = 0;
    if (S_ISDIR(information->st_mode)) attributes |= FILE_ATTRIBUTE_DIRECTORY;
    if (S_ISLNK(information->st_mode)) attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    if (!(information->st_mode & S_IWUSR)) attributes |= FILE_ATTRIBUTE_READONLY;
    if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
    return attributes;
}

static DWORD attributes_of_path(LPCWSTR path)
{
    char *bytes = path_bytes(path);
    struct stat information;
    if (lstat(bytes, &information) != 0)
    {
        free(bytes);
        set_error_from_errno();
        return INVALID_FILE_ATTRIBUTES;
    }
    free(bytes);
    SetLastError(ERROR_SUCCESS);
    return attributes_from_stat(&information);
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    return attributes_of_path(path);
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *information)
{
    if (level != GetFileExInfoStandard || !information)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    char *bytes = path_bytes(path);
    struct stat status;
    if (lstat(bytes, &status) != 0)
    {
        free(bytes);
        set_error_from_errno();
        return FALSE;
    }
    free(bytes);
    WIN32_FILE_ATTRIBUTE_DATA *data = information;
    data->dwFileAttributes = attributes_from_stat(&status);
    filetime_from_timespec(&status.st_ctim, &data->ftCreationTime);
    filetime_from_timespec(&status.st_atim, &data->ftLastAccessTime);
    filetime_from_timespec(&status.st_mtim, &data->ftLastWriteTime);
    data->nFileSizeHigh = (DWORD)((uint64_t)status.st_size >> 32);
    data->nFileSizeLow = (DWORD)((uint64_t)status.st_size & 0xFFFFFFFFu);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes)
{
    char *bytes = path_bytes(path);
    struct stat status;
    if (stat(bytes, &status) != 0)
    {
        free(bytes);
        set_error_from_errno();
        return FALSE;
    }
    mode_t mode = status.st_mode;
    if (attributes & FILE_ATTRIBUTE_READONLY) mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
    else mode |= S_IWUSR;
    int result = chmod(bytes, mode);
    free(bytes);
    if (result != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    return TRUE;
}

// ───────────── 目录枚举 ─────────────

static void fill_find_data(const char *directory, const char *name, WIN32_FIND_DATAW *data)
{
    memset(data, 0, sizeof(*data));
    char *full = NULL;
    size_t needed = strlen(directory) + strlen(name) + 2;
    full = xalloc(needed);
    snprintf(full, needed, "%s/%s", directory, name);
    struct stat status;
    if (lstat(full, &status) == 0)
    {
        data->dwFileAttributes = attributes_from_stat(&status);
        filetime_from_timespec(&status.st_ctim, &data->ftCreationTime);
        filetime_from_timespec(&status.st_atim, &data->ftLastAccessTime);
        filetime_from_timespec(&status.st_mtim, &data->ftLastWriteTime);
        data->nFileSizeHigh = (DWORD)((uint64_t)status.st_size >> 32);
        data->nFileSizeLow = (DWORD)((uint64_t)status.st_size & 0xFFFFFFFFu);
    }
    free(full);
    wchar_t *wide_name = wide_from_utf8(name, strlen(name));
    wcsncpy(data->cFileName, wide_name, 1023);
    data->cFileName[1023] = 0;
    free(wide_name);
}

/// 一次 FindFirstFile：把目录和通配符拆开，打开目录。
static HANDLE find_first(LPCWSTR pattern, WIN32_FIND_DATAW *data)
{
    if (!pattern || !data)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const wchar_t *name = path_file_name(pattern);
    wchar_t *directory = name == pattern ? wcs_dup(L".") : wcs_dup_n(pattern, (size_t)(name - pattern));
    if (directory[0] == 0)
    {
        free(directory);
        directory = wcs_dup(L".");
    }

    char *directory_bytes = path_bytes(directory);
    char *pattern_bytes = path_bytes(name);
    // Windows 的 *.* 和 * 一样。
    if (strcmp(pattern_bytes, "*.*") == 0)
    {
        free(pattern_bytes);
        pattern_bytes = xalloc(2);
        memcpy(pattern_bytes, "*", 2);
    }
    free(directory);

    DIR *opened = opendir(directory_bytes);
    if (!opened)
    {
        set_error_from_errno();
        free(directory_bytes);
        free(pattern_bytes);
        return INVALID_HANDLE_VALUE;
    }

    FindState *state = xcalloc(1, sizeof(*state));
    state->directory = opened;
    state->directory_path = directory_bytes;
    state->pattern = pattern_bytes;
    HANDLE handle = FindNextFileW(state, data) ? (HANDLE)state : INVALID_HANDLE_VALUE;
    if (handle == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_NO_MORE_FILES);
        closedir(opened);
        free(directory_bytes);
        free(pattern_bytes);
        free(state);
    }
    return handle;
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *data)
{
    return find_first(pattern, data);
}

HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, void *data, FINDEX_SEARCH_OPS search, void *filter, DWORD flags)
{
    (void)level;
    (void)search;
    (void)filter;
    (void)flags;
    return find_first(pattern, data);
}

BOOL FindNextFileW(HANDLE find, WIN32_FIND_DATAW *data)
{
    FindState *state = find;
    if (!state || !data)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    struct dirent *entry;
    while ((entry = readdir(state->directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            if (strcmp(state->pattern, ".") != 0 && strcmp(state->pattern, "..") != 0) continue;
        }
        if (fnmatch(state->pattern, entry->d_name, 0) != 0) continue;
        fill_find_data(state->directory_path, entry->d_name, data);
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

BOOL FindClose(HANDLE find)
{
    FindState *state = find;
    if (!state)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    closedir(state->directory);
    free(state->directory_path);
    free(state->pattern);
    free(state);
    return TRUE;
}

// ───────────── 目录和文件操作 ─────────────

BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES *security)
{
    (void)security;
    char *bytes = path_bytes(path);
    int result = mkdir(bytes, 0777);
    free(bytes);
    if (result != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    return TRUE;
}

BOOL RemoveDirectoryW(LPCWSTR path)
{
    char *bytes = path_bytes(path);
    int result = rmdir(bytes);
    free(bytes);
    if (result != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR path)
{
    char *bytes = path_bytes(path);
    int result = unlink(bytes);
    free(bytes);
    if (result != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    return TRUE;
}

static BOOL copy_file_impl(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists, LPPROGRESS_ROUTINE progress, void *data, BOOL *cancel)
{
    char *source = path_bytes(from);
    char *target = path_bytes(to);
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0)
    {
        set_error_from_errno();
        free(source);
        free(target);
        return FALSE;
    }
    int output = open(target, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0666);
    if (output < 0)
    {
        set_error_from_errno();
        close(input);
        free(source);
        free(target);
        return FALSE;
    }

    char buffer[64 * 1024];
    LARGE_INTEGER total = { .QuadPart = 0 }, transferred = { .QuadPart = 0 };
    struct stat information;
    memset(&information, 0, sizeof(information));
    bool have_information = fstat(input, &information) == 0;
    if (have_information) total.QuadPart = information.st_size;
    BOOL ok = TRUE;
    for (;;)
    {
        if (cancel && *cancel)
        {
            SetLastError(ERROR_CANCELLED);
            ok = FALSE;
            break;
        }
        ssize_t got = read(input, buffer, sizeof(buffer));
        if (got < 0)
        {
            if (errno == EINTR) continue;
            set_error_from_errno();
            ok = FALSE;
            break;
        }
        if (got == 0) break;
        size_t done = 0;
        while (done < (size_t)got)
        {
            ssize_t put = write(output, buffer + done, (size_t)got - done);
            if (put < 0)
            {
                if (errno == EINTR) continue;
                set_error_from_errno();
                ok = FALSE;
                break;
            }
            done += (size_t)put;
        }
        if (!ok) break;
        transferred.QuadPart += got;
        if (progress)
        {
            DWORD reason = progress(total, transferred, total, transferred, 0, 0, (HANDLE)(intptr_t)(input + 1),
                (HANDLE)(intptr_t)(output + 1), data);
            if (reason != 0)
            {
                SetLastError(ERROR_CANCELLED);
                ok = FALSE;
                break;
            }
        }
    }
    // 权限照源文件保留；保留不了也不影响内容。
    if (ok && have_information) fchmod(output, information.st_mode & 07777);
    close(input);
    if (close(output) != 0) ok = FALSE;
    if (!ok) unlink(target);
    free(source);
    free(target);
    return ok;
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    return copy_file_impl(from, to, fail_if_exists, NULL, NULL, NULL);
}

BOOL CopyFileExW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, void *data, BOOL *cancel, DWORD flags)
{
    return copy_file_impl(from, to, (flags & COPY_FILE_FAIL_IF_EXISTS) != 0, progress, data, cancel);
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    char *source = path_bytes(from);
    char *target = path_bytes(to);
    if (!(flags & MOVEFILE_REPLACE_EXISTING))
    {
        struct stat information;
        if (lstat(target, &information) == 0)
        {
            SetLastError(ERROR_ALREADY_EXISTS);
            free(source);
            free(target);
            return FALSE;
        }
    }
    int result = rename(source, target);
    if (result != 0 && errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED))
    {
        BOOL ok = copy_file_impl(from, to, TRUE, NULL, NULL, NULL);
        if (ok) unlink(source);
        free(source);
        free(target);
        return ok;
    }
    if (result != 0)
    {
        set_error_from_errno();
        free(source);
        free(target);
        return FALSE;
    }
    free(source);
    free(target);
    return TRUE;
}

BOOL GetDiskFreeSpaceExW(LPCWSTR directory, ULARGE_INTEGER *available, ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes)
{
    char *bytes = path_bytes(directory);
    struct statvfs information;
    if (statvfs(bytes, &information) != 0)
    {
        free(bytes);
        set_error_from_errno();
        return FALSE;
    }
    free(bytes);
    if (available) available->QuadPart = (ULONGLONG)information.f_bavail * information.f_frsize;
    if (total) total->QuadPart = (ULONGLONG)information.f_blocks * information.f_frsize;
    if (free_bytes) free_bytes->QuadPart = (ULONGLONG)information.f_bfree * information.f_frsize;
    return TRUE;
}

BOOL GetFileInformationByHandle(HANDLE handle, BY_HANDLE_FILE_INFORMATION *information)
{
    int fd = fd_from_handle(handle);
    struct stat status;
    if (fd < 0 || !information || fstat(fd, &status) != 0)
    {
        set_error_from_errno();
        return FALSE;
    }
    memset(information, 0, sizeof(*information));
    information->dwFileAttributes = attributes_from_stat(&status);
    filetime_from_timespec(&status.st_ctim, &information->ftCreationTime);
    filetime_from_timespec(&status.st_atim, &information->ftLastAccessTime);
    filetime_from_timespec(&status.st_mtim, &information->ftLastWriteTime);
    information->dwVolumeSerialNumber = (DWORD)status.st_dev;
    information->nFileSizeHigh = (DWORD)((uint64_t)status.st_size >> 32);
    information->nFileSizeLow = (DWORD)((uint64_t)status.st_size & 0xFFFFFFFFu);
    information->nNumberOfLinks = (DWORD)status.st_nlink;
    information->nFileIndexHigh = (DWORD)((uint64_t)status.st_ino >> 32);
    information->nFileIndexLow = (DWORD)((uint64_t)status.st_ino & 0xFFFFFFFFu);
    return TRUE;
}

// ───────────── io_*：先换成长路径再调用 ─────────────

HANDLE io_create_file(const wchar_t *path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *security, DWORD disposition, DWORD flags, HANDLE template_file)
{
    wchar_t *target = path_for_io(path);
    HANDLE handle = CreateFileW(target, access, share, security, disposition, flags, template_file);
    free(target);
    return handle;
}

DWORD io_get_attributes(const wchar_t *path)
{
    wchar_t *target = path_for_io(path);
    DWORD attributes = GetFileAttributesW(target);
    free(target);
    return attributes;
}

BOOL io_get_attributes_ex(const wchar_t *path, GET_FILEEX_INFO_LEVELS level, void *information)
{
    wchar_t *target = path_for_io(path);
    BOOL ok = GetFileAttributesExW(target, level, information);
    free(target);
    return ok;
}

BOOL io_set_attributes(const wchar_t *path, DWORD attributes)
{
    wchar_t *target = path_for_io(path);
    BOOL ok = SetFileAttributesW(target, attributes);
    free(target);
    return ok;
}

HANDLE io_find_first(const wchar_t *pattern, FINDEX_INFO_LEVELS level, void *data, FINDEX_SEARCH_OPS search, void *filter, DWORD flags)
{
    // pattern 已经是完整的通配符路径。FindFirstFileExW 在 Linux 上不区分级别和筛选。
    (void)level;
    (void)search;
    (void)filter;
    (void)flags;
    return FindFirstFileW(pattern, data);
}

BOOL io_create_directory(const wchar_t *path, SECURITY_ATTRIBUTES *security)
{
    wchar_t *target = path_for_io(path);
    BOOL ok = CreateDirectoryW(target, security);
    free(target);
    return ok;
}

BOOL io_remove_directory(const wchar_t *path)
{
    wchar_t *target = path_for_io(path);
    BOOL ok = RemoveDirectoryW(target);
    free(target);
    return ok;
}

BOOL io_delete_file(const wchar_t *path)
{
    wchar_t *target = path_for_io(path);
    BOOL ok = DeleteFileW(target);
    free(target);
    return ok;
}

BOOL io_move_file(const wchar_t *from, const wchar_t *to, DWORD flags)
{
    wchar_t *source = path_for_io(from), *target = path_for_io(to);
    BOOL ok = MoveFileExW(source, target, flags);
    free(source);
    free(target);
    return ok;
}

BOOL io_copy_file(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists)
{
    wchar_t *source = path_for_io(from), *target = path_for_io(to);
    BOOL ok = CopyFileW(source, target, fail_if_exists);
    free(source);
    free(target);
    return ok;
}

BOOL io_copy_file_ex(const wchar_t *from, const wchar_t *to, LPPROGRESS_ROUTINE progress, void *data, BOOL *cancel, DWORD flags)
{
    wchar_t *source = path_for_io(from), *target = path_for_io(to);
    BOOL ok = CopyFileExW(source, target, progress, data, cancel, flags);
    free(source);
    free(target);
    return ok;
}

BOOL io_disk_free_space(const wchar_t *directory, ULARGE_INTEGER *available, ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes)
{
    wchar_t *target = path_for_io(directory);
    BOOL ok = GetDiskFreeSpaceExW(target, available, total, free_bytes);
    free(target);
    return ok;
}

// ───────────── 路径判断和创建 ─────────────

bool path_exists(const wchar_t *path)
{
    return io_get_attributes(path) != INVALID_FILE_ATTRIBUTES;
}

bool path_is_directory(const wchar_t *path)
{
    DWORD attributes = io_get_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool find_data_is_link(const WIN32_FIND_DATAW *data)
{
    return (data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool path_is_link(const wchar_t *path)
{
    char *bytes = path_bytes(path);
    struct stat status;
    bool result = lstat(bytes, &status) != 0 || S_ISLNK(status.st_mode);
    free(bytes);
    return result;
}

bool directory_create(const wchar_t *path)
{
    if (!path || !*path) return false;
    if (path_is_directory(path)) return true;
    wchar_t *parent = path_directory(path);
    if (parent && wcscmp(parent, path) != 0 && !path_is_directory(parent)) directory_create(parent);
    free(parent);
    if (io_create_directory(path, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS;
}

// ───────────── 设置文件 ─────────────

wchar_t *file_read_text(const wchar_t *path, size_t maximum)
{
    HANDLE file = io_create_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    wchar_t *text = NULL;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart <= maximum && size.QuadPart < 0x7FFFFFFF)
    {
        size_t length = (size_t)size.QuadPart;
        unsigned char *bytes = xalloc(length + 2);
        DWORD read_count = 0;
        if (ReadFile(file, bytes, (DWORD)length, &read_count, NULL) && read_count == length)
        {
            if (length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
                text = wide_from_utf16le(bytes + 2, length - 2);
            else if (length >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
                text = wide_from_utf16be(bytes + 2, length - 2);
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

bool file_name_is_reserved(const wchar_t *name)
{
    // Linux 没有 CON、NUL 这样的设备名。
    (void)name;
    return false;
}

void file_keep_unreadable(const wchar_t *path)
{
    WStr backup = {0};
    wstr_appendf(&backup, L"%ls.bad", path);
    if (io_copy_file(path, backup.data, FALSE)) log_error(L"原来的文件已另存为 %ls", backup.data);
    else log_win32_error(L"无法另存读不懂的文件", GetLastError());
    wstr_free(&backup);
}

bool file_write_atomic(const wchar_t *path, const void *data, size_t length)
{
    wchar_t *directory = path_directory(path);
    if (directory) directory_create(directory);

    char *target = path_bytes(path);
    char *directory_bytes = directory ? path_bytes(directory) : NULL;
    free(directory);
    if (!target)
    {
        free(directory_bytes);
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    size_t target_length = strlen(target);
    char *temporary = xalloc(target_length + 12);
    snprintf(temporary, target_length + 12, "%s.XXXXXX", target);

    struct stat existing;
    bool has_existing = stat(target, &existing) == 0 && S_ISREG(existing.st_mode);

    bool succeeded = false;
    int saved_errno = 0;
    int fd = mkstemp(temporary);
    if (fd >= 0)
    {
        const unsigned char *bytes = data;
        size_t written = 0;
        succeeded = true;
        while (written < length)
        {
            ssize_t part = write(fd, bytes + written, length - written);
            if (part < 0)
            {
                if (errno == EINTR) continue;
                saved_errno = errno;
                succeeded = false;
                break;
            }
            if (part == 0)
            {
                saved_errno = EIO;
                succeeded = false;
                break;
            }
            written += (size_t)part;
        }
        // 新建文件保留 mkstemp 的 0600，覆盖已有文件时沿用原权限，避免绕过 umask 暴露私有配置和历史记录。
        if (succeeded && has_existing) (void)fchmod(fd, existing.st_mode & 0777);
        if (succeeded && fsync(fd) != 0)
        {
            saved_errno = errno;
            succeeded = false;
        }
        if (close(fd) != 0 && succeeded)
        {
            saved_errno = errno;
            succeeded = false;
        }
        if (succeeded)
        {
            if (rename(temporary, target) != 0)
            {
                saved_errno = errno;
                succeeded = false;
            }
            else if (directory_bytes)
            {
                int dir_fd = open(directory_bytes, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (dir_fd >= 0)
                {
                    (void)fsync(dir_fd);
                    close(dir_fd);
                }
            }
        }
        if (!succeeded) unlink(temporary);
    }
    else
    {
        saved_errno = errno;
    }

    free(temporary);
    free(target);
    free(directory_bytes);
    if (!succeeded) SetLastError(error_from_errno(saved_errno));
    return succeeded;
}

// ───────────── 环境变量和目录 ─────────────

wchar_t *env_get(const wchar_t *name)
{
    char *bytes = path_bytes(name);
    const char *value = getenv(bytes);
    free(bytes);
    if (!value || !*value) return NULL;
    return wide_from_utf8(value, strlen(value));
}

/// XDG 目录：变量有值时用它，否则用 $HOME 下的默认位置。
static wchar_t *xdg_directory(const char *variable, const char *fallback)
{
    const char *root = getenv(variable);
    if (root && *root) return wide_from_utf8(root, strlen(root));
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    WStr path = {0};
    wchar_t *wide_home = wide_from_utf8(home, strlen(home));
    wstr_append(&path, wide_home);
    free(wide_home);
    wstr_append_char(&path, L'/');
    wchar_t *wide_fallback = wide_from_utf8(fallback, strlen(fallback));
    wstr_append(&path, wide_fallback);
    free(wide_fallback);
    return wstr_take(&path);
}

static wchar_t *xdg_app_directory(const char *variable, const char *fallback)
{
    wchar_t *root = xdg_directory(variable, fallback);
    wchar_t *directory = path_join(root, L"dropshelf");
    free(root);
    return directory;
}

wchar_t *local_app_data_directory(void)
{
    return data_directory();
}

wchar_t *config_directory(void)
{
    return xdg_app_directory("XDG_CONFIG_HOME", ".config");
}

wchar_t *data_directory(void)
{
    return xdg_app_directory("XDG_DATA_HOME", ".local/share");
}

wchar_t *cache_directory(void)
{
    return xdg_app_directory("XDG_CACHE_HOME", ".cache");
}

wchar_t *state_directory(void)
{
    return xdg_app_directory("XDG_STATE_HOME", ".local/state");
}

wchar_t *executable_path(void)
{
    size_t capacity = 256;
    for (;;)
    {
        char *buffer = xalloc(capacity);
        ssize_t length = readlink("/proc/self/exe", buffer, capacity);
        if (length > 0 && (size_t)length < capacity)
        {
            wchar_t *path = wide_from_utf8(buffer, (size_t)length);
            free(buffer);
            return path;
        }
        free(buffer);
        if (capacity > 1024 * 1024) return wcs_dup(L"dropshelf");
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

// ───────────── 时间 ─────────────

static uint64_t ticks_from_timespec(const struct timespec *time)
{
    return (uint64_t)(time->tv_sec + 11644473600LL) * 10000000ULL + (uint64_t)time->tv_nsec / 100ULL;
}

static void filetime_from_ticks(uint64_t ticks, FILETIME *file_time)
{
    file_time->dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFu);
    file_time->dwHighDateTime = (DWORD)(ticks >> 32);
}

static uint64_t ticks_from_filetime(const FILETIME *file_time)
{
    return ((uint64_t)file_time->dwHighDateTime << 32) | file_time->dwLowDateTime;
}

void GetSystemTimeAsFileTime(FILETIME *time)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    filetime_from_ticks(ticks_from_timespec(&now), time);
}

void GetSystemTimePreciseAsFileTime(FILETIME *time)
{
    GetSystemTimeAsFileTime(time);
}

BOOL FileTimeToLocalFileTime(const FILETIME *utc, FILETIME *local)
{
    if (!utc || !local) return FALSE;
    time_t seconds = (time_t)(ticks_from_filetime(utc) / 10000000ULL - 11644473600ULL);
    struct tm broken;
    if (!localtime_r(&seconds, &broken))
    {
        *local = *utc;
        return FALSE;
    }
    time_t adjusted = mktime(&broken);
    if (adjusted == (time_t)-1)
    {
        *local = *utc;
        return FALSE;
    }
    struct timespec result = { .tv_sec = adjusted, .tv_nsec = (long)((ticks_from_filetime(utc) % 10000000ULL) * 100ULL) };
    filetime_from_ticks(ticks_from_timespec(&result), local);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *file_time, SYSTEMTIME *system_time)
{
    if (!file_time || !system_time) return FALSE;
    uint64_t ticks = ticks_from_filetime(file_time);
    time_t seconds = (time_t)(ticks / 10000000ULL - 11644473600ULL);
    struct tm broken;
    if (!gmtime_r(&seconds, &broken)) return FALSE;
    system_time->wYear = (WORD)(broken.tm_year + 1900);
    system_time->wMonth = (WORD)(broken.tm_mon + 1);
    system_time->wDayOfWeek = (WORD)broken.tm_wday;
    system_time->wDay = (WORD)broken.tm_mday;
    system_time->wHour = (WORD)broken.tm_hour;
    system_time->wMinute = (WORD)broken.tm_min;
    system_time->wSecond = (WORD)broken.tm_sec;
    system_time->wMilliseconds = (WORD)((ticks % 10000000ULL) / 10000ULL);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME *system_time, FILETIME *file_time)
{
    if (!system_time || !file_time) return FALSE;
    struct tm broken = {0};
    broken.tm_year = system_time->wYear - 1900;
    broken.tm_mon = system_time->wMonth - 1;
    broken.tm_mday = system_time->wDay;
    broken.tm_hour = system_time->wHour;
    broken.tm_min = system_time->wMinute;
    broken.tm_sec = system_time->wSecond;
    broken.tm_isdst = -1;
    time_t seconds = timegm(&broken);
    if (seconds == (time_t)-1) return FALSE;
    uint64_t ticks = (uint64_t)(seconds + 11644473600LL) * 10000000ULL + (uint64_t)system_time->wMilliseconds * 10000ULL;
    filetime_from_ticks(ticks, file_time);
    return TRUE;
}

static void system_time_from_tm(const struct tm *broken, WORD milliseconds, SYSTEMTIME *system_time)
{
    system_time->wYear = (WORD)(broken->tm_year + 1900);
    system_time->wMonth = (WORD)(broken->tm_mon + 1);
    system_time->wDayOfWeek = (WORD)broken->tm_wday;
    system_time->wDay = (WORD)broken->tm_mday;
    system_time->wHour = (WORD)broken->tm_hour;
    system_time->wMinute = (WORD)broken->tm_min;
    system_time->wSecond = (WORD)broken->tm_sec;
    system_time->wMilliseconds = milliseconds;
}

static void tm_from_system_time(const SYSTEMTIME *system_time, struct tm *broken)
{
    memset(broken, 0, sizeof(*broken));
    broken->tm_year = system_time->wYear - 1900;
    broken->tm_mon = system_time->wMonth - 1;
    broken->tm_mday = system_time->wDay;
    broken->tm_hour = system_time->wHour;
    broken->tm_min = system_time->wMinute;
    broken->tm_sec = system_time->wSecond;
    broken->tm_isdst = -1;
}

BOOL SystemTimeToTzSpecificLocalTime(const void *zone, const SYSTEMTIME *universal, SYSTEMTIME *local)
{
    (void)zone;
    if (!universal || !local) return FALSE;
    struct tm broken;
    tm_from_system_time(universal, &broken);
    broken.tm_isdst = 0;
    time_t seconds = timegm(&broken);
    if (seconds == (time_t)-1) return FALSE;
    struct tm adjusted;
    if (!localtime_r(&seconds, &adjusted)) return FALSE;
    system_time_from_tm(&adjusted, universal->wMilliseconds, local);
    return TRUE;
}

BOOL TzSpecificLocalTimeToSystemTime(const void *zone, const SYSTEMTIME *local, SYSTEMTIME *universal)
{
    (void)zone;
    if (!local || !universal) return FALSE;
    struct tm broken;
    tm_from_system_time(local, &broken);
    time_t seconds = mktime(&broken);
    if (seconds == (time_t)-1) return FALSE;
    struct tm adjusted;
    if (!gmtime_r(&seconds, &adjusted)) return FALSE;
    system_time_from_tm(&adjusted, local->wMilliseconds, universal);
    return TRUE;
}

ULONGLONG GetTickCount64(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (ULONGLONG)now.tv_sec * 1000ULL + (ULONGLONG)now.tv_nsec / 1000000ULL;
}

LANGID GetUserDefaultUILanguage(void)
{
    const char *language = getenv("LANG");
    if (!language) language = getenv("LC_ALL");
    if (!language) language = getenv("LC_MESSAGES");
    if (language && (strncmp(language, "zh", 2) == 0 || strncmp(language, "cmn", 3) == 0))
        return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    if (language && strncmp(language, "en", 2) == 0) return MAKELANGID(LANG_ENGLISH, 1);
    return MAKELANGID(LANG_ENGLISH, 1);
}

// ───────────── 标识 ─────────────

wchar_t *new_guid_text(void)
{
    unsigned char bytes[16];
    bool filled = false;
    if (getrandom(bytes, sizeof(bytes), 0) == (ssize_t)sizeof(bytes)) filled = true;
    if (!filled)
    {
        FILE *random = fopen("/dev/urandom", "rb");
        if (random)
        {
            filled = fread(bytes, 1, sizeof(bytes), random) == sizeof(bytes);
            fclose(random);
        }
    }
    if (!filled)
    {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t seed = ticks_from_timespec(&now) ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)&bytes;
        for (size_t index = 0; index < sizeof(bytes); index++)
        {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            bytes[index] = (unsigned char)(seed >> 33);
        }
    }
    bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80);
    wchar_t *text = xalloc(33 * sizeof(wchar_t));
    for (size_t index = 0; index < 16; index++)
        swprintf(text + index * 2, 3, L"%02x", bytes[index]);
    text[32] = 0;
    return text;
}

// ───────────── 后台保存 ─────────────

static void *save_queue_thread(void *parameter)
{
    SaveQueue *queue = parameter;
    pthread_mutex_lock(&queue->lock);
    for (;;)
    {
        while (!queue->data) pthread_cond_wait(&queue->changed, &queue->lock);
        wchar_t *path = queue->path;
        char *data = queue->data;
        size_t length = queue->length;
        queue->path = NULL;
        queue->data = NULL;
        queue->busy = true;
        pthread_mutex_unlock(&queue->lock);

        if (!file_write_atomic(path, data, length)) log_win32_error(queue->failure_message, GetLastError());
        free(path);
        free(data);

        pthread_mutex_lock(&queue->lock);
        queue->busy = false;
        pthread_cond_broadcast(&queue->changed);
    }
    return NULL;
}

void save_queue_post(SaveQueue *queue, const wchar_t *path, char *data, size_t length)
{
    pthread_mutex_lock(&queue->lock);
    free(queue->path);
    free(queue->data);
    queue->path = wcs_dup(path);
    queue->data = data;
    queue->length = length;
    if (!queue->thread_started)
    {
        queue->thread_started = pthread_create(&queue->thread, NULL, save_queue_thread, queue) == 0;
        if (queue->thread_started) pthread_detach(queue->thread);
    }
    if (!queue->thread_started)
    {
        // 开不了线程时就在这里写，不能丢掉这次保存。
        if (!file_write_atomic(queue->path, queue->data, queue->length)) log_win32_error(queue->failure_message, GetLastError());
        free(queue->path);
        free(queue->data);
        queue->path = NULL;
        queue->data = NULL;
    }
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
}

void save_queue_flush(SaveQueue *queue)
{
    pthread_mutex_lock(&queue->lock);
    while (queue->data || queue->busy) pthread_cond_wait(&queue->changed, &queue->lock);
    pthread_mutex_unlock(&queue->lock);
}
LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    if (!target) return 0;
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

BOOL InitOnceExecuteOnce(INIT_ONCE *once, BOOL (*function)(PINIT_ONCE once, void *parameter, void **context), void *parameter,
    void **context)
{
    if (!once || !function) return FALSE;
    pthread_mutex_lock(&once->mutex);
    bool call = !once->done;
    if (call) once->done = true;
    pthread_mutex_unlock(&once->mutex);
    if (call) function(once, parameter, context);
    return TRUE;
}

DWORD GetTempPathW(DWORD length, LPWSTR buffer)
{
    const char *directory = g_get_tmp_dir();
    if (!directory) directory = "/tmp";
    size_t utf8_length = strlen(directory);
    wchar_t *wide = wide_from_utf8(directory, utf8_length);
    if (!wide) return 0;
    // Windows 的临时目录总是以分隔符结尾。
    size_t needed = wcslen(wide) + 2;
    DWORD result = (DWORD)needed;
    if (buffer && length >= needed)
    {
        wcscpy(buffer, wide);
        size_t written = wcslen(buffer);
        if (written == 0 || buffer[written - 1] != L'/') wcscat(buffer, L"/");
        result = (DWORD)wcslen(buffer) + 1;
    }
    free(wide);
    return result;
}

static char *normalize_posix_path(const char *path)
{
    char *joined = NULL;
    if (path[0] == '/')
    {
        joined = strdup(path);
    }
    else
    {
        char *current = getcwd(NULL, 0);
        if (!current) return NULL;
        size_t need = strlen(current) + 1 + strlen(path) + 1;
        joined = xalloc(need);
        snprintf(joined, need, "%s/%s", current, path);
        free(current);
    }
    if (!joined) return NULL;

    // 词法规范化：折叠多余斜杠、. 和 ..，但不跟随符号链接，和 Windows 的 GetFullPathNameW 语义一致。
    GPtrArray *parts = g_ptr_array_new();
    char *cursor = joined;
    while (*cursor)
    {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        char *start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (*cursor) *cursor++ = 0;
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0)
        {
            if (parts->len > 0) g_ptr_array_remove_index(parts, parts->len - 1);
            continue;
        }
        g_ptr_array_add(parts, start);
    }

    GString *out = g_string_new("/");
    for (guint index = 0; index < parts->len; index++)
    {
        if (index > 0) g_string_append_c(out, '/');
        g_string_append(out, parts->pdata[index]);
    }
    g_ptr_array_free(parts, TRUE);
    free(joined);
    return g_string_free(out, FALSE);
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD length, LPWSTR buffer, LPWSTR *file_part)
{
    if (!path || !*path) return 0;
    size_t utf8_length = 0;
    char *utf8 = utf8_from_wide(path, &utf8_length);
    if (!utf8) return 0;
    char *full = normalize_posix_path(utf8);
    free(utf8);
    if (!full) return 0;
    wchar_t *wide = wide_from_utf8(full, strlen(full));
    g_free(full);
    if (!wide) return 0;
    DWORD needed = (DWORD)wcslen(wide) + 1;
    if (buffer && length >= needed)
    {
        wcscpy(buffer, wide);
        if (file_part)
        {
            wchar_t *slash = wcsrchr(buffer, L'/');
            *file_part = slash ? slash + 1 : buffer;
        }
    }
    free(wide);
    return needed;
}

// ───────────── 路径（文件操作还在 M4） ─────────────

/// 绝对路径；纯逻辑，和 Windows 版的写法一样。返回值由调用方 free。
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
    // 根目录（单独一个 / 或 C:\ 之外）不去掉。
    while (length > 1 && (trimmed[length - 1] == L'/' || trimmed[length - 1] == L'\\')
        && !(length == 3 && trimmed[1] == L':'))
        trimmed[--length] = 0;
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

bool path_is_available(const wchar_t *path)
{
    return !wcs_is_blank(path) && path_exists(path);
}

/// 在 directory 里找一个还没被占用的名字：基名、基名 (2)、…。返回值由调用方 free。
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

/// 临时附件处理后的保存位置：xdg-user-dirs 的下载文件夹，没有配置时用 ~/Downloads。
wchar_t *persistent_output_directory(void)
{
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    wchar_t *directory = downloads ? wide_from_utf8(downloads, strlen(downloads)) : NULL;
    if (!directory)
    {
        const char *home = g_get_home_dir();
        if (home)
        {
            wchar_t *wide_home = wide_from_utf8(home, strlen(home));
            if (wide_home)
            {
                directory = path_join(wide_home, L"Downloads");
                free(wide_home);
            }
        }
    }
    if (directory && !directory_create(directory))
    {
        free(directory);
        directory = NULL;
    }
    return directory;
}
