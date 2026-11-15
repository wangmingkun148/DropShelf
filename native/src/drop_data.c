#include "drop_data.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>

/// 虚拟附件最多读这么多个，异常的数据不会让程序分配大量内存。
#define MAXIMUM_ATTACHMENT_COUNT 256
/// 读一个格式最多读这么多字节。
#define MAXIMUM_FORMAT_BYTES (256u * 1024 * 1024)

static const wchar_t *const url_formats[] = { L"UniformResourceLocatorW", L"UniformResourceLocator", L"text/x-moz-url" };
static const wchar_t *const qq_rich_edit_formats[] = { L"QQ_Unicode_RichEdit_Format", L"QQ_RichEdit_Format" };
/// 以压缩数据提供的图片格式，按优先顺序排列。
static const wchar_t *const encoded_image_formats[] = { L"PNG", L"image/png", L"JFIF", L"image/jpeg", L"GIF", L"image/gif" };

#define COUNT_OF(array) ((int)(sizeof(array) / sizeof((array)[0])))

CLIPFORMAT drop_format(const wchar_t *name)
{
    return (CLIPFORMAT)RegisterClipboardFormatW(name);
}

/// 与 WPF 相同：同时接受所有常见的存储方式，只问有没有这个格式。
bool drop_has_format(IDataObject *data, CLIPFORMAT format)
{
    if (!data || !format) return false;
    FORMATETC request = { format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ENHMF | TYMED_MFPICT | TYMED_GDI };
    return IDataObject_QueryGetData(data, &request) == S_OK;
}

static unsigned char *read_global(HGLOBAL global, size_t *size)
{
    SIZE_T length = GlobalSize(global);
    if (length > MAXIMUM_FORMAT_BYTES) return NULL;
    const void *source = GlobalLock(global);
    if (!source) return NULL;
    unsigned char *bytes = xalloc(length + 2);
    memcpy(bytes, source, length);
    bytes[length] = bytes[length + 1] = 0;
    GlobalUnlock(global);
    *size = length;
    return bytes;
}

static unsigned char *read_stream(IStream *stream, size_t *size)
{
    LARGE_INTEGER start = {0};
    // shlwapi.h 里有同名的 IStream_Read 等函数，直接走接口表。
    stream->lpVtbl->Seek(stream, start, STREAM_SEEK_SET, NULL);
    size_t length = 0, capacity = 4096;
    unsigned char *bytes = xalloc(capacity + 2);
    for (;;)
    {
        if (capacity - length < 4096)
        {
            if (capacity >= MAXIMUM_FORMAT_BYTES)
            {
                free(bytes);
                return NULL;
            }
            capacity *= 2;
            bytes = xrealloc(bytes, capacity + 2);
        }
        ULONG read = 0;
        HRESULT result = stream->lpVtbl->Read(stream, bytes + length, (ULONG)(capacity - length), &read);
        length += read;
        if (FAILED(result) || read == 0) break;
    }
    bytes[length] = bytes[length + 1] = 0;
    *size = length;
    return bytes;
}

/// 先按全局内存读，不行再按流读，和 WPF 的顺序相同。末尾多留两个 0，按字符串读时不会越界。
unsigned char *drop_read_bytes(IDataObject *data, CLIPFORMAT format, size_t *size)
{
    *size = 0;
    if (!data || !format) return NULL;
    static const DWORD tymeds[] = { TYMED_HGLOBAL, TYMED_ISTREAM };
    for (int index = 0; index < COUNT_OF(tymeds); index++)
    {
        FORMATETC request = { format, NULL, DVASPECT_CONTENT, -1, tymeds[index] };
        if (IDataObject_QueryGetData(data, &request) != S_OK) continue;
        STGMEDIUM medium = {0};
        if (IDataObject_GetData(data, &request, &medium) != S_OK) continue;
        unsigned char *bytes = NULL;
        if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) bytes = read_global(medium.hGlobal, size);
        else if (medium.tymed == TYMED_ISTREAM && medium.pstm) bytes = read_stream(medium.pstm, size);
        ReleaseStgMedium(&medium);
        if (bytes) return bytes;
    }
    return NULL;
}

/// UTF-16 文本，到第一个 0 为止。
wchar_t *drop_read_unicode(IDataObject *data, CLIPFORMAT format)
{
    size_t size;
    unsigned char *bytes = drop_read_bytes(data, format, &size);
    if (!bytes) return NULL;
    wchar_t *text = wcs_dup_n((const wchar_t *)bytes, wcsnlen((const wchar_t *)bytes, size / sizeof(wchar_t)));
    free(bytes);
    return text;
}

/// 按系统的 ANSI 代码页读出的文本，到第一个 0 为止。
wchar_t *drop_read_ansi(IDataObject *data, CLIPFORMAT format)
{
    size_t size;
    unsigned char *bytes = drop_read_bytes(data, format, &size);
    if (!bytes) return NULL;
    int length = (int)strnlen((const char *)bytes, size);
    int count = length ? MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, length, NULL, 0) : 0;
    wchar_t *text = xalloc(((size_t)count + 1) * sizeof(wchar_t));
    if (count) MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, length, text, count);
    text[count] = 0;
    free(bytes);
    return text;
}

/// UTF-8 文本，到第一个 0 为止（WPF 读 HTML Format 的方式）。
wchar_t *drop_read_utf8(IDataObject *data, CLIPFORMAT format)
{
    size_t size;
    unsigned char *bytes = drop_read_bytes(data, format, &size);
    if (!bytes) return NULL;
    wchar_t *text = wide_from_utf8((const char *)bytes, strnlen((const char *)bytes, size));
    free(bytes);
    return text;
}

wchar_t *drop_source_shelf_id(IDataObject *data)
{
    CLIPFORMAT format = drop_format(SHELF_ID_FORMAT);
    return drop_has_format(data, format) ? drop_read_unicode(data, format) : NULL;
}

bool drop_can_provide_files(DropContentKind kind)
{
    return kind == DROP_FILES || kind == DROP_VIRTUAL_FILES || kind == DROP_IMAGE;
}

static bool file_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/// 只提供 FileNameW 或 FileName（一个路径）的来源；路径不存在时返回 NULL。
wchar_t *drop_read_single_file_name(IDataObject *data)
{
    CLIPFORMAT unicode = drop_format(L"FileNameW"), ansi = drop_format(L"FileName");
    wchar_t *path = drop_has_format(data, unicode) ? drop_read_unicode(data, unicode)
                  : drop_has_format(data, ansi) ? drop_read_ansi(data, ansi)
                  : NULL;
    if (!wcs_is_blank(path) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return path;
    free(path);
    return NULL;
}

bool drop_has_virtual_files(IDataObject *data)
{
    return drop_has_format(data, drop_format(CFSTR_FILEDESCRIPTORW)) || drop_has_format(data, drop_format(CFSTR_FILEDESCRIPTORA));
}

void drop_descriptor_names(IDataObject *data, StrList *names)
{
    *names = (StrList){0};
    CLIPFORMAT unicode_format = drop_format(CFSTR_FILEDESCRIPTORW);
    bool unicode = drop_has_format(data, unicode_format);
    size_t size;
    unsigned char *bytes = drop_read_bytes(data, unicode ? unicode_format : drop_format(CFSTR_FILEDESCRIPTORA), &size);
    if (!bytes) return;
    UINT count = size >= sizeof(UINT) ? *(const UINT *)bytes : 0;
    size_t descriptor_size = unicode ? sizeof(FILEDESCRIPTORW) : sizeof(FILEDESCRIPTORA);
    if (count == 0 || count > MAXIMUM_ATTACHMENT_COUNT || size < sizeof(UINT) + descriptor_size * count)
    {
        free(bytes);
        return;
    }
    for (UINT index = 0; index < count; index++)
    {
        const unsigned char *pointer = bytes + sizeof(UINT) + index * descriptor_size;
        DWORD flags, attributes;
        wchar_t *name;
        if (unicode)
        {
            FILEDESCRIPTORW descriptor;
            memcpy(&descriptor, pointer, sizeof(descriptor));
            flags = descriptor.dwFlags;
            attributes = descriptor.dwFileAttributes;
            name = wcs_dup_n(descriptor.cFileName, wcsnlen(descriptor.cFileName, MAX_PATH));
        }
        else
        {
            FILEDESCRIPTORA descriptor;
            memcpy(&descriptor, pointer, sizeof(descriptor));
            flags = descriptor.dwFlags;
            attributes = descriptor.dwFileAttributes;
            int length = (int)strnlen(descriptor.cFileName, MAX_PATH);
            int wide = length ? MultiByteToWideChar(CP_ACP, 0, descriptor.cFileName, length, NULL, 0) : 0;
            name = xalloc(((size_t)wide + 1) * sizeof(wchar_t));
            if (wide) MultiByteToWideChar(CP_ACP, 0, descriptor.cFileName, length, name, wide);
            name[wide] = 0;
        }
        if ((flags & FD_ATTRIBUTES) && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            free(name);
            continue;
        }
        if (wcs_is_blank(name))
        {
            free(name);
            WStr generated = {0};
            wstr_appendf(&generated, L"attachment-%u", index + 1);
            name = wstr_take(&generated);
        }
        strlist_add_owned(names, name);
    }
    free(bytes);
}

static bool ends_with_ignore_case(const wchar_t *text, const wchar_t *suffix)
{
    size_t length = wcslen(text), suffix_length = wcslen(suffix);
    return length >= suffix_length &&
           CompareStringOrdinal(text + length - suffix_length, (int)suffix_length, suffix, (int)suffix_length, TRUE) == CSTR_EQUAL;
}

static bool has_image_data(IDataObject *data)
{
    for (int index = 0; index < COUNT_OF(encoded_image_formats); index++)
        if (drop_has_format(data, drop_format(encoded_image_formats[index]))) return true;
    return drop_has_format(data, CF_BITMAP) || drop_has_format(data, CF_DIB) || drop_has_format(data, CF_DIBV5);
}

static void keep_existing_files(StrList *paths)
{
    int kept = 0;
    for (int index = 0; index < paths->count; index++)
    {
        if (file_exists(paths->items[index])) paths->items[kept++] = paths->items[index];
        else free(paths->items[index]);
    }
    paths->count = kept;
}

/// 旧版 QQ 的富文本或只含图片的网页片段中，仍然存在的本地图片。
void drop_read_local_image_paths(IDataObject *data, StrList *paths)
{
    *paths = (StrList){0};
    for (int index = 0; index < COUNT_OF(qq_rich_edit_formats); index++)
    {
        CLIPFORMAT format = drop_format(qq_rich_edit_formats[index]);
        if (!drop_has_format(data, format)) continue;
        size_t size;
        unsigned char *bytes = drop_read_bytes(data, format, &size);
        wchar_t *xml = bytes ? chat_decode_rich_edit(bytes, size) : NULL;
        free(bytes);
        chat_parse_qq_rich_edit(xml, paths);
        free(xml);
        keep_existing_files(paths);
        if (paths->count > 0) return;
        strlist_clear(paths);
    }
    CLIPFORMAT html_format = drop_format(L"HTML Format");
    if (!drop_has_format(data, html_format)) return;
    wchar_t *html = drop_read_utf8(data, html_format);
    if (html) chat_parse_html_local_images(html, paths);
    free(html);
    keep_existing_files(paths);
}

DropContentKind drop_classify(IDataObject *data)
{
    if (!data) return DROP_NONE;
    if (drop_has_format(data, CF_HDROP)) return DROP_FILES;
    wchar_t *single = drop_read_single_file_name(data);
    free(single);
    if (single) return DROP_FILES;

    bool has_url = false;
    for (int index = 0; index < COUNT_OF(url_formats) && !has_url; index++)
        has_url = drop_has_format(data, drop_format(url_formats[index]));
    if (drop_has_virtual_files(data))
    {
        if (has_url)
        {
            StrList names;
            drop_descriptor_names(data, &names);
            bool all_links = names.count > 0;
            for (int index = 0; index < names.count && all_links; index++)
                all_links = ends_with_ignore_case(names.items[index], L".url");
            strlist_clear(&names);
            if (all_links) return DROP_LINK;
        }
        return DROP_VIRTUAL_FILES;
    }
    // 聊天软件拖出图片时常同时提供图片地址，有图片数据时优先当作图片。
    if (has_image_data(data)) return DROP_IMAGE;
    StrList images;
    drop_read_local_image_paths(data, &images);
    bool has_images = images.count > 0;
    strlist_clear(&images);
    if (has_images) return DROP_FILES;
    if (has_url) return DROP_LINK;
    if (drop_has_format(data, CF_UNICODETEXT) || drop_has_format(data, CF_TEXT)) return DROP_TEXT;
    return DROP_NONE;
}

static void append_format_name(WStr *names, CLIPFORMAT format)
{
    // 与 WPF 的格式名称相同，便于和 C# 版的日志对照。
    static const wchar_t *const standard[] = {
        NULL, L"Text", L"Bitmap", L"MetaFilePict", L"SymbolicLink", L"DataInterchangeFormat", L"TaggedImageFileFormat",
        L"OEMText", L"DeviceIndependentBitmap", L"Palette", L"PenData", L"RiffAudio", L"WaveAudio", L"UnicodeText",
        L"EnhancedMetafile", L"FileDrop", L"Locale"
    };
    wchar_t name[256];
    if (format < COUNT_OF(standard) && standard[format]) wstr_append(names, standard[format]);
    else if (format >= 0xC000 && GetClipboardFormatNameW(format, name, COUNT_OF(name)) > 0) wstr_append(names, name);
    else wstr_appendf(names, L"Format%u", (unsigned)format);
}

wchar_t *drop_format_names(IDataObject *data, int maximum)
{
    WStr names = {0};
    IEnumFORMATETC *formats = NULL;
    if (data && IDataObject_EnumFormatEtc(data, DATADIR_GET, &formats) == S_OK && formats)
    {
        CLIPFORMAT seen[64];
        int count = 0;
        FORMATETC format;
        while (count < maximum && count < COUNT_OF(seen) && IEnumFORMATETC_Next(formats, 1, &format, NULL) == S_OK)
        {
            if (format.ptd) CoTaskMemFree(format.ptd);
            bool repeated = false;
            for (int index = 0; index < count; index++) repeated |= seen[index] == format.cfFormat;
            if (repeated) continue;
            seen[count++] = format.cfFormat;
            if (names.length) wstr_append(&names, L", ");
            append_format_name(&names, format.cfFormat);
        }
        IEnumFORMATETC_Release(formats);
    }
    return wstr_take(&names);
}

// ───────────── 字符引用 ─────────────

typedef struct HtmlEntity
{
    const wchar_t *name;
    wchar_t value;
} HtmlEntity;

/// 常见的命名字符引用（HTML 4 的拉丁字母表和常用标点）。
static const HtmlEntity html_entities[] = {
    { L"quot", 34 }, { L"amp", 38 }, { L"apos", 39 }, { L"lt", 60 }, { L"gt", 62 },
    { L"nbsp", 160 }, { L"iexcl", 161 }, { L"cent", 162 }, { L"pound", 163 }, { L"curren", 164 }, { L"yen", 165 },
    { L"brvbar", 166 }, { L"sect", 167 }, { L"uml", 168 }, { L"copy", 169 }, { L"ordf", 170 }, { L"laquo", 171 },
    { L"not", 172 }, { L"shy", 173 }, { L"reg", 174 }, { L"macr", 175 }, { L"deg", 176 }, { L"plusmn", 177 },
    { L"sup2", 178 }, { L"sup3", 179 }, { L"acute", 180 }, { L"micro", 181 }, { L"para", 182 }, { L"middot", 183 },
    { L"cedil", 184 }, { L"sup1", 185 }, { L"ordm", 186 }, { L"raquo", 187 }, { L"frac14", 188 }, { L"frac12", 189 },
    { L"frac34", 190 }, { L"iquest", 191 }, { L"Agrave", 192 }, { L"Aacute", 193 }, { L"Acirc", 194 }, { L"Atilde", 195 },
    { L"Auml", 196 }, { L"Aring", 197 }, { L"AElig", 198 }, { L"Ccedil", 199 }, { L"Egrave", 200 }, { L"Eacute", 201 },
    { L"Ecirc", 202 }, { L"Euml", 203 }, { L"Igrave", 204 }, { L"Iacute", 205 }, { L"Icirc", 206 }, { L"Iuml", 207 },
    { L"ETH", 208 }, { L"Ntilde", 209 }, { L"Ograve", 210 }, { L"Oacute", 211 }, { L"Ocirc", 212 }, { L"Otilde", 213 },
    { L"Ouml", 214 }, { L"times", 215 }, { L"Oslash", 216 }, { L"Ugrave", 217 }, { L"Uacute", 218 }, { L"Ucirc", 219 },
    { L"Uuml", 220 }, { L"Yacute", 221 }, { L"THORN", 222 }, { L"szlig", 223 }, { L"agrave", 224 }, { L"aacute", 225 },
    { L"acirc", 226 }, { L"atilde", 227 }, { L"auml", 228 }, { L"aring", 229 }, { L"aelig", 230 }, { L"ccedil", 231 },
    { L"egrave", 232 }, { L"eacute", 233 }, { L"ecirc", 234 }, { L"euml", 235 }, { L"igrave", 236 }, { L"iacute", 237 },
    { L"icirc", 238 }, { L"iuml", 239 }, { L"eth", 240 }, { L"ntilde", 241 }, { L"ograve", 242 }, { L"oacute", 243 },
    { L"ocirc", 244 }, { L"otilde", 245 }, { L"ouml", 246 }, { L"divide", 247 }, { L"oslash", 248 }, { L"ugrave", 249 },
    { L"uacute", 250 }, { L"ucirc", 251 }, { L"uuml", 252 }, { L"yacute", 253 }, { L"thorn", 254 }, { L"yuml", 255 },
    { L"ensp", 8194 }, { L"emsp", 8195 }, { L"thinsp", 8201 }, { L"zwnj", 8204 }, { L"zwj", 8205 }, { L"lrm", 8206 },
    { L"rlm", 8207 }, { L"ndash", 8211 }, { L"mdash", 8212 }, { L"lsquo", 8216 }, { L"rsquo", 8217 }, { L"ldquo", 8220 },
    { L"rdquo", 8221 }, { L"bull", 8226 }, { L"hellip", 8230 }, { L"euro", 8364 }, { L"trade", 8482 },
};

static wchar_t lookup_entity(const wchar_t *name, size_t length)
{
    for (int index = 0; index < COUNT_OF(html_entities); index++)
        if (wcslen(html_entities[index].name) == length && wcsncmp(html_entities[index].name, name, length) == 0)
            return html_entities[index].value;
    return 0;
}

static bool parse_number(const wchar_t *text, size_t length, bool hex, unsigned *value)
{
    if (length == 0) return false;
    unsigned long long result = 0;
    for (size_t index = 0; index < length; index++)
    {
        wchar_t c = text[index];
        unsigned digit;
        if (c >= L'0' && c <= L'9') digit = (unsigned)(c - L'0');
        else if (hex && c >= L'a' && c <= L'f') digit = (unsigned)(c - L'a' + 10);
        else if (hex && c >= L'A' && c <= L'F') digit = (unsigned)(c - L'A' + 10);
        else return false;
        result = result * (hex ? 16 : 10) + digit;
        if (result > 0xFFFFFFFFu) return false;
    }
    *value = (unsigned)result;
    return true;
}

wchar_t *html_decode(const wchar_t *text, size_t length)
{
    WStr output = {0};
    wstr_append_n(&output, L"", 0);
    for (size_t index = 0; index < length; index++)
    {
        wchar_t c = text[index];
        if (c == L'&')
        {
            size_t end = index + 1;
            while (end < length && text[end] != L';' && text[end] != L'&') end++;
            if (end < length && text[end] == L';')
            {
                const wchar_t *entity = text + index + 1;
                size_t entity_length = end - index - 1;
                if (entity_length > 1 && entity[0] == L'#')
                {
                    bool hex = entity[1] == L'x' || entity[1] == L'X';
                    unsigned value;
                    bool parsed = hex ? parse_number(entity + 2, entity_length - 2, true, &value)
                                      : parse_number(entity + 1, entity_length - 1, false, &value);
                    if (parsed) parsed = value < 0xD800 || (value > 0xDFFF && value <= 0x10FFFF);
                    if (parsed)
                    {
                        if (value <= 0xFFFF) wstr_append_char(&output, (wchar_t)value);
                        else
                        {
                            value -= 0x10000;
                            wstr_append_char(&output, (wchar_t)(0xD800 + (value >> 10)));
                            wstr_append_char(&output, (wchar_t)(0xDC00 + (value & 0x3FF)));
                        }
                        index = end;
                        continue;
                    }
                }
                else
                {
                    index = end;
                    wchar_t value = lookup_entity(entity, entity_length);
                    if (value) c = value;
                    else
                    {
                        wstr_append_char(&output, L'&');
                        wstr_append_n(&output, entity, entity_length);
                        wstr_append_char(&output, L';');
                        continue;
                    }
                }
            }
        }
        wstr_append_char(&output, c);
    }
    return wstr_take(&output);
}

// ───────────── 聊天软件 ─────────────

static bool is_word_character(wchar_t c)
{
    return c == L'_' || iswalnum(c);
}

static bool starts_with_ignore_case(const wchar_t *text, const wchar_t *end, const wchar_t *part)
{
    size_t length = wcslen(part);
    return (size_t)(end - text) >= length && CompareStringOrdinal(text, (int)length, part, (int)length, TRUE) == CSTR_EQUAL;
}

/// 在 [from, end) 里找 word（前面不能紧挨着字母、数字或下划线，相当于正则表达式的 \b）。
static const wchar_t *find_word(const wchar_t *text, const wchar_t *from, const wchar_t *end, const wchar_t *word)
{
    for (const wchar_t *p = from; p < end; p++)
        if ((p == text || !is_word_character(p[-1])) && starts_with_ignore_case(p, end, word)) return p;
    return NULL;
}

static const wchar_t *find_text(const wchar_t *from, const wchar_t *end, const wchar_t *part)
{
    for (const wchar_t *p = from; p < end; p++)
        if (starts_with_ignore_case(p, end, part)) return p;
    return NULL;
}

static const wchar_t *find_char(const wchar_t *from, const wchar_t *end, wchar_t c)
{
    for (const wchar_t *p = from; p < end; p++)
        if (*p == c) return p;
    return NULL;
}

static bool range_is_blank(const wchar_t *from, const wchar_t *end)
{
    for (const wchar_t *p = from; p < end; p++)
        if (!wchar_is_white_space(*p)) return false;
    return true;
}

static void add_distinct(StrList *paths, wchar_t *path)
{
    if (strlist_index_of(paths, path, true) < 0) strlist_add_owned(paths, path);
    else free(path);
}

bool chat_is_local_path(const wchar_t *path)
{
    if (wcs_is_blank(path)) return false;
    // 只接受 C:\ 这样带盘符的完整路径；网络共享（\\server）直接拒绝。
    bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
                 (path[2] == L'\\' || path[2] == L'/');
    if (!drive) return false;
    wchar_t root[4] = { path[0], L':', L'\\', 0 };
    UINT type = GetDriveTypeW(root);
    return type != DRIVE_REMOTE && type != DRIVE_NO_ROOT_DIR;
}

// <EditElement ... type="0" ...>（可能带 CDATA）文字</EditElement>：有文字说明拖的不只是图片。
static bool qq_has_text(const wchar_t *xml, const wchar_t *end)
{
    const wchar_t *p = xml;
    while ((p = find_text(p, end, L"<EditElement")))
    {
        const wchar_t *tag = p + 12;
        const wchar_t *tag_end = find_char(tag, end, L'>');
        if (!tag_end) return false;
        if (!find_word(xml, tag, tag_end, L"type=\"0\""))
        {
            p++;
            continue;
        }
        const wchar_t *content = tag_end + 1;
        if (starts_with_ignore_case(content, end, L"<![CDATA[")) content += 9;
        const wchar_t *close = find_text(content, end, L"</EditElement>");
        if (!close)
        {
            p++;
            continue;
        }
        const wchar_t *content_end = close;
        if (content_end - content >= 3 && wcsncmp(content_end - 3, L"]]>", 3) == 0) content_end -= 3;
        if (!range_is_blank(content, content_end)) return true;
        p = close + 14;
    }
    return false;
}

void chat_parse_qq_rich_edit(const wchar_t *xml, StrList *paths)
{
    *paths = (StrList){0};
    if (wcs_is_blank(xml) || !wcs_find_ignore_case(xml, L"<QQRichEditFormat")) return;
    const wchar_t *end = xml + wcslen(xml);
    if (qq_has_text(xml, end)) return;

    // <EditElement ... type="1" ... filepath="路径"：图片。
    const wchar_t *p = xml;
    while ((p = find_text(p, end, L"<EditElement")))
    {
        const wchar_t *tag = p + 12;
        const wchar_t *tag_end = find_char(tag, end, L'>');
        if (!tag_end) tag_end = end;
        const wchar_t *type = find_word(xml, tag, tag_end, L"type=\"1\"");
        const wchar_t *value = NULL, *value_end = NULL;
        // 正则表达式是贪婪的：同一个标签里有几个 filepath 时取最后一个。
        for (const wchar_t *f = type ? find_word(xml, type + 8, tag_end, L"filepath=\"") : NULL; f;
             f = find_word(xml, f + 1, tag_end, L"filepath=\""))
        {
            const wchar_t *start = f + 10;
            const wchar_t *close = find_char(start, end, L'"');
            if (close && close > start)
            {
                value = start;
                value_end = close;
            }
        }
        if (!value)
        {
            p++;
            continue;
        }
        wchar_t *path = html_decode(value, (size_t)(value_end - value));
        if (chat_is_local_path(path)) add_distinct(paths, path);
        else free(path);
        p = value_end + 1;
    }
}

/// 图片地址换成本机路径：file: 地址和 C:\ 这样的路径；其他地址返回 NULL。
static wchar_t *local_path_from_source(const wchar_t *source)
{
    if (_wcsnicmp(source, L"file:", 5) == 0)
    {
        wchar_t path[2048];
        DWORD length = (DWORD)COUNT_OF(path);
        return PathCreateFromUrlW(source, path, &length, 0) == S_OK ? wcs_dup(path) : NULL;
    }
    bool drive = ((source[0] >= L'A' && source[0] <= L'Z') || (source[0] >= L'a' && source[0] <= L'z')) && source[1] == L':' &&
                 (source[2] == L'\\' || source[2] == L'/');
    if (!drive) return NULL;
    wchar_t *path = wcs_dup(source);
    for (wchar_t *c = path; *c; c++)
        if (*c == L'/') *c = L'\\';
    return path;
}

void chat_parse_html_local_images(const wchar_t *html, StrList *paths)
{
    *paths = (StrList){0};
    if (wcs_is_blank(html)) return;
    const wchar_t *start = wcs_find_ignore_case(html, L"<!--StartFragment-->");
    const wchar_t *end_marker = wcs_find_ignore_case(html, L"<!--EndFragment-->");
    const wchar_t *fragment = html, *end = html + wcslen(html);
    if (start && end_marker && end_marker > start)
    {
        fragment = start + 20;
        end = end_marker;
    }

    // 去掉标签后还有文字（字符引用解开后不全是空白）说明不只是图片。
    WStr text = {0};
    wstr_append_n(&text, L"", 0);
    for (const wchar_t *p = fragment; p < end; p++)
    {
        const wchar_t *close = *p == L'<' && p + 1 < end && p[1] != L'>' ? find_char(p + 1, end, L'>') : NULL;
        if (close)
        {
            wstr_append_char(&text, L' ');
            p = close;
        }
        else wstr_append_char(&text, *p);
    }
    wchar_t *decoded = html_decode(text.data, text.length);
    wstr_free(&text);
    bool blank = wcs_is_blank(decoded);
    free(decoded);
    if (!blank) return;

    // <img ... src="地址">；正则表达式是贪婪的：同一个标签里有几个 src 时取最后一个。
    const wchar_t *p = fragment;
    while ((p = find_text(p, end, L"<img")))
    {
        const wchar_t *tag = p + 4;
        if (tag < end && is_word_character(*tag))
        {
            p++;
            continue;
        }
        const wchar_t *tag_end = find_char(tag, end, L'>');
        if (!tag_end) tag_end = end;
        const wchar_t *value = NULL, *value_end = NULL;
        for (const wchar_t *s = tag_end - 1; s >= tag && !value; s--)
        {
            if (!(s == fragment || !is_word_character(s[-1])) || !starts_with_ignore_case(s, end, L"src")) continue;
            const wchar_t *c = s + 3;
            while (c < end && wchar_is_white_space(*c)) c++;
            if (c >= end || *c != L'=') continue;
            c++;
            while (c < end && wchar_is_white_space(*c)) c++;
            if (c >= end || (*c != L'"' && *c != L'\'')) continue;
            const wchar_t *close = find_char(c + 1, end, *c);
            if (!close) continue;
            value = c + 1;
            value_end = close;
        }
        if (!value)
        {
            p++;
            continue;
        }
        wchar_t *source = html_decode(value, (size_t)(value_end - value));
        wchar_t *path = local_path_from_source(source);
        free(source);
        if (path && chat_is_local_path(path)) add_distinct(paths, path);
        else free(path);
        p = value_end + 1;
    }
}

wchar_t *chat_decode_rich_edit(const unsigned char *bytes, size_t size)
{
    if (!bytes || size == 0) return NULL;
    // 末尾的 0 在转换后成了字符串的结尾，相当于 C# 版的 TrimEnd('\0')。
    wchar_t *utf8 = wide_from_utf8((const char *)bytes, size);
    if (wcs_find_ignore_case(utf8, L"<QQRichEditFormat")) return utf8;
    free(utf8);
    size_t count = size / sizeof(wchar_t);
    const wchar_t *wide = (const wchar_t *)bytes;
    while (count > 0 && wide[count - 1] == 0) count--;
    return wcs_dup_n(wide, count);
}
