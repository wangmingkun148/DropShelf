#include "file_identity.h"

#include <stdlib.h>
#include <string.h>

#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
#define MAXIMUM_PATH 32767

/// 本地磁盘上的完整路径（C:\…）；网络路径和相对路径不取标识。
static bool is_local_path(const wchar_t *path)
{
    if (!path || wcslen(path) < 3) return false;
    wchar_t letter = path[0];
    bool ascii_letter = (letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z');
    return ascii_letter && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

/// 只读属性（FILE_READ_ATTRIBUTES），不涉及文件内容，别的程序独占打开着的文件也能取到；打开文件夹需要 BACKUP_SEMANTICS。
static HANDLE open_attributes(const wchar_t *path)
{
    return CreateFileW(path, FILE_READ_ATTRIBUTES, SHARE_ALL, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

static bool read_identity(HANDLE handle, FileIdentity *identity)
{
    wchar_t file_system[16] = {0};
    if (!GetVolumeInformationByHandleW(handle, NULL, 0, NULL, NULL, NULL, file_system, ARRAYSIZE(file_system))) return false;
    if (_wcsicmp(file_system, L"NTFS") != 0) return false;
    BY_HANDLE_FILE_INFORMATION information;
    if (!GetFileInformationByHandle(handle, &information)) return false;
    ULONGLONG index = ((ULONGLONG)information.nFileIndexHigh << 32) | information.nFileIndexLow;
    if (index == 0) return false;
    identity->volume_serial = information.dwVolumeSerialNumber;
    identity->file_index = index;
    return true;
}

bool file_identity_get(const wchar_t *path, FileIdentity *identity)
{
    *identity = (FileIdentity){0};
    if (!is_local_path(path)) return false;
    HANDLE handle = open_attributes(path);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = read_identity(handle, identity);
    CloseHandle(handle);
    if (!ok) *identity = (FileIdentity){0};
    return ok;
}

wchar_t *file_identity_resolve(const FileIdentity *identity, const wchar_t *original_path, bool is_folder)
{
    if (!file_identity_valid(identity) || !is_local_path(original_path)) return NULL;
    wchar_t root[4] = { original_path[0], L':', L'\\', 0 };
    HANDLE volume = open_attributes(root);
    if (volume == INVALID_HANDLE_VALUE) return NULL;
    FILE_ID_DESCRIPTOR descriptor = { .dwSize = sizeof(descriptor), .Type = FileIdType };
    descriptor.FileId.QuadPart = (LONGLONG)identity->file_index;
    HANDLE handle = OpenFileById(volume, &descriptor, FILE_READ_ATTRIBUTES, SHARE_ALL, NULL, FILE_FLAG_BACKUP_SEMANTICS);
    CloseHandle(volume);
    if (handle == INVALID_HANDLE_VALUE) return NULL;

    wchar_t *result = NULL;
    FileIdentity found;
    BY_HANDLE_FILE_INFORMATION information;
    if (read_identity(handle, &found) && found.volume_serial == identity->volume_serial && found.file_index == identity->file_index &&
        GetFileInformationByHandle(handle, &information) &&
        ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == is_folder)
    {
        wchar_t *buffer = xalloc(MAXIMUM_PATH * sizeof(wchar_t));
        DWORD length = GetFinalPathNameByHandleW(handle, buffer, MAXIMUM_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length > 0 && length < MAXIMUM_PATH && _wcsnicmp(buffer, L"\\\\?\\UNC\\", 8) != 0)
        {
            const wchar_t *path = wcsncmp(buffer, L"\\\\?\\", 4) == 0 ? buffer + 4 : buffer;
            // 删除到回收站也是在同一个磁盘里移动：这种算删除，不跟过去。
            wchar_t *upper = wcs_dup(path);
            _wcsupr_s(upper, wcslen(upper) + 1);
            if (!wcsstr(upper, L"\\$RECYCLE.BIN\\")) result = wcs_dup(path);
            free(upper);
        }
        free(buffer);
    }
    CloseHandle(handle);
    return result;
}
