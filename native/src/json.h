// 一个小的 JSON 解析器和写入器，供设置文件、语言包和最近关闭的置物架使用。
#pragma once

#include "base.h"

typedef enum JsonType
{
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue
{
    JsonType type;
    bool boolean;
    double number;
    bool is_integer;        // 数字写成了不带小数点和指数的整数
    wchar_t *string;
    JsonValue **items;      // 数组的元素，或对象里各个属性的值
    wchar_t **keys;         // 对象里各个属性的名称，与 items 一一对应
    int count;
};

/// 语言包是给人手写的，允许 // 和 /* */ 注释，以及最后一项后面多一个逗号；设置文件不允许。
#define JSON_ALLOW_COMMENTS 1u
#define JSON_ALLOW_TRAILING_COMMAS 2u

/// 解析失败（格式不对、嵌套太深）时返回 NULL。
JsonValue *json_parse(const wchar_t *text, unsigned flags);
void json_free(JsonValue *value);

/// 按名称（区分大小写）取对象的属性；同名属性出现多次时取最后一个。没有时返回 NULL。
const JsonValue *json_get(const JsonValue *object, const wchar_t *key);

/// 写出带缩进的 JSON：每层两个空格，换行为 \r\n，与 C# 版写出的文件格式相同。
#define JSON_WRITER_MAX_DEPTH 32

typedef struct JsonWriter
{
    WStr out;
    int depth;
    bool has_items[JSON_WRITER_MAX_DEPTH];
    bool overflow;
} JsonWriter;

// key 为 NULL 表示写的是数组元素或最外层的值。
void json_write_begin_object(JsonWriter *writer, const wchar_t *key);
void json_write_end_object(JsonWriter *writer);
void json_write_begin_array(JsonWriter *writer, const wchar_t *key);
void json_write_end_array(JsonWriter *writer);
void json_write_string(JsonWriter *writer, const wchar_t *key, const wchar_t *value);   // value 为 NULL 时写 null
void json_write_int(JsonWriter *writer, const wchar_t *key, long long value);
void json_write_double(JsonWriter *writer, const wchar_t *key, double value);           // NaN 和无穷大写成 "NaN"、"Infinity"、"-Infinity"
void json_write_bool(JsonWriter *writer, const wchar_t *key, bool value);
void json_write_null(JsonWriter *writer, const wchar_t *key);
wchar_t *json_write_finish(JsonWriter *writer);
