// archive.h 的 Linux 实现：ZIP 用 miniz 读写，7z 交给 bsdtar（libarchive）。
// 和 Windows 版一样同步执行，从后台线程调用；解压前检查条目路径，不会写到目标文件夹外。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "archive.h"
#include "archive_paths.h"
#include "file_ops.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <miniz.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define ARCHIVE_MAX_TREE_DEPTH 128
#define ARCHIVE_MAX_ENTRIES 100000u
#define ARCHIVE_MAX_TOTAL_BYTES (8ULL * 1024ULL * 1024ULL * 1024ULL)
#define ARCHIVE_MAX_ENTRY_NAME 4096u
#define ARCHIVE_7Z_TIMEOUT_US (60LL * 1000LL * 1000LL)

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

/// 递归加入文件。目录内部的符号链接不打包，免得解压后指向压缩包外面；顶层显式传入的符号链接按目标文件处理（与 Windows 版一致）。
/// 里面什么都没写进去的文件夹（空文件夹）单独留一个条目，解压后还在。wrote 记下写进了东西。
static bool add_tree(mz_zip_archive *zip, const char *disk, const char *entry, bool *wrote, int depth)
{
    if (depth > ARCHIVE_MAX_TREE_DEPTH)
    {
        log_error(L"压缩时文件夹层级过深");
        return false;
    }
    struct stat info;
    if ((depth == 0 ? stat(disk, &info) : lstat(disk, &info)) != 0) return false;
    if (depth > 0 && S_ISLNK(info.st_mode)) return true;
    if (S_ISDIR(info.st_mode))
    {
        DIR *directory = opendir(disk);
        if (!directory)
        {
            log_error(L"压缩时无法读取文件夹");
            return false;
        }
        bool ok = true, inner = false;
        struct dirent *item;
        while (ok && (item = readdir(directory)) != NULL)
        {
            if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
            char *child_disk = g_build_filename(disk, item->d_name, NULL);
            char *child_entry = entry_name(entry, item->d_name);
            ok = add_tree(zip, child_disk, child_entry, &inner, depth + 1);
            g_free(child_disk);
            g_free(child_entry);
        }
        closedir(directory);
        if (ok && !inner)
        {
            char *folder = g_strdup_printf("%s/", entry);
            ok = mz_zip_writer_add_mem(zip, folder, NULL, 0, MZ_NO_COMPRESSION) != 0;
            g_free(folder);
        }
        if (ok) *wrote = true;
        return ok;
    }
    if (!S_ISREG(info.st_mode)) return true;
    if (!mz_zip_writer_add_file(zip, entry, disk, NULL, 0, MZ_DEFAULT_COMPRESSION)) return false;
    *wrote = true;
    return true;
}

void archive_compress(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result)
{
    StrList sources = archive_sources(paths);
    if (sources.count == 0)
    {
        quick_action_result_fail(result, STR_NothingToCompress);
        return;
    }
    wchar_t *directory = archive_output_directory(sources.items[0], output_directory);
    wchar_t *output = NULL;
    bool ok = directory != NULL;
    if (ok)
    {
        wchar_t *name = sources.count == 1 ? archive_source_name(sources.items[0]) : wcs_dup(S(STR_DefaultArchiveName));
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
    // 条目名和 Windows 版一样：文件带扩展名，文件夹用它的名称，重名的加 (2)。
    StrList used = {0};
    bool any_written = false;
    for (int index = 0; ok && index < sources.count; index++)
    {
        const wchar_t *source = sources.items[index];
        char *disk = narrow(source);
        wchar_t *requested = path_is_directory(source) ? path_folder_name(source) : wcs_dup(path_file_name(source));
        wchar_t *name = requested ? archive_entry_name(requested, &used) : NULL;
        free(requested);
        char *entry = name ? utf8_from_wide(name, NULL) : NULL;
        free(name);
        bool wrote = false;
        ok = disk && entry && add_tree(&zip, disk, entry, &wrote, 0);
        if (wrote) any_written = true;
        free(entry);
        free(disk);
    }
    strlist_clear(&used);
    if (ok && !any_written)
    {
        log_error(L"没有可压缩的内容");
        ok = false;
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
        quick_action_result_fail(result, any_written ? STR_CompressFailed : STR_NothingToCompress);
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

typedef struct LimitedWriter
{
    int fd;
    uint64_t entry_written;
    uint64_t entry_limit;
    uint64_t *total_written;
    uint64_t total_limit;
    bool exceeded;
} LimitedWriter;

/// 限制实际写出的字节数，防止声明很小但解压出海量数据的 zip 炸弹写满磁盘。
static size_t write_limited(void *opaque, mz_uint64 offset, const void *buffer, size_t count)
{
    (void)offset;
    LimitedWriter *writer = opaque;
    if (count > writer->entry_limit - writer->entry_written || count > writer->total_limit - *writer->total_written)
    {
        writer->exceeded = true;
        return 0;
    }
    size_t written = 0;
    while (written < count)
    {
        ssize_t part = write(writer->fd, (const char *)buffer + written, count - written);
        if (part < 0)
        {
            if (errno == EINTR) continue;
            return 0;
        }
        if (part == 0) return 0;
        written += (size_t)part;
    }
    writer->entry_written += written;
    *writer->total_written += written;
    return written;
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
    if (count > ARCHIVE_MAX_ENTRIES)
    {
        log_error(L"压缩包条目数量超过上限");
        mz_zip_reader_end(&zip);
        return false;
    }

    uint64_t required = 0;
    for (mz_uint index = 0; ok && index < count; index++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat))
        {
            ok = false;
            break;
        }
        // 拒绝 Unix 符号链接条目，和 Windows 版一致。
        mode_t mode = (mode_t)(stat.m_external_attr >> 16);
        if (S_ISLNK(mode))
        {
            log_error(L"压缩包包含符号链接，已拒绝解压");
            ok = false;
            break;
        }
        if (stat.m_uncomp_size > ARCHIVE_MAX_TOTAL_BYTES - required)
        {
            log_error(L"压缩包解压后体积超过上限");
            ok = false;
            break;
        }
        required += stat.m_uncomp_size;
    }

    uint64_t total_limit = ARCHIVE_MAX_TOTAL_BYTES;
    struct statvfs fs;
    if (ok && statvfs(temporary, &fs) == 0)
    {
        uint64_t available = (uint64_t)fs.f_bavail * (uint64_t)fs.f_frsize;
        if (required > available)
        {
            log_error(L"磁盘空间不足，无法解压");
            ok = false;
        }
        else if (available < total_limit)
        {
            total_limit = available;
        }
    }

    uint64_t total_written = 0;
    for (mz_uint index = 0; ok && index < count; index++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) { ok = false; break; }
        mz_uint needed = mz_zip_reader_get_filename(&zip, index, NULL, 0);
        if (needed == 0 || needed > ARCHIVE_MAX_ENTRY_NAME)
        {
            log_error(L"压缩包条目名称过长或无效");
            ok = false;
            break;
        }
        char name[ARCHIVE_MAX_ENTRY_NAME];
        if (mz_zip_reader_get_filename(&zip, index, name, sizeof(name)) == 0) { ok = false; break; }
        if (!safe_entry(name))
        {
            log_error(L"压缩包包含不安全路径");
            ok = false;
            break;
        }
        char *target = g_build_filename(temporary, name, NULL);
        if (mz_zip_reader_is_file_a_directory(&zip, index) || name[strlen(name) - 1] == '/')
        {
            ok = g_mkdir_with_parents(target, 0755) == 0;
            g_free(target);
            continue;
        }
        char *parent = g_path_get_dirname(target);
        ok = g_mkdir_with_parents(parent, 0755) == 0;
        g_free(parent);
        if (ok)
        {
            int fd = open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
            if (fd < 0)
            {
                log_error(L"解压时无法创建文件（可能存在重复条目）");
                ok = false;
            }
            else
            {
                LimitedWriter writer = {
                    .fd = fd,
                    .entry_written = 0,
                    .entry_limit = stat.m_uncomp_size,
                    .total_written = &total_written,
                    .total_limit = total_limit,
                    .exceeded = false,
                };
                ok = mz_zip_reader_extract_to_callback(&zip, index, write_limited, &writer, 0) != 0 && !writer.exceeded;
                if (close(fd) != 0) ok = false;
                if (writer.exceeded) log_error(L"压缩包实际解压体积超出限制");
            }
        }
        g_free(target);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

static bool tree_has_link(const char *path, int depth)
{
    if (depth > ARCHIVE_MAX_TREE_DEPTH) return true;
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
        found = tree_has_link(child, depth + 1);
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
    // 7z 里的文件名是 UTF-16，bsdtar 按当前区域设置转换；会话没设 LANG（C 区域）时中文名会解不出来。
    char **environment = g_environ_setenv(g_get_environ(), "LC_ALL", "C.UTF-8", TRUE);
    GPid pid = 0;
    GError *error = NULL;
    bool started = g_spawn_async(NULL, argv, environment, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &pid, &error);
    if (error) g_error_free(error);
    g_strfreev(environment);
    free(archive);
    if (!started) return false;

    int status = 0;
    bool exited = false;
    gint64 deadline = g_get_monotonic_time() + ARCHIVE_7Z_TIMEOUT_US;
    for (;;)
    {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
        {
            exited = true;
            break;
        }
        if (done < 0 && errno != EINTR) break;
        if (g_get_monotonic_time() >= deadline)
        {
            log_error(L"解压 7z 超时，已中止");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        g_usleep(10000);
    }
    g_spawn_close_pid(pid);
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    // Windows 版不允许压缩包里带链接；这里解压后发现有链接就当作失败。
    return !tree_has_link(temporary, 0);
}

static void delete_tree(const char *path, int depth)
{
    if (depth > ARCHIVE_MAX_TREE_DEPTH) return;
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
                delete_tree(child, depth + 1);
                g_free(child);
            }
            closedir(directory);
        }
        rmdir(path);
        return;
    }
    unlink(path);
}

static int rename_no_replace(const char *from, const char *to)
{
#ifdef RENAME_NOREPLACE
    int result = renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
    if (result == 0 || (errno != ENOSYS && errno != EINVAL)) return result;
#endif
    struct stat info;
    if (lstat(to, &info) == 0)
    {
        errno = EEXIST;
        return -1;
    }
    return rename(from, to);
}

static void extract(const wchar_t *source, const wchar_t *output_directory, bool seven_zip, QuickActionResult *result)
{
    wchar_t *parent = archive_output_directory(source, output_directory);
    wchar_t *stem = path_file_stem(source);
    wchar_t *destination = parent ? path_available(parent, stem, L"") : NULL;
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
        // 解压到临时文件夹是为了不留下半成品；改名时禁止覆盖已有目录，重名时重新找可用名称。
        ok = false;
        for (int attempt = 0; attempt < 8; attempt++)
        {
            if (rename_no_replace(temporary, destination_narrow) == 0)
            {
                ok = true;
                break;
            }
            if (errno != EEXIST || !parent) break;
            free(destination);
            free(destination_narrow);
            destination = path_available(parent, stem, L"");
            destination_narrow = destination ? narrow(destination) : NULL;
            if (!destination_narrow) break;
        }
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
        if (lstat(temporary, &info) == 0) delete_tree(temporary, 0);
    }
    g_free(temporary);
    free(destination_narrow);
    free(stem);
    free(parent);
    free(destination);
}

void archive_run(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result)
{
    StrList sources = archive_sources(paths);
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