#include "log.h"

#include <stdlib.h>

static SRWLOCK log_lock = SRWLOCK_INIT;

wchar_t *log_directory(void)
{
    wchar_t *custom = env_get(L"DROPSHELF_LOG_DIRECTORY");
    if (custom) return custom;
    wchar_t *base = local_app_data_directory();
    wchar_t *directory = path_join(base, L"DropShelf\\Logs");
    free(base);
    return directory;
}

bool log_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        wchar_t *value = env_get(L"DROPSHELF_TRACE");
        enabled = value && wcscmp(value, L"1") == 0;
        free(value);
    }
    return enabled == 1;
}

/// 日志写入失败不能影响主程序运行，所以这里出什么错都只是放弃这一条。
static void log_write(const wchar_t *level, const wchar_t *message)
{
    // 时间写成 2026-11-14T10:23:45.1234567+08:00，和 C# 的 DateTimeOffset.Now:O 一样。
    FILETIME utc, local;
    GetSystemTimePreciseAsFileTime(&utc);
    FileTimeToLocalFileTime(&utc, &local);
    SYSTEMTIME time;
    FileTimeToSystemTime(&local, &time);
    ULARGE_INTEGER utc_ticks = { .LowPart = utc.dwLowDateTime, .HighPart = utc.dwHighDateTime };
    ULARGE_INTEGER local_ticks = { .LowPart = local.dwLowDateTime, .HighPart = local.dwHighDateTime };
    int offset_minutes = (int)(((LONGLONG)local_ticks.QuadPart - (LONGLONG)utc_ticks.QuadPart) / 600000000LL);
    int fraction = (int)(local_ticks.QuadPart % 10000000ULL);

    WStr line = {0};
    wstr_appendf(&line, L"%04d-%02d-%02dT%02d:%02d:%02d.%07d%lc%02d:%02d [%ls] %ls\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, fraction,
        offset_minutes < 0 ? L'-' : L'+', abs(offset_minutes) / 60, abs(offset_minutes) % 60, level, message);
    size_t length = 0;
    char *bytes = utf8_from_wide(line.data, &length);
    wstr_free(&line);

    wchar_t *directory = log_directory();
    directory_create(directory);
    wchar_t name[40];
    swprintf(name, 40, L"dropshelf-%04d-%02d-%02d.log", time.wYear, time.wMonth, time.wDay);
    wchar_t *path = path_join(directory, name);

    AcquireSRWLockExclusive(&log_lock);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, bytes, (DWORD)length, &written, NULL);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&log_lock);

    free(path);
    free(directory);
    free(bytes);
}

static void log_writev(const wchar_t *level, const wchar_t *format, va_list arguments)
{
    WStr message = {0};
    wstr_appendv(&message, format, arguments);
    log_write(level, message.data ? message.data : L"");
    wstr_free(&message);
}

void log_info(const wchar_t *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    log_writev(L"INFO", format, arguments);
    va_end(arguments);
}

void log_trace(const wchar_t *format, ...)
{
    if (!log_trace_enabled()) return;
    va_list arguments;
    va_start(arguments, format);
    log_writev(L"TRACE", format, arguments);
    va_end(arguments);
}

void log_error(const wchar_t *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    log_writev(L"ERROR", format, arguments);
    va_end(arguments);
}

void log_win32_error(const wchar_t *message, DWORD code)
{
    wchar_t description[512] = L"";
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, description, 512, NULL);
    while (length > 0 && (description[length - 1] == L'\r' || description[length - 1] == L'\n' || description[length - 1] == L' '))
        description[--length] = 0;
    log_error(L"%ls\r\nWin32 错误 %lu (0x%08lX): %ls", message ? message : L"", code, code, description);
}

void log_cleanup_old(void)
{
    wchar_t *directory = log_directory();
    wchar_t *pattern = path_join(directory, L"dropshelf-*.log");
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cutoff = { .LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime };
    cutoff.QuadPart -= 14ULL * 24 * 60 * 60 * 10000000ULL;

    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            ULARGE_INTEGER written = { .LowPart = found.ftLastWriteTime.dwLowDateTime, .HighPart = found.ftLastWriteTime.dwHighDateTime };
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || written.QuadPart >= cutoff.QuadPart) continue;
            wchar_t *path = path_join(directory, found.cFileName);
            DeleteFileW(path);
            free(path);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    free(pattern);
    free(directory);
}
