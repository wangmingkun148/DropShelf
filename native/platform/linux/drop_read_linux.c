// drop_read.h 在 Linux 上的路径部分：命令行、恢复最近关闭的置物架只给路径，读好文件信息后交给 done。
// 从 GdkDrop 读数据（文件、文字、图片）在拖入接上时另写，GdkDrop 的格式和 COM 的数据对象对不上。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "drop_read.h"
#include "attachment_cache.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>

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
    if (!result) return;
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

// ───────────── 路径 ─────────────

typedef struct PathJob
{
    StrList paths;
    DropReadResult *result;
    DropReadDone done;
    void *context;
} PathJob;

static void read_one(DropReadResult *result, const wchar_t *path)
{
    DroppedEntry entry = {0};
    entry.kind = ITEM_FILE;
    entry.value = wcs_dup(path);
    entry.has_facts = file_facts_read(path, &entry.facts);
    if (entry.has_facts)
    {
        entry.kind = entry.facts.is_folder ? ITEM_FOLDER : ITEM_FILE;
        if (!entry.facts.is_folder) entry.type_name = file_type_name(path);
    }
    else result->failed++;
    result->entries = xrealloc(result->entries, (size_t)(result->count + 1) * sizeof(DroppedEntry));
    result->entries[result->count++] = entry;
}

static void work(void *data)
{
    PathJob *job = data;
    DropReadResult *result = xcalloc(1, sizeof(DropReadResult));
    result->with_facts = true;
    for (int index = 0; index < job->paths.count; index++)
    {
        if (wcs_is_blank(job->paths.items[index])) continue;
        read_one(result, job->paths.items[index]);
    }
    if (result->count == 0) result->error = wcs_dup(L"没有可读取的文件");
    job->result = result;
}

static void finish(void *data)
{
    PathJob *job = data;
    strlist_clear(&job->paths);
    if (job->done) job->done(job->result, job->context);
    free(job);
}

void drop_read_paths(const StrList *paths, DropReadDone done, void *context)
{
    if (!paths || paths->count == 0)
    {
        if (done)
        {
            DropReadResult *empty = xcalloc(1, sizeof(DropReadResult));
            done(empty, context);
        }
        return;
    }
    PathJob *job = xcalloc(1, sizeof(PathJob));
    for (int index = 0; index < paths->count; index++) strlist_add(&job->paths, paths->items[index]);
    job->done = done;
    job->context = context;
    task_run(work, finish, job);
}