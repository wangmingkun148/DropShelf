#include "card_face.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static const wchar_t *const image_extensions[] = {
    L".png", L".jpg", L".jpeg", L".jfif", L".gif", L".bmp", L".webp", L".tif", L".tiff", L".heic", L".heif", L".avif", L".svg", L".ico",
    L".psd", L".raw", L".cr2", L".cr3", L".nef", L".arw", L".dng"
};

static const wchar_t *const document_extensions[] = {
    L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp", L".rtf", L".txt", L".md", L".csv",
    L".json", L".xml", L".html", L".htm", L".epub", L".pages", L".numbers", L".key", L".wps", L".et", L".dps"
};

static bool is_separator(wchar_t character)
{
    return character == L'\\' || character == L'/';
}

/// 和 .NET 的 Path.GetExtension 相同（含点），先去掉末尾的分隔符；没有扩展名时 *length 为 0。
static const wchar_t *extension_of(const wchar_t *path, size_t *length)
{
    *length = 0;
    if (!path) return L"";
    size_t end = wcslen(path);
    while (end > 0 && is_separator(path[end - 1])) end--;
    for (size_t index = end; index > 0; index--)
    {
        wchar_t character = path[index - 1];
        if (is_separator(character) || character == L':') break;
        if (character != L'.') continue;
        // 点在最后时没有扩展名。
        if (index == end) return L"";
        *length = end - (index - 1);
        return path + index - 1;
    }
    return L"";
}

static bool in_list(const wchar_t *extension, size_t length, const wchar_t *const *list, size_t count)
{
    for (size_t index = 0; index < count; index++)
        if (wcslen(list[index]) == length && _wcsnicmp(list[index], extension, length) == 0) return true;
    return false;
}

FileSuit file_suit_of_path(const wchar_t *path)
{
    size_t length;
    const wchar_t *extension = extension_of(path, &length);
    if (length == 0) return SUIT_FOLDER;
    if (in_list(extension, length, image_extensions, sizeof(image_extensions) / sizeof(image_extensions[0]))) return SUIT_IMAGE;
    return in_list(extension, length, document_extensions, sizeof(document_extensions) / sizeof(document_extensions[0])) ? SUIT_DOCUMENT : SUIT_OTHER;
}

FileSuit file_suit_dominant(const FileSuit *suits, int count)
{
    int counts[SUIT_COUNT] = { 0 }, last[SUIT_COUNT] = { 0 };
    for (int index = 0; index < count; index++)
    {
        if (suits[index] < 0 || suits[index] >= SUIT_COUNT) continue;
        counts[suits[index]]++;
        last[suits[index]] = index + 1;
    }
    FileSuit best = SUIT_OTHER;
    for (int suit = 0; suit < SUIT_COUNT; suit++)
        if (counts[suit] > counts[best] || (counts[suit] == counts[best] && counts[suit] > 0 && last[suit] > last[best])) best = (FileSuit)suit;
    return best;
}

wchar_t *file_card_mark(const wchar_t *path)
{
    size_t length;
    const wchar_t *extension = extension_of(path, &length);
    // 去掉开头的点，最多四个字母。
    if (length > 0) extension++, length--;
    if (length > 4) length = 4;
    wchar_t *mark = xalloc((length + 1) * sizeof(wchar_t));
    for (size_t index = 0; index < length; index++) mark[index] = (wchar_t)towupper(extension[index]);
    mark[length] = 0;
    return mark;
}

void card_paint_suit(FileSuit suit, float center_x, float top, float size, float opacity)
{
    if (opacity <= 0.001f) return;
    Color color = theme_color(suit == SUIT_DOCUMENT || suit == SUIT_FOLDER ? THEME_TEXT_PRIMARY : THEME_ACCENT_TEXT);
    color.a = (uint8_t)(color.a * (opacity > 1 ? 1 : opacity));
    float left = center_x - size / 2, half = size / 2;
    switch (suit)
    {
        case SUIT_DOCUMENT:
            gfx_fill_rect((GfxRect){ left, top, left + size, top + size }, color);
            break;
        case SUIT_FOLDER:
            gfx_fill_ellipse(center_x, top + half, half, half, color);
            break;
        case SUIT_OTHER:
        {
            float diamond[] = { center_x, top, left + size, top + half, center_x, top + size, left, top + half };
            gfx_polygon(diamond, 4, color, COLOR_TRANSPARENT, 0);
            break;
        }
        default:
        {
            float triangle[] = { center_x, top, left + size, top + size, left, top + size };
            gfx_polygon(triangle, 3, color, COLOR_TRANSPARENT, 0);
            break;
        }
    }
}

static GfxTextStyle rank_style(const CardCorner *corner)
{
    GfxTextStyle style = { GFX_FONT_RANK, corner->rank_size, 600, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START, corner->line_height };
    return style;
}

/// 牌角占多大：宽是数字和花色里较宽的那个，高是数字那一行加上空隙和花色。
GfxSize card_corner_size(const wchar_t *rank, const CardCorner *corner)
{
    GfxTextStyle style = rank_style(corner);
    float width = rank && *rank ? gfx_text_measure(rank, &style, 0).width : 0;
    if (width < corner->suit_size) width = corner->suit_size;
    float height = (rank && *rank ? corner->line_height : 0) + corner->suit_gap + corner->suit_size;
    return (GfxSize){ width, height };
}

void card_paint_corner_rolling(const wchar_t *rank, FileSuit suit, const CardCorner *corner, float left, float top, Color color, float opacity,
    float rank_shift, float rank_opacity)
{
    if (opacity <= 0.001f) return;
    GfxSize size = card_corner_size(rank, corner);
    float y = top;
    if (rank && *rank)
    {
        GfxTextStyle style = rank_style(corner);
        Color ink = color;
        float alpha = opacity * rank_opacity;
        ink.a = (uint8_t)(ink.a * (alpha > 1 ? 1 : alpha < 0 ? 0 : alpha));
        if (ink.a > 0)
            gfx_text_draw(rank, (GfxRect){ left - 20, top + rank_shift, left + size.width + 20, top + rank_shift + corner->line_height }, &style, ink);
        y += corner->line_height;
    }
    card_paint_suit(suit, left + size.width / 2, y + corner->suit_gap, corner->suit_size, opacity);
}

void card_paint_corner(const wchar_t *rank, FileSuit suit, const CardCorner *corner, float left, float top, Color color, float opacity)
{
    card_paint_corner_rolling(rank, suit, corner, left, top, color, opacity, 0, 1);
}

void card_paint_corner_inverted(const wchar_t *rank, FileSuit suit, const CardCorner *corner, GfxRect card, float right, float bottom, Color color,
    float opacity)
{
    if (opacity <= 0.001f) return;
    GfxSize size = card_corner_size(rank, corner);
    float left = card.right - right - size.width, top = card.bottom - bottom - size.height;
    gfx_push_transform(gfx_matrix_rotate(180, left + size.width / 2, top + size.height / 2));
    card_paint_corner(rank, suit, corner, left, top, color, opacity);
    gfx_pop_transform();
}
