// 共享代码用到的一点点 Windows 外壳接口在 Linux 上的替代。
// 文件类型名没有系统查询：返回失败，shelf_item 会按扩展名用内置的翻译名称。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "base.h"

#include <gio/gio.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

DWORD_PTR SHGetFileInfoW(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags)
{
    (void)path;
    (void)attributes;
    (void)info;
    (void)size;
    (void)flags;
    return 0;
}

LPWSTR CharUpperW(LPWSTR text)
{
    if (!text) return text;
    for (wchar_t *cursor = text; *cursor; cursor++) *cursor = (wchar_t)towupper((wint_t)*cursor);
    return text;
}

int GetSystemMetrics(int index)
{
    // 拖动的起始阈值：和 Windows 的默认值一样是 4 像素。
    if (index == SM_CXDRAG || index == SM_CYDRAG) return 4;
    return 0;
}

BOOL SystemParametersInfoW(UINT action, UINT param, void *value, UINT flags)
{
    (void)param;
    (void)flags;
    if (action == SPI_GETWHEELSCROLLLINES && value)
    {
        *(UINT *)value = 3;
        return TRUE;
    }
    return FALSE;
}

void QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
    if (frequency) frequency->QuadPart = 1000000000LL;
}

void QueryPerformanceCounter(LARGE_INTEGER *counter)
{
    if (!counter) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    counter->QuadPart = (LONGLONG)now.tv_sec * 1000000000LL + now.tv_nsec;
}

bool platform_open_external(const wchar_t *target)
{
    if (!target || !*target) return false;
    size_t length = 0;
    char *utf8 = utf8_from_wide(target, &length);
    if (!utf8 || length == 0)
    {
        free(utf8);
        return false;
    }
    bool is_uri = g_uri_parse_scheme(utf8) != NULL;
    char *uri = is_uri ? g_strdup(utf8) : g_filename_to_uri(utf8, NULL, NULL);
    free(utf8);
    if (!uri) return false;
    GError *error = NULL;
    gboolean opened = g_app_info_launch_default_for_uri(uri, NULL, &error);
    if (error) g_error_free(error);
    g_free(uri);
    return opened != FALSE;
}
