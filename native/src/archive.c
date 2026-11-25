#include "archive.h"
#include "archive_paths.h"
#include "attachment_cache.h"
#include "file_ops.h"
#include "log.h"

#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAR_TIMEOUT_MILLISECONDS (30u * 60 * 1000)

static bool is_separator(wchar_t character)
{
    return character == L'\\' || character == L'/';
}

static wchar_t *temporary_name(const wchar_t *directory, const wchar_t *suffix)
{
    wchar_t *guid = new_guid_text();
    WStr name = {0};
    wstr_appendf(&name, L".dropshelf-%ls%ls", guid, suffix);
    wchar_t *path = path_join(directory, name.data);
    wstr_free(&name);
    free(guid);
    return path;
}

// ───────────── 写入 ZIP ─────────────

typedef struct ZipWriter
{
    HANDLE file;
    unsigned long long position;
} ZipWriter;

static size_t write_archive(void *opaque, mz_uint64 offset, const void *buffer, size_t count)
{
    ZipWriter *writer = opaque;
    if (offset != writer->position)
    {
        LARGE_INTEGER target = { .QuadPart = (LONGLONG)offset };
        if (!SetFilePointerEx(writer->file, target, NULL, FILE_BEGIN)) return 0;
        writer->position = offset;
    }
    size_t written = 0;
    while (written < count)
    {
        DWORD chunk = count - written > (1u << 30) ? (1u << 30) : (DWORD)(count - written), done = 0;
        if (!WriteFile(writer->file, (const char *)buffer + written, chunk, &done, NULL) || done == 0) break;
        written += done;
    }
    writer->position += written;
    return written;
}

typedef struct FileReader
{
    HANDLE file;
    unsigned long long position;
} FileReader;

static size_t read_file_at(void *opaque, mz_uint64 offset, void *buffer, size_t count)
{
    FileReader *reader = opaque;
    if (offset != reader->position)
    {
        LARGE_INTEGER target = { .QuadPart = (LONGLONG)offset };
        if (!SetFilePointerEx(reader->file, target, NULL, FILE_BEGIN)) return 0;
        reader->position = offset;
    }
    size_t total = 0;
    while (total < count)
    {
        DWORD chunk = count - total > (1u << 30) ? (1u << 30) : (DWORD)(count - total), done = 0;
        if (!ReadFile(reader->file, (char *)buffer + total, chunk, &done, NULL) || done == 0) break;
        total += done;
    }
    reader->position += total;
    return total;
}

static time_t unix_time(FILETIME time)
{
    ULARGE_INTEGER value = { .LowPart = time.dwLowDateTime, .HighPart = time.dwHighDateTime };
    if (value.QuadPart < 116444736000000000ull) return 0;
    return (time_t)((value.QuadPart - 116444736000000000ull) / 10000000ull);
}

/// 这么大以上的文件先取样看值不值得压缩。
#define SAMPLE_FROM_SIZE (1024 * 1024)
#define SAMPLE_BYTES (32 * 1024)
#define SAMPLE_COUNT 3

/// 视频、照片、压缩包这类内容本身已经压缩过，再压一遍几乎不会变小，却要花掉绝大部分时间
/// （压缩每秒只能处理几十 MB，原样存入只受磁盘速度限制）。在文件的 1/4、1/2、3/4 处各取一小段快速压一下，
/// 合起来省不到 3% 就原样存入。小文件不取样，照常压缩；读不到样本时也照常压缩。
static bool is_already_compressed(FileReader *reader, unsigned long long size)
{
    if (size < SAMPLE_FROM_SIZE) return false;
    unsigned char *sample = xalloc(SAMPLE_BYTES), *packed = xalloc(SAMPLE_BYTES);
    int flags = (int)tdefl_create_comp_flags_from_zip_params(MZ_BEST_SPEED, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    size_t packed_total = 0;
    bool read = true;
    for (int index = 1; index <= SAMPLE_COUNT && read; index++)
    {
        unsigned long long offset = size / (SAMPLE_COUNT + 1) * (unsigned long long)index;
        read = read_file_at(reader, offset, sample, SAMPLE_BYTES) == SAMPLE_BYTES;
        // 压完比原来还大（缓冲区放不下）时返回 0。
        size_t length = read ? tdefl_compress_mem_to_mem(packed, SAMPLE_BYTES, sample, SAMPLE_BYTES, flags) : 0;
        packed_total += length ? length : SAMPLE_BYTES;
    }
    free(sample);
    free(packed);
    return read && packed_total >= (size_t)SAMPLE_BYTES * SAMPLE_COUNT * 97 / 100;
}

static bool add_file(mz_zip_archive *zip, const wchar_t *path, const wchar_t *entry_name)
{
    HANDLE file = io_create_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        log_win32_error(L"压缩时无法读取文件", GetLastError());
        return false;
    }
    LARGE_INTEGER size;
    FILETIME written = {0};
    bool ok = GetFileSizeEx(file, &size) && GetFileTime(file, NULL, NULL, &written);
    if (ok)
    {
        char *name = utf8_from_wide(entry_name, NULL);
        time_t modified = unix_time(written);
        FileReader reader = { file, 0 };
        mz_uint level = is_already_compressed(&reader, (unsigned long long)size.QuadPart) ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL;
        ok = mz_zip_writer_add_read_buf_callback(zip, name, read_file_at, &reader, (mz_uint64)size.QuadPart, &modified, NULL, 0,
            level, NULL, 0, NULL, 0);
        free(name);
    }
    CloseHandle(file);
    return ok;
}

static bool add_folder_entry(mz_zip_archive *zip, const wchar_t *entry_name)
{
    WStr name = {0};
    wstr_appendf(&name, L"%ls/", entry_name);
    char *utf8 = utf8_from_wide(name.data, NULL);
    wstr_free(&name);
    bool ok = mz_zip_writer_add_mem(zip, utf8, NULL, 0, MZ_NO_COMPRESSION);
    free(utf8);
    return ok;
}

/// 整棵目录树只走一遍：每个文件夹列一次，文件直接写入，子文件夹排队，什么都没有的文件夹留一个空条目。
/// 符号链接和目录联接不跟进也不写入，但所在的文件夹不算空。OneDrive 等按需下载的文件和文件夹也带重解析点属性，
/// 但是真正的内容，照常压缩（见 find_data_is_link）。
static bool add_directory(mz_zip_archive *zip, const wchar_t *directory, StrList *used)
{
    wchar_t *folder_name = path_folder_name(directory);
    wchar_t *root_name = archive_entry_name(folder_name, used);
    free(folder_name);
    bool ok = true, has_entries = false, first = true;
    StrList pending_paths = {0}, pending_names = {0};
    strlist_add(&pending_paths, directory);
    strlist_add(&pending_names, root_name);
    while (ok && pending_paths.count > 0)
    {
        wchar_t *current = pending_paths.items[--pending_paths.count];
        wchar_t *current_name = pending_names.items[--pending_names.count];
        bool is_root = first;
        first = false;
        bool empty = true;
        wchar_t *pattern = path_join(current, L"*");
        WIN32_FIND_DATAW data;
        HANDLE find = io_find_first(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        free(pattern);
        if (find == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                log_win32_error(L"压缩时无法读取文件夹", GetLastError());
                ok = false;
            }
        }
        else
        {
            do
            {
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                empty = false;
                if (find_data_is_link(&data)) continue;
                WStr name = {0};
                wstr_appendf(&name, L"%ls/%ls", current_name, data.cFileName);
                wchar_t *full = path_join(current, data.cFileName);
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    strlist_add_owned(&pending_paths, full);
                    strlist_add_owned(&pending_names, wstr_take(&name));
                    continue;
                }
                ok = add_file(zip, full, name.data);
                has_entries = true;
                free(full);
                wstr_free(&name);
            } while (ok && FindNextFileW(find, &data));
            FindClose(find);
        }
        if (ok && empty && !is_root)
        {
            ok = add_folder_entry(zip, current_name);
            has_entries = true;
        }
        free(current);
        free(current_name);
    }
    if (ok && !has_entries) ok = add_folder_entry(zip, root_name);
    strlist_clear(&pending_paths);
    strlist_clear(&pending_names);
    free(root_name);
    return ok;
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
    wchar_t *output = NULL, *temporary = NULL;
    bool ok = directory != NULL;
    if (ok)
    {
        wchar_t *name = sources.count == 1 ? archive_source_name(sources.items[0]) : wcs_dup(S(STR_DefaultArchiveName));
        output = path_available(directory, name, L".zip");
        free(name);
        temporary = temporary_name(directory, L".tmp");
        ok = output != NULL;
    }
    HANDLE file = INVALID_HANDLE_VALUE;
    if (ok)
    {
        file = io_create_file(temporary, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = file != INVALID_HANDLE_VALUE;
        if (!ok) log_win32_error(L"无法创建压缩文件", GetLastError());
    }
    if (ok)
    {
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        ZipWriter writer = { file, 0 };
        zip.m_pWrite = write_archive;
        zip.m_pIO_opaque = &writer;
        ok = mz_zip_writer_init_v2(&zip, 0, 0);
        StrList used = {0};
        for (int index = 0; ok && index < sources.count; index++)
        {
            const wchar_t *source = sources.items[index];
            if (path_is_directory(source)) ok = add_directory(&zip, source, &used);
            else
            {
                wchar_t *entry = archive_entry_name(path_file_name(source), &used);
                ok = add_file(&zip, source, entry);
                free(entry);
            }
        }
        strlist_clear(&used);
        if (ok) ok = mz_zip_writer_finalize_archive(&zip);
        if (!ok) log_error(L"写入压缩文件失败：%hs", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        mz_zip_writer_end(&zip);
        CloseHandle(file);
        if (ok && !io_move_file(temporary, output, 0))
        {
            log_win32_error(L"无法保存压缩文件", GetLastError());
            ok = false;
        }
    }
    if (ok)
    {
        strlist_add(&result->outputs, output);
        quick_action_result_set(result, true, str_created(path_file_name(output)));
    }
    else
    {
        if (temporary && file != INVALID_HANDLE_VALUE) io_delete_file(temporary);
        log_error(L"快速压缩失败");
        quick_action_result_fail(result, STR_CompressFailed);
    }
    free(directory);
    free(output);
    free(temporary);
    strlist_clear(&sources);
}

// ───────────── 解压 ZIP ─────────────

/// 条目名称：标明是 UTF-8 的按 UTF-8；没有标明的先试 UTF-8，不是再按系统的 OEM 代码页（和资源管理器一样）。
static wchar_t *entry_name(mz_zip_archive *zip, mz_uint index, bool utf8_flag)
{
    mz_uint length = mz_zip_reader_get_filename(zip, index, NULL, 0);
    char *bytes = xalloc(length > 0 ? length : 1);
    mz_zip_reader_get_filename(zip, index, bytes, length > 0 ? length : 1);
    int size = (int)strlen(bytes);
    wchar_t *name = NULL;
    UINT code_page = CP_UTF8;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, NULL, 0);
    if (count == 0 && size > 0)
    {
        if (utf8_flag) count = MultiByteToWideChar(CP_UTF8, 0, bytes, size, NULL, 0);
        else
        {
            code_page = CP_OEMCP;
            count = MultiByteToWideChar(code_page, 0, bytes, size, NULL, 0);
        }
    }
    name = xalloc(((size_t)count + 1) * sizeof(wchar_t));
    if (count > 0) MultiByteToWideChar(code_page, 0, bytes, size, name, count);
    name[count] = 0;
    free(bytes);
    return name;
}

/// 条目放到 destination 里面的完整路径；会写到外面（..、绝对路径、盘符、备用数据流）时返回 NULL。
static wchar_t *safe_target(const wchar_t *destination_base, const wchar_t *relative)
{
    if (!relative || is_separator(relative[0]) || wcschr(relative, L':')) return NULL;
    WStr joined = {0};
    wstr_appendf(&joined, L"%ls\\", destination_base);
    for (const wchar_t *cursor = relative; *cursor; cursor++) wstr_append_char(&joined, *cursor == L'/' ? L'\\' : *cursor);
    wchar_t *target = path_full(joined.data);
    wstr_free(&joined);
    size_t base = wcslen(destination_base);
    bool inside = _wcsicmp(target, destination_base) == 0 ||
                  (_wcsnicmp(target, destination_base, base) == 0 && target[base] == L'\\');
    if (!inside)
    {
        free(target);
        return NULL;
    }
    return target;
}

typedef struct LimitedWriter
{
    HANDLE file;
    unsigned long long written, limit;
    bool exceeded;
} LimitedWriter;

/// 实际解出的内容超过条目声明的大小时视为损坏或恶意的压缩包，防止声明很小、解出很大的压缩包写满磁盘。
static size_t write_limited(void *opaque, mz_uint64 offset, const void *buffer, size_t count)
{
    (void)offset;
    LimitedWriter *writer = opaque;
    if (writer->written + count > writer->limit)
    {
        writer->exceeded = true;
        return 0;
    }
    size_t total = 0;
    while (total < count)
    {
        DWORD chunk = count - total > (1u << 30) ? (1u << 30) : (DWORD)(count - total), done = 0;
        if (!WriteFile(writer->file, (const char *)buffer + total, chunk, &done, NULL) || done == 0) break;
        total += done;
    }
    writer->written += total;
    return total;
}

static bool extract_zip_safely(const wchar_t *source, const wchar_t *destination)
{
    HANDLE file = io_create_file(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        log_win32_error(L"无法打开压缩包", GetLastError());
        return false;
    }
    LARGE_INTEGER size;
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    FileReader reader = { file, 0 };
    zip.m_pRead = read_file_at;
    zip.m_pIO_opaque = &reader;
    bool ok = GetFileSizeEx(file, &size) && mz_zip_reader_init(&zip, (mz_uint64)size.QuadPart, 0);
    if (!ok)
    {
        log_error(L"无法读取压缩包：%hs", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        CloseHandle(file);
        return false;
    }
    wchar_t *base = path_full(destination);
    mz_uint count = mz_zip_reader_get_num_files(&zip);

    // 压缩包声明的解压后总大小放不下时直接失败，不会先把磁盘写满；查不到剩余空间时照常解压，仍受每个条目声明大小的限制。
    unsigned long long required = 0;
    for (mz_uint index = 0; ok && index < count; index++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) ok = false;
        else if (required + stat.m_uncomp_size < required) ok = false;
        else required += stat.m_uncomp_size;
    }
    long long available = ok ? disk_available(destination) : -1;
    if (ok && available >= 0 && required > (unsigned long long)available)
    {
        log_error(L"磁盘空间不足，无法解压");
        ok = false;
    }

    for (mz_uint index = 0; ok && index < count; index++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat))
        {
            ok = false;
            break;
        }
        wchar_t *name = entry_name(&zip, index, (stat.m_bit_flag & (1 << 11)) != 0);
        wchar_t *target = safe_target(base, name);
        size_t name_length = wcslen(name);
        bool folder = name_length > 0 && is_separator(name[name_length - 1]);
        free(name);
        if (!target)
        {
            log_error(L"压缩包包含不安全路径");
            ok = false;
            break;
        }
        if (folder)
        {
            ok = directory_create(target);
            free(target);
            continue;
        }
        wchar_t *parent = path_directory(target);
        if (parent) ok = directory_create(parent);
        free(parent);
        HANDLE output = ok ? io_create_file(target, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (output == INVALID_HANDLE_VALUE)
        {
            if (ok) log_win32_error(L"解压时无法创建文件", GetLastError());
            ok = false;
        }
        else
        {
            LimitedWriter writer = { output, 0, stat.m_uncomp_size, false };
            ok = stat.m_is_supported && mz_zip_reader_extract_to_callback(&zip, index, write_limited, &writer, 0);
            if (writer.exceeded) log_error(L"压缩包条目的实际大小超过声明的大小");
            else if (!ok) log_error(L"解压条目失败：%hs", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
            // 和原条目的修改时间一致。
            if (ok && stat.m_time > 0)
            {
                ULARGE_INTEGER value = { .QuadPart = (unsigned long long)stat.m_time * 10000000ull + 116444736000000000ull };
                FILETIME time = { value.LowPart, value.HighPart };
                SetFileTime(output, NULL, NULL, &time);
            }
            CloseHandle(output);
        }
        free(target);
    }
    mz_zip_reader_end(&zip);
    CloseHandle(file);
    free(base);
    return ok;
}

static void finish_extraction(const wchar_t *source, wchar_t *temporary, const wchar_t *destination, QuickActionResult *result)
{
    zone_identifier_propagate(source, temporary);
    if (io_move_file(temporary, destination, 0))
    {
        strlist_add(&result->outputs, destination);
        quick_action_result_set(result, true, str_extracted_to(path_file_name(destination)));
        return;
    }
    log_win32_error(L"无法保存解压的文件夹", GetLastError());
}

static void extract_zip(const wchar_t *source, const wchar_t *output_directory, QuickActionResult *result)
{
    wchar_t *parent = archive_output_directory(source, output_directory);
    wchar_t *stem = path_file_stem(source);
    wchar_t *destination = parent ? path_available(parent, stem, L"") : NULL;
    wchar_t *temporary = parent ? temporary_name(parent, L"") : NULL;
    free(stem);
    bool created = destination && io_create_directory(temporary, NULL);
    if (created && extract_zip_safely(source, temporary)) finish_extraction(source, temporary, destination, result);
    if (!result->succeeded)
    {
        log_error(L"快速解压失败：%ls", source);
        quick_action_result_fail(result, STR_ExtractFailed);
    }
    if (created && path_exists(temporary)) directory_delete_tree(temporary);
    free(parent);
    free(destination);
    free(temporary);
}

// ───────────── 解压 7z ─────────────

static wchar_t *tar_path(void)
{
    wchar_t system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return NULL;
    return path_join(system, L"tar.exe");
}

typedef struct PipeReader
{
    HANDLE pipe;
    char *data;
    size_t length, capacity;
} PipeReader;

static DWORD WINAPI drain_pipe(void *parameter)
{
    PipeReader *reader = parameter;
    char buffer[8192];
    DWORD read;
    while (ReadFile(reader->pipe, buffer, sizeof(buffer), &read, NULL) && read > 0)
    {
        // 列表很长时照单全收；错误输出只留前面一段写进日志。
        if (reader->length + read + 1 > reader->capacity)
        {
            reader->capacity = (reader->length + read + 1) * 2;
            reader->data = xrealloc(reader->data, reader->capacity);
        }
        memcpy(reader->data + reader->length, buffer, read);
        reader->length += read;
        reader->data[reader->length] = 0;
    }
    return 0;
}

static void append_argument(WStr *line, const wchar_t *argument)
{
    wstr_append_char(line, L' ');
    wstr_append_char(line, L'"');
    wstr_append(line, argument);
    // 以反斜杠结尾时补一个，否则会转义结尾的引号。
    if (*argument && argument[wcslen(argument) - 1] == L'\\') wstr_append_char(line, L'\\');
    wstr_append_char(line, L'"');
}

typedef struct TarRun
{
    bool started;
    bool stopped;               // 超时或磁盘快满时结束了它
    DWORD exit_code;
    char *output;               // 标准输出（需要时）
    char *error;
} TarRun;

/// 运行 tar：等它结束，期间超时就结束它；watch 不为 NULL 时盯住那个文件夹所在磁盘的剩余空间，低于保留量就结束它。
static TarRun run_tar(const wchar_t *tar, const wchar_t *const *arguments, int argument_count, bool capture_output, const wchar_t *watch)
{
    TarRun run = {0};
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE out_read = NULL, out_write = NULL, err_read = NULL, err_write = NULL, null_device = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&err_read, &err_write, &inherit, 0)) return run;
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    if (capture_output)
    {
        if (!CreatePipe(&out_read, &out_write, &inherit, 0))
        {
            CloseHandle(err_read);
            CloseHandle(err_write);
            return run;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    }
    else null_device = io_create_file(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);

    WStr line = {0};
    wstr_appendf(&line, L"\"%ls\"", tar);
    for (int index = 0; index < argument_count; index++) append_argument(&line, arguments[index]);
    STARTUPINFOEXW startup = { .StartupInfo.cb = sizeof(startup) };
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = NULL;
    startup.StartupInfo.hStdOutput = capture_output ? out_write : null_device;
    startup.StartupInfo.hStdError = err_write;
    // 只让 tar 继承它自己的两个输出：同时解压两个 7z 文件时，另一个 tar 若也继承了这里的管道，
    // 这边的 tar 结束后管道迟迟不断开，读取输出的线程就一直等着。
    HANDLE inherited[2];
    int inherited_count = 0;
    if (startup.StartupInfo.hStdOutput && startup.StartupInfo.hStdOutput != INVALID_HANDLE_VALUE) inherited[inherited_count++] = startup.StartupInfo.hStdOutput;
    inherited[inherited_count++] = err_write;
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &list_size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = xalloc(list_size);
    DWORD flags = CREATE_NO_WINDOW;
    if (InitializeProcThreadAttributeList(list, 1, 0, &list_size))
    {
        if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, (SIZE_T)inherited_count * sizeof(HANDLE), NULL, NULL))
        {
            startup.lpAttributeList = list;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        else DeleteProcThreadAttributeList(list);
    }
    else
    {
        free(list);
        list = NULL;
    }
    PROCESS_INFORMATION process = {0};
    run.started = CreateProcessW(tar, line.data, NULL, NULL, TRUE, flags, NULL, NULL, &startup.StartupInfo, &process) != 0;
    if (!run.started) log_win32_error(L"Windows 解压程序无法启动", GetLastError());
    if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(list);
    wstr_free(&line);
    CloseHandle(err_write);
    if (out_write) CloseHandle(out_write);
    if (null_device != INVALID_HANDLE_VALUE) CloseHandle(null_device);

    PipeReader out_reader = { out_read, NULL, 0, 0 }, err_reader = { err_read, NULL, 0, 0 };
    HANDLE threads[2] = { NULL, NULL };
    if (run.started)
    {
        threads[0] = CreateThread(NULL, 0, drain_pipe, &err_reader, 0, NULL);
        if (capture_output) threads[1] = CreateThread(NULL, 0, drain_pipe, &out_reader, 0, NULL);
        ULONGLONG started = GetTickCount64();
        while (WaitForSingleObject(process.hProcess, 500) == WAIT_TIMEOUT)
        {
            bool timed_out = GetTickCount64() - started > TAR_TIMEOUT_MILLISECONDS;
            long long free_space = watch ? disk_available(watch) : -1;
            bool full = watch && free_space >= 0 && free_space < DISK_RESERVE;
            if (!timed_out && !full) continue;
            log_error(timed_out ? L"7z 解压时间过长" : L"磁盘空间不足，无法解压");
            // 结束进程并等它真正退出，之后才能删除它写了一半的文件。
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            run.stopped = true;
            break;
        }
        GetExitCodeProcess(process.hProcess, &run.exit_code);
        // 读取线程用着这里栈上的 PipeReader，必须等它们真正结束；管道迟迟不断开时打断它们正在等的读取。
        for (int index = 0; index < 2; index++)
            if (threads[index])
            {
                while (WaitForSingleObject(threads[index], 5000) == WAIT_TIMEOUT) CancelSynchronousIo(threads[index]);
                CloseHandle(threads[index]);
            }
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    CloseHandle(err_read);
    if (out_read) CloseHandle(out_read);
    run.output = out_reader.data;
    run.error = err_reader.data;
    return run;
}

static void tar_free(TarRun *run)
{
    free(run->output);
    free(run->error);
}

static bool tar_succeeded(TarRun *run, const wchar_t *what)
{
    if (run->started && !run->stopped && run->exit_code == 0) return true;
    if (run->started && !run->stopped)
    {
        wchar_t *error = run->error ? wide_from_utf8(run->error, strlen(run->error)) : NULL;
        log_error(L"%ls（退出代码 %lu）\r\n%ls", what, (unsigned long)run->exit_code, error ? error : L"");
        free(error);
    }
    return false;
}

/// 列出的每个条目都必须落在 destination 里面。
static bool listing_is_safe(const char *listing, const wchar_t *destination)
{
    if (!listing) return true;
    wchar_t *text = wide_from_utf8(listing, strlen(listing));
    wchar_t *base = path_full(destination);
    bool safe = true;
    wchar_t *context = NULL;
    for (wchar_t *entry = wcstok_s(text, L"\r\n", &context); entry && safe; entry = wcstok_s(NULL, L"\r\n", &context))
    {
        if (wcs_is_blank(entry)) continue;
        wchar_t *target = safe_target(base, entry);
        safe = target != NULL;
        free(target);
    }
    if (!safe) log_error(L"7z 文件包含不安全路径");
    free(base);
    free(text);
    return safe;
}

static bool contains_links(const wchar_t *directory)
{
    StrList pending = {0};
    strlist_add(&pending, directory);
    bool found = false;
    while (pending.count > 0 && !found)
    {
        wchar_t *current = pending.items[--pending.count];
        wchar_t *pattern = path_join(current, L"*");
        WIN32_FIND_DATAW data;
        HANDLE find = io_find_first(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0);
        free(pattern);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) found = true;
                else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) strlist_add_owned(&pending, path_join(current, data.cFileName));
            } while (!found && FindNextFileW(find, &data));
            FindClose(find);
        }
        free(current);
    }
    strlist_clear(&pending);
    if (found) log_error(L"7z 文件包含不支持的链接项目");
    return found;
}

static void extract_7zip(const wchar_t *source, const wchar_t *output_directory, QuickActionResult *result)
{
    wchar_t *tar = tar_path();
    if (!tar || !path_exists(tar))
    {
        free(tar);
        quick_action_result_fail(result, STR_SevenZipUnsupported);
        return;
    }
    wchar_t *parent = archive_output_directory(source, output_directory);
    wchar_t *stem = path_file_stem(source);
    wchar_t *destination = parent ? path_available(parent, stem, L"") : NULL;
    wchar_t *temporary = parent ? temporary_name(parent, L"") : NULL;
    free(stem);
    bool created = destination && io_create_directory(temporary, NULL);
    if (created)
    {
        const wchar_t *list_arguments[] = { L"-tf", source };
        TarRun listing = run_tar(tar, list_arguments, 2, true, NULL);
        bool ok = tar_succeeded(&listing, L"Windows 无法读取此 7z 文件") && listing_is_safe(listing.output, temporary);
        tar_free(&listing);
        if (ok)
        {
            const wchar_t *extract_arguments[] = { L"-xf", source, L"-C", temporary };
            TarRun extraction = run_tar(tar, extract_arguments, 4, false, temporary);
            ok = tar_succeeded(&extraction, L"Windows 无法解压此 7z 文件") && !contains_links(temporary);
            tar_free(&extraction);
        }
        if (ok) finish_extraction(source, temporary, destination, result);
    }
    if (!result->succeeded)
    {
        log_error(L"Windows 内置 7z 解压失败：%ls", source);
        quick_action_result_fail(result, STR_SevenZipFailed);
    }
    if (created && path_exists(temporary)) directory_delete_tree(temporary);
    free(parent);
    free(destination);
    free(temporary);
    free(tar);
}

// ───────────── 入口 ─────────────

void archive_run(const StrList *paths, const wchar_t *output_directory, QuickActionResult *result)
{
    StrList sources = archive_sources(paths);
    if (sources.count == 0) quick_action_result_fail(result, STR_NothingToArchive);
    else if (sources.count == 1 && !path_is_directory(sources.items[0]))
    {
        const wchar_t *dot = wcsrchr(path_file_name(sources.items[0]), L'.');
        if (dot && _wcsicmp(dot, L".zip") == 0) extract_zip(sources.items[0], output_directory, result);
        else if (dot && _wcsicmp(dot, L".7z") == 0) extract_7zip(sources.items[0], output_directory, result);
        else archive_compress(&sources, output_directory, result);
    }
    else archive_compress(&sources, output_directory, result);
    strlist_clear(&sources);
}

void zone_identifier_propagate(const wchar_t *archive, const wchar_t *directory)
{
    size_t length = 0;
    char *zone = zone_identifier_read(archive, &length);
    if (!zone) return;
    int zone_id = -1;
    char *context = NULL;
    for (char *line = strtok_s(zone, "\n", &context); line; line = strtok_s(NULL, "\n", &context))
    {
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        if (_strnicmp(line, "ZoneId=", 7) == 0)
        {
            char *end = NULL;
            long value = strtol(line + 7, &end, 10);
            if (end != line + 7) zone_id = (int)value;
            break;
        }
    }
    free(zone);
    if (zone_id < 0) return;
    wchar_t *full = path_full(archive);
    char *referrer = utf8_from_wide(full, NULL);
    free(full);
    char mark[4096];
    int mark_length = snprintf(mark, sizeof(mark), "[ZoneTransfer]\r\nZoneId=%d\r\nReferrerUrl=%s\r\n", zone_id, referrer);
    free(referrer);
    if (mark_length <= 0 || mark_length >= (int)sizeof(mark)) return;

    StrList pending = {0};
    strlist_add(&pending, directory);
    while (pending.count > 0)
    {
        wchar_t *current = pending.items[--pending.count];
        wchar_t *pattern = path_join(current, L"*");
        WIN32_FIND_DATAW data;
        HANDLE find = io_find_first(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        free(pattern);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                wchar_t *child = path_join(current, data.cFileName);
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) strlist_add_owned(&pending, child);
                else
                {
                    zone_identifier_write(child, mark, (size_t)mark_length);
                    free(child);
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        free(current);
    }
    strlist_clear(&pending);
}
