#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAXIMUM_DEPTH 64

typedef struct Parser
{
    const wchar_t *cursor;
    unsigned flags;
    int depth;
} Parser;

static JsonValue *parse_value(Parser *parser);

/// 跳过空白和（允许时的）注释。注释没有写完时返回 false。
static bool skip_blank(Parser *parser)
{
    for (;;)
    {
        wchar_t character = *parser->cursor;
        if (character == L' ' || character == L'\t' || character == L'\r' || character == L'\n')
        {
            parser->cursor++;
        }
        else if (character == L'/' && (parser->flags & JSON_ALLOW_COMMENTS) && parser->cursor[1] == L'/')
        {
            while (*parser->cursor && *parser->cursor != L'\n') parser->cursor++;
        }
        else if (character == L'/' && (parser->flags & JSON_ALLOW_COMMENTS) && parser->cursor[1] == L'*')
        {
            const wchar_t *end = wcsstr(parser->cursor + 2, L"*/");
            if (!end) return false;
            parser->cursor = end + 2;
        }
        else
        {
            return true;
        }
    }
}

static int hex_digit(wchar_t character)
{
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    return -1;
}

static wchar_t *parse_string(Parser *parser)
{
    if (*parser->cursor != L'"') return NULL;
    parser->cursor++;
    WStr text = {0};
    for (;;)
    {
        wchar_t character = *parser->cursor++;
        if (character == L'"') return wstr_take(&text);
        if (character < 0x20) break;     // 包括字符串没有写完就到了结尾
        if (character != L'\\')
        {
            wstr_append_char(&text, character);
            continue;
        }
        wchar_t escape = *parser->cursor++;
        switch (escape)
        {
            case L'"': wstr_append_char(&text, L'"'); break;
            case L'\\': wstr_append_char(&text, L'\\'); break;
            case L'/': wstr_append_char(&text, L'/'); break;
            case L'b': wstr_append_char(&text, L'\b'); break;
            case L'f': wstr_append_char(&text, L'\f'); break;
            case L'n': wstr_append_char(&text, L'\n'); break;
            case L'r': wstr_append_char(&text, L'\r'); break;
            case L't': wstr_append_char(&text, L'\t'); break;
            case L'u':
            {
                unsigned code = 0;
                for (int index = 0; index < 4; index++)
                {
                    int digit = hex_digit(parser->cursor[index]);
                    if (digit < 0) goto failed;
                    code = code * 16 + (unsigned)digit;
                }
                parser->cursor += 4;
                wstr_append_char(&text, (wchar_t)code);
                break;
            }
            default: goto failed;
        }
    }
failed:
    wstr_free(&text);
    return NULL;
}

static JsonValue *new_value(JsonType type)
{
    JsonValue *value = xcalloc(1, sizeof(JsonValue));
    value->type = type;
    return value;
}

static void append_item(JsonValue *container, wchar_t *key, JsonValue *item)
{
    // 每满 8 项再多留 8 项的位置。
    if (container->count % 8 == 0)
    {
        int capacity = container->count + 8;
        container->items = xrealloc(container->items, (size_t)capacity * sizeof(JsonValue *));
        if (container->type == JSON_OBJECT) container->keys = xrealloc(container->keys, (size_t)capacity * sizeof(wchar_t *));
    }
    if (container->type == JSON_OBJECT) container->keys[container->count] = key;
    container->items[container->count++] = item;
}

static JsonValue *parse_number(Parser *parser)
{
    const wchar_t *start = parser->cursor;
    const wchar_t *cursor = start;
    bool is_integer = true;
    if (*cursor == L'-') cursor++;
    if (*cursor == L'0') cursor++;
    else if (*cursor >= L'1' && *cursor <= L'9') while (*cursor >= L'0' && *cursor <= L'9') cursor++;
    else return NULL;
    if (*cursor == L'.')
    {
        is_integer = false;
        cursor++;
        if (!(*cursor >= L'0' && *cursor <= L'9')) return NULL;
        while (*cursor >= L'0' && *cursor <= L'9') cursor++;
    }
    if (*cursor == L'e' || *cursor == L'E')
    {
        is_integer = false;
        cursor++;
        if (*cursor == L'+' || *cursor == L'-') cursor++;
        if (!(*cursor >= L'0' && *cursor <= L'9')) return NULL;
        while (*cursor >= L'0' && *cursor <= L'9') cursor++;
    }
    JsonValue *value = new_value(JSON_NUMBER);
    value->number = wcstod(start, NULL);
    value->is_integer = is_integer;
    parser->cursor = cursor;
    return value;
}

static bool match_word(Parser *parser, const wchar_t *word)
{
    size_t length = wcslen(word);
    if (wcsncmp(parser->cursor, word, length) != 0) return false;
    parser->cursor += length;
    return true;
}

static JsonValue *parse_container(Parser *parser, JsonType type)
{
    wchar_t close = type == JSON_OBJECT ? L'}' : L']';
    JsonValue *container = new_value(type);
    parser->cursor++;
    if (++parser->depth > JSON_MAXIMUM_DEPTH) goto failed;
    if (!skip_blank(parser)) goto failed;
    if (*parser->cursor == close)
    {
        parser->cursor++;
        parser->depth--;
        return container;
    }
    for (;;)
    {
        wchar_t *key = NULL;
        if (type == JSON_OBJECT)
        {
            key = parse_string(parser);
            if (!key) goto failed;
            if (!skip_blank(parser) || *parser->cursor != L':')
            {
                free(key);
                goto failed;
            }
            parser->cursor++;
        }
        JsonValue *item = parse_value(parser);
        if (!item)
        {
            free(key);
            goto failed;
        }
        append_item(container, key, item);
        if (!skip_blank(parser)) goto failed;
        if (*parser->cursor == L',')
        {
            parser->cursor++;
            if (!skip_blank(parser)) goto failed;
            if (*parser->cursor != close) continue;
            if (!(parser->flags & JSON_ALLOW_TRAILING_COMMAS)) goto failed;
        }
        if (*parser->cursor != close) goto failed;
        parser->cursor++;
        parser->depth--;
        return container;
    }
failed:
    json_free(container);
    return NULL;
}

static JsonValue *parse_value(Parser *parser)
{
    if (!skip_blank(parser)) return NULL;
    switch (*parser->cursor)
    {
        case L'{': return parse_container(parser, JSON_OBJECT);
        case L'[': return parse_container(parser, JSON_ARRAY);
        case L'"':
        {
            wchar_t *text = parse_string(parser);
            if (!text) return NULL;
            JsonValue *value = new_value(JSON_STRING);
            value->string = text;
            return value;
        }
        case L't':
        case L'f':
        {
            bool is_true = *parser->cursor == L't';
            if (!match_word(parser, is_true ? L"true" : L"false")) return NULL;
            JsonValue *value = new_value(JSON_BOOL);
            value->boolean = is_true;
            return value;
        }
        case L'n': return match_word(parser, L"null") ? new_value(JSON_NULL) : NULL;
        default: return parse_number(parser);
    }
}

JsonValue *json_parse(const wchar_t *text, unsigned flags)
{
    if (!text) return NULL;
    if (*text == 0xFEFF) text++;
    Parser parser = { text, flags, 0 };
    JsonValue *value = parse_value(&parser);
    if (!value) return NULL;
    if (!skip_blank(&parser) || *parser.cursor != 0)
    {
        json_free(value);
        return NULL;
    }
    return value;
}

void json_free(JsonValue *value)
{
    if (!value) return;
    for (int index = 0; index < value->count; index++)
    {
        json_free(value->items[index]);
        if (value->keys) free(value->keys[index]);
    }
    free(value->items);
    free(value->keys);
    free(value->string);
    free(value);
}

const JsonValue *json_get(const JsonValue *object, const wchar_t *key)
{
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (int index = object->count - 1; index >= 0; index--)
        if (wcscmp(object->keys[index], key) == 0) return object->items[index];
    return NULL;
}

// ───────────── 写入 ─────────────

static void write_escaped(WStr *out, const wchar_t *text)
{
    wstr_append_char(out, L'"');
    for (; *text; text++)
    {
        switch (*text)
        {
            case L'"': wstr_append(out, L"\\\""); break;
            case L'\\': wstr_append(out, L"\\\\"); break;
            case L'\n': wstr_append(out, L"\\n"); break;
            case L'\r': wstr_append(out, L"\\r"); break;
            case L'\t': wstr_append(out, L"\\t"); break;
            default:
                if (*text < 0x20) wstr_appendf(out, L"\\u%04X", (unsigned)*text);
                else wstr_append_char(out, *text);
        }
    }
    wstr_append_char(out, L'"');
}

/// 写一个值之前：补上逗号、换行和缩进，以及属性名。
static void write_prefix(JsonWriter *writer, const wchar_t *key)
{
    if (writer->overflow) return;
    if (writer->depth > 0)
    {
        if (writer->has_items[writer->depth]) wstr_append_char(&writer->out, L',');
        writer->has_items[writer->depth] = true;
        wstr_append(&writer->out, L"\r\n");
        for (int level = 0; level < writer->depth; level++) wstr_append(&writer->out, L"  ");
    }
    if (key)
    {
        write_escaped(&writer->out, key);
        wstr_append(&writer->out, L": ");
    }
}

static void write_begin(JsonWriter *writer, const wchar_t *key, wchar_t open)
{
    if (writer->overflow) return;
    if (writer->depth >= JSON_WRITER_MAX_DEPTH - 1)
    {
        writer->overflow = true;
        return;
    }
    write_prefix(writer, key);
    wstr_append_char(&writer->out, open);
    writer->depth++;
    writer->has_items[writer->depth] = false;
}

static void write_end(JsonWriter *writer, wchar_t close)
{
    if (writer->overflow) return;
    bool had_items = writer->has_items[writer->depth];
    if (writer->depth > 0) writer->depth--;
    if (had_items)
    {
        wstr_append(&writer->out, L"\r\n");
        for (int level = 0; level < writer->depth; level++) wstr_append(&writer->out, L"  ");
    }
    wstr_append_char(&writer->out, close);
}

void json_write_begin_object(JsonWriter *writer, const wchar_t *key) { write_begin(writer, key, L'{'); }
void json_write_end_object(JsonWriter *writer) { write_end(writer, L'}'); }
void json_write_begin_array(JsonWriter *writer, const wchar_t *key) { write_begin(writer, key, L'['); }
void json_write_end_array(JsonWriter *writer) { write_end(writer, L']'); }

void json_write_null(JsonWriter *writer, const wchar_t *key)
{
    if (writer->overflow) return;
    write_prefix(writer, key);
    wstr_append(&writer->out, L"null");
}

void json_write_string(JsonWriter *writer, const wchar_t *key, const wchar_t *value)
{
    if (writer->overflow) return;
    if (!value)
    {
        json_write_null(writer, key);
        return;
    }
    write_prefix(writer, key);
    write_escaped(&writer->out, value);
}

void json_write_int(JsonWriter *writer, const wchar_t *key, long long value)
{
    if (writer->overflow) return;
    write_prefix(writer, key);
    wstr_appendf(&writer->out, L"%lld", value);
}

void json_write_double(JsonWriter *writer, const wchar_t *key, double value)
{
    if (writer->overflow) return;
    if (isnan(value))
    {
        json_write_string(writer, key, L"NaN");
        return;
    }
    if (isinf(value))
    {
        json_write_string(writer, key, value > 0 ? L"Infinity" : L"-Infinity");
        return;
    }
    write_prefix(writer, key);
    // 用能原样读回来的最短写法：860 而不是 860.00000000000000。
    wchar_t text[40];
    if (value == floor(value) && fabs(value) < 1e15)
    {
        swprintf(text, 40, L"%.0f", value);
    }
    else
    {
        for (int precision = 1; precision <= 17; precision++)
        {
            swprintf(text, 40, L"%.*g", precision, value);
            if (wcstod(text, NULL) == value) break;
        }
    }
    wstr_append(&writer->out, text);
}

void json_write_bool(JsonWriter *writer, const wchar_t *key, bool value)
{
    if (writer->overflow) return;
    write_prefix(writer, key);
    wstr_append(&writer->out, value ? L"true" : L"false");
}

wchar_t *json_write_finish(JsonWriter *writer)
{
    bool overflow = writer->overflow;
    writer->depth = 0;
    writer->overflow = false;
    if (overflow)
    {
        wstr_free(&writer->out);
        return NULL;
    }
    return wstr_take(&writer->out);
}
