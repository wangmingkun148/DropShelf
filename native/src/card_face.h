// 牌面上的记号：牌角的数字（或扩展名）和花色。花色按文件类型分：▲ 图片、■ 文档、● 文件夹、◆ 其他。
// 和 C# style 版的 FileSuit、FileSuits、ShelfItem.CardMark，以及 Controls.xaml 里的 CardRankTextStyle、SuitStyle 对应。
#pragma once

#include "gfx.h"

typedef enum FileSuit
{
    SUIT_IMAGE,
    SUIT_DOCUMENT,
    SUIT_FOLDER,
    SUIT_OTHER,
    SUIT_COUNT
} FileSuit;

/// 按扩展名判断；没有扩展名的按文件夹算，不用访问磁盘。
FileSuit file_suit_of_path(const wchar_t *path);

/// 数量最多的一种；一样多时取最后放入的那一种。没有时为其他。
FileSuit file_suit_dominant(const FileSuit *suits, int count);

/// 牌角印的类型：文件的扩展名（最多四个字母，大写），没有扩展名时为空。调用方负责 free。
wchar_t *file_card_mark(const wchar_t *path);

/// 画一个花色：中心在 center_x，上边在 top，边长 size。图片和其他用强调色，文档和文件夹用墨色，形状本身也不同。
void card_paint_suit(FileSuit suit, float center_x, float top, float size, float opacity);

/// 牌角：数字在上、花色在下，两者居中对齐。rank_size 是字号，line_height 是数字那一行的高度，suit_gap 是两者之间的空隙。
typedef struct CardCorner
{
    float rank_size, line_height;
    float suit_size, suit_gap;
} CardCorner;

/// 左上角的牌角，(left, top) 是它的左上角；inverted 时把同样的牌角倒过来画在 card 的右下角，right、bottom 是到牌边的距离。
void card_paint_corner(const wchar_t *rank, FileSuit suit, const CardCorner *corner, float left, float top, Color color, float opacity);
void card_paint_corner_inverted(const wchar_t *rank, FileSuit suit, const CardCorner *corner, GfxRect card, float right, float bottom, Color color,
    float opacity);
