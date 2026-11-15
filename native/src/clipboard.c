#include "clipboard.h"
#include "log.h"

#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

static HGLOBAL global_from(const void *bytes, size_t size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size ? size : 1);
    if (!global) return NULL;
    void *target = GlobalLock(global);
    if (!target)
    {
        GlobalFree(global);
        return NULL;
    }
    memcpy(target, bytes, size);
    GlobalUnlock(global);
    return global;
}

static bool open_clipboard(void)
{
    for (int attempt = 0; attempt < 5; attempt++)
    {
        if (OpenClipboard(NULL)) return true;
        if (attempt < 4) Sleep(50);
    }
    return false;
}

static bool put(UINT format, const void *bytes, size_t size)
{
    HGLOBAL global = global_from(bytes, size);
    if (!global) return false;
    if (SetClipboardData(format, global)) return true;
    GlobalFree(global);
    return false;
}

bool clipboard_set_text(const wchar_t *text)
{
    if (!open_clipboard())
    {
        log_win32_error(L"写入剪贴板失败", GetLastError());
        return false;
    }
    EmptyClipboard();
    bool ok = put(CF_UNICODETEXT, text, (wcslen(text) + 1) * sizeof(wchar_t));
    CloseClipboard();
    return ok;
}

bool clipboard_set_files(const StrList *paths)
{
    // DROPFILES：20 字节的头，后面是以两个 0 结尾的 UTF-16 路径列表。
    WStr list = {0};
    WStr text = {0};
    for (int index = 0; index < paths->count; index++)
    {
        wstr_append(&list, paths->items[index]);
        wstr_append_char(&list, 0);
        if (index > 0) wstr_append(&text, L"\r\n");
        wstr_append(&text, paths->items[index]);
    }
    wstr_append_char(&list, 0);
    size_t list_bytes = list.length * sizeof(wchar_t);
    unsigned char *drop = xcalloc(1, sizeof(DROPFILES) + list_bytes);
    DROPFILES *header = (DROPFILES *)drop;
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    memcpy(drop + sizeof(DROPFILES), list.data, list_bytes);
    DWORD copy = DROPEFFECT_COPY;

    bool ok = false;
    if (open_clipboard())
    {
        EmptyClipboard();
        ok = put(CF_HDROP, drop, sizeof(DROPFILES) + list_bytes);
        ok = ok && put(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), &copy, sizeof(copy));
        if (ok && text.data) put(CF_UNICODETEXT, text.data, (text.length + 1) * sizeof(wchar_t));
        CloseClipboard();
    }
    else log_win32_error(L"写入剪贴板失败", GetLastError());
    free(drop);
    wstr_free(&list);
    wstr_free(&text);
    return ok;
}

IDataObject *clipboard_get_data(void)
{
    for (int attempt = 0; attempt < 4; attempt++)
    {
        IDataObject *data = NULL;
        HRESULT result = OleGetClipboard(&data);
        if (SUCCEEDED(result) && data) return data;
        if (result != CLIPBRD_E_CANT_OPEN)
        {
            log_error(L"读取剪贴板失败\r\nHRESULT 0x%08lX", (unsigned long)result);
            return NULL;
        }
        Sleep(40);
    }
    return NULL;
}
