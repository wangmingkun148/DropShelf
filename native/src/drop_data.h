// 拖放数据：判断拖进来的是什么（文件、未落盘附件、网页链接、图片或文本）。
// 读取内容放进置物架在第 4 步移植；这里先有探针窗口确认拖放时要用的判断。
// 格式的查询方式与 WPF 的 GetDataPresent(format, autoConvert: false) 相同，判断结果与 C# 版一致。
#pragma once

#define COBJMACROS
#include "base.h"

#ifdef _WIN32
#include <ole2.h>
#endif

typedef enum DropContentKind
{
    DROP_NONE,
    DROP_FILES,
    DROP_VIRTUAL_FILES,
    DROP_LINK,
    DROP_IMAGE,
    DROP_TEXT
} DropContentKind;

/// 置物架拖出时附带的格式，内容是来源置物架的标识。
#define SHELF_ID_FORMAT L"DropShelf.ShelfId"

DropContentKind drop_classify(IDataObject *data);

/// 是否能转换为磁盘文件，供快速操作使用。
bool drop_can_provide_files(DropContentKind kind);

/// 来源置物架的标识；不是从置物架拖出来的时返回 NULL。
wchar_t *drop_source_shelf_id(IDataObject *data);

CLIPFORMAT drop_format(const wchar_t *name);
bool drop_has_format(IDataObject *data, CLIPFORMAT format);

/// 读出某个格式的全部字节（全局内存或流）。读不到时返回 NULL。
unsigned char *drop_read_bytes(IDataObject *data, CLIPFORMAT format, size_t *size);

/// 按 UTF-16、系统 ANSI 代码页、UTF-8 读出文本，到第一个 0 为止。读不到时返回 NULL。
wchar_t *drop_read_unicode(IDataObject *data, CLIPFORMAT format);
wchar_t *drop_read_ansi(IDataObject *data, CLIPFORMAT format);
wchar_t *drop_read_utf8(IDataObject *data, CLIPFORMAT format);

/// 只提供 FileNameW 或 FileName（一个路径）的来源；路径不存在时返回 NULL。
wchar_t *drop_read_single_file_name(IDataObject *data);

/// 有没有虚拟附件（FileGroupDescriptor）。
bool drop_has_virtual_files(IDataObject *data);

/// 旧版 QQ 的富文本或只含图片的网页片段中，仍然存在的本地图片。
void drop_read_local_image_paths(IDataObject *data, StrList *paths);

/// 诊断日志用：数据里前 maximum 个格式的名称，用逗号隔开。
wchar_t *drop_format_names(IDataObject *data, int maximum);

/// 虚拟附件（FileGroupDescriptor）的文件名，跳过文件夹。读不了时列表为空。
void drop_descriptor_names(IDataObject *data, StrList *names);

// ───────────── 聊天软件 ─────────────
// 旧版 QQ 和部分程序拖出图片时只提供带本地图片路径的富文本或网页片段。

/// 旧版 QQ 拖出的富文本：只包含图片时返回图片的本地路径，含有文字时为空（按文本处理）。
void chat_parse_qq_rich_edit(const wchar_t *xml, StrList *paths);

/// 网页片段（HTML Format）只包含本地图片时返回图片路径；含有文字时为空，仍按文本或链接处理。
void chat_parse_html_local_images(const wchar_t *html, StrList *paths);

/// 读取以字节形式提供的富文本：先按 UTF-8 判断，含有 QQ 标记时使用，否则按 UTF-16。
wchar_t *chat_decode_rich_edit(const unsigned char *bytes, size_t size);

/// 富文本中间接引用的图片只接受本机磁盘上的路径。检查是否存在时会访问对应位置，
/// 网络共享或映射的网络驱动器会发起网络连接，可能卡住界面，甚至向对方发送登录凭据。
bool chat_is_local_path(const wchar_t *path);

/// 与 .NET 的 WebUtility.HtmlDecode 相同地解开常见的字符引用；认不得的原样保留。
wchar_t *html_decode(const wchar_t *text, size_t length);
