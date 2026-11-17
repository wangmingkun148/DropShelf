#define COBJMACROS
#include "drop_read.h"
#include "attachment_cache.h"
#include "log.h"
#include "strings.h"
#include "task.h"

#include <shellapi.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

/// 超过这个大小的来源临时文件不复制，直接用原路径，免得松手后长时间等待。
#define MAXIMUM_SNAPSHOT_BYTES (512ll * 1024 * 1024)
/// 接收附件时给磁盘留下的最少剩余空间。
#define DISK_RESERVE (512ll * 1024 * 1024)
#define MAXIMUM_ATTACHMENT_COUNT 256
#define COPY_BUFFER 81920

// ───────────── 结果 ─────────────

void drop_result_free(DropReadResult *result)
{
    if (!result) return;
    for (int index = 0; index < result->count; index++)
    {
        free(result->entries[index].value);
        free(result->entries[index].title);
        free(result->entries[index].type_name);
    }
    free(result->entries);
    strlist_clear(&result->caches);
    free(result->error);
    free(result);
}

void drop_result_release_caches(DropReadResult *result)
{
    for (int index = 0; index < result->caches.count; index++) attachment_cache_release(result->caches.items[index]);
    strlist_clear(&result->caches);
}

int drop_result_file_count(const DropReadResult *result)
{
    int count = 0;
    for (int index = 0; index < result->count; index++)
        if (result->entries[index].kind == ITEM_FILE || result->entries[index].kind == ITEM_FOLDER) count++;
    return count;
}

bool drop_result_has_temporary(const DropReadResult *result)
{
    for (int index = 0; index < result->count; index++)
        if (result->entries[index].temporary) return true;
    return false;
}

static void add_entry(DropReadResult *result, ShelfItemKind kind, const wchar_t *value, const wchar_t *title, bool temporary)
{
    result->entries = xrealloc(result->entries, (size_t)(result->count + 1) * sizeof(DroppedEntry));
    result->entries[result->count++] = (DroppedEntry){ kind, wcs_dup(value), wcs_is_blank(title) ? NULL : wcs_dup(title), temporary };
}

static DropReadResult *failed_result(StringId message)
{
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->failed = 1;
    result->error = wcs_dup(S(message));
    return result;
}

/// 后台读好文件信息：是不是文件夹（文件夹的类型随之改过来）、多大、什么类型。
static void read_facts(DropReadResult *result)
{
    result->with_facts = true;
    for (int index = 0; index < result->count; index++)
    {
        DroppedEntry *entry = &result->entries[index];
        if (entry->kind != ITEM_FILE && entry->kind != ITEM_FOLDER) continue;
        entry->has_facts = file_facts_read(entry->value, &entry->facts);
        if (!entry->has_facts) continue;
        entry->kind = entry->facts.is_folder ? ITEM_FOLDER : ITEM_FILE;
        if (!entry->facts.is_folder) entry->type_name = file_type_name(entry->value);
    }
}

// ───────────── 聊天软件的临时文件 ─────────────

static bool is_under(const wchar_t *path, const wchar_t *folder)
{
    size_t length = wcslen(folder);
    while (length > 3 && (folder[length - 1] == L'\\' || folder[length - 1] == L'/')) length--;
    return wcslen(path) > length && (path[length] == L'\\' || path[length] == L'/') &&
           CompareStringOrdinal(path, (int)length, folder, (int)length, TRUE) == CSTR_EQUAL;
}

bool chat_is_volatile_path(const wchar_t *path)
{
    static const wchar_t *const chat_folders[] = {
        L"\\wechat files\\", L"\\xwechat_files\\", L"\\wxwork\\", L"\\tencent files\\", L"\\tencent\\", L"\\dingtalk", L"\\feishu", L"\\lark"
    };
    static const wchar_t *const temporary_folders[] = { L"\\temp\\", L"\\tmp\\", L"\\rwtemp\\", L"\\cache\\" };
    if (wcs_is_blank(path)) return false;
    wchar_t full[MAX_PATH * 4];
    DWORD length = GetFullPathNameW(path, MAX_PATH * 4, full, NULL);
    if (!length || length >= MAX_PATH * 4) return false;
    if (is_under(full, attachment_cache_root())) return false;
    wchar_t temp[MAX_PATH + 1];
    DWORD temp_length = GetTempPathW(MAX_PATH + 1, temp);
    if (temp_length && temp_length <= MAX_PATH && is_under(full, temp)) return true;
    CharLowerW(full);
    bool chat = false, temporary = false;
    for (size_t index = 0; index < sizeof(chat_folders) / sizeof(chat_folders[0]) && !chat; index++) chat = wcsstr(full, chat_folders[index]) != NULL;
    for (size_t index = 0; index < sizeof(temporary_folders) / sizeof(temporary_folders[0]) && !temporary; index++)
        temporary = wcsstr(full, temporary_folders[index]) != NULL;
    return chat && temporary;
}

// ───────────── 工作 ─────────────

typedef enum ReadKind { READ_FILES, READ_VIRTUAL, READ_IMAGE, READ_DONE } ReadKind;

typedef struct Snapshot
{
    int index;
    HANDLE file;
    char *zone;
    size_t zone_length;
} Snapshot;

typedef struct VirtualFile
{
    wchar_t *destination;
    bool has_write_time;
    FILETIME write_time;
    STGMEDIUM medium;
    IStream *marshaled;         // 封送过的附件流，后台线程取用
} VirtualFile;

typedef struct ReadJob
{
    ReadKind kind;
    bool with_facts;
    volatile LONG *cancelled;
    DropReadDone done;
    void *context;
    DropReadResult *result;

    // 文件
    bool *volatile_paths;
    Snapshot *snapshots;
    int snapshot_count;
    wchar_t *snapshot_cache;

    // 虚拟附件
    VirtualFile *files;
    int file_count;
    wchar_t *cache;

    // 图片
    unsigned char *image_bytes;     // 编码好的图片，或 BMP 文件（补好文件头的 DIB）
    size_t image_size;
    bool image_encoded;
    wchar_t *image_path;
} ReadJob;

static void finish_job(void *data)
{
    ReadJob *job = data;
    // 附件流和介质是界面线程的套间给的，回到这里再释放。
    for (int index = 0; index < job->file_count; index++)
    {
        VirtualFile *file = &job->files[index];
        if (file->marshaled)
        {
            // 后台线程没有取走封送的流时，在这里取回并释放，否则会留下一个引用。
            IStream *stream = NULL;
            if (SUCCEEDED(CoGetInterfaceAndReleaseStream((IStream *)file->marshaled, &IID_IStream, (void **)&stream)) && stream) IStream_Release(stream);
        }
        ReleaseStgMedium(&file->medium);
        free(file->destination);
    }
    free(job->files);
    free(job->cache);
    free(job->volatile_paths);
    free(job->snapshots);
    free(job->snapshot_cache);
    free(job->image_bytes);
    free(job->image_path);
    job->done(job->result, job->context);
    free(job);
}

static bool cancelled(const ReadJob *job)
{
    return job->cancelled && *job->cancelled;
}

/// 一次接收的累计写入量上限；每写一段之前扣除，超出或已取消时中止。
typedef struct WriteBudget
{
    long long remaining;
    const ReadJob *job;
} WriteBudget;

static bool budget_take(WriteBudget *budget, size_t count)
{
    if (cancelled(budget->job)) return false;
    budget->remaining -= (long long)count;
    return budget->remaining >= 0;
}

static bool write_all(HANDLE output, const void *bytes, DWORD count)
{
    DWORD written = 0;
    return WriteFile(output, bytes, count, &written, NULL) && written == count;
}

/// 从文件句柄一直读到末尾写进去；读出错不能当成读完，否则会把只复制了一半的文件当成成功。
static bool copy_handle(HANDLE input, HANDLE output, WriteBudget *budget)
{
    unsigned char *buffer = xalloc(COPY_BUFFER);
    bool ok = true;
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(input, buffer, COPY_BUFFER, &read, NULL))
        {
            ok = false;
            break;
        }
        if (read == 0) break;
        if ((budget && !budget_take(budget, read)) || !write_all(output, buffer, read))
        {
            ok = false;
            break;
        }
    }
    free(buffer);
    return ok;
}

/// 来源文件此时可能已被删除，从仍然打开的句柄复制出来，修改时间也从句柄读。
static void copy_snapshots(ReadJob *job)
{
    StrList used = {0};
    for (int index = 0; index < job->snapshot_count; index++)
    {
        Snapshot *snapshot = &job->snapshots[index];
        DroppedEntry *entry = &job->result->entries[snapshot->index];
        wchar_t *name = attachment_safe_name(path_file_name(entry->value), &used);
        wchar_t *destination = path_join(job->snapshot_cache, name);
        free(name);
        bool ok = !cancelled(job);
        HANDLE output = ok ? io_create_file(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
        ok = output != INVALID_HANDLE_VALUE;
        if (ok)
        {
            ok = copy_handle(snapshot->file, output, NULL);
            FILETIME created, accessed, written;
            if (ok && GetFileTime(snapshot->file, &created, &accessed, &written)) SetFileTime(output, NULL, NULL, &written);
            CloseHandle(output);
        }
        if (ok)
        {
            zone_identifier_write(destination, snapshot->zone, snapshot->zone_length);
            free(entry->value);
            entry->value = destination;
            entry->kind = ITEM_FILE;
            entry->temporary = true;
        }
        else
        {
            // 复制失败时保留原路径，文件只要还在就仍然可用。
            log_error(L"保存来源程序的临时文件失败：%ls", entry->value);
            io_delete_file(destination);
            free(destination);
        }
        CloseHandle(snapshot->file);
        free(snapshot->zone);
    }
    strlist_clear(&used);
}

static void work_files(ReadJob *job)
{
    DropReadResult *result = job->result;
    for (int index = 0; index < result->count; index++)
        if (!job->volatile_paths[index] && path_is_directory(result->entries[index].value)) result->entries[index].kind = ITEM_FOLDER;
    if (job->snapshot_cache) copy_snapshots(job);
}

static bool copy_stream(IStream *input, HANDLE output, WriteBudget *budget)
{
    unsigned char *buffer = xalloc(COPY_BUFFER);
    bool ok = true;
    for (;;)
    {
        ULONG read = 0;
        HRESULT hr = input->lpVtbl->Read(input, buffer, COPY_BUFFER, &read);
        if (FAILED(hr))
        {
            ok = false;
            break;
        }
        if (read == 0) break;
        if (!budget_take(budget, read) || !write_all(output, buffer, read))
        {
            ok = false;
            break;
        }
        if (hr == S_FALSE) break;
    }
    free(buffer);
    return ok;
}

static bool copy_virtual_file(VirtualFile *file, WriteBudget *budget)
{
    HANDLE output = io_create_file(file->destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (output == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    switch (file->medium.tymed)
    {
        case TYMED_ISTREAM:
        {
            IStream *stream = NULL;
            IStream *marshaled = file->marshaled;
            file->marshaled = NULL;
            if (marshaled && SUCCEEDED(CoGetInterfaceAndReleaseStream(marshaled, &IID_IStream, (void **)&stream)) && stream)
            {
                ok = copy_stream(stream, output, budget);
                IStream_Release(stream);
            }
            break;
        }
        case TYMED_HGLOBAL:
        {
            SIZE_T size = file->medium.hGlobal ? GlobalSize(file->medium.hGlobal) : 0;
            const unsigned char *bytes = size ? GlobalLock(file->medium.hGlobal) : NULL;
            ok = size == 0 || bytes != NULL;
            for (SIZE_T offset = 0; ok && offset < size; offset += COPY_BUFFER)
            {
                DWORD count = (DWORD)(size - offset < COPY_BUFFER ? size - offset : COPY_BUFFER);
                ok = budget_take(budget, count) && write_all(output, bytes + offset, count);
            }
            if (bytes) GlobalUnlock(file->medium.hGlobal);
            break;
        }
        case TYMED_FILE:
        {
            HANDLE input = file->medium.lpszFileName ? io_create_file(file->medium.lpszFileName, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
            if (input == INVALID_HANDLE_VALUE) break;
            ok = copy_handle(input, output, budget);
            CloseHandle(input);
            break;
        }
    }
    if (ok && file->has_write_time) SetFileTime(output, NULL, NULL, &file->write_time);
    CloseHandle(output);
    return ok;
}

static void work_virtual(ReadJob *job)
{
    // 后台线程要按 COM 的规则取用界面线程交出的附件流。
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ULARGE_INTEGER free_bytes;
    WriteBudget budget = { LLONG_MAX, job };
    if (io_disk_free_space(job->cache, &free_bytes, NULL, NULL)) budget.remaining = (long long)free_bytes.QuadPart - DISK_RESERVE;
    for (int index = 0; index < job->file_count; index++)
    {
        VirtualFile *file = &job->files[index];
        if (cancelled(job)) break;
        if (copy_virtual_file(file, &budget))
        {
            add_entry(job->result, ITEM_FILE, file->destination, NULL, true);
            continue;
        }
        job->result->failed++;
        log_error(L"无法写入虚拟附件 %ls", path_file_name(file->destination));
        io_delete_file(file->destination);
    }
    if (job->result->count == 0)
    {
        free(job->result->error);
        job->result->error = wcs_dup(S(STR_AttachmentReceiveFailed));
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}

/// 用系统的图像组件把 BMP 解码后存成 PNG。
static bool encode_png(const unsigned char *bmp, size_t size, const wchar_t *path)
{
    IWICImagingFactory *factory = NULL;
    IWICStream *input = NULL, *output = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *target = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &input);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(input, (BYTE *)bmp, (DWORD)size);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)input, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &output);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(output, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)output, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &target, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(target, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(target, (IWICBitmapSource *)frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(target);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (target) IWICBitmapFrameEncode_Release(target);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (output) IWICStream_Release(output);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (input) IWICStream_Release(input);
    if (factory) IWICImagingFactory_Release(factory);
    return SUCCEEDED(hr);
}

static void work_image(ReadJob *job)
{
    bool ok;
    if (job->image_encoded)
    {
        HANDLE output = io_create_file(job->image_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        ok = output != INVALID_HANDLE_VALUE && write_all(output, job->image_bytes, (DWORD)job->image_size);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    }
    else ok = encode_png(job->image_bytes, job->image_size, job->image_path);
    if (ok)
    {
        add_entry(job->result, ITEM_FILE, job->image_path, NULL, true);
        return;
    }
    log_error(L"保存拖入的图片失败：%ls", job->image_path);
    io_delete_file(job->image_path);
    drop_result_release_caches(job->result);
    job->result->failed = 1;
    job->result->error = wcs_dup(S(STR_ImageSaveFailed));
}

static void work(void *data)
{
    ReadJob *job = data;
    switch (job->kind)
    {
        case READ_FILES: work_files(job); break;
        case READ_VIRTUAL: work_virtual(job); break;
        case READ_IMAGE: work_image(job); break;
        default: break;
    }
    if (job->with_facts) read_facts(job->result);
}

static void start(ReadJob *job)
{
    if (job->kind == READ_IMAGE) task_run_imaging(work, finish_job, job);
    else if (job->kind == READ_DONE && !job->with_facts) task_post(finish_job, job);
    else task_run(work, finish_job, job);
}

// ───────────── 文件 ─────────────

static void read_drop_files(IDataObject *data, StrList *paths)
{
    *paths = (StrList){0};
    FORMATETC request = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {0};
    if (IDataObject_GetData(data, &request, &medium) == S_OK)
    {
        HDROP drop = (HDROP)GlobalLock(medium.hGlobal);
        if (drop)
        {
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
            for (UINT index = 0; index < count; index++)
            {
                UINT length = DragQueryFileW(drop, index, NULL, 0);
                wchar_t *path = xalloc(((size_t)length + 1) * sizeof(wchar_t));
                DragQueryFileW(drop, index, path, length + 1);
                strlist_add_owned(paths, path);
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
}

static int begin_files(ReadJob *job, IDataObject *data)
{
    StrList paths;
    read_drop_files(data, &paths);
    if (paths.count == 0)
    {
        wchar_t *single = drop_read_single_file_name(data);
        if (single) strlist_add_owned(&paths, single);
        else drop_read_local_image_paths(data, &paths);
    }
    DropReadResult *result = job->result;
    for (int index = 0; index < paths.count; index++)
        if (!wcs_is_blank(paths.items[index])) add_entry(result, ITEM_FILE, paths.items[index], NULL, false);
    strlist_clear(&paths);
    if (result->count == 0)
    {
        result->failed = 1;
        result->error = wcs_dup(S(STR_NoFilesRead));
        job->kind = READ_DONE;
        return 0;
    }

    // 是不是文件夹要逐个查磁盘，很慢时界面和来源程序都在等；只有来源程序的临时文件必须当场打开，当场查，其余的到后台再查。
    job->volatile_paths = xcalloc((size_t)result->count, sizeof(bool));
    job->snapshots = xcalloc((size_t)result->count, sizeof(Snapshot));
    for (int index = 0; index < result->count; index++)
    {
        DroppedEntry *entry = &result->entries[index];
        job->volatile_paths[index] = chat_is_volatile_path(entry->value);
        if (job->volatile_paths[index] && path_is_directory(entry->value)) entry->kind = ITEM_FOLDER;
        // 从另一个置物架拖来的临时附件仍是临时附件：读取时就一起持有它所在的缓存。
        wchar_t *cache = attachment_cache_directory_of(entry->value);
        if (cache)
        {
            entry->temporary = true;
            if (strlist_index_of(&result->caches, cache, true) < 0)
            {
                attachment_cache_retain(cache);
                strlist_add(&result->caches, cache);
            }
            free(cache);
        }
        if (entry->kind != ITEM_FILE || !job->volatile_paths[index]) continue;
        // 来源程序放在临时文件夹里的文件会在拖放结束后被删除，必须在松手的回调返回前打开。
        HANDLE file = io_create_file(entry->value, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (file == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file, &size) || size.QuadPart > MAXIMUM_SNAPSHOT_BYTES)
        {
            CloseHandle(file);
            continue;
        }
        Snapshot *snapshot = &job->snapshots[job->snapshot_count++];
        snapshot->index = index;
        snapshot->file = file;
        snapshot->zone = zone_identifier_read(entry->value, &snapshot->zone_length);
    }
    if (job->snapshot_count > 0)
    {
        job->snapshot_cache = attachment_cache_create();
        if (job->snapshot_cache) strlist_add(&result->caches, job->snapshot_cache);
        else
        {
            for (int index = 0; index < job->snapshot_count; index++)
            {
                CloseHandle(job->snapshots[index].file);
                free(job->snapshots[index].zone);
            }
            job->snapshot_count = 0;
        }
    }
    job->kind = READ_FILES;
    return result->count;
}

// ───────────── 虚拟附件 ─────────────

static int begin_virtual(ReadJob *job, IDataObject *data)
{
    DropReadResult *result = job->result;
    job->kind = READ_DONE;
    CLIPFORMAT unicode_format = drop_format(CFSTR_FILEDESCRIPTORW);
    bool unicode = drop_has_format(data, unicode_format);
    size_t size;
    unsigned char *bytes = drop_read_bytes(data, unicode ? unicode_format : drop_format(CFSTR_FILEDESCRIPTORA), &size);
    if (!bytes)
    {
        result->error = wcs_dup(S(STR_AttachmentInfoUnreadable));
        return 0;
    }
    UINT count = size >= sizeof(UINT) ? *(const UINT *)bytes : 0;
    size_t descriptor_size = unicode ? sizeof(FILEDESCRIPTORW) : sizeof(FILEDESCRIPTORA);
    if (count == 0 || count > MAXIMUM_ATTACHMENT_COUNT || size < sizeof(UINT) + descriptor_size * count)
    {
        free(bytes);
        result->error = wcs_dup(S(STR_NoFileAttachments));
        return 0;
    }

    job->cache = attachment_cache_create();
    if (!job->cache)
    {
        free(bytes);
        result->failed = (int)count;
        result->error = wcs_dup(S(STR_AttachmentReceiveFailed));
        return 0;
    }
    job->files = xcalloc(count, sizeof(VirtualFile));
    StrList used = {0};
    CLIPFORMAT contents = drop_format(CFSTR_FILECONTENTS);
    int failed = 0;
    for (UINT index = 0; index < count; index++)
    {
        const unsigned char *pointer = bytes + sizeof(UINT) + index * descriptor_size;
        DWORD flags, attributes;
        FILETIME write_time;
        wchar_t *name;
        if (unicode)
        {
            FILEDESCRIPTORW descriptor;
            memcpy(&descriptor, pointer, sizeof(descriptor));
            flags = descriptor.dwFlags;
            attributes = descriptor.dwFileAttributes;
            write_time = descriptor.ftLastWriteTime;
            name = wcs_dup_n(descriptor.cFileName, wcsnlen(descriptor.cFileName, MAX_PATH));
        }
        else
        {
            FILEDESCRIPTORA descriptor;
            memcpy(&descriptor, pointer, sizeof(descriptor));
            flags = descriptor.dwFlags;
            attributes = descriptor.dwFileAttributes;
            write_time = descriptor.ftLastWriteTime;
            int length = (int)strnlen(descriptor.cFileName, MAX_PATH);
            int wide = length ? MultiByteToWideChar(CP_ACP, 0, descriptor.cFileName, length, NULL, 0) : 0;
            name = xalloc(((size_t)wide + 1) * sizeof(wchar_t));
            if (wide) MultiByteToWideChar(CP_ACP, 0, descriptor.cFileName, length, name, wide);
            name[wide] = 0;
        }
        if ((flags & FD_ATTRIBUTES) && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            free(name);
            continue;
        }
        if (wcs_is_blank(name))
        {
            free(name);
            WStr generated = {0};
            wstr_appendf(&generated, L"attachment-%u", index + 1);
            name = wstr_take(&generated);
        }
        wchar_t *safe = attachment_safe_name(name, &used);
        free(name);

        // 必须在松手的回调里取得介质；部分来源程序不会从后台线程交出附件流。
        FORMATETC request = { contents, NULL, DVASPECT_CONTENT, (LONG)index, TYMED_ISTREAM | TYMED_HGLOBAL | TYMED_FILE };
        STGMEDIUM medium = {0};
        HRESULT hr = IDataObject_GetData(data, &request, &medium);
        if (hr != S_OK)
        {
            failed++;
            log_error(L"来源程序未能交出附件 %ls\r\nHRESULT 0x%08lX", safe, (unsigned long)hr);
            free(safe);
            continue;
        }
        VirtualFile *file = &job->files[job->file_count];
        file->destination = path_join(job->cache, safe);
        free(safe);
        file->medium = medium;
        file->has_write_time = (flags & FD_WRITESTIME) != 0 && (write_time.dwHighDateTime || write_time.dwLowDateTime);
        file->write_time = write_time;
        if (medium.tymed == TYMED_ISTREAM && medium.pstm)
        {
            // 附件流属于界面线程的套间，先封送，后台线程才能按 COM 的规则使用。
            IStream *marshaled = NULL;
            if (FAILED(CoMarshalInterThreadInterfaceInStream(&IID_IStream, (IUnknown *)medium.pstm, &marshaled)))
            {
                failed++;
                ReleaseStgMedium(&file->medium);
                free(file->destination);
                continue;
            }
            file->marshaled = marshaled;
        }
        job->file_count++;
    }
    strlist_clear(&used);
    free(bytes);
    result->failed = failed;
    if (job->file_count == 0)
    {
        attachment_cache_release(job->cache);
        result->error = wcs_dup(S(STR_AttachmentUnavailable));
        return 0;
    }
    strlist_add(&result->caches, job->cache);
    job->kind = READ_VIRTUAL;
    return job->file_count;
}

// ───────────── 图片 ─────────────

/// 把 DIB（位图信息头 + 像素）补上 BMP 文件头。部分程序只提供 CF_DIBV5。
static unsigned char *bmp_from_dib(const unsigned char *dib, size_t size, size_t *bmp_size)
{
    if (size <= 40) return NULL;
    int header_size = *(const int *)dib;
    short bit_count = *(const short *)(dib + 14);
    int compression = *(const int *)(dib + 16);
    int colors_used = *(const int *)(dib + 32);
    int palette = colors_used > 0 ? colors_used : bit_count <= 8 ? 1 << bit_count : 0;
    // BI_BITFIELDS 且只有基本信息头时，三个颜色掩码紧跟在信息头后面。
    int masks = header_size == 40 && compression == 3 ? 12 : 0;
    int offset = 14 + header_size + masks + palette * 4;
    *bmp_size = 14 + size;
    unsigned char *bmp = xalloc(*bmp_size);
    bmp[0] = 'B';
    bmp[1] = 'M';
    unsigned int total = (unsigned int)*bmp_size;
    memcpy(bmp + 2, &total, 4);
    memset(bmp + 6, 0, 4);
    memcpy(bmp + 10, &offset, 4);
    memcpy(bmp + 14, dib, size);
    return bmp;
}

/// 只有 CF_BITMAP 的来源：当场把位图读成 32 位的 DIB。
static unsigned char *dib_from_bitmap(IDataObject *data, size_t *size)
{
    FORMATETC request = { CF_BITMAP, NULL, DVASPECT_CONTENT, -1, TYMED_GDI };
    STGMEDIUM medium = {0};
    if (IDataObject_GetData(data, &request, &medium) != S_OK) return NULL;
    unsigned char *dib = NULL;
    BITMAP info;
    if (medium.hBitmap && GetObjectW(medium.hBitmap, sizeof(info), &info) && info.bmWidth > 0 && info.bmHeight != 0)
    {
        int width = info.bmWidth, height = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
        BITMAPINFOHEADER header = { sizeof(BITMAPINFOHEADER), width, height, 1, 32, BI_RGB };
        *size = sizeof(header) + (size_t)width * height * 4;
        dib = xalloc(*size);
        memcpy(dib, &header, sizeof(header));
        HDC screen = GetDC(NULL);
        if (GetDIBits(screen, medium.hBitmap, 0, (UINT)height, dib + sizeof(header), (BITMAPINFO *)dib, DIB_RGB_COLORS) != height)
        {
            free(dib);
            dib = NULL;
        }
        ReleaseDC(NULL, screen);
    }
    ReleaseStgMedium(&medium);
    return dib;
}

static int begin_image(ReadJob *job, IDataObject *data)
{
    static const wchar_t *const formats[] = { L"PNG", L"image/png", L"JFIF", L"image/jpeg", L"GIF", L"image/gif" };
    static const wchar_t *const extensions[] = { L".png", L".png", L".jpg", L".jpg", L".gif", L".gif" };
    const wchar_t *extension = L".png";
    for (int index = 0; index < 6 && !job->image_bytes; index++)
    {
        CLIPFORMAT format = drop_format(formats[index]);
        if (!drop_has_format(data, format)) continue;
        size_t size;
        unsigned char *bytes = drop_read_bytes(data, format, &size);
        if (!bytes || size == 0)
        {
            free(bytes);
            continue;
        }
        job->image_bytes = bytes;
        job->image_size = size;
        job->image_encoded = true;
        extension = extensions[index];
    }
    // 已有编码好的图片时直接保存它，不再解码用不到的位图。
    if (!job->image_bytes)
    {
        static const CLIPFORMAT dibs[] = { CF_DIBV5, CF_DIB };
        size_t size = 0;
        unsigned char *dib = NULL;
        for (int index = 0; index < 2 && !dib; index++)
            if (drop_has_format(data, dibs[index])) dib = drop_read_bytes(data, dibs[index], &size);
        if (!dib) dib = dib_from_bitmap(data, &size);
        if (dib) job->image_bytes = bmp_from_dib(dib, size, &job->image_size);
        free(dib);
    }
    job->kind = READ_DONE;
    if (!job->image_bytes)
    {
        job->result->failed = 1;
        job->result->error = wcs_dup(S(STR_NoImageRead));
        return 0;
    }
    wchar_t *cache = attachment_cache_create();
    if (!cache)
    {
        job->result->failed = 1;
        job->result->error = wcs_dup(S(STR_ImageSaveFailed));
        return 0;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    WStr name = {0};
    wstr_appendf(&name, L"%ls %04d-%02d-%02d %02d.%02d.%02d%ls", S(STR_DroppedImageName), now.wYear, now.wMonth, now.wDay, now.wHour,
        now.wMinute, now.wSecond, extension);
    job->image_path = path_join(cache, name.data);
    wstr_free(&name);
    strlist_add_owned(&job->result->caches, cache);
    job->kind = READ_IMAGE;
    return 1;
}

// ───────────── 链接和文本 ─────────────

static wchar_t *read_text(IDataObject *data)
{
    wchar_t *text = drop_has_format(data, CF_UNICODETEXT) ? drop_read_unicode(data, CF_UNICODETEXT) : NULL;
    if (!text && drop_has_format(data, CF_TEXT)) text = drop_read_ansi(data, CF_TEXT);
    return text;
}

static void read_link(IDataObject *data, DropReadResult *result)
{
    wchar_t *url = NULL, *title = NULL;
    CLIPFORMAT moz = drop_format(L"text/x-moz-url");
    if (drop_has_format(data, moz))
    {
        wchar_t *text = drop_read_unicode(data, moz);
        if (text)
        {
            wchar_t *newline = wcschr(text, L'\n');
            if (newline)
            {
                *newline = 0;
                wchar_t *rest = wcs_trim_dup(newline + 1);
                if (!wcs_is_blank(rest)) title = rest;
                else free(rest);
            }
            url = wcs_trim_dup(text);
            free(text);
        }
    }
    CLIPFORMAT unicode = drop_format(L"UniformResourceLocatorW"), ansi = drop_format(L"UniformResourceLocator");
    if (!url && drop_has_format(data, unicode)) url = drop_read_unicode(data, unicode);
    if (!url && drop_has_format(data, ansi)) url = drop_read_ansi(data, ansi);
    if (!url)
    {
        wchar_t *text = read_text(data);
        url = text ? wcs_trim_dup(text) : NULL;
        free(text);
    }
    if (!title)
    {
        StrList names;
        drop_descriptor_names(data, &names);
        if (names.count == 1) title = path_file_stem(names.items[0]);
        strlist_clear(&names);
    }
    wchar_t *trimmed = url ? wcs_trim_dup(url) : NULL;
    free(url);
    if (wcs_is_blank(trimmed))
    {
        result->failed = 1;
        result->error = wcs_dup(S(STR_NoLinkRead));
    }
    // 只有网页地址按链接保存；其他协议（本地程序、自定义协议等）双击时会交给系统执行，按文本保存。
    else if (text_is_single_url(trimmed)) add_entry(result, ITEM_LINK, trimmed, title, false);
    else add_entry(result, ITEM_TEXT, trimmed, NULL, false);
    free(trimmed);
    free(title);
}

static void read_plain_text(IDataObject *data, DropReadResult *result)
{
    wchar_t *text = read_text(data);
    if (wcs_is_blank(text))
    {
        result->failed = 1;
        result->error = wcs_dup(S(STR_NoTextToAdd));
    }
    else
    {
        wchar_t *trimmed = wcs_trim_dup(text);
        add_entry(result, text_is_single_url(trimmed) ? ITEM_LINK : ITEM_TEXT, trimmed, NULL, false);
        free(trimmed);
    }
    free(text);
}

// ───────────── 入口 ─────────────

int drop_read(IDataObject *data, bool files_only, bool with_facts, volatile LONG *cancelled_flag, DropReadDone done, void *context)
{
    ReadJob *job = xcalloc(1, sizeof(ReadJob));
    job->with_facts = with_facts;
    job->cancelled = cancelled_flag;
    job->done = done;
    job->context = context;
    job->result = xcalloc(1, sizeof(DropReadResult));
    job->kind = READ_DONE;
    int known = 0;

    DropContentKind kind = drop_classify(data);
    if (files_only && kind == DROP_LINK && drop_has_virtual_files(data)) kind = DROP_VIRTUAL_FILES;
    switch (kind)
    {
        case DROP_FILES:
            known = begin_files(job, data);
            break;
        case DROP_VIRTUAL_FILES:
            known = begin_virtual(job, data);
            break;
        case DROP_IMAGE:
            known = begin_image(job, data);
            break;
        case DROP_LINK:
        case DROP_TEXT:
            if (files_only)
            {
                job->result->failed = 1;
                job->result->error = wcs_dup(S(STR_FilesAndFoldersOnly));
            }
            else if (kind == DROP_LINK) read_link(data, job->result);
            else read_plain_text(data, job->result);
            break;
        default:
            job->result->failed = 1;
            job->result->error = wcs_dup(S(STR_SourceProvidedNothing));
            break;
    }
    if (job->kind == READ_DONE && job->result->count == 0) job->with_facts = false;
    start(job);
    return known;
}

void drop_read_paths(const StrList *paths, DropReadDone done, void *context)
{
    ReadJob *job = xcalloc(1, sizeof(ReadJob));
    job->with_facts = true;
    job->done = done;
    job->context = context;
    job->result = xcalloc(1, sizeof(DropReadResult));
    job->kind = READ_DONE;
    for (int index = 0; index < paths->count; index++)
        if (!wcs_is_blank(paths->items[index])) add_entry(job->result, ITEM_FILE, paths->items[index], NULL, false);
    task_run(work, finish_job, job);
}
