// 开机启动：写一份 XDG autostart 的 .desktop 文件，登录后在后台运行，不弹出窗口。
// 和 Windows 版写注册表 Run 项的作用相同。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "startup.h"
#include "log.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#define AUTOSTART_FILENAME "io.github.wangmingkun148.DropShelf.desktop"

wchar_t *startup_build_command(const wchar_t *executable)
{
    // 按 Freedesktop Desktop Entry 规范转义 Exec= 引号内的特殊字符和字段代码 %。
    WStr command = {0};
    wstr_append_char(&command, L'"');
    for (const wchar_t *cursor = executable ? executable : L""; *cursor; cursor++)
    {
        if (*cursor == L'\r' || *cursor == L'\n') continue;
        if (*cursor == L'\\') wstr_append(&command, L"\\\\");
        else if (*cursor == L'"') wstr_append(&command, L"\\\"");
        else if (*cursor == L'$') wstr_append(&command, L"\\$");
        else if (*cursor == L'`') wstr_append(&command, L"\\`");
        else if (*cursor == L'%') wstr_append(&command, L"%%");
        else wstr_append_char(&command, *cursor);
    }
    wstr_append(&command, L"\" --background");
    return wstr_take(&command);
}

static wchar_t *autostart_executable_path(void)
{
    // AppImage 运行时 /proc/self/exe 指向临时的 /tmp/.mount_*，开机自启必须记录外部的 .AppImage 路径。
    const char *appimage = getenv("APPIMAGE");
    if (appimage && *appimage)
    {
        wchar_t *path = wide_from_utf8(appimage, strlen(appimage));
        if (path) return path;
    }
    return executable_path();
}

static wchar_t *autostart_path(void)
{
    const char *root = g_get_user_config_dir();
    wchar_t *wide_root = wide_from_utf8(root, strlen(root));
    if (!wide_root) return NULL;
    wchar_t *folder = path_join(wide_root, L"autostart");
    free(wide_root);
    if (!folder) return NULL;
    wchar_t *file = path_join(folder, L"io.github.wangmingkun148.DropShelf.desktop");
    free(folder);
    return file;
}

bool startup_set_enabled(bool enabled)
{
    wchar_t *path = autostart_path();
    if (!path) return false;
    if (!enabled)
    {
        bool removed = io_delete_file(path);
        free(path);
        if (removed) return true;
        log_error(L"删除开机启动文件失败");
        return false;
    }
    wchar_t *executable = autostart_executable_path();
    wchar_t *command = startup_build_command(executable);
    free(executable);
    WStr entry = {0};
    wstr_appendf(&entry,
        L"[Desktop Entry]\n"
        L"Type=Application\n"
        L"Name=DropShelf\n"
        L"Comment=拖一拖就收好的置物架\n"
        L"Exec=%ls\n"
        L"Icon=io.github.wangmingkun148.DropShelf\n"
        L"Terminal=false\n"
        L"X-GNOME-Autostart-enabled=true\n"
        L"StartupNotify=false\n",
        command);
    free(command);
    wchar_t *folder = path_directory(path);
    bool ready = folder && directory_create(folder);
    free(folder);
    size_t length = 0;
    char *bytes = utf8_from_wide(entry.data, &length);
    wstr_free(&entry);
    bool written = ready && bytes && file_write_atomic(path, bytes, length);
    free(bytes);
    free(path);
    if (!written) log_error(L"写入开机启动文件失败");
    return written;
}