// archive.h 的 Linux 实现：ZIP 用 miniz 读写，7z 交给 bsdtar（libarchive）。
// 和 Windows 版一样同步执行，从后台线程调用；解压前检查条目路径，不会写到目标文件夹外。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "archive.h"
#include "file_ops.h"
#include "log.h"

#include <dirent.h>
#include <glib.h>
#include <miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ───────────── 来源和输出位置 ─────────────

static bool is_separator(wchar_t character)
{
    return character == L'/' || character == L'\\';
}

static StrList normalize_sources(const StrList *paths)
{
    StrList sources = {0};
    TextSet seen = { .ignore_case = true };
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

static wchar_t *destination_directory(const wchar_t *source, const wchar_t *requested)
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

static wchar_t *source_name(const wchar_t *source)
{
    if (!path_is_directory(source)) return path_file_stem(source);
    return path_folder_name(source);
}

/// 写入压缩包时的条目名：统一用 /，去掉开头的分隔符。
static char *entry_name(const char *prefix, const char *name)
{
    if (!prefix || !*prefix) return g_strdup(name);
    return g_strdup_printf("%s/%s", prefix, name);
}

static char *narrow(const wchar_t *path)
{
    return utf8_from_wide(path, NULL);
}

// ───────────── 写入 ZIP ─────────────

/// 递归加入文件。符号链接不打包，免得解压后指向压缩包外面。
static bool add_tree(mz_zip_archive *zip, const char *disk, const char *entry)
{
    struct stat info;
    if (lstat(disk, &info) != 0) return false;
    if (S_ISLNK(info.st_mode)) return true;
    if (S_ISDIR(info.st_mode))
    {
        DIR *directory = opendir(disk);
        if (!directory)
        {
            log_error(L"压缩时无法读取文件夹");
            return false;
        }
        bool ok = true;
        struct dirent *item;
        while (ok && (item = readdir(directory)) != NULL)
        {
            if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
            char *child_disk = g_build_filename(disk, item->d_name, NULL);
            char *child_entry = entry_name(entry, item->d_name);
            ok = add_tree(zip, child_disk, child_entry);
            g_free(child_disk);
            g_free(child_entry);
        }
        closedir(directory);
        return ok;
    }
    if (!S_ISREG(info.st_mode)) return true;
    return mz_zip_writer_add_file(zip, entry, disk, NULL, 0, MZ_DEFAULT_COMPRESSION) != 0;
}

void archive_compress(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result)
{
    StrList sources = normalize_sources(paths);
    if (sources.count == 0)
    {
        quick_action_result_fail(result, STR_NothingToCompress);
        return;
    }
    wchar_t *directory = destination_directory(sources.items[0], output_directory);
    wchar_t *output = NULL;
    bool ok = directory != NULL;
    if (ok)
    {
        wchar_t *name = sources.count == 1 ? source_name(sources.items[0]) : wcs_dup(S(STR_DefaultArchiveName));
        output = path_available(directory, name, L".zip");
        free(name);
        ok = output != NULL;
    }
    char *output_narrow = ok ? narrow(output) : NULL;
    ok = ok && output_narrow;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (ok)
    {
        ok = mz_zip_writer_init_file(&zip, output_narrow, 0) != 0;
        if (!ok) log_error(L"无法创建压缩文件");
    }
    for (int index = 0; ok && index < sources.count; index++)
    {
        char *disk = narrow(sources.items[index]);
        if (!disk) { ok = false; break; }
        wchar_t *name = sources.count == 1 ? source_name(sources.items[index]) : wcs_dup(path_file_name(sources.items[index]));
        char *entry = utf8_from_wide(name, NULL);
        free(name);
        ok = entry && add_tree(&zip, disk, entry);
        free(entry);
        free(disk);
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&zip) != 0;
    if (!ok && output_narrow && path_exists(output)) io_delete_file(output);
    if (zip.m_zip_mode != MZ_ZIP_MODE_INVALID) mz_zip_writer_end(&zip);
    if (ok)
    {
        strlist_add(&result->outputs, output);
        quick_action_result_set(result, true, str_created(path_file_name(output)));
    }
    else
    {
        log_error(L"快速压缩失败");
        quick_action_result_fail(result, STR_CompressFailed);
    }
    free(output_narrow);
    free(directory);
    free(output);
    strlist_clear(&sources);
}

// ───────────── 解压 ─────────────

/// 条目路径不安全（绝对路径、..、空名字）时返回 false。
static bool safe_entry(const char *name)
{
    if (!name || !*name) return false;
    if (name[0] == '/') return false;
    const char *start = name;
    for (const char *cursor = name;; cursor++)
    {
        if (*cursor == '/' || *cursor == 0)
        {
            size_t length = (size_t)(cursor - start);
            if (length == 2 && start[0] == '.' && start[1] == '.') return false;
            if (*cursor == 0) break;
            start = cursor + 1;
        }
    }
    return true;
}

static bool extract_zip(const wchar_t *source, const char *temporary)
{
    char *archive = narrow(source);
    if (!archive) return false;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    bool ok = mz_zip_reader_init_file(&zip, archive, 0) != 0;
    free(archive);
    if (!ok) return false;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; ok && index < count; index++)
    {
        char name[4096];
        if (mz_zip_reader_get_filename(&zip, index, name, sizeof(name)) == 0) { ok = false; break; }
        if (!safe_entry(name)) { ok = false; break; }
        char *target = g_build_filename(temporary, name, NULL);
        if (mz_zip_reader_is_file_a_directory(&zip, index) || name[strlen(name) - 1] == '/')
        {
            g_mkdir_with_parents(target, 0755);
            g_free(target);
            continue;
        }
        char *parent = g_path_get_dirname(target);
        g_mkdir_with_parents(parent, 0755);
        g_free(parent);
        ok = mz_zip_reader_extract_to_file(&zip, index, target, 0) != 0;
        g_free(target);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

static bool tree_has_link(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) return false;
    if (S_ISLNK(info.st_mode)) return true;
    if (!S_ISDIR(info.st_mode)) return false;
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool found = false;
    struct dirent *item;
    while (!found && (item = readdir(directory)) != NULL)
    {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
        char *child = g_build_filename(path, item->d_name, NULL);
        found = tree_has_link(child);
        g_free(child);
    }
    closedir(directory);
    return found;
}

static bool extract_7z(const wchar_t *source, const char *temporary)
{
    char *archive = narrow(source);
    if (!archive) return false;
    char *argv[] = { "bsdtar", "-xf", archive, "-C", (char *)temporary, NULL };
    int status = 0;
    GError *error = NULL;
    bool started = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &error);
    if (error) g_error_free(error);
    free(archive);
    if (!started || status != 0) return false;
    // Windows 版不允许压缩包里带链接；这里解压后发现有链接就当作失败。
    return !tree_has_link(temporary);
}

static void delete_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (S_ISDIR(info.st_mode))
    {
        DIR *directory = opendir(path);
        if (directory)
        {
            struct dirent *item;
            while ((item = readdir(directory)) != NULL)
            {
                if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
                char *child = g_build_filename(path, item->d_name, NULL);
                delete_tree(child);
                g_free(child);
            }
            closedir(directory);
        }
        rmdir(path);
        return;
    }
    unlink(path);
}

static void extract(const wchar_t *source, const wchar_t *output_directory, bool seven_zip, QuickActionResult *result)
{
    wchar_t *parent = destination_directory(source, output_directory);
    wchar_t *stem = path_file_stem(source);
    wchar_t *destination = parent ? path_available(parent, stem, L"") : NULL;
    free(stem);
    char *destination_narrow = destination ? narrow(destination) : NULL;
    char *temporary = NULL;
    if (destination_narrow)
    {
        char *folder = g_path_get_dirname(destination_narrow);
        temporary = g_build_filename(folder, ".dropshelf-extract-XXXXXX", NULL);
        g_free(folder);
    }
    bool created = temporary && g_mkdtemp(temporary) != NULL;
    bool ok = created && (seven_zip ? extract_7z(source, temporary) : extract_zip(source, temporary));
    if (ok)
    {
        // 解压到临时文件夹是为了不留下半成品；成功后再改成目标名字。
        ok = rename(temporary, destination_narrow) == 0;
        if (!ok) log_error(L"无法保存解压的文件夹");
    }
    if (ok)
    {
        strlist_add(&result->outputs, destination);
        quick_action_result_set(result, true, str_extracted_to(path_file_name(destination)));
    }
    else
    {
        log_error(L"快速解压失败：%ls", source);
        quick_action_result_fail(result, STR_ExtractFailed);
    }
    if (created && temporary)
    {
        // 成功时临时文件夹已经改名，不存在了；失败时把它清理掉。
        struct stat info;
        if (lstat(temporary, &info) == 0) delete_tree(temporary);
    }
    g_free(temporary);
    g_free(destination_narrow);
    free(parent);
    free(destination);
}

void archive_run(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result)
{
    StrList sources = normalize_sources(paths);
    if (sources.count == 0) quick_action_result_fail(result, STR_NothingToArchive);
    else if (sources.count == 1 && !path_is_directory(sources.items[0]))
    {
        const wchar_t *dot = wcsrchr(path_file_name(sources.items[0]), L'.');
        if (dot && _wcsicmp(dot, L".zip") == 0) extract(sources.items[0], output_directory, false, result);
        else if (dot && _wcsicmp(dot, L".7z") == 0) extract(sources.items[0], output_directory, true, result);
        else archive_compress(&sources, output_directory, result);
    }
    else archive_compress(&sources, output_directory, result);
    strlist_clear(&sources);
}

void zone_identifier_propagate(const wchar_t *archive, const wchar_t *directory)
{
    // Linux 没有“下载来源标记”。
    (void)archive;
    (void)directory;
}