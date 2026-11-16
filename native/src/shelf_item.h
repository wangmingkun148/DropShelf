// 置物架上的一个项目：文件、文件夹、一段文本或一个网页链接。和 C# 版的 ShelfItem 对应。
// 项目只在界面线程上读写；后台的工作（统计文件夹、取缩略图）拿着引用，做完回到界面线程再写回来。
#pragma once

#include "anim.h"
#include "base.h"
#include "card_face.h"
#include "file_identity.h"
#include "gfx.h"

/// 平铺里这张牌相对自己格子的姿态（以牌面中心为原点缩放、转动，再平移），以及牌下面名字的透明度。
typedef struct ShelfItemMotion
{
    float dx, dy, angle, scale, opacity;
    float name_opacity;
} ShelfItemMotion;

typedef enum ShelfItemKind
{
    ITEM_FILE,
    ITEM_FOLDER,
    ITEM_TEXT,
    ITEM_LINK
} ShelfItemKind;

typedef struct ShelfItem
{
    LONG references;
    wchar_t *id;
    ShelfItemKind kind;
    wchar_t *value;             // 文件或文件夹的完整路径、文本内容或网址
    wchar_t *title;             // 链接的标题，可以是 NULL
    bool temporary;             // 由 DropShelf 接收并暂存在临时文件夹里的附件
    bool available;             // 磁盘上还在（文本和链接始终为 true）
    bool has_size;
    long long size_bytes;       // 文件的大小；文件夹里全部文件的大小
    int child_count;            // 文件夹第一层有几项，-1 表示还没统计出来
    wchar_t *type_name;         // Windows 显示的文件类型，例如“PDF 文件”
    GfxImage *thumbnail;
    FileIdentity identity;      // 在别处被改名或移动后按它找到现在的路径，见 shelf_item_relocate；取不到时为空

    // 显示用的文字，按需生成，内容或语言变了就重新生成。
    wchar_t *display_name;
    wchar_t *detail;
    unsigned text_generation;

    // 以下由置物架使用。
    bool selected;
    bool thumbnail_wanted;      // 正在取或已经取到缩略图
    unsigned thumbnail_request; // 每次请求加一，回来的结果对不上就丢掉
    bool measuring;             // 正在统计文件夹
    ShelfItemMotion motion;
    Anim dim;                   // 拖出期间被拖的牌变淡（1 是原样）
    Anim lift;                  // 平铺里悬停时牌抬起（0 到 1）
} ShelfItem;

void shelf_item_motion_reset(ShelfItem *item);

ShelfItem *shelf_item_new(ShelfItemKind kind, const wchar_t *value, const wchar_t *title, bool temporary);
ShelfItem *shelf_item_retain(ShelfItem *item);
void shelf_item_release(ShelfItem *item);

bool shelf_item_is_file_system(const ShelfItem *item);
const wchar_t *shelf_item_display_name(ShelfItem *item);
const wchar_t *shelf_item_detail(ShelfItem *item);
const wchar_t *shelf_item_glyph(const ShelfItem *item);

/// 花色：按文件类型分，印在牌角。文件夹是文件夹，文字算文档，链接算其他。
FileSuit shelf_item_suit(const ShelfItem *item);

/// 牌角印的类型：文件的扩展名（最多四个字母），链接为 URL，文字为 TXT，文件夹不印。调用方负责 free。
wchar_t *shelf_item_card_mark(const ShelfItem *item);

/// 内容变了（大小、类型、可用性），下次重新生成说明文字。
void shelf_item_text_changed(ShelfItem *item);

/// 界面语言变了：所有项目的说明文字都要重新生成。
void shelf_items_language_changed(void);

void shelf_item_set_thumbnail(ShelfItem *item, GfxImage *image);   // 收下 image 的引用

/// 文件在别处被改名或移动了：换成现在的路径，名称和说明随之更新；选中状态、缩略图都留着。
void shelf_item_relocate(ShelfItem *item, const wchar_t *path);

/// 交给另一个置物架时用的副本：内容、缩略图、文件夹统计和标识都带过去，选中状态和动画是新的。
ShelfItem *shelf_item_copy(const ShelfItem *item);

/// 只检查不修改，可以在后台线程上调用。文本和链接始终算存在。
bool shelf_item_exists_on_disk(ShelfItemKind kind, const wchar_t *value);

/// 检查磁盘上的项目是否还在，并记下结果。
bool shelf_item_refresh_availability(ShelfItem *item);

// ───────────── 文字 ─────────────

/// 1.5 MB 这样的大小写法，与 C# 版的 FormatSize 相同。调用方负责 free。
wchar_t *format_size(long long bytes);

/// 取第一行非空内容，连续空白合并为一个空格，超过 maximum 个字符时截断并加“…”。调用方负责 free。
wchar_t *text_summarize(const wchar_t *text, int maximum);

/// 只有一行、没有空格的 http 或 https 网址。
bool text_is_single_url(const wchar_t *text);

/// 网址的主机名（小写）；不是 http 或 https 网址时返回 NULL。调用方负责 free。
wchar_t *url_host(const wchar_t *url);

/// 和 Path.GetExtension 一样：带点的扩展名，没有时为空字符串。指向 path 里面。
const wchar_t *path_extension(const wchar_t *path);

// ───────────── 文件信息 ─────────────

/// 文件是不是文件夹、有多大，以及它在磁盘上的标识；读不到（已删除、没有权限）时返回 false。可以在后台线程上调用。
typedef struct FileFacts
{
    bool is_folder;
    bool has_size;
    long long size_bytes;
    FileIdentity identity;
} FileFacts;

bool file_facts_read(const wchar_t *path, FileFacts *facts);

/// Windows 显示的文件类型名称，例如“PDF 文件”。只按扩展名查询，不访问磁盘。可以在后台线程上调用。调用方负责 free。
wchar_t *file_type_name(const wchar_t *path);

/// 文件的大小；读不到时返回 false。
bool file_size_of(const wchar_t *path, long long *size);

/// 文件夹第一层有几项、一共多大（含子文件夹）。统计大小最多花 3 秒、看 20 万个文件，超过了 has_size 为 false。
/// 文件夹读不了时返回 false。cancelled 不为 NULL 且变为非零时尽快返回 false。可以在后台线程上调用。
bool folder_measure(const wchar_t *path, int *children, bool *has_size, long long *size, volatile LONG *cancelled);
