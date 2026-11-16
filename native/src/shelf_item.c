#include "shelf_item.h"
#include "strings.h"

#include <math.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

static unsigned text_generation = 1;

ShelfItem *shelf_item_new(ShelfItemKind kind, const wchar_t *value, const wchar_t *title, bool temporary)
{
    ShelfItem *item = xcalloc(1, sizeof(ShelfItem));
    item->references = 1;
    item->id = new_guid_text();
    item->kind = kind;
    item->value = wcs_dup(value ? value : L"");
    item->title = wcs_is_blank(title) ? NULL : wcs_dup(title);
    item->temporary = temporary;
    item->available = true;
    item->child_count = -1;
    item->type_name = wcs_dup(L"");
    shelf_item_motion_reset(item);
    anim_set(&item->dim, 1);
    return item;
}

void shelf_item_motion_reset(ShelfItem *item)
{
    item->motion = (ShelfItemMotion){ 1, 0, 0, 0, 0, 0, 1, 1, 1, 0 };
}

ShelfItem *shelf_item_retain(ShelfItem *item)
{
    if (item) item->references++;
    return item;
}

void shelf_item_release(ShelfItem *item)
{
    if (!item || --item->references > 0) return;
    free(item->id);
    free(item->value);
    free(item->title);
    free(item->type_name);
    free(item->display_name);
    free(item->detail);
    gfx_image_release(item->thumbnail);
    free(item);
}

void shelf_item_relocate(ShelfItem *item, const wchar_t *path)
{
    if (!shelf_item_is_file_system(item) || wcs_equal(item->value, path)) return;
    bool same_extension = _wcsicmp(path_extension(item->value), path_extension(path)) == 0;
    free(item->value);
    item->value = wcs_dup(path);
    item->available = true;
    if (item->kind == ITEM_FILE && !same_extension)
    {
        free(item->type_name);
        item->type_name = file_type_name(path);
        if (!item->type_name) item->type_name = wcs_dup(L"");
    }
    shelf_item_text_changed(item);
}

ShelfItem *shelf_item_copy(const ShelfItem *item)
{
    ShelfItem *copy = shelf_item_new(item->kind, item->value, item->title, item->temporary);
    free(copy->id);
    copy->id = wcs_dup(item->id);
    copy->available = item->available;
    copy->has_size = item->has_size;
    copy->size_bytes = item->size_bytes;
    copy->child_count = item->child_count;
    free(copy->type_name);
    copy->type_name = wcs_dup(item->type_name ? item->type_name : L"");
    copy->identity = item->identity;
    // 已有的缩略图跟着过去，算作已经要过：之后不在顶部三张或显示的行里时照常释放。
    if (item->thumbnail)
    {
        copy->thumbnail = gfx_image_retain(item->thumbnail);
        copy->thumbnail_wanted = true;
    }
    return copy;
}

bool shelf_item_is_file_system(const ShelfItem *item)
{
    return item->kind == ITEM_FILE || item->kind == ITEM_FOLDER;
}

void shelf_item_text_changed(ShelfItem *item)
{
    free(item->display_name);
    free(item->detail);
    item->display_name = item->detail = NULL;
}

void shelf_items_language_changed(void)
{
    text_generation++;
}

void shelf_item_set_thumbnail(ShelfItem *item, GfxImage *image)
{
    gfx_image_release(item->thumbnail);
    item->thumbnail = image;
}

// ───────────── 文字 ─────────────

const wchar_t *path_extension(const wchar_t *path)
{
    if (!path) return L"";
    size_t length = wcslen(path);
    for (size_t index = length; index > 0; index--)
    {
        wchar_t c = path[index - 1];
        if (c == L'\\' || c == L'/' || c == L':') break;
        // 末尾就是点时没有扩展名。
        if (c == L'.') return index == length ? path + length : path + index - 1;
    }
    return path + length;
}

static wchar_t *folder_name(const wchar_t *path)
{
    wchar_t *trimmed = wcs_dup(path);
    size_t length = wcslen(trimmed);
    while (length > 3 && (trimmed[length - 1] == L'\\' || trimmed[length - 1] == L'/')) trimmed[--length] = 0;
    const wchar_t *name = path_file_name(trimmed);
    wchar_t *result = wcs_dup(name && *name && !(length <= 3 && wcschr(trimmed, L':')) ? name : path);
    free(trimmed);
    return result;
}

wchar_t *text_summarize(const wchar_t *text, int maximum)
{
    WStr builder = {0};
    wstr_append(&builder, L"");
    bool space = false;
    for (const wchar_t *c = text ? text : L""; *c; c++)
    {
        if (*c == L'\n' && builder.length > 0) break;
        if (wchar_is_white_space(*c))
        {
            space = builder.length > 0;
            continue;
        }
        if (space) wstr_append_char(&builder, L' ');
        space = false;
        wstr_append_char(&builder, *c);
        if ((int)builder.length > maximum)
        {
            builder.length = (size_t)maximum;
            // 表情等字符占两个 UTF-16 单元，别把它们截成半个（留下的半个会显示成方框，写进文件名也不合法）。
            if (builder.length > 0 && IS_HIGH_SURROGATE(builder.data[builder.length - 1])) builder.length--;
            builder.data[builder.length] = 0;
            wstr_append(&builder, L"…");
            break;
        }
    }
    return wstr_take(&builder);
}

/// 网址的各部分：协议后面的主机和路径。只认 http 和 https。
static bool url_parts(const wchar_t *url, const wchar_t **host, size_t *host_length, const wchar_t **path, size_t *path_length)
{
    if (!url) return false;
    size_t skip;
    if (_wcsnicmp(url, L"http://", 7) == 0) skip = 7;
    else if (_wcsnicmp(url, L"https://", 8) == 0) skip = 8;
    else return false;
    const wchar_t *start = url + skip;
    const wchar_t *end = start;
    while (*end && *end != L'/' && *end != L'?' && *end != L'#' && *end != L'\\') end++;
    // 去掉用户信息和端口。
    const wchar_t *at = NULL;
    for (const wchar_t *c = start; c < end; c++)
        if (*c == L'@') at = c;
    if (at) start = at + 1;
    const wchar_t *host_end = end;
    if (*start == L'[')
    {
        const wchar_t *close = start;
        while (close < end && *close != L']') close++;
        host_end = close < end ? close + 1 : end;
    }
    else
    {
        for (const wchar_t *c = start; c < end; c++)
            if (*c == L':')
            {
                host_end = c;
                break;
            }
    }
    if (host_end <= start) return false;
    *host = start;
    *host_length = (size_t)(host_end - start);
    const wchar_t *path_start = end;
    const wchar_t *path_end = path_start;
    while (*path_end && *path_end != L'?' && *path_end != L'#') path_end++;
    *path = path_start;
    *path_length = (size_t)(path_end - path_start);
    return true;
}

bool text_is_single_url(const wchar_t *text)
{
    if (!text || wcschr(text, L'\n') || wcschr(text, L' ')) return false;
    for (const wchar_t *c = text; *c; c++)
        if (*c == L'\t' || *c == L'\r') return false;
    const wchar_t *host, *path;
    size_t host_length, path_length;
    return url_parts(text, &host, &host_length, &path, &path_length);
}

wchar_t *url_host(const wchar_t *url)
{
    const wchar_t *host, *path;
    size_t host_length, path_length;
    if (!url_parts(url, &host, &host_length, &path, &path_length)) return NULL;
    wchar_t *result = wcs_dup_n(host, host_length);
    CharLowerW(result);
    return result;
}

/// 和 Uri.AbsolutePath 一样：反斜杠换成斜杠，非 ASCII 字符和空格按 UTF-8 转义。
static void append_escaped_path(WStr *out, const wchar_t *path, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        wchar_t c = path[index];
        if (c == L'\\') c = L'/';
        if (c > 0x20 && c < 0x7F && c != L'"' && c != L'<' && c != L'>' && c != L'`' && c != L'{' && c != L'}' && c != L'|' && c != L'^')
        {
            wstr_append_char(out, c);
            continue;
        }
        wchar_t unit[3] = { c, 0, 0 };
        int units = 1;
        if (IS_HIGH_SURROGATE(c) && index + 1 < length && IS_LOW_SURROGATE(path[index + 1]))
        {
            unit[1] = path[++index];
            units = 2;
        }
        char bytes[8];
        int count = WideCharToMultiByte(CP_UTF8, 0, unit, units, bytes, sizeof(bytes), NULL, NULL);
        for (int byte = 0; byte < count; byte++) wstr_appendf(out, L"%%%02X", (unsigned char)bytes[byte]);
    }
}

static wchar_t *shorten_url(const wchar_t *url)
{
    const wchar_t *host, *path;
    size_t host_length, path_length;
    if (!url_parts(url, &host, &host_length, &path, &path_length)) return text_summarize(url, 60);
    WStr text = {0};
    wstr_append_n(&text, host, host_length);
    CharLowerW(text.data);
    append_escaped_path(&text, path, path_length);
    while (text.length > host_length && text.data[text.length - 1] == L'/') text.data[--text.length] = 0;
    if (text.length > 60)
    {
        text.length = 60;
        text.data[60] = 0;
        wstr_append(&text, L"…");
    }
    return wstr_take(&text);
}

wchar_t *format_size(long long bytes)
{
    static const wchar_t *const units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024 && unit < 4)
    {
        value /= 1024;
        unit++;
    }
    WStr text = {0};
    if (unit == 0)
    {
        wstr_appendf(&text, L"%lld B", bytes);
        return wstr_take(&text);
    }
    // 自定义格式 "0.#" 保留一位小数，恰好一半时远离零取整。
    double rounded = floor(value * 10 + 0.5) / 10;
    if (fabs(rounded - floor(rounded)) < 1e-9) wstr_appendf(&text, L"%.0f %ls", rounded, units[unit]);
    else wstr_appendf(&text, L"%.1f %ls", rounded, units[unit]);
    return wstr_take(&text);
}

const wchar_t *shelf_item_display_name(ShelfItem *item)
{
    if (item->display_name && item->text_generation == text_generation) return item->display_name;
    if (item->text_generation != text_generation)
    {
        shelf_item_text_changed(item);
        item->text_generation = text_generation;
    }
    switch (item->kind)
    {
        case ITEM_FILE: item->display_name = wcs_dup(path_file_name(item->value)); break;
        case ITEM_FOLDER: item->display_name = folder_name(item->value); break;
        case ITEM_LINK: item->display_name = item->title ? wcs_dup(item->title) : shorten_url(item->value); break;
        default: item->display_name = text_summarize(item->value, 60); break;
    }
    return item->display_name;
}

static wchar_t *build_detail(const ShelfItem *item)
{
    if (!item->available) return wcs_dup(S(STR_MovedOrDeleted));
    WStr text = {0};
    switch (item->kind)
    {
        case ITEM_FILE:
            if (item->has_size)
            {
                wchar_t *size = format_size(item->size_bytes);
                if (wcs_is_blank(item->type_name) || !*item->type_name) wstr_append(&text, size);
                else wstr_appendf(&text, L"%ls · %ls", size, item->type_name);
                free(size);
            }
            else wstr_append(&text, item->type_name ? item->type_name : L"");
            break;
        case ITEM_FOLDER:
            if (item->child_count < 0) wstr_append(&text, S(STR_Folder));
            else if (item->child_count == 0) wstr_append(&text, S(STR_EmptyFolder));
            else
            {
                wchar_t *count = str_folder_items(item->child_count);
                if (item->has_size)
                {
                    wchar_t *size = format_size(item->size_bytes);
                    wstr_appendf(&text, L"%ls · %ls", count, size);
                    free(size);
                }
                else wstr_append(&text, count);
                free(count);
            }
            break;
        case ITEM_LINK:
        {
            wchar_t *host = url_host(item->value);
            wstr_append(&text, host ? host : S(STR_Link));
            free(host);
            break;
        }
        default:
        {
            wchar_t *detail = str_text_detail((int)wcslen(item->value));
            wstr_append(&text, detail);
            free(detail);
            break;
        }
    }
    return wstr_take(&text);
}

const wchar_t *shelf_item_detail(ShelfItem *item)
{
    if (item->text_generation != text_generation)
    {
        shelf_item_text_changed(item);
        item->text_generation = text_generation;
    }
    if (!item->detail) item->detail = build_detail(item);
    return item->detail;
}

const wchar_t *shelf_item_glyph(const ShelfItem *item)
{
    switch (item->kind)
    {
        case ITEM_FOLDER: return L"\xE8B7";
        case ITEM_TEXT: return L"\xE8D2";
        case ITEM_LINK: return L"\xE71B";
        default: return L"\xE8A5";
    }
}

bool shelf_item_exists_on_disk(ShelfItemKind kind, const wchar_t *value)
{
    if (kind != ITEM_FILE && kind != ITEM_FOLDER) return true;
    DWORD attributes = GetFileAttributesW(value);
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return kind == ITEM_FOLDER ? (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 : (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool shelf_item_refresh_availability(ShelfItem *item)
{
    if (!shelf_item_is_file_system(item)) return true;
    bool available = shelf_item_exists_on_disk(item->kind, item->value);
    if (available != item->available)
    {
        item->available = available;
        shelf_item_text_changed(item);
    }
    return available;
}

// ───────────── 文件信息 ─────────────

bool file_size_of(const wchar_t *path, long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return true;
}

bool file_facts_read(const wchar_t *path, FileFacts *facts)
{
    *facts = (FileFacts){0};
    if (wcs_is_blank(path)) return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
    facts->is_folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!facts->is_folder)
    {
        facts->has_size = true;
        facts->size_bytes = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    }
    file_identity_get(path, &facts->identity);
    return true;
}

typedef struct TypeNameEntry
{
    wchar_t *extension;
    wchar_t *name;              // NULL 表示 Windows 没有提供
} TypeNameEntry;

static SRWLOCK type_names_lock = SRWLOCK_INIT;
static TypeNameEntry *type_names;
static int type_name_count;

wchar_t *file_type_name(const wchar_t *path)
{
    const wchar_t *extension = path_extension(path);
    if (!*extension) return wcs_dup(S(STR_GenericFileType));
    wchar_t *found = NULL;
    bool known = false;
    AcquireSRWLockShared(&type_names_lock);
    for (int index = 0; index < type_name_count; index++)
    {
        if (!wcs_equal_ignore_case(type_names[index].extension, extension)) continue;
        known = true;
        found = wcs_dup(type_names[index].name);
        break;
    }
    ReleaseSRWLockShared(&type_names_lock);
    if (!known)
    {
        // 只缓存 Windows 提供的名称；备用名称跟随界面语言，每次重新生成。
        WStr fake = {0};
        wstr_appendf(&fake, L"file%ls", extension);
        SHFILEINFOW info = {0};
        if (SHGetFileInfoW(fake.data, FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) &&
            !wcs_is_blank(info.szTypeName))
            found = wcs_dup(info.szTypeName);
        wstr_free(&fake);
        AcquireSRWLockExclusive(&type_names_lock);
        type_names = xrealloc(type_names, (size_t)(type_name_count + 1) * sizeof(TypeNameEntry));
        type_names[type_name_count++] = (TypeNameEntry){ wcs_dup(extension), wcs_dup(found) };
        ReleaseSRWLockExclusive(&type_names_lock);
    }
    if (found) return found;
    wchar_t *upper = wcs_dup(extension + 1);
    CharUpperW(upper);
    wchar_t *name = str_file_type(upper);
    free(upper);
    return name;
}

#define FOLDER_SIZE_BUDGET_MS 3000
#define FOLDER_MAXIMUM_FILES 200000

typedef struct FolderWalk
{
    ULONGLONG started;
    int files;
    long long size;
    bool exceeded;
    volatile LONG *cancelled;
} FolderWalk;

/// 递归累加文件大小。联接点和符号链接不进去也不计入（OneDrive 等按需下载的文件和文件夹照常计入），没有权限的子文件夹跳过。
static bool walk_folder(const wchar_t *path, FolderWalk *walk)
{
    wchar_t *pattern = path_join(path, L"*");
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileExW(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do
    {
        if (walk->cancelled && *walk->cancelled)
        {
            ok = false;
            break;
        }
        if (find_data_is_link(&found)) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            wchar_t *child = path_join(path, found.cFileName);
            ok = walk_folder(child, walk);
            free(child);
            if (!ok || walk->exceeded) break;
            continue;
        }
        walk->size += ((long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        if (++walk->files > FOLDER_MAXIMUM_FILES || GetTickCount64() - walk->started > FOLDER_SIZE_BUDGET_MS)
        {
            walk->exceeded = true;
            break;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return ok;
}

/// 第一层只列一遍：一边数有几项，一边累加大小（原来先列一遍数个数，再从头列一遍算大小）。
/// 大小超出预算以后不再往子文件夹里走，但第一层照样数完。
bool folder_measure(const wchar_t *path, int *children, bool *has_size, long long *size, volatile LONG *cancelled)
{
    *children = 0;
    *has_size = false;
    *size = 0;
    wchar_t *pattern = path_join(path, L"*");
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileExW(pattern, FindExInfoBasic, &found, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE)
    {
        // 空的根目录也会走到这里；文件夹本身在就算 0 项。
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND || !path_is_directory(path)) return false;
        *has_size = true;
        return true;
    }
    FolderWalk walk = { GetTickCount64(), 0, 0, false, cancelled };
    bool ok = true;
    do
    {
        if (cancelled && *cancelled)
        {
            ok = false;
            break;
        }
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        (*children)++;
        // 链接算一项，但不进去也不计大小。
        if (walk.exceeded || find_data_is_link(&found)) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            wchar_t *child = path_join(path, found.cFileName);
            ok = walk_folder(child, &walk);
            free(child);
            if (!ok) break;
            continue;
        }
        walk.size += ((long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        if (++walk.files > FOLDER_MAXIMUM_FILES || GetTickCount64() - walk.started > FOLDER_SIZE_BUDGET_MS) walk.exceeded = true;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (!ok) return false;
    *has_size = !walk.exceeded;
    *size = walk.exceeded ? 0 : walk.size;
    return true;
}
