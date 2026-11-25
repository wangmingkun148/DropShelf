#define COBJMACROS
#include "shortcut_icons.h"
#include "log.h"
#include "task.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <stdlib.h>

typedef struct SyncJob
{
    int icon_index;
    wchar_t exe[MAX_PATH];
} SyncJob;

/// 一个快捷方式指向本程序时，图标改成 icon_index（已经是的不动）。
static void sync_link(const wchar_t *path, const SyncJob *job)
{
    IShellLinkW *link = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return;
    IPersistFile *file = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)) && SUCCEEDED(IPersistFile_Load(file, path, STGM_READWRITE)))
    {
        wchar_t target[MAX_PATH] = L"", icon[MAX_PATH] = L"";
        int index = 0;
        IShellLinkW_GetPath(link, target, MAX_PATH, NULL, SLGP_RAWPATH);
        IShellLinkW_GetIconLocation(link, icon, MAX_PATH, &index);
        // 没设过图标时用的就是 exe 的第一个图标。
        bool same = (icon[0] == 0 && job->icon_index == 0) || (_wcsicmp(icon, job->exe) == 0 && index == job->icon_index);
        if (_wcsicmp(target, job->exe) == 0 && !same && SUCCEEDED(IShellLinkW_SetIconLocation(link, job->exe, job->icon_index)) &&
            SUCCEEDED(IPersistFile_Save(file, path, TRUE)))
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path, NULL);
    }
    if (file) IPersistFile_Release(file);
    IShellLinkW_Release(link);
}

/// folder 里的 .lnk（depth 大于 0 时再往下找一层）。
static void sync_folder(const wchar_t *folder, int depth, const SyncJob *job)
{
    wchar_t pattern[MAX_PATH];
    if (swprintf(pattern, MAX_PATH, L"%ls\\*", folder) < 0) return;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do
    {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        wchar_t path[MAX_PATH];
        if (swprintf(path, MAX_PATH, L"%ls\\%ls", folder, found.cFileName) < 0) continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth > 0) sync_folder(path, depth - 1, job);
            continue;
        }
        size_t length = wcslen(found.cFileName);
        if (length > 4 && _wcsicmp(found.cFileName + length - 4, L".lnk") == 0) sync_link(path, job);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

static void sync_known_folder(REFKNOWNFOLDERID id, const wchar_t *below, int depth, const SyncJob *job)
{
    PWSTR base = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, NULL, &base)))
    {
        wchar_t folder[MAX_PATH];
        if (swprintf(folder, MAX_PATH, below ? L"%ls\\%ls" : L"%ls", base, below) >= 0) sync_folder(folder, depth, job);
    }
    CoTaskMemFree(base);
}

static void sync_work(void *data)
{
    SyncJob *job = data;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // 开始菜单里程序的文件夹、桌面、固定到任务栏的。
    sync_known_folder(&FOLDERID_Programs, NULL, 1, job);
    sync_known_folder(&FOLDERID_Desktop, NULL, 0, job);
    sync_known_folder(&FOLDERID_RoamingAppData, L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", 0, job);
    if (SUCCEEDED(com)) CoUninitialize();
}

static void sync_done(void *data)
{
    free(data);
}

void shortcut_icons_sync(Look look)
{
    SyncJob *job = xcalloc(1, sizeof(SyncJob));
    job->icon_index = look == LOOK_CARDS ? 1 : 0;
    if (!GetModuleFileNameW(NULL, job->exe, MAX_PATH))
    {
        free(job);
        return;
    }
    task_run(sync_work, sync_done, job);
}
