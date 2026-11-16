#include "single_instance.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define COPY_DATA_MARKER 0x44534831
#define ARGUMENT_SEPARATOR L'\n'

static wchar_t *with_suffix(const wchar_t *name)
{
    WStr text = {0};
    wstr_append(&text, name);
    wchar_t *suffix = env_get(L"DROPSHELF_INSTANCE");
    if (suffix)
    {
        wstr_append_char(&text, L'.');
        wstr_append(&text, suffix);
        free(suffix);
    }
    return wstr_take(&text);
}

wchar_t *single_instance_mutex_name(void)
{
    return with_suffix(L"Local\\DropShelf.SingleInstance.v1");
}

wchar_t *single_instance_show_event_name(void)
{
    return with_suffix(L"Local\\DropShelf.ShowMainWindow.v1");
}

wchar_t *single_instance_window_title(void)
{
    return with_suffix(L"DropShelfMessageWindow");
}

void single_instance_path_arguments(const StrList *arguments, StrList *paths)
{
    memset(paths, 0, sizeof(*paths));
    for (int index = 0; index < arguments->count; index++)
    {
        const wchar_t *argument = arguments->items[index];
        if (wcsncmp(argument, L"--", 2) != 0 && path_exists(argument)) strlist_add(paths, argument);
    }
}

bool single_instance_is_shelf_request(const StrList *arguments)
{
    for (int index = 0; index < arguments->count; index++)
        if (_wcsicmp(arguments->items[index], L"--shelf") == 0) return true;
    StrList paths;
    single_instance_path_arguments(arguments, &paths);
    bool has_paths = paths.count > 0;
    strlist_clear(&paths);
    return has_paths;
}

bool single_instance_send_arguments(const wchar_t *title, const StrList *arguments)
{
    HWND window = FindWindowW(NULL, title);
    if (!window) return false;
    WStr text = {0};
    for (int index = 0; index < arguments->count; index++)
    {
        if (index > 0) wstr_append_char(&text, ARGUMENT_SEPARATOR);
        wstr_append(&text, arguments->items[index]);
    }
    wchar_t *joined = wstr_take(&text);
    COPYDATASTRUCT data = { COPY_DATA_MARKER, (DWORD)((wcslen(joined) + 1) * sizeof(wchar_t)), joined };
    DWORD_PTR result = 0;
    bool delivered = SendMessageTimeoutW(window, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 3000, &result) != 0;
    free(joined);
    return delivered;
}

bool single_instance_read_arguments(const COPYDATASTRUCT *data, StrList *arguments)
{
    memset(arguments, 0, sizeof(*arguments));
    if (!data || data->dwData != COPY_DATA_MARKER || !data->lpData || data->cbData < sizeof(wchar_t)) return false;
    const wchar_t *text = data->lpData;
    size_t length = data->cbData / sizeof(wchar_t);
    while (length > 0 && text[length - 1] == 0) length--;
    size_t start = 0;
    for (size_t index = 0; index <= length; index++)
    {
        if (index < length && text[index] != ARGUMENT_SEPARATOR) continue;
        if (index > start) strlist_add_owned(arguments, wcs_dup_n(text + start, index - start));
        start = index + 1;
    }
    return true;
}

static void signal_existing_instance(void)
{
    wchar_t *name = single_instance_show_event_name();
    // 另一个实例可能刚启动、还没来得及创建事件，稍等几次。
    for (int attempt = 0; attempt < 5; attempt++)
    {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        if (event)
        {
            SetEvent(event);
            CloseHandle(event);
            break;
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            log_win32_error(L"无法唤醒已经运行的 DropShelf", GetLastError());
            break;
        }
        Sleep(80);
    }
    free(name);
}

void single_instance_hand_over(const StrList *arguments)
{
    // 先允许现有实例切到前台，否则它的窗口可能打开在其他窗口后面，看起来像没有反应。
    AllowSetForegroundWindow(ASFW_ANY);
    wchar_t *title = single_instance_window_title();
    bool forwarded = single_instance_is_shelf_request(arguments) && single_instance_send_arguments(title, arguments);
    free(title);
    if (!forwarded) signal_existing_instance();
}
