#include "startup.h"
#include "log.h"

#include <stdlib.h>

#define RUN_KEY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE_NAME L"DropShelf"

wchar_t *startup_build_command(const wchar_t *executable)
{
    WStr command = {0};
    wstr_appendf(&command, L"\"%ls\" --background", executable);
    return wstr_take(&command);
}

bool startup_set_enabled(bool enabled)
{
    HKEY key;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (status == ERROR_SUCCESS)
    {
        if (enabled)
        {
            wchar_t *executable = executable_path();
            wchar_t *command = startup_build_command(executable);
            status = RegSetValueExW(key, RUN_VALUE_NAME, 0, REG_SZ, (const BYTE *)command, (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
            free(command);
            free(executable);
        }
        else
        {
            status = RegDeleteValueW(key, RUN_VALUE_NAME);
            if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        }
        RegCloseKey(key);
    }
    if (status == ERROR_SUCCESS) return true;
    log_win32_error(L"修改开机启动设置失败", (DWORD)status);
    return false;
}
