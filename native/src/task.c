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

static INIT_ONCE imaging_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION imaging_lock;
static CONDITION_VARIABLE imaging_ready;
static Task *imaging_head, *imaging_tail;
static HANDLE imaging_thread;

static DWORD WINAPI imaging_main(void *parameter)
{
    (void)parameter;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;)
    {
        EnterCriticalSection(&imaging_lock);
        while (!imaging_head && !shutting_down) SleepConditionVariableCS(&imaging_ready, &imaging_lock, INFINITE);
        Task *task = imaging_head;
        if (task)
        {
            imaging_head = task->next;
            if (!imaging_head) imaging_tail = NULL;
        }
        LeaveCriticalSection(&imaging_lock);
        if (!task) break;
        if (task->work && !shutting_down) task->work(task->data);
        finish(task);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return 0;
}

static BOOL CALLBACK start_imaging(INIT_ONCE *once, void *parameter, void **context)
{
    (void)once, (void)parameter, (void)context;
    InitializeCriticalSection(&imaging_lock);
    InitializeConditionVariable(&imaging_ready);
    imaging_thread = CreateThread(NULL, 0, imaging_main, NULL, 0, NULL);
    if (!imaging_thread) log_win32_error(L"图片处理线程启动失败", GetLastError());
    return TRUE;
}

void task_run_imaging(TaskWork work, TaskDone done, void *data)
{
    InitOnceExecuteOnce(&imaging_once, start_imaging, NULL, NULL);
    Task *task = xcalloc(1, sizeof(Task));
    task->work = work;
    task->done = done;
    task->data = data;
    if (!imaging_thread)
    {
        run_task(task);
        return;
    }
    EnterCriticalSection(&imaging_lock);
    if (imaging_tail) imaging_tail->next = task;
    else imaging_head = task;
    imaging_tail = task;
    LeaveCriticalSection(&imaging_lock);
    WakeConditionVariable(&imaging_ready);
}

void task_shutdown(void)
{
    InterlockedExchange(&shutting_down, 1);
    if (imaging_thread)
    {
        EnterCriticalSection(&imaging_lock);
        LeaveCriticalSection(&imaging_lock);
        WakeAllConditionVariable(&imaging_ready);
        // 正在取的缩略图最多再等一会儿，等不到就不管了，进程退出时一起结束。
        WaitForSingleObject(imaging_thread, 1000);
        CloseHandle(imaging_thread);
        imaging_thread = NULL;
    }
    if (task_window) DestroyWindow(task_window);
    task_window = NULL;
}
