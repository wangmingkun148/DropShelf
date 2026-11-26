#include "update_check.h"
#include "json.h"
#include "log.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

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

#ifdef _WIN32

static char *download(long *status)
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
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        *status = (long)code;
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

#else

typedef struct Downloaded
{
    char *body;
    size_t length;
} Downloaded;

static size_t curl_received(char *part, size_t size, size_t count, void *data)
{
    Downloaded *downloaded = data;
    size_t bytes = size * count;
    // 发布列表不会很大；太大说明不对，不再读。
    if (downloaded->length + bytes > 8 * 1024 * 1024) return 0;
    downloaded->body = xrealloc(downloaded->body, downloaded->length + bytes + 1);
    memcpy(downloaded->body + downloaded->length, part, bytes);
    downloaded->length += bytes;
    downloaded->body[downloaded->length] = 0;
    return bytes;
}

static char *download(long *status)
{
    *status = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    Downloaded downloaded = {0};
    WStr agent = {0};
    wstr_appendf(&agent, L"DropShelf/%ls", DROPSHELF_VERSION);
    char *narrow_agent = utf8_from_wide(agent.data, NULL);
    wstr_free(&agent);
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/repos/wangmingkun148/dropshelf/releases?per_page=30");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, narrow_agent ? narrow_agent : "DropShelf");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // 只连本项目用的地址，不接受跳转，且仅允许 HTTPS。
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT_MILLISECONDS / 1000);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_MILLISECONDS / 1000);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_received);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &downloaded);
    CURLcode result = curl_easy_perform(curl);
    long code = 0;
    if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    *status = code;
    curl_slist_free_all(headers);
    if (narrow_agent) free(narrow_agent);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK)
    {
        free(downloaded.body);
        return NULL;
    }
    return downloaded.body ? downloaded.body : xcalloc(1, 1);
}

#endif

static void check_work(void *data)
{
    Check *check = data;
    long status = 0;
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
