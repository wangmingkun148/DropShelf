// 程序入口。先判断是不是已经在运行：“发送到”菜单、拖到程序图标上或重复启动时打开的第二个进程，
// 把文件转交给已经运行的实例（或唤醒它的主窗口）后直接退出。
#include "app.h"
#include "log.h"
#include "single_instance.h"

#include <shellapi.h>
#include <stdlib.h>

/// 命令行参数，不含程序自己的路径。
static void read_command_line(StrList *arguments)
{
    int count = 0;
    wchar_t **values = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int index = 1; values && index < count; index++) strlist_add(arguments, values[index]);
    LocalFree(values);
}

static LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS *exception)
{
    static LONG reported;
    if (InterlockedExchange(&reported, 1) == 0 && exception && exception->ExceptionRecord)
        log_error(L"未处理的程序异常\r\n异常代码 0x%08lX，地址 %p", exception->ExceptionRecord->ExceptionCode,
            exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show)
{
    (void)previous;
    (void)command_line;
    (void)show;
    StrList arguments = {0};
    read_command_line(&arguments);

    wchar_t *mutex_name = single_instance_mutex_name();
    HANDLE mutex = CreateMutexW(NULL, TRUE, mutex_name);
    bool already_running = !mutex || GetLastError() == ERROR_ALREADY_EXISTS;
    free(mutex_name);
    if (already_running)
    {
        single_instance_hand_over(&arguments);
        if (mutex) CloseHandle(mutex);
        strlist_clear(&arguments);
        return 0;
    }

    SetUnhandledExceptionFilter(on_unhandled_exception);

    bool shelf_request = single_instance_is_shelf_request(&arguments);
    bool start_in_background = shelf_request;
    for (int index = 0; index < arguments.count; index++)
        if (_wcsicmp(arguments.items[index], L"--background") == 0) start_in_background = true;

    int exit_code = 1;
    if (app_start(instance, start_in_background))
    {
        if (shelf_request) app_handle_arguments(&arguments);
        exit_code = app_run();
    }
    app_prepare_for_shutdown();
    strlist_clear(&arguments);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exit_code;
}
