#include "update_check.h"
#include "json.h"
#include "log.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

#ifndef DROPSHELF_VERSION
#define DROPSHELF_VERSION L"0.0.0"
#endif

#define RELEASES_HOST L"api.github.com"
#define RELEASES_PATH L"/repos/wangmingkun148/dropshelf/releases?per_page=30"
#define TIMEOUT_MILLISECONDS 10000

bool update_parse_tag(const wchar_t *tag, int *major, int *minor, int *patch)
{
    if (!tag || (tag[0] != L'v' && tag[0] != L'V')) return false;
    const wchar_t *text = tag + 1;
    // 前缀后面必须直接是数字：别的前缀（比如同样以 v 开头的单词）不算。
    if (*text < L'0' || *text > L'9') return false;
    int parts[4] = { 0, 0, 0, 0 }, count = 0;
    const wchar_t *cursor = text;
    while (count < 4)
    {
        if (*cursor < L'0' || *cursor > L'9') return false;
        long value = 0;
        while (*cursor >= L'0' && *cursor <= L'9')
        {
            value = value * 10 + (*cursor - L'0');
            if (value > 1000000) return false;
            cursor++;
        }
        parts[count++] = (int)value;
        if (*cursor != L'.') break;
        cursor++;
    }
    // 和 C# 的 Version.TryParse 一样：两到四段，后面不能再有别的。
    if (*cursor || count < 2) return false;
    *major = parts[0];
    *minor = parts[1];
    *patch = parts[2];
    return true;
}

static int compare_versions(int a1, int a2, int a3, int b1, int b2, int b3)
{
    if (a1 != b1) return a1 < b1 ? -1 : 1;
    if (a2 != b2) return a2 < b2 ? -1 : 1;
    if (a3 != b3) return a3 < b3 ? -1 : 1;
    return 0;
}

static bool is_true(const JsonValue *release, const wchar_t *name)
{
    const JsonValue *value = json_get(release, name);
    return value && value->type == JSON_BOOL && value->boolean;
}

UpdateOutcome update_find_newer(const wchar_t *json, int major, int minor, int patch, UpdateInfo *info)
{
    memset(info, 0, sizeof(*info));
    JsonValue *root = json_parse(json, 0);
    if (!root || root->type != JSON_ARRAY)
    {
        json_free(root);
        info->outcome = UPDATE_ERROR;
        return UPDATE_ERROR;
    }
    bool found = false;
    int best[3] = { 0, 0, 0 };
    wchar_t *url = NULL;
    for (int index = 0; index < root->count; index++)
    {
        const JsonValue *release = root->items[index];
        if (release->type != JSON_OBJECT || is_true(release, L"draft") || is_true(release, L"prerelease")) continue;
        const JsonValue *tag = json_get(release, L"tag_name");
        int v[3];
        if (!tag || tag->type != JSON_STRING || !update_parse_tag(tag->string, &v[0], &v[1], &v[2])) continue;
        if (compare_versions(v[0], v[1], v[2], major, minor, patch) <= 0) continue;
        if (found && compare_versions(v[0], v[1], v[2], best[0], best[1], best[2]) <= 0) continue;
        found = true;
        memcpy(best, v, sizeof(best));
        // 只打开本项目在 GitHub 上的页面：返回的内容里即使混进了别的地址，也不会被交给浏览器。
        const JsonValue *page = json_get(release, L"html_url");
        free(url);
        size_t prefix = wcslen(UPDATE_RELEASES_PAGE L"/");
        url = page && page->type == JSON_STRING && _wcsnicmp(page->string, UPDATE_RELEASES_PAGE L"/", prefix) == 0 ? wcs_dup(page->string)
                                                                                                             : wcs_dup(UPDATE_RELEASES_PAGE);
    }
    json_free(root);
    if (!found)
    {
        info->outcome = UPDATE_LATEST;
        return UPDATE_LATEST;
    }
    info->outcome = UPDATE_NEWER;
    info->major = best[0];
    info->minor = best[1];
    info->patch = best[2];
    info->url = url;
    return UPDATE_NEWER;
}

typedef struct Check
{
    UpdateInfo info;
    void (*done)(const UpdateInfo *info, void *context);
    void *context;
} Check;

static char *download(DWORD *status)
{
    *status = 0;
    WStr agent = {0};
    wstr_appendf(&agent, L"DropShelf/%ls", DROPSHELF_VERSION);
    HINTERNET session = WinHttpOpen(agent.data, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    wstr_free(&agent);
    if (!session) return NULL;
    WinHttpSetTimeouts(session, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS);
    HINTERNET connection = WinHttpConnect(session, RELEASES_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", RELEASES_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE) : NULL;
    char *body = NULL;
    size_t length = 0;
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL))
    {
        DWORD size = sizeof(*status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX);
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            // 发布列表不会很大；太大说明不对，不再读。
            if (length + available > 8 * 1024 * 1024) break;
            body = xrealloc(body, length + available + 1);
            DWORD read = 0;
            if (!WinHttpReadData(request, body + length, available, &read) || read == 0) break;
            length += read;
        }
        if (body) body[length] = 0;
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return body;
}

static void check_work(void *data)
{
    Check *check = data;
    DWORD status = 0;
    char *body = download(&status);
    if (!body || status != 200)
    {
        log_error(L"检查更新失败\r\nHTTP %lu", (unsigned long)status);
        check->info.outcome = UPDATE_ERROR;
        free(body);
        return;
    }
    wchar_t *json = wide_from_utf8(body, strlen(body));
    free(body);
    int major = 0, minor = 0, patch = 0;
    swscanf(DROPSHELF_VERSION, L"%d.%d.%d", &major, &minor, &patch);
    if (update_find_newer(json ? json : L"", major, minor, patch, &check->info) == UPDATE_ERROR) log_error(L"检查更新失败：发布列表的格式不对");
    free(json);
}

static void check_done(void *data)
{
    Check *check = data;
    check->done(&check->info, check->context);
    free(check->info.url);
    free(check);
}

void update_check_start(void (*done)(const UpdateInfo *info, void *context), void *context)
{
    Check *check = xcalloc(1, sizeof(Check));
    check->done = done;
    check->context = context;
    task_run(check_work, check_done, check);
}
