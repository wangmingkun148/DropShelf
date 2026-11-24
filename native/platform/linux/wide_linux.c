// MSVC 的宽字符函数在 glibc 上的对应实现。
//
// 共享代码里的格式串是按 MSVC 的宽 printf 写的：%s 是宽字符串、%hs/%S 是窄字符串、%I64d 是 64 位整数。
// glibc 的语义正好相反（%s 是窄字符串、%ls 才是宽字符串），所以这里先把格式串翻译一遍再交给 vswprintf。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

int _wcsicmp(const wchar_t *first, const wchar_t *second)
{
    if (!first || !second) return first == second ? 0 : (first ? 1 : -1);
    for (;;)
    {
        wchar_t a = (wchar_t)towlower((wint_t)*first);
        wchar_t b = (wchar_t)towlower((wint_t)*second);
        if (a != b) return a < b ? -1 : 1;
        if (!*first) return 0;
        first++;
        second++;
    }
}

int _wcsnicmp(const wchar_t *first, const wchar_t *second, size_t count)
{
    if (!first || !second) return first == second ? 0 : (first ? 1 : -1);
    for (size_t index = 0; index < count; index++)
    {
        wchar_t a = (wchar_t)towlower((wint_t)first[index]);
        wchar_t b = (wchar_t)towlower((wint_t)second[index]);
        if (a != b) return a < b ? -1 : 1;
        if (!first[index]) return 0;
    }
    return 0;
}

int CompareStringEx(LPCWSTR locale, DWORD flags, LPCWSTR first, int first_length, LPCWSTR second, int second_length,
    void *reserved1, void *reserved2, LONG_PTR parameter)
{
    (void)locale;
    (void)flags;
    (void)reserved1;
    (void)reserved2;
    (void)parameter;
    if (!first || !second) return 0;
    int result;
    if (first_length < 0 && second_length < 0) result = _wcsicmp(first, second);
    else
    {
        // 只比较给出的长度；-1 表示到结尾。
        size_t left = first_length < 0 ? wcslen(first) : (size_t)first_length;
        size_t right = second_length < 0 ? wcslen(second) : (size_t)second_length;
        size_t length = left < right ? left : right;
        result = _wcsnicmp(first, second, length);
        if (result == 0 && left != right) result = left < right ? -1 : 1;
    }
    return result < 0 ? CSTR_LESS_THAN : result > 0 ? CSTR_GREATER_THAN : CSTR_EQUAL;
}

int CompareStringOrdinal(LPCWCH first, int first_length, LPCWCH second, int second_length, BOOL ignore_case)
{
    if (!first || !second) return 0;
    size_t left = first_length < 0 ? wcslen(first) : (size_t)first_length;
    size_t right = second_length < 0 ? wcslen(second) : (size_t)second_length;
    size_t length = left < right ? left : right;
    for (size_t index = 0; index < length; index++)
    {
        wchar_t a = ignore_case ? (wchar_t)towlower((wint_t)first[index]) : first[index];
        wchar_t b = ignore_case ? (wchar_t)towlower((wint_t)second[index]) : second[index];
        if (a != b) return a < b ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    if (left == right) return CSTR_EQUAL;
    return left < right ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

typedef enum FormatLength
{
    LENGTH_NONE,
    LENGTH_H,
    LENGTH_HH,
    LENGTH_L,
    LENGTH_LL,
    LENGTH_Z,
    LENGTH_J,
    LENGTH_T,
    LENGTH_BIG_L,
    LENGTH_W
} FormatLength;

static void append_length(WStr *out, FormatLength length)
{
    switch (length)
    {
        case LENGTH_H: wstr_append(out, L"h"); break;
        case LENGTH_HH: wstr_append(out, L"hh"); break;
        case LENGTH_L: wstr_append(out, L"l"); break;
        case LENGTH_LL: wstr_append(out, L"ll"); break;
        case LENGTH_Z: wstr_append(out, L"z"); break;
        case LENGTH_J: wstr_append(out, L"j"); break;
        case LENGTH_T: wstr_append(out, L"t"); break;
        case LENGTH_BIG_L: wstr_append(out, L"L"); break;
        case LENGTH_W: break;
        default: break;
    }
}

/// 把一段 MSVC 风格的格式串翻译成 glibc 能按原意解释的格式串。
wchar_t *wide_format_translate(const wchar_t *format)
{
    if (!format) return NULL;
    WStr out = {0};
    const wchar_t *cursor = format;
    while (*cursor)
    {
        if (*cursor != L'%')
        {
            wstr_append_char(&out, *cursor++);
            continue;
        }
        wstr_append_char(&out, *cursor++);
        if (*cursor == L'%')
        {
            wstr_append_char(&out, *cursor++);
            continue;
        }

        // 标志
        while (*cursor == L'-' || *cursor == L'+' || *cursor == L' ' || *cursor == L'#' || *cursor == L'0' || *cursor == L'\'')
            wstr_append_char(&out, *cursor++);

        // 宽度。* 表示宽度由参数给出。
        if (*cursor == L'*') wstr_append_char(&out, *cursor++);
        else while (iswdigit((wint_t)*cursor)) wstr_append_char(&out, *cursor++);

        // 精度
        if (*cursor == L'.')
        {
            wstr_append_char(&out, *cursor++);
            if (*cursor == L'*') wstr_append_char(&out, *cursor++);
            else while (iswdigit((wint_t)*cursor)) wstr_append_char(&out, *cursor++);
        }

        // 长度修饰
        FormatLength length = LENGTH_NONE;
        if (cursor[0] == L'I' && cursor[1] == L'6' && cursor[2] == L'4')
        {
            length = LENGTH_LL;
            cursor += 3;
        }
        else if (cursor[0] == L'I' && cursor[1] == L'3' && cursor[2] == L'2')
        {
            length = LENGTH_NONE;
            cursor += 3;
        }
        else if (*cursor == L'I')
        {
            length = LENGTH_Z;
            cursor++;
        }
        else if (cursor[0] == L'l' && cursor[1] == L'l')
        {
            length = LENGTH_LL;
            cursor += 2;
        }
        else if (*cursor == L'l')
        {
            length = LENGTH_L;
            cursor++;
        }
        else if (cursor[0] == L'h' && cursor[1] == L'h')
        {
            length = LENGTH_HH;
            cursor += 2;
        }
        else if (*cursor == L'h')
        {
            length = LENGTH_H;
            cursor++;
        }
        else if (*cursor == L'z')
        {
            length = LENGTH_Z;
            cursor++;
        }
        else if (*cursor == L'j')
        {
            length = LENGTH_J;
            cursor++;
        }
        else if (*cursor == L't')
        {
            length = LENGTH_T;
            cursor++;
        }
        else if (*cursor == L'L')
        {
            length = LENGTH_BIG_L;
            cursor++;
        }
        else if (*cursor == L'w')
        {
            length = LENGTH_W;
            cursor++;
        }

        wchar_t conversion = *cursor;
        if (!conversion)
        {
            // 格式串在转换符前结束了：原样留下，交给 printf 报错。
            append_length(&out, length);
            break;
        }
        cursor++;

        switch (conversion)
        {
            case L's':
                // MSVC：%s 是宽字符串，%hs 是窄字符串。
                if (length == LENGTH_H || length == LENGTH_HH) append_length(&out, length);
                else wstr_append(&out, L"l");
                wstr_append_char(&out, L's');
                break;
            case L'S':
                // MSVC：%S 在宽 printf 里是窄字符串；%lS 才是宽字符串。
                if (length == LENGTH_L || length == LENGTH_LL) wstr_append(&out, L"l");
                wstr_append_char(&out, L's');
                break;
            case L'c':
                if (length == LENGTH_H || length == LENGTH_HH) append_length(&out, length);
                else wstr_append(&out, L"l");
                wstr_append_char(&out, L'c');
                break;
            case L'C':
                if (length == LENGTH_L || length == LENGTH_LL) wstr_append(&out, L"l");
                wstr_append_char(&out, L'c');
                break;
            default:
                append_length(&out, length);
                wstr_append_char(&out, conversion);
                break;
        }
    }
    return wstr_take(&out);
}

int _vscwprintf(const wchar_t *format, va_list arguments)
{
    wchar_t *translated = wide_format_translate(format);
    const wchar_t *fmt = translated ? translated : format;
    size_t capacity = 256;
    for (;;)
    {
        wchar_t *buffer = xalloc(capacity * sizeof(wchar_t));
        va_list copy;
        va_copy(copy, arguments);
        int written = vswprintf(buffer, capacity, fmt, copy);
        va_end(copy);
        free(buffer);
        if (written >= 0)
        {
            free(translated);
            return written;
        }
        // glibc 在截断时也返回 -1：加大缓冲再试。
        if (capacity >= (size_t)16 * 1024 * 1024)
        {
            free(translated);
            return -1;
        }
        capacity *= 2;
    }
}

int _snwprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...)
{
    wchar_t *translated = wide_format_translate(format);
    const wchar_t *fmt = translated ? translated : format;
    va_list arguments;
    va_start(arguments, format);
    int written = vswprintf(buffer, count, fmt, arguments);
    va_end(arguments);
    free(translated);
    return written;
}
// ───────────── 共享代码用到的其它宽字符函数 ─────────────

static size_t encode_utf8(uint32_t codepoint, char bytes[4])
{
    if (codepoint < 0x80)
    {
        bytes[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800)
    {
        bytes[0] = (char)(0xC0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000)
    {
        bytes[0] = (char)(0xE0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    bytes[0] = (char)(0xF0 | (codepoint >> 18));
    bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWCH wide, int wide_length, LPSTR out, int out_length,
    LPCCH default_char, LPBOOL used_default)
{
    (void)flags;
    (void)default_char;
    if (used_default) *used_default = FALSE;
    if (code_page != CP_UTF8 || !wide) return 0;
    size_t length = wide_length < 0 ? wcslen(wide) : (size_t)wide_length;
    size_t written = 0;
    for (size_t index = 0; index < length; index++)
    {
        uint32_t codepoint = (uint32_t)wide[index];
        if (IS_HIGH_SURROGATE(codepoint) && index + 1 < length)
        {
            uint32_t low = (uint32_t)wide[index + 1];
            if (IS_LOW_SURROGATE(low))
            {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                index++;
            }
        }
        char bytes[4];
        size_t count = encode_utf8(codepoint, bytes);
        if (out && written + count <= (size_t)out_length)
            memcpy(out + written, bytes, count);
        written += count;
    }
    if (!out) return (int)written;
    if (written > (size_t)out_length)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return (int)written;
}

LPWSTR CharLowerW(LPWSTR text)
{
    if (!text) return text;
    for (wchar_t *cursor = text; *cursor; cursor++) *cursor = (wchar_t)towlower((wint_t)*cursor);
    return text;
}

int _wcsupr_s(wchar_t *text, size_t count)
{
    if (!text || count == 0) return 22;     // EINVAL
    for (size_t index = 0; index + 1 < count && text[index]; index++) text[index] = (wchar_t)towupper((wint_t)text[index]);
    return 0;
}
