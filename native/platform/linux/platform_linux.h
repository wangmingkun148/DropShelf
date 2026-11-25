// Linux 侧的平台底座：把共享代码用到的 Win32 类型、常量和函数换成 POSIX 上说得通的东西。
//
// 共享代码（native/src 里的元素树、布局、动画、业务模型和文件逻辑）是按 Win32 的形状写的。
// Linux 后端不重写它们，而是在这里给出同名同形的垫片：句柄、错误码、文件属性、虚拟键码……
// 真正的实现分散在 base_linux.c、wide_linux.c、ui_window_gtk.c 等文件里。
//
// 只用 Wayland / GTK 的接口，不依赖 X11。
#pragma once

#ifdef _WIN32
#error "platform_linux.h 只用于非 Windows 构建"
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>

// 共享代码假设 wchar_t 是 UTF-16 的 16 位单元。Linux 的 wchar_t 是 32 位的 UTF-32，
// 对绝大多数用法没有区别，只有按 UTF-16 代理对拆字的少数地方需要改成按码位处理。
_Static_assert(sizeof(wchar_t) == 4, "Linux 构建要求 32 位的 wchar_t");

typedef int BOOL;
typedef int INT;
typedef unsigned int UINT;
typedef unsigned int DWORD;
typedef int LONG;
typedef unsigned int ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef short SHORT;
typedef unsigned short USHORT;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef char CHAR;
typedef float FLOAT;
typedef double DOUBLE;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t DWORD_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *HINSTANCE;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t WCHAR;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef DWORD *LPDWORD;
typedef BOOL *LPBOOL;
typedef long HRESULT;
typedef long NTSTATUS;
typedef uint32_t COLORREF;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define WINAPI
#define CALLBACK
#define APIENTRY
#define CONST const

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define SUCCEEDED(result) ((HRESULT)(result) >= 0)
#define FAILED(result) ((HRESULT)(result) < 0)

#define MAX_PATH 4096
#define MAX_LONG_PATH 32767

typedef struct POINT
{
    LONG x, y;
} POINT, *LPPOINT;

typedef struct RECT
{
    LONG left, top, right, bottom;
} RECT, *LPRECT;

typedef struct SIZE
{
    LONG cx, cy;
} SIZE, *LPSIZE;

typedef struct FILETIME
{
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct SYSTEMTIME
{
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef union LARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union ULARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct SECURITY_ATTRIBUTES
{
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct GUID
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID, IID, CLSID;

typedef struct WIN32_FILE_ATTRIBUTE_DATA
{
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef struct BY_HANDLE_FILE_INFORMATION
{
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

/// 和 Win32 一样：cFileName 是当前这一项的名字，不含文件夹。
typedef struct WIN32_FIND_DATAW
{
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[1024];
    WCHAR cAlternateFileName[64];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef enum GET_FILEEX_INFO_LEVELS
{
    GetFileExInfoStandard,
    GetFileExMaxInfoLevel
} GET_FILEEX_INFO_LEVELS;

typedef enum FINDEX_INFO_LEVELS
{
    FindExInfoStandard,
    FindExInfoBasic,
    FindExInfoMaxInfoLevel
} FINDEX_INFO_LEVELS;

typedef enum FINDEX_SEARCH_OPS
{
    FindExSearchNameMatch,
    FindExSearchLimitToDirectories,
    FindExSearchLimitToDevices
} FINDEX_SEARCH_OPS;

typedef DWORD(WINAPI *LPPROGRESS_ROUTINE)(LARGE_INTEGER total_size, LARGE_INTEGER total_transferred, LARGE_INTEGER stream_size,
    LARGE_INTEGER stream_transferred, DWORD stream_number, DWORD reason, HANDLE source, HANDLE destination, LPVOID data);

// ───────────── 不透明的系统对象 ─────────────
// 共享代码只把窗口句柄、图标、位图传来传去；真正的对象由各自的 Linux 实现照看。

struct UiWindow;
typedef struct UiWindow *HWND;
typedef void *HICON;
typedef void *HCURSOR;
typedef void *HBITMAP;
typedef void *HBRUSH;
typedef void *HPEN;
typedef void *HFONT;
typedef void *HDC;
typedef void *HMONITOR;
typedef void *HMENU;
typedef void *HGDIOBJ;
typedef void *HKEY;

// COM 只出现在剪贴板和拖放的接口里；Linux 后端用 GdkClipboard / GdkDrop，不实现这些接口。
typedef struct IDataObject IDataObject;
typedef struct IDropTarget IDropTarget;
typedef struct IDropSource IDropSource;
typedef struct IStream IStream;
typedef struct IEnumFORMATETC IEnumFORMATETC;

// ───────────── 错误码 ─────────────

#define ERROR_SUCCESS 0u
#define ERROR_INVALID_FUNCTION 1u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_TOO_MANY_OPEN_FILES 4u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_NOT_SAME_DEVICE 17u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_DISK_FULL 112u
#define ERROR_FILE_EXISTS 80u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_CANCELLED 1223u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_NOT_SUPPORTED 50u

typedef void *HLOCAL;

DWORD GetLastError(void);
void SetLastError(DWORD error);

// ───────────── 文件 ─────────────

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFFu)

#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_HIDDEN 0x00000002u
#define FILE_ATTRIBUTE_SYSTEM 0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020u
#define FILE_ATTRIBUTE_DEVICE 0x00000040u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100u
#define FILE_ATTRIBUTE_SPARSE_FILE 0x00000200u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u
#define FILE_ATTRIBUTE_COMPRESSED 0x00000800u
#define FILE_ATTRIBUTE_OFFLINE 0x00001000u
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000u
#define FILE_ATTRIBUTE_ENCRYPTED 0x00004000u
#define FILE_ATTRIBUTE_INTEGRITY_STREAM 0x00008000u
#define FILE_ATTRIBUTE_VIRTUAL 0x00010000u
#define FILE_ATTRIBUTE_NO_SCRUB_DATA 0x00020000u

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define FILE_READ_DATA 0x0001u
#define FILE_WRITE_DATA 0x0002u
#define FILE_APPEND_DATA 0x0004u
#define FILE_READ_ATTRIBUTES 0x0080u
#define FILE_WRITE_ATTRIBUTES 0x0100u
#define DELETE 0x00010000u
#define SYNCHRONIZE 0x00100000u

#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define FILE_SHARE_DELETE 0x00000004u

#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u

#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u

#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#define MOVEFILE_COPY_ALLOWED 0x00000002u
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004u
#define MOVEFILE_WRITE_THROUGH 0x00000008u

#define COPY_FILE_FAIL_IF_EXISTS 0x00000001u
#define COPY_FILE_RESTARTABLE 0x00000002u
#define COPY_FILE_OPEN_SOURCE_FOR_WRITE 0x00000004u
#define COPY_FILE_ALLOW_DECRYPTED_DESTINATION 0x00000008u
#define COPY_FILE_NO_BUFFERING 0x00001000u

#define FindExInfoBasicFlag 0x00000001u

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *security, DWORD disposition, DWORD flags, HANDLE template_file);
BOOL CloseHandle(HANDLE handle);
BOOL ReadFile(HANDLE handle, LPVOID buffer, DWORD bytes, DWORD *read, void *overlapped);
BOOL WriteFile(HANDLE handle, LPCVOID buffer, DWORD bytes, DWORD *written, void *overlapped);
BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER *size);
BOOL FlushFileBuffers(HANDLE handle);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *information);
BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *data);
HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, void *data, FINDEX_SEARCH_OPS search, void *filter, DWORD flags);
BOOL FindNextFileW(HANDLE find, WIN32_FIND_DATAW *data);
BOOL FindClose(HANDLE find);
BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES *security);
BOOL RemoveDirectoryW(LPCWSTR path);
BOOL DeleteFileW(LPCWSTR path);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
BOOL CopyFileExW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, void *data, BOOL *cancel, DWORD flags);
BOOL GetDiskFreeSpaceExW(LPCWSTR directory, ULARGE_INTEGER *available, ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes);
BOOL GetFileInformationByHandle(HANDLE handle, BY_HANDLE_FILE_INFORMATION *information);
DWORD GetTempPathW(DWORD length, LPWSTR buffer);
DWORD GetFullPathNameW(LPCWSTR path, DWORD length, LPWSTR buffer, LPWSTR *file_part);

// ───────────── 时间 ─────────────

void GetSystemTimeAsFileTime(FILETIME *time);
void GetSystemTimePreciseAsFileTime(FILETIME *time);
BOOL FileTimeToLocalFileTime(const FILETIME *utc, FILETIME *local);
BOOL FileTimeToSystemTime(const FILETIME *file_time, SYSTEMTIME *system_time);
BOOL SystemTimeToFileTime(const SYSTEMTIME *system_time, FILETIME *file_time);

// ───────────── 语言 ─────────────

typedef uint16_t LANGID;
#define MAKELANGID(primary, sub) ((LANGID)(((WORD)(sub) << 10) | (WORD)(primary)))
#define PRIMARYLANGID(id) ((WORD)((id) & 0x3ff))
#define SUBLANGID(id) ((WORD)((id) >> 10))
#define LANG_NEUTRAL 0x00
#define LANG_ENGLISH 0x09
#define LANG_CHINESE 0x04
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define LANG_USER_DEFAULT MAKELANGID(LANG_NEUTRAL, 0x01)

LANGID GetUserDefaultUILanguage(void);

// ───────────── 字符和格式 ─────────────
// 共享代码里用到的 MSVC 名称。Linux 上由 wide_linux.c 实现，语义尽量和 MSVC 一致。

int _wcsicmp(const wchar_t *first, const wchar_t *second);
int _wcsnicmp(const wchar_t *first, const wchar_t *second, size_t count);
int _vscwprintf(const wchar_t *format, va_list arguments);
int _snwprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...);

#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
#define LOCALE_NAME_USER_DEFAULT NULL
#define NORM_IGNORECASE 0x00000001u

/// 语言包按名称排序时只需要不区分大小写的比较，不需要完整的区域规则。
int CompareStringEx(LPCWSTR locale, DWORD flags, LPCWSTR first, int first_length, LPCWSTR second, int second_length,
    void *reserved1, void *reserved2, LONG_PTR parameter);
int CompareStringOrdinal(const wchar_t *first, int first_length, const wchar_t *second, int second_length, BOOL ignore_case);

/// Windows 的宽字符格式串在 glibc 上的翻译：%s 是宽字符串、%hs/%S 是窄字符串、%I64d 是 64 位整数。
/// 返回的格式串由调用方 free。失败时返回 NULL。
wchar_t *wide_format_translate(const wchar_t *format);

// ───────────── 虚拟键码 ─────────────
// 值就是 Windows 的值；后续按 Wayland 的键符号映射回来，快捷键的解析和显示因此可以共用。

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_KANA 0x15
#define VK_HANGEUL 0x15
#define VK_HANGUL 0x15
#define VK_JUNJA 0x17
#define VK_FINAL 0x18
#define VK_HANJA 0x19
#define VK_KANJI 0x19
#define VK_ESCAPE 0x1B
#define VK_CONVERT 0x1C
#define VK_NONCONVERT 0x1D
#define VK_ACCEPT 0x1E
#define VK_MODECHANGE 0x1F
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_EXECUTE 0x2B
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_SLEEP 0x5F
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_BROWSER_BACK 0xA6
#define VK_BROWSER_FORWARD 0xA7
#define VK_BROWSER_REFRESH 0xA8
#define VK_BROWSER_STOP 0xA9
#define VK_BROWSER_SEARCH 0xAA
#define VK_BROWSER_FAVORITES 0xAB
#define VK_BROWSER_HOME 0xAC
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP 0xAF
#define VK_MEDIA_NEXT_TRACK 0xB0
#define VK_MEDIA_PREV_TRACK 0xB1
#define VK_MEDIA_STOP 0xB2
#define VK_MEDIA_PLAY_PAUSE 0xB3
#define VK_LAUNCH_MAIL 0xB4
#define VK_LAUNCH_MEDIA_SELECT 0xB5
#define VK_LAUNCH_APP1 0xB6
#define VK_LAUNCH_APP2 0xB7
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_8 0xDF
#define VK_OEM_102 0xE2
#define VK_PROCESSKEY 0xE5
#define VK_PACKET 0xE7
#define VK_ATTN 0xF6
#define VK_CRSEL 0xF7
#define VK_EXSEL 0xF8
#define VK_EREOF 0xF9
#define VK_PLAY 0xFA
#define VK_ZOOM 0xFB
#define VK_NONAME 0xFC
#define VK_PA1 0xFD
#define VK_OEM_CLEAR 0xFE

#define MOD_ALT 0x0001u
#define MOD_CONTROL 0x0002u
#define MOD_SHIFT 0x0004u
#define MOD_WIN 0x0008u
#define MOD_NOREPEAT 0x4000u

// ───────────── 拖放效果 ─────────────

#define DROPEFFECT_NONE 0u
#define DROPEFFECT_COPY 1u
#define DROPEFFECT_MOVE 2u
#define DROPEFFECT_LINK 4u

// ───────────── 窗口消息 ─────────────

#define WM_NULL 0x0000u
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000Au
#define WM_SETREDRAW 0x000Bu
#define WM_SETTEXT 0x000Cu
#define WM_GETTEXT 0x000Du
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUERYENDSESSION 0x0011u
#define WM_QUIT 0x0012u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETTINGCHANGE 0x001Au
#define WM_DEVMODECHANGE 0x001Bu
#define WM_CANCELMODE 0x001Fu
#define WM_TIMER 0x0113u
#define WM_HOTKEY 0x0312u
#define WM_USER 0x0400u
#define WM_APP 0x8000u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_RBUTTONDBLCLK 0x0206u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MBUTTONDBLCLK 0x0209u
#define WM_MOUSEWHEEL 0x020Au
#define WM_XBUTTONDOWN 0x020Bu
#define WM_XBUTTONUP 0x020Cu
#define WM_MOUSEHWHEEL 0x020Eu
#define WM_MOVING 0x0216u
#define WM_CAPTURECHANGED 0x0215u
#define WM_DPICHANGED 0x02E0u
#define WM_ERASEBKGND 0x0014u

#define MK_LBUTTON 0x0001u
#define MK_RBUTTON 0x0002u
#define MK_SHIFT 0x0004u
#define MK_CONTROL 0x0008u
#define MK_MBUTTON 0x0010u

#define WS_EX_TOPMOST 0x00000008u
#define WS_EX_NOACTIVATE 0x08000000u
#define HWND_TOPMOST ((HWND)0)

#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb) >> 16))
#define CLR_INVALID 0xFFFFFFFFu

#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
#define LOWORD(value) ((WORD)(((DWORD_PTR)(value)) & 0xFFFF))
#define HIWORD(value) ((WORD)((((DWORD_PTR)(value)) >> 16) & 0xFFFF))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))

// ───────────── 显示器和工作区 ─────────────

typedef struct MONITORINFO
{
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

#define MONITOR_DEFAULTTONULL 0u
#define MONITOR_DEFAULTTONEAREST 2u
#define MDT_EFFECTIVE_DPI 0
#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOACTIVATE 0x0010u

/// 返回的句柄是显示器列表里的序号加一；显示器重新排列前一直有效。
HMONITOR MonitorFromPoint(POINT point, DWORD flags);
BOOL GetMonitorInfoW(HMONITOR monitor, MONITORINFO *info);
HRESULT GetDpiForMonitor(HMONITOR monitor, int dpi_type, UINT *dpi_x, UINT *dpi_y);

/// 窗口在屏幕上的位置和大小（物理像素）；没有显示时为 false。
BOOL GetWindowRect(HWND window, RECT *rect);
/// 客户区大小和窗口左上角（物理像素）；我们的窗口没有标题栏，客户区就是整个窗口。
BOOL GetClientRect(HWND window, RECT *rect);
BOOL ClientToScreen(HWND window, POINT *point);
UINT GetDpiForWindow(HWND window);
BOOL SetWindowPos(HWND window, HWND insert_after, int x, int y, int width, int height, UINT flags);
BOOL IsWindowVisible(HWND window);
BOOL IsWindow(HWND window);
BOOL IsIconic(HWND window);
BOOL IsZoomed(HWND window);
BOOL EnableWindow(HWND window, BOOL enable);
BOOL DestroyIcon(HICON icon);
#define MB_OK 0x00000000u
#define MB_ICONWARNING 0x00000030u
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);

/// 最近一次在任意窗口里看到的鼠标位置（屏幕像素）；Wayland 不允许随时查询全局光标，
/// 光标不在自己的窗口上时这里只是最后看到的位置。M2 会用 Hyprland IPC 补上全局位置。
BOOL GetCursorPos(POINT *point);
void ui_pointer_seen_global(int x, int y);

static inline BOOL PtInRect(const RECT *rect, POINT point)
{
    return point.x >= rect->left && point.x < rect->right && point.y >= rect->top && point.y < rect->bottom;
}

// ───────────── 同步 ─────────────
// 共享代码里的 SRWLOCK 只用来保护短小的临界区，读写都换成一回事的互斥量就够了。

typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER

// 只用来“算一次”的惰性初始化（缓存根目录之类）。
typedef struct INIT_ONCE
{
    pthread_mutex_t mutex;
    bool done;
} INIT_ONCE;

typedef INIT_ONCE *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { PTHREAD_MUTEX_INITIALIZER, false }
BOOL InitOnceExecuteOnce(INIT_ONCE *once, BOOL (*function)(PINIT_ONCE once, void *parameter, void **context), void *parameter, void **context);
#define AcquireSRWLockShared(lock) pthread_mutex_lock(lock)
#define ReleaseSRWLockShared(lock) pthread_mutex_unlock(lock)
#define AcquireSRWLockExclusive(lock) pthread_mutex_lock(lock)
#define ReleaseSRWLockExclusive(lock) pthread_mutex_unlock(lock)

// ───────────── 外壳 ─────────────
// Linux 没有 Windows 外壳的文件类型名；shelf_item 会退回到按扩展名的内置名称。

#define SHGFI_TYPENAME 0x000000400u
#define SHGFI_USEFILEATTRIBUTES 0x000000010u

typedef struct SHFILEINFOW
{
    void *hIcon;
    int iIcon;
    DWORD dwAttributes;
    WCHAR szDisplayName[260];
    WCHAR szTypeName[80];
} SHFILEINFOW;

DWORD_PTR SHGetFileInfoW(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags);
LPWSTR CharUpperW(LPWSTR text);

// 用系统的默认程序打开一个路径或网址（对应 Windows 的 ShellExecuteW"open"）。
bool platform_open_external(const wchar_t *target);

// ───────────── 光标 ─────────────
// 共享代码只拿光标和 IDC_* 比较来决定鼠标指针的样子；Linux 后端再把这些字符串映射成 GdkCursor 的名字。
// 用字符串而不是句柄，是因为 Windows 的 IDC_* 本来就是 MAKEINTRESOURCE 出来的假指针，没法直接照搬。

#define IDC_ARROW L"IDC_ARROW"
#define IDC_IBEAM L"IDC_IBEAM"
#define IDC_HAND L"IDC_HAND"
#define IDC_CROSS L"IDC_CROSS"
#define IDC_SIZENS L"IDC_SIZENS"
#define IDC_SIZEWE L"IDC_SIZEWE"
#define IDC_SIZEALL L"IDC_SIZEALL"
#define IDC_WAIT L"IDC_WAIT"
#define IDC_NO L"IDC_NO"

// ───────────── 其它 ─────────────

#define MAXIMUM_WAIT_OBJECTS 64
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u

typedef void *PVOID;

// ───────────── COM 形状的占位 ─────────────
// 共享的拖放代码在 Windows 上使用 IDataObject / IDropTarget；Linux 版用 GdkDrop / GdkDrag，
// 只保留类型名让头文件能编译，真正的实现不会在 Linux 上调用。

typedef WORD CLIPFORMAT;
typedef struct POINTL
{
    LONG x, y;
} POINTL;
typedef const GUID *REFIID;
typedef const wchar_t *LPCWCH;
typedef const char *LPCCH;
#define STDMETHODCALLTYPE
struct IDropTarget
{
    const void *vtable;
};

// Linux 的拖放不走 COM；共享代码里少数清理用的调用变成空操作。
#define IDataObject_Release(object) ((void)(object))
#define IDataObject_AddRef(object) ((void)(object))

// ───────────── 窗口辅助 ─────────────

#define SM_CXDRAG 68
#define SM_CYDRAG 69
#define SM_SWAPBUTTON 23

// Wayland 上没有“点透”的消息：主窗口的收尾动画卡片窗口改用 ui_window_set_click_through。
#define WM_NCHITTEST 0x0084u
#define HTTRANSPARENT (-1)
#define HTCLIENT 1
#define GWL_EXSTYLE (-20)
#define GWLP_HWNDPARENT (-8)
#define WS_EX_TRANSPARENT 0x00000020L
#define SWP_SHOWWINDOW 0x0040u
#define SIZE_MINIMIZED 1
#define IMAGE_ICON 1
#define LR_DEFAULTCOLOR 0x0000u
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002
#define SPI_GETWHEELSCROLLLINES 104u
#define WHEEL_PAGESCROLL ((UINT)-1)

// ───────────── 窗口显示、通知状态和程序图标 ─────────────

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_MAXIMIZE 3
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
BOOL ShowWindow(HWND window, int command);

/// 系统现在适不适合弹通知。Linux 上只把“前台全屏”当作忙；勿扰之类的交给通知服务自己判断。
typedef int QUERY_USER_NOTIFICATION_STATE;
#define QUNS_NOT_PRESENT 1
#define QUNS_BUSY 2
#define QUNS_RUNNING_D3D_FULL_SCREEN 3
#define QUNS_PRESENTATION_MODE 4
#define QUNS_ACCEPTS_NOTIFICATIONS 5
#define QUNS_QUIET_TIME 6
#define QUNS_APP 7
HRESULT SHQueryUserNotificationState(QUERY_USER_NOTIFICATION_STATE *state);

// Linux 上没有嵌在可执行文件里的图标资源；托盘用的是独立的 PNG/SVG，这些调用只为了让共享代码编译。
#define MAKEINTRESOURCEW(value) ((LPCWSTR)(ULONG_PTR)(WORD)(value))
HMODULE GetModuleHandleW(LPCWSTR name);
HICON LoadIconW(HMODULE instance, LPCWSTR name);

int GetSystemMetrics(int index);
short GetKeyState(int key);
short GetAsyncKeyState(int key);
BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
BOOL SystemParametersInfoW(UINT action, UINT param, void *value, UINT flags);
void QueryPerformanceCounter(LARGE_INTEGER *counter);
void QueryPerformanceFrequency(LARGE_INTEGER *frequency);
UINT_PTR SetTimer(HWND window, UINT_PTR id, UINT elapse, void *procedure);
BOOL KillTimer(HWND window, UINT_PTR id);
BOOL ScreenToClient(HWND window, POINT *point);
BOOL SetWindowTextW(HWND window, LPCWSTR text);
BOOL SetForegroundWindow(HWND window);
HWND SetCapture(HWND window);
HWND GetCapture(void);
BOOL ReleaseCapture(void);

// ───────────── UTF-16 / 时间 / 计时 ─────────────

#define CP_UTF8 65001u
#define IS_HIGH_SURROGATE(c) ((c) >= 0xD800 && (c) <= 0xDBFF)
#define IS_LOW_SURROGATE(c) ((c) >= 0xDC00 && (c) <= 0xDFFF)
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002u

int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWCH wide, int wide_length, LPSTR out, int out_length,
    LPCCH default_char, LPBOOL used_default);
LPWSTR CharLowerW(LPWSTR text);
int _wcsupr_s(wchar_t *text, size_t count);
#define sprintf_s(buffer, size, ...) snprintf((buffer), (size), __VA_ARGS__)
#define wcscpy_s(destination, size, source) wcscpy((destination), (source))
LONG InterlockedExchange(volatile LONG *target, LONG value);

static inline LONG InterlockedIncrement(volatile LONG *target)
{
    return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}

static inline LONG InterlockedDecrement(volatile LONG *target)
{
    return __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST);
}

/// Linux 没有嵌在可执行文件里的图标资源：主窗口和“关于”用 .desktop 的图标，这里返回 NULL。
HICON LoadImageW(HMODULE instance, LPCWSTR name, UINT type, int width, int height, UINT flags);

ULONGLONG GetTickCount64(void);
BOOL SystemTimeToTzSpecificLocalTime(const void *zone, const SYSTEMTIME *universal, SYSTEMTIME *local);
BOOL TzSpecificLocalTimeToSystemTime(const void *zone, const SYSTEMTIME *local, SYSTEMTIME *universal);

#define ERROR_INSUFFICIENT_BUFFER 122u