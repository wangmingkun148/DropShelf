// task.h 的 Linux 实现：后台线程用 GLib 的线程池，做完回到默认主循环。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "task.h"

#include <glib.h>
#include <stdlib.h>

typedef struct Task
{
    TaskWork work;
    TaskDone done;
    void *data;
} Task;

static GThreadPool *pool;
static GThreadPool *imaging_pool;
static GThreadPool *long_imaging_pool;
static gboolean shutting_down;

static gboolean deliver(gpointer data)
{
    Task *task = data;
    if (!shutting_down && task->done) task->done(task->data);
    free(task);
    return G_SOURCE_REMOVE;
}

static void run_work(gpointer data, gpointer user_data)
{
    (void)user_data;
    Task *task = data;
    if (task->work) task->work(task->data);
    g_idle_add(deliver, task);
}

static GThreadPool *make_pool(int threads)
{
    GError *error = NULL;
    GThreadPool *created = g_thread_pool_new(run_work, NULL, threads, FALSE, &error);
    if (!created)
    {
        if (error) g_error_free(error);
        return NULL;
    }
    return created;
}

bool task_initialize(void)
{
    if (pool) return true;
    int processors = (int)g_get_num_processors();
    int threads = processors < 2 ? 2 : processors > 4 ? 4 : processors;
    pool = make_pool(threads);
    imaging_pool = make_pool(1);
    long_imaging_pool = make_pool(1);
    return pool != NULL;
}

void task_shutdown(void)
{
    shutting_down = true;
    GThreadPool *pools[] = { pool, imaging_pool, long_imaging_pool };
    pool = imaging_pool = long_imaging_pool = NULL;
    for (size_t index = 0; index < sizeof(pools) / sizeof(pools[0]); index++)
    {
        if (!pools[index]) continue;
        g_thread_pool_free(pools[index], FALSE, TRUE);
    }
}

static void submit(GThreadPool *target, TaskWork work, TaskDone done, void *data)
{
    if (!target)
    {
        // 还没初始化（或者已经退出）：在界面线程上直接做完，至少不会把工作丢掉。
        if (work) work(data);
        if (done) done(data);
        return;
    }
    Task *task = xalloc(sizeof(Task));
    *task = (Task){ work, done, data };
    if (!g_thread_pool_push(target, task, NULL))
    {
        free(task);
        task_post(done, data);
    }
}

void task_run(TaskWork work, TaskDone done, void *data)
{
    submit(pool, work, done, data);
}

typedef struct Posted
{
    TaskDone done;
    void *data;
} Posted;

static gboolean call_posted(gpointer data)
{
    Posted *posted = data;
    if (!shutting_down) posted->done(posted->data);
    free(posted);
    return G_SOURCE_REMOVE;
}

void task_post(TaskDone done, void *data)
{
    if (!done) return;
    Posted *posted = xalloc(sizeof(Posted));
    *posted = (Posted){ done, data };
    g_idle_add(call_posted, posted);
}

void task_delay(unsigned milliseconds, TaskDone done, void *data)
{
    if (!done) return;
    Posted *posted = xalloc(sizeof(Posted));
    *posted = (Posted){ done, data };
    g_timeout_add(milliseconds ? milliseconds : 1, call_posted, posted);
}

void task_run_imaging(TaskWork work, TaskDone done, void *data)
{
    submit(imaging_pool, work, done, data);
}

void task_run_long_imaging(TaskWork work, TaskDone done, void *data)
{
    submit(long_imaging_pool, work, done, data);
}