#define COBJMACROS
#include "shelf_drag.h"
#include "log.h"
#include "theme.h"

#include <math.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>

// ───────────── 数据对象 ─────────────

bool data_set_bytes(IDataObject *data, CLIPFORMAT format, const void *bytes, size_t size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size ? size : 1);
    if (!global) return false;
    void *target = GlobalLock(global);
    if (!target)
    {
        GlobalFree(global);
        return false;
    }
    if (size) memcpy(target, bytes, size);
    GlobalUnlock(global);
    FORMATETC format_etc = { format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = { .tymed = TYMED_HGLOBAL, .hGlobal = global };
    if (SUCCEEDED(IDataObject_SetData(data, &format_etc, &medium, TRUE))) return true;
    GlobalFree(global);
    return false;
}

bool data_set_unicode(IDataObject *data, const wchar_t *format, const wchar_t *text)
{
    return data_set_bytes(data, drop_format(format), text, (wcslen(text) + 1) * sizeof(wchar_t));
}

int data_get_int32(IDataObject *data, const wchar_t *format)
{
    CLIPFORMAT id = drop_format(format);
    FORMATETC request = { id, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    if (IDataObject_QueryGetData(data, &request) != S_OK) return -1;
    STGMEDIUM medium = {0};
    if (IDataObject_GetData(data, &request, &medium) != S_OK) return -1;
    int value = -1;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal && GlobalSize(medium.hGlobal) >= 4)
    {
        const int *pointer = GlobalLock(medium.hGlobal);
        if (pointer)
        {
            value = *pointer;
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return value;
}

static IDataObject *create_empty_data(void)
{
    IDataObject *data = NULL;
    if (FAILED(SHCreateDataObject(NULL, 0, NULL, NULL, &IID_IDataObject, (void **)&data))) return NULL;
    return data;
}

/// 资源管理器同款的数据对象；解析不了的路径（个别文件刚被删）时返回 NULL，由调用方退回文件列表。
static IDataObject *create_shell_data(const StrList *paths)
{
    PIDLIST_ABSOLUTE *pidls = xcalloc((size_t)paths->count, sizeof(PIDLIST_ABSOLUTE));
    int parsed = 0;
    for (int index = 0; index < paths->count; index++)
    {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(SHParseDisplayName(paths->items[index], NULL, &pidl, 0, NULL)) && pidl) pidls[parsed++] = pidl;
    }
    IDataObject *data = NULL;
    IShellItemArray *array = NULL;
    if (parsed == paths->count && parsed > 0 &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)parsed, (PCIDLIST_ABSOLUTE_ARRAY)pidls, &array)) && array)
    {
        IShellItemArray_BindToHandler(array, NULL, &BHID_DataObject, &IID_IDataObject, (void **)&data);
        IShellItemArray_Release(array);
    }
    for (int index = 0; index < parsed; index++) CoTaskMemFree(pidls[index]);
    free(pidls);
    return data;
}

static IDataObject *create_file_data(const StrList *paths)
{
    IDataObject *shell = create_shell_data(paths);
    if (shell) return shell;
    IDataObject *data = create_empty_data();
    if (!data) return NULL;
    WStr list = {0};
    for (int index = 0; index < paths->count; index++)
    {
        wstr_append(&list, paths->items[index]);
        wstr_append_char(&list, 0);
    }
    wstr_append_char(&list, 0);
    size_t list_bytes = list.length * sizeof(wchar_t);
    unsigned char *bytes = xcalloc(1, sizeof(DROPFILES) + list_bytes);
    ((DROPFILES *)bytes)->pFiles = sizeof(DROPFILES);
    ((DROPFILES *)bytes)->fWide = TRUE;
    memcpy(bytes + sizeof(DROPFILES), list.data, list_bytes);
    bool ok = data_set_bytes(data, CF_HDROP, bytes, sizeof(DROPFILES) + list_bytes);
    free(bytes);
    wstr_free(&list);
    if (ok) return data;
    IDataObject_Release(data);
    return NULL;
}

// ───────────── 结果 ─────────────

DWORD drag_allowed_effects(bool move_mode, bool has_temporary_files)
{
    DWORD effects = DROPEFFECT_COPY;
    if (move_mode) effects |= DROPEFFECT_MOVE;
    if (!has_temporary_files) effects |= DROPEFFECT_LINK;
    return effects;
}

DragOutResult drag_resolve(HRESULT result, DWORD effect, int performed, int logical, bool source_missing)
{
    DWORD performed_effect = performed < 0 ? DROPEFFECT_NONE : (DWORD)performed;
    DWORD logical_effect = logical < 0 ? DROPEFFECT_NONE : (DWORD)logical;
    if (result == DRAGDROP_S_CANCEL && !source_missing) return DRAG_OUT_CANCELLED;
    if ((logical_effect & DROPEFFECT_MOVE) || (effect & DROPEFFECT_MOVE)) return DRAG_OUT_MOVED;
    if (performed_effect & DROPEFFECT_MOVE) return DRAG_OUT_MOVED;
    if (effect & DROPEFFECT_COPY) return DRAG_OUT_COPIED;
    if (effect & DROPEFFECT_LINK) return DRAG_OUT_LINKED;
    if (logical_effect & DROPEFFECT_COPY) return DRAG_OUT_COPIED;
    return source_missing ? DRAG_OUT_MOVED : DRAG_OUT_CANCELLED;
}

// ───────────── 拖动图像 ─────────────

#define IMAGE_CANVAS 120.0f
#define IMAGE_CARD 78.0f

static void draw_card(ShelfItem *item, float angle)
{
    bool dark = theme_is_dark();
    float center = IMAGE_CANVAS / 2;
    GfxRect rect = { center - IMAGE_CARD / 2, center - IMAGE_CARD / 2 + 6, center + IMAGE_CARD / 2, center + IMAGE_CARD / 2 + 6 };
    gfx_push_transform(gfx_matrix_rotate(angle, center, center + 6));
    GfxRect shadow = { rect.left, rect.top + 2, rect.right, rect.bottom + 2 };
    gfx_fill_rounded(shadow, 9, COLOR_ARGB(0x22, 0, 0, 0));
    gfx_fill_rounded(rect, 9, dark ? COLOR_RGB(0x3B, 0x3B, 0x3B) : COLOR_WHITE);
    // WPF 的笔画在边缘两侧各一半：往外扩半个线宽再按“画在里面”画。
    GfxRect outline = { rect.left - 0.5f, rect.top - 0.5f, rect.right + 0.5f, rect.bottom + 0.5f };
    gfx_stroke_rounded(outline, 9.5f, dark ? COLOR_ARGB(0x40, 255, 255, 255) : COLOR_ARGB(0x26, 0, 0, 0), 1);
    GfxRect inner = { rect.left + 8, rect.top + 8, rect.right - 8, rect.bottom - 8 };
    if (item->thumbnail)
    {
        float width = (float)gfx_image_width(item->thumbnail), height = (float)gfx_image_height(item->thumbnail);
        float ratio = fminf((inner.right - inner.left) / width, (inner.bottom - inner.top) / height);
        float w = width * ratio, h = height * ratio;
        float x = inner.left + (inner.right - inner.left - w) / 2, y = inner.top + (inner.bottom - inner.top - h) / 2;
        gfx_draw_image(item->thumbnail, (GfxRect){ x, y, x + w, y + h }, 1);
    }
    else
    {
        GfxTextStyle style = { GFX_FONT_ICON, 34, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
        gfx_text_draw(shelf_item_glyph(item), inner, &style, dark ? COLOR_RGB(0xDD, 0xDD, 0xDD) : COLOR_RGB(0x5A, 0x5A, 0x5A));
    }
    gfx_pop_transform();
}

static void draw_badge(int count)
{
    wchar_t text[16];
    if (count > 99) wcscpy_s(text, 16, L"99+");
    else swprintf(text, 16, L"%d", count);
    GfxTextStyle style = { GFX_FONT_TEXT, 13, 600, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_CENTER };
    GfxSize size = gfx_text_measure(text, &style, 0);
    float width = fmaxf(24, size.width + 12);
    float left = IMAGE_CANVAS / 2 + IMAGE_CARD / 2 - width / 2 - 4;
    GfxRect rect = { left, 8, left + width, 32 };
    gfx_fill_rounded(rect, 12, theme_accent().dark1);
    GfxRect outline = { rect.left - 1, rect.top - 1, rect.right + 1, rect.bottom + 1 };
    gfx_stroke_rounded(outline, 13, COLOR_WHITE, 2);
    gfx_text_draw(text, rect, &style, COLOR_WHITE);
}

/// 拖动时跟着光标的缩略图堆叠和数量角标。
static bool render_drag_image(ShelfItem **items, int count, float scale, SHDRAGIMAGE *image)
{
    int pixels = (int)lroundf(IMAGE_CANVAS * scale);
    if (!gfx_begin_image(pixels, pixels, 96.0f * scale)) return false;
    gfx_clear(COLOR_TRANSPARENT);
    gfx_push_opacity(0.94f);
    int shown = count < 3 ? count : 3;
    static const float one[] = { 0 }, two[] = { -7, 0 }, three[] = { -9, 6, 0 };
    const float *angles = shown == 1 ? one : shown == 2 ? two : three;
    for (int index = 0; index < shown; index++) draw_card(items[count - shown + index], angles[index]);
    if (count > 1) draw_badge(count);
    gfx_pop_layer();
    HBITMAP bitmap = gfx_end_image_bitmap();
    if (!bitmap) return false;
    image->sizeDragImage = (SIZE){ pixels, pixels };
    image->ptOffset = (POINT){ pixels / 2, (int)(pixels * 0.62) };
    image->hbmpDragImage = bitmap;
    image->crColorKey = 0xFFFFFFFF;
    return true;
}

static void set_drag_image(IDataObject *data, ShelfItem **items, int count, float scale)
{
    IDragSourceHelper2 *helper = NULL;
    if (FAILED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDragSourceHelper2, (void **)&helper)) || !helper) return;
    IDragSourceHelper2_SetFlags(helper, DSH_ALLOWDROPDESCRIPTIONTEXT);
    SHDRAGIMAGE image = {0};
    if (render_drag_image(items, count, scale, &image) && FAILED(IDragSourceHelper2_InitializeFromBitmap(helper, &image, data)))
        DeleteObject(image.hbmpDragImage);
    IDragSourceHelper2_Release(helper);
}

// ───────────── 拖放来源（系统的默认来源不可用时） ─────────────

static HRESULT STDMETHODCALLTYPE source_query_interface(IDropSource *self, REFIID id, void **object)
{
    if (IsEqualIID(id, &IID_IUnknown) || IsEqualIID(id, &IID_IDropSource))
    {
        *object = self;
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE source_add_ref(IDropSource *self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE source_release(IDropSource *self) { (void)self; return 1; }

/// 按住的左键松开时放下，按 Esc 时取消。
static HRESULT STDMETHODCALLTYPE source_query_continue(IDropSource *self, BOOL escape, DWORD keys)
{
    (void)self;
    if (escape) return DRAGDROP_S_CANCEL;
    return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
}

static HRESULT STDMETHODCALLTYPE source_give_feedback(IDropSource *self, DWORD effect)
{
    (void)self, (void)effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl source_functions = { source_query_interface, source_add_ref, source_release, source_query_continue, source_give_feedback };
static IDropSource default_source = { &source_functions };

// ───────────── 拖出 ─────────────

DragOutOutcome drag_out(const DragOutRequest *request)
{
    DragOutOutcome outcome = { DRAG_OUT_CANCELLED, false };
    StrList files = {0};
    bool has_temporary = false;
    for (int index = 0; index < request->count; index++)
    {
        ShelfItem *item = request->items[index];
        if (!shelf_item_is_file_system(item)) continue;
        strlist_add(&files, item->value);
        has_temporary |= item->temporary;
    }
    IDataObject *data = NULL;
    DWORD allowed;
    if (files.count > 0)
    {
        if (request->extra_files)
            for (int index = 0; index < request->extra_files->count; index++) strlist_add(&files, request->extra_files->items[index]);
        data = create_file_data(&files);
        allowed = drag_allowed_effects(request->move_mode, has_temporary || (request->extra_files && request->extra_files->count > 0));
        // 告诉资源管理器默认移动还是复制（跨磁盘时也一样）；按住 Ctrl 或 Shift 仍可临时切换。
        DWORD preferred = request->move_mode ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        if (data) data_set_bytes(data, drop_format(CFSTR_PREFERREDDROPEFFECT), &preferred, sizeof(preferred));
    }
    else
    {
        data = create_empty_data();
        WStr text = {0};
        for (int index = 0; index < request->count; index++)
        {
            if (index > 0) wstr_append(&text, L"\r\n");
            wstr_append(&text, request->items[index]->value);
        }
        if (data && text.data)
        {
            data_set_bytes(data, CF_UNICODETEXT, text.data, (text.length + 1) * sizeof(wchar_t));
            if (request->count == 1 && request->items[0]->kind == ITEM_LINK) data_set_unicode(data, L"UniformResourceLocatorW", request->items[0]->value);
        }
        wstr_free(&text);
        allowed = DROPEFFECT_COPY;
    }
    if (!data)
    {
        strlist_clear(&files);
        return outcome;
    }

    data_set_unicode(data, SHELF_ID_FORMAT, request->shelf_id);
    set_drag_image(data, request->items, request->count, request->scale);
    ReleaseCapture();
    DWORD effect = DROPEFFECT_NONE;
    HRESULT result = SHDoDragDrop(request->owner, data, NULL, allowed, &effect);
    if (result == E_INVALIDARG) result = SHDoDragDrop(request->owner, data, &default_source, allowed, &effect);

    int performed = data_get_int32(data, CFSTR_PERFORMEDDROPEFFECT);
    int logical = data_get_int32(data, CFSTR_LOGICALPERFORMEDDROPEFFECT);
    bool source_missing = false;
    for (int index = 0; index < request->count; index++)
    {
        ShelfItem *item = request->items[index];
        if (shelf_item_is_file_system(item) && !shelf_item_refresh_availability(item)) source_missing = true;
    }
    outcome.refused = result == DRAGDROP_S_DROP && effect == DROPEFFECT_NONE && performed <= 0 && logical <= 0 && !source_missing;
    outcome.result = drag_resolve(result, effect, performed, logical, source_missing);
    IDataObject_Release(data);
    strlist_clear(&files);
    return outcome;
}

// ───────────── 拖入时的系统反馈 ─────────────

struct DropTargetHelper
{
    IDropTargetHelper *helper;
    bool entered;
};

DropTargetHelper *drop_helper_create(void)
{
    DropTargetHelper *helper = xcalloc(1, sizeof(DropTargetHelper));
    if (FAILED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDropTargetHelper, (void **)&helper->helper)))
        helper->helper = NULL;
    return helper;
}

void drop_helper_destroy(DropTargetHelper *helper)
{
    if (!helper) return;
    if (helper->helper) IDropTargetHelper_Release(helper->helper);
    free(helper);
}

void drop_helper_enter(DropTargetHelper *helper, HWND window, IDataObject *data, DWORD effect)
{
    if (!helper || !helper->helper) return;
    POINT point;
    GetCursorPos(&point);
    helper->entered = SUCCEEDED(IDropTargetHelper_DragEnter(helper->helper, window, data, &point, effect));
}

void drop_helper_over(DropTargetHelper *helper, DWORD effect)
{
    if (!helper || !helper->helper || !helper->entered) return;
    POINT point;
    GetCursorPos(&point);
    IDropTargetHelper_DragOver(helper->helper, &point, effect);
}

void drop_helper_leave(DropTargetHelper *helper)
{
    if (!helper || !helper->helper || !helper->entered) return;
    helper->entered = false;
    IDropTargetHelper_DragLeave(helper->helper);
}

void drop_helper_drop(DropTargetHelper *helper, IDataObject *data, DWORD effect)
{
    if (!helper || !helper->helper || !helper->entered) return;
    helper->entered = false;
    POINT point;
    GetCursorPos(&point);
    IDropTargetHelper_Drop(helper->helper, data, &point, effect);
}

void drop_set_description(IDataObject *data, DropImageType type, const wchar_t *message)
{
    DROPDESCRIPTION description = {0};
    description.type = (DROPIMAGETYPE)type;
    if (message) wcsncpy_s(description.szMessage, MAX_PATH, message, _TRUNCATE);
    data_set_bytes(data, drop_format(CFSTR_DROPDESCRIPTION), &description, sizeof(description));
}
