// 运行日志：$XDG_STATE_HOME/dropshelf/logs/dropshelf-yyyy-MM-dd.log，格式与 Windows 版相同。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

wchar_t *log_directory(void)
{
    wchar_t *custom = env_get(L"DROPSHELF_LOG_DIRECTORY");
    if (custom) return custom;
    wchar_t *base = state_directory();
    wchar_t *directory = path_join(base, L"logs");
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

static void write_all(int fd, const char *bytes, size_t length)
{
    size_t done = 0;
    while (done < length)
    {
        ssize_t put = write(fd, bytes + done, length - done);
        if (put < 0)
        {
            if (errno == EINTR) continue;
            return;
        }
        done += (size_t)put;
    }
}

/// 日志写入失败不能影响主程序运行，所以这里出什么错都只是放弃这一条。
static void log_write(const wchar_t *level, const wchar_t *message)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm broken;
    localtime_r(&now.tv_sec, &broken);
    int offset_minutes = (int)(broken.tm_gmtoff / 60);
    int fraction = (int)(now.tv_nsec / 100);

    WStr line = {0};
    wstr_appendf(&line, L"%04d-%02d-%02dT%02d:%02d:%02d.%07d%lc%02d:%02d [%ls] %ls\n",
        broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday, broken.tm_hour, broken.tm_min, broken.tm_sec, fraction,
        offset_minutes < 0 ? L'-' : L'+', abs(offset_minutes) / 60, abs(offset_minutes) % 60, level, message);
    size_t length = 0;
    char *bytes = utf8_from_wide(line.data, &length);
    wstr_free(&line);

    wchar_t *directory = log_directory();
    directory_create(directory);
    wchar_t name[64];
    swprintf(name, 64, L"dropshelf-%04d-%02d-%02d.log", broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday);
    wchar_t *path = path_join(directory, name);
    char *path_bytes = utf8_from_wide(path, NULL);

    pthread_mutex_lock(&log_lock);
    int file = open(path_bytes, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (file >= 0)
    {
        write_all(file, bytes, length);
        close(file);
    }
    pthread_mutex_unlock(&log_lock);

    free(path_bytes);
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
    // code 是 base_linux.c 按 errno 换成的 Windows 错误码；能找回系统说明的就写上。
    const char *description = code < 4096 ? strerror((int)code) : "";
    log_error(L"%ls\n错误 %lu (0x%08lX): %hs", message ? message : L"", (unsigned long)code, (unsigned long)code, description);
}

void log_cleanup_old(void)
{
    wchar_t *directory = log_directory();
    char *directory_bytes = utf8_from_wide(directory, NULL);
    DIR *opened = opendir(directory_bytes);
    if (opened)
    {
        time_t now = time(NULL);
        struct dirent *entry;
        while ((entry = readdir(opened)) != NULL)
        {
            if (fnmatch("dropshelf-*.log", entry->d_name, 0) != 0) continue;
            size_t path_size = strlen(directory_bytes) + strlen(entry->d_name) + 2;
            char *path = xalloc(path_size);
            snprintf(path, path_size, "%s/%s", directory_bytes, entry->d_name);
            struct stat information;
            if (stat(path, &information) == 0 && S_ISREG(information.st_mode) && information.st_mtime < now - 14 * 24 * 60 * 60)
                unlink(path);
            free(path);
        }
        closedir(opened);
    }
    free(directory_bytes);
    free(directory);
}