#include "archive_paths.h"
#include "file_ops.h"

#include <stdlib.h>
#include <string.h>

StrList archive_sources(const StrList *paths)
{
    StrList sources = {0};
    // 查重用排好序的集合：一次压缩几千个文件时不用两两比较。
    TextSet seen = { .ignore_case = PATHS_IGNORE_CASE };
    for (int index = 0; index < paths->count; index++)
    {
        if (!path_is_available(paths->items[index])) continue;
        wchar_t *full = path_full(paths->items[index]);
        if (text_set_add(&seen, full)) strlist_add_owned(&sources, full);
        else free(full);
    }
    text_set_clear(&seen);
    return sources;
}

wchar_t *archive_output_directory(const wchar_t *source, const wchar_t *requested)
{
    if (!wcs_is_blank(requested)) return directory_create(requested) ? wcs_dup(requested) : NULL;
    wchar_t *trimmed = path_trim_end_separator(source);
    wchar_t *parent = path_directory(trimmed);
    if (parent && wcslen(parent) >= wcslen(trimmed))
    {
        free(parent);
        parent = NULL;
    }
    free(trimmed);
    return parent;
}

wchar_t *archive_source_name(const wchar_t *source)
{
    if (!path_is_directory(source)) return path_file_stem(source);
    return path_folder_name(source);
}

wchar_t *archive_entry_name(const wchar_t *requested, StrList *used)
{
    // 重名时编号加在扩展名前面：photo.jpg → photo (2).jpg。
    const wchar_t *dot = wcsrchr(requested, L'.');
    size_t stem_length = dot && dot != requested && dot[1] ? (size_t)(dot - requested) : wcslen(requested);
    wchar_t *stem = wcs_dup_n(requested, stem_length);
    wchar_t *candidate = wcs_dup(requested);
    for (int suffix = 2; strlist_index_of(used, candidate, true) >= 0; suffix++)
    {
        free(candidate);
        WStr next = {0};
        wstr_appendf(&next, L"%ls (%d)%ls", stem, suffix, requested + stem_length);
        candidate = wstr_take(&next);
    }
    free(stem);
    strlist_add(used, candidate);
    for (wchar_t *cursor = candidate; *cursor; cursor++)
        if (*cursor == L'\\') *cursor = L'/';
    return candidate;
}
