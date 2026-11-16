#include "task.h"
#include "log.h"

#include <objbase.h>
#include <stdlib.h>

#define TASK_WINDOW_CLASS L"DropShelfTaskWindow"
#define MESSAGE_TASK_DONE (WM_APP + 1)

typedef struct Task
{
    TaskWork work;
    TaskDone done;
    void *data;
    struct Task *next;          // 图片处理线程的队列
} Task;

static HWND task_window;
static volatile LONG shutting_down;

static LRESULT CALLBACK task_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == MESSAGE_TASK_DONE)
    {
        Task *task = (Task *)lparam;
        if (task->done && !shutting_down) task->done(task->data);
        free(task);
        return 0;
    }
    if (message == WM_TIMER)
    {
        KillTimer(window, wparam);
        Task *task = (Task *)wparam;
        if (task->done && !shutting_down) task->done(task->data);
        free(task);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool task_initialize(void)
{
    if (task_window) return true;
    WNDCLASSEXW class = { .cbSize = sizeof(class) };
    class.lpfnWndProc = task_window_procedure;
    class.hInstance = GetModuleHandleW(NULL);
    class.lpszClassName = TASK_WINDOW_CLASS;
    if (!RegisterClassExW(&class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    task_window = CreateWindowExW(0, TASK_WINDOW_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, class.hInstance, NULL);
    shutting_down = 0;
    return task_window != NULL;
}

static void finish(Task *task)
{
    // 窗口已经没了（正在退出）就不再回调。
    if (shutting_down || !task_window || !PostMessageW(task_window, MESSAGE_TASK_DONE, 0, (LPARAM)task)) free(task);
}

static DWORD WINAPI run_task(void *parameter)
{
    Task *task = parameter;
    if (task->work && !shutting_down) task->work(task->data);
    finish(task);
    return 0;
}

void task_run(TaskWork work, TaskDone done, void *data)
{
    Task *task = xcalloc(1, sizeof(Task));
    task->work = work;
    task->done = done;
    task->data = data;
    if (!work)
    {
        finish(task);
        return;
    }
    // 线程池里的线程可能已经初始化成多线程套间，也可能还没有初始化；用到 COM 的工作自己负责。
    if (!QueueUserWorkItem(run_task, task, WT_EXECUTELONGFUNCTION))
    {
        // 排不进线程池时就地做完。
        run_task(task);
    }
}

void task_post(TaskDone done, void *data)
{
    task_run(NULL, done, data);
}

void task_delay(unsigned milliseconds, TaskDone done, void *data)
{
    Task *task = xcalloc(1, sizeof(Task));
    task->done = done;
    task->data = data;
    // 计时器的编号就是任务本身，每个任务一个计时器。
    if (!task_window || !SetTimer(task_window, (UINT_PTR)task, milliseconds ? milliseconds : 1, NULL)) free(task);
}

// ───────────── 图片处理线程 ─────────────

/// 一个单线程套间的工作线程和它的队列，按顺序做完排进来的工作。
typedef struct Worker
{
    INIT_ONCE once;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE ready;
    Task *head, *tail;
    HANDLE thread;
    const wchar_t *name;
} Worker;

/// 取缩略图（晚一点出现不影响操作）；压缩、转换图片（用户在等结果，可能要好几秒）另用一个，不和缩略图互相耽误。
static Worker imaging = { INIT_ONCE_STATIC_INIT, {0}, {0}, NULL, NULL, NULL, L"图片处理线程" };
static Worker long_imaging = { INIT_ONCE_STATIC_INIT, {0}, {0}, NULL, NULL, NULL, L"图片转换线程" };

static DWORD WINAPI worker_main(void *parameter)
{
    Worker *worker = parameter;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;)
    {
        EnterCriticalSection(&worker->lock);
        while (!worker->head && !shutting_down) SleepConditionVariableCS(&worker->ready, &worker->lock, INFINITE);
        Task *task = worker->head;
        if (task)
        {
            worker->head = task->next;
            if (!worker->head) worker->tail = NULL;
        }
        LeaveCriticalSection(&worker->lock);
        if (!task) break;
        if (task->work && !shutting_down) task->work(task->data);
        finish(task);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return 0;
}

static BOOL CALLBACK start_worker(INIT_ONCE *once, void *parameter, void **context)
{
    (void)once, (void)context;
    Worker *worker = parameter;
    InitializeCriticalSection(&worker->lock);
    InitializeConditionVariable(&worker->ready);
    worker->thread = CreateThread(NULL, 0, worker_main, worker, 0, NULL);
    if (!worker->thread) log_win32_error(worker->name, GetLastError());
    return TRUE;
}

static void worker_run(Worker *worker, TaskWork work, TaskDone done, void *data)
{
    InitOnceExecuteOnce(&worker->once, start_worker, worker, NULL);
    Task *task = xcalloc(1, sizeof(Task));
    task->work = work;
    task->done = done;
    task->data = data;
    if (!worker->thread)
    {
        run_task(task);
        return;
    }
    EnterCriticalSection(&worker->lock);
    if (worker->tail) worker->tail->next = task;
    else worker->head = task;
    worker->tail = task;
    LeaveCriticalSection(&worker->lock);
    WakeConditionVariable(&worker->ready);
}

void task_run_imaging(TaskWork work, TaskDone done, void *data)
{
    worker_run(&imaging, work, done, data);
}

void task_run_long_imaging(TaskWork work, TaskDone done, void *data)
{
    worker_run(&long_imaging, work, done, data);
}

static void worker_stop(Worker *worker)
{
    if (!worker->thread) return;
    EnterCriticalSection(&worker->lock);
    LeaveCriticalSection(&worker->lock);
    WakeAllConditionVariable(&worker->ready);
    // 正在做的最多再等一会儿，等不到就不管了，进程退出时一起结束。
    WaitForSingleObject(worker->thread, 1000);
    CloseHandle(worker->thread);
    worker->thread = NULL;
}

void task_shutdown(void)
{
    InterlockedExchange(&shutting_down, 1);
    worker_stop(&imaging);
    worker_stop(&long_imaging);
    if (task_window) DestroyWindow(task_window);
    task_window = NULL;
}
