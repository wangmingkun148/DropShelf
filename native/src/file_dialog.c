#define COBJMACROS
#include "file_dialog.h"

#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>

/// 把 "名称|模式|名称|模式" 拆成对话框要的一组筛选项。filter 的副本放在 storage 里，用完 free。
static int parse_filter(const wchar_t *filter, COMDLG_FILTERSPEC *specs, int capacity, wchar_t **storage)
{
    *storage = NULL;
    if (!filter || !*filter) return 0;
    *storage = wcs_dup(filter);
    int count = 0;
    wchar_t *context = NULL;
    wchar_t *name = wcstok(*storage, L"|", &context);
    while (name && count < capacity)
    {
        wchar_t *pattern = wcstok(NULL, L"|", &context);
        if (!pattern) break;
        specs[count].pszName = name;
        specs[count].pszSpec = pattern;
        count++;
        name = wcstok(NULL, L"|", &context);
    }
    return count;
}

static void set_folder(IFileDialog *dialog, const wchar_t *directory)
{
    if (!directory || !path_is_directory(directory)) return;
    IShellItem *folder = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(directory, NULL, &IID_IShellItem, (void **)&folder)) && folder)
    {
        IFileDialog_SetFolder(dialog, folder);
        IShellItem_Release(folder);
    }
}

static wchar_t *show(IFileDialog *dialog, HWND owner)
{
    wchar_t *result = NULL;
    if (SUCCEEDED(IFileDialog_Show(dialog, owner)))
    {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileDialog_GetResult(dialog, &item)) && item)
        {
            PWSTR path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) result = wcs_dup(path);
            CoTaskMemFree(path);
            IShellItem_Release(item);
        }
    }
    return result;
}

static wchar_t *open_dialog(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *initial_directory, bool folders)
{
    IFileDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&dialog))) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileDialog_GetOptions(dialog, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    // 选到快捷方式时取它指向的程序（和 WPF 的 DereferenceLinks 一样）。
    options = folders ? options | FOS_PICKFOLDERS : options | FOS_FILEMUSTEXIST;
    IFileDialog_SetOptions(dialog, options);
    if (title) IFileDialog_SetTitle(dialog, title);
    COMDLG_FILTERSPEC specs[8];
    wchar_t *storage = NULL;
    int count = folders ? 0 : parse_filter(filter, specs, 8, &storage);
    if (count > 0) IFileDialog_SetFileTypes(dialog, (UINT)count, specs);
    set_folder(dialog, initial_directory);
    wchar_t *result = show(dialog, owner);
    IFileDialog_Release(dialog);
    free(storage);
    return result;
}

wchar_t *file_dialog_open(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *initial_directory)
{
    return open_dialog(owner, title, filter, initial_directory, false);
}

wchar_t *file_dialog_folder(HWND owner, const wchar_t *title, const wchar_t *initial_directory)
{
    return open_dialog(owner, title, NULL, initial_directory, true);
}

wchar_t *file_dialog_save(HWND owner, const wchar_t *title, const wchar_t *filter, const wchar_t *file_name, const wchar_t *initial_directory,
    const wchar_t *default_extension)
{
    IFileDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&dialog))) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileDialog_GetOptions(dialog, &options);
    IFileDialog_SetOptions(dialog, options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR);
    if (title) IFileDialog_SetTitle(dialog, title);
    COMDLG_FILTERSPEC specs[8];
    wchar_t *storage = NULL;
    int count = parse_filter(filter, specs, 8, &storage);
    if (count > 0) IFileDialog_SetFileTypes(dialog, (UINT)count, specs);
    if (default_extension) IFileDialog_SetDefaultExtension(dialog, default_extension);
    if (file_name) IFileDialog_SetFileName(dialog, file_name);
    set_folder(dialog, initial_directory);
    wchar_t *result = show(dialog, owner);
    IFileDialog_Release(dialog);
    free(storage);
    return result;
}
