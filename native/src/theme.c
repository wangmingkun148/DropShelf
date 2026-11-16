#include "theme.h"
#include "settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PERSONALIZE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define ACCENT_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent"

bool color_equal(Color first, Color second)
{
    return first.a == second.a && first.r == second.r && first.g == second.g && first.b == second.b;
}

Color color_with_alpha(Color color, uint8_t alpha)
{
    color.a = alpha;
    return color;
}

static uint8_t mix_channel(uint8_t from, uint8_t to, double amount)
{
    // 与 C# 的 Math.Round 一样，恰好一半时取偶数。
    return (uint8_t)rint(from + (to - from) * amount);
}

/// 只混合 RGB，结果不透明。
static Color mix(Color from, Color to, double amount)
{
    return COLOR_RGB(mix_channel(from.r, to.r, amount), mix_channel(from.g, to.g, amount), mix_channel(from.b, to.b, amount));
}

Color color_mix(Color from, Color to, double amount)
{
    return mix(from, to, amount);
}

Color color_over(Color top, Color bottom)
{
    double top_alpha = top.a / 255.0, bottom_alpha = bottom.a / 255.0;
    double alpha = top_alpha + bottom_alpha * (1 - top_alpha);
    if (alpha <= 0) return COLOR_TRANSPARENT;
#define OVER(channel) (uint8_t)lround((top.channel * top_alpha + bottom.channel * bottom_alpha * (1 - top_alpha)) / alpha)
    Color result = { (uint8_t)lround(alpha * 255), OVER(r), OVER(g), OVER(b) };
#undef OVER
    return result;
}

Color color_lerp(Color from, Color to, double amount)
{
    if (amount <= 0) return from;
    if (amount >= 1) return to;
    // 全透明的一端没有颜色可言：借用另一端的颜色，只过渡透明度，中途不会发灰。
    if (from.a == 0) from = color_with_alpha(to, 0);
    if (to.a == 0) to = color_with_alpha(from, 0);
    Color result = mix(from, to, amount);
    result.a = mix_channel(from.a, to.a, amount);
    return result;
}

// ───────────── 强调色 ─────────────

AccentPalette accent_palette_default(void)
{
    return (AccentPalette){
        COLOR_RGB(0x99, 0xEB, 0xFF), COLOR_RGB(0x4C, 0xC2, 0xFF), COLOR_RGB(0x00, 0x91, 0xF8), COLOR_RGB(0x00, 0x78, 0xD4),
        COLOR_RGB(0x00, 0x67, 0xC0), COLOR_RGB(0x00, 0x3E, 0x92), COLOR_RGB(0x00, 0x1A, 0x68)
    };
}

bool accent_palette_parse(const uint8_t *bytes, size_t length, AccentPalette *palette)
{
    if (!bytes || length < 28) return false;
    Color colors[7];
    for (int index = 0; index < 7; index++) colors[index] = COLOR_RGB(bytes[index * 4], bytes[index * 4 + 1], bytes[index * 4 + 2]);
    *palette = (AccentPalette){ colors[0], colors[1], colors[2], colors[3], colors[4], colors[5], colors[6] };
    return true;
}

AccentPalette accent_palette_from_color(Color color)
{
    color.a = 255;
    return (AccentPalette){
        mix(color, COLOR_WHITE, 0.65), mix(color, COLOR_WHITE, 0.45), mix(color, COLOR_WHITE, 0.25), color,
        mix(color, COLOR_BLACK, 0.15), mix(color, COLOR_BLACK, 0.35), mix(color, COLOR_BLACK, 0.55)
    };
}

// ───────────── 对比度 ─────────────

static double luminance_channel(uint8_t value)
{
    double c = value / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

double color_relative_luminance(Color color)
{
    return 0.2126 * luminance_channel(color.r) + 0.7152 * luminance_channel(color.g) + 0.0722 * luminance_channel(color.b);
}

double color_contrast_ratio(Color first, Color second)
{
    double a = color_relative_luminance(first) + 0.05;
    double b = color_relative_luminance(second) + 0.05;
    return a > b ? a / b : b / a;
}

bool color_prefers_dark_text(Color fill)
{
    // Windows 默认的蓝色上黑字的对比度略高一点，但白字也够清楚，按惯例用白字。
    return color_contrast_ratio(fill, COLOR_WHITE) < 4.5;
}

Color color_ensure_contrast(Color color, Color background, double ratio, bool lighten)
{
    Color target = lighten ? COLOR_WHITE : COLOR_BLACK;
    Color current = color;
    for (int step = 1; step <= 20 && color_contrast_ratio(current, background) < ratio; step++) current = mix(color, target, step / 20.0);
    return current;
}

// ───────────── 调色板 ─────────────

typedef struct NeutralColor
{
    ThemeColor id;
    uint32_t light;     // 0xAARRGGBB
    uint32_t dark;
} NeutralColor;

#define LIGHT_BACKGROUND 0xFFE8E5DE
#define DARK_BACKGROUND 0xFF1C1B19
#define NEUTRAL_INK 0xFF15171C

// 浅色是象牙白的牌桌和牌面，深色是暖调的深炭色，两套的冷暖一致。
static const NeutralColor neutral_palette[] = {
    { THEME_TEXT_PRIMARY, NEUTRAL_INK, 0xFFEDEAE3 },
    { THEME_TEXT_SECONDARY, 0xFF575A63, 0xFFA6A39C },
    { THEME_TEXT_TERTIARY, 0xFF7A7D85, 0xFF86837C },
    { THEME_TEXT_DISABLED, 0xFFA3A5AB, 0xFF5F5C56 },
    { THEME_WINDOW_BACKGROUND, LIGHT_BACKGROUND, DARK_BACKGROUND },
    { THEME_LAYER, 0x00000000, 0x00000000 },
    { THEME_CARD, 0xFFFBFAF6, 0xFF262522 },
    { THEME_CARD_HOVER, 0xFFFFFFFF, 0xFF2E2D2A },
    { THEME_CARD_BORDER, 0xFFD2CEC5, 0xFF34322E },
    { THEME_TABLE_CARD_BORDER, 0xFFC9C5BB, 0xFF3D3B36 },
    { THEME_CONTROL, 0xFFFBFAF6, 0xFF2A2926 },
    { THEME_CONTROL_HOVER, 0xFFFFFFFF, 0xFF32312D },
    { THEME_CONTROL_PRESSED, 0xFFF2EFE8, 0xFF262522 },
    { THEME_CONTROL_BORDER, 0xFFBDB8AE, 0xFF4A4843 },
    { THEME_CONTROL_STRONG_BORDER, 0xFF5E6066, 0xFFA6A39C },
    { THEME_CONTROL_ALT, 0x0D15171C, 0x14EDEAE3 },
    { THEME_SUBTLE_HOVER, 0x0F15171C, 0x12EDEAE3 },
    { THEME_SUBTLE_PRESSED, 0x0A15171C, 0x0AEDEAE3 },
    { THEME_SUBTLE_SELECTED, 0x1415171C, 0x1AEDEAE3 },
    { THEME_DIVIDER, 0xFFDDD9D0, 0xFF34322E },
    { THEME_FLYOUT, 0xFFFBFAF6, 0xFF262522 },
    { THEME_FLYOUT_BORDER, 0xFFC9C5BB, 0xFF3D3B36 },
    { THEME_SETTING_ROW, 0xFFF2EFE8, 0xFF2E2D2A },
    { THEME_SETTING_ROW_BORDER, 0xFFE2DED5, 0xFF3A3834 },
    { THEME_SHELF_SOLID, LIGHT_BACKGROUND, DARK_BACKGROUND },
    { THEME_SHELF_BORDER, 0xFFB4AFA4, 0xFF3A3834 },
    { THEME_SHELF_TILE, 0xFFFFFFFF, 0xFF2E2D2A },
    { THEME_SHELF_TILE_BORDER, NEUTRAL_INK, 0xFFA6A39C },
    { THEME_SHELF_UNDER_CARD_BORDER, NEUTRAL_INK, 0xFF6E6B65 },
    { THEME_ART_FRAME, 0xFFDDD9D0, 0xFF3A3834 },
    { THEME_SCROLL_THUMB, 0xFF8A8C93, 0xFF6E6B65 },
    { THEME_SUCCESS, 0xFF0F7B0F, 0xFF6CCB5F },
    { THEME_CAUTION, 0xFF9D5D00, 0xFFFCE100 },
    { THEME_CRITICAL, 0xFFC42B1C, 0xFFFF99A4 },
    { THEME_SMOKE, 0xCCE8E5DE, 0xCC1C1B19 },
};

static Color color_from_argb(uint32_t value)
{
    return COLOR_ARGB((uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value);
}

void theme_build_palette(bool dark, AccentPalette accent, Color palette[THEME_COLOR_COUNT])
{
    Color fill = accent.base;
    Color background = color_from_argb(dark ? DARK_BACKGROUND : LIGHT_BACKGROUND);
    // 牌背和牌桌：浅色主题直接用强调色，深色主题压暗，免得在暗处一大片发亮。
    Color back = dark ? mix(fill, COLOR_BLACK, 0.45) : fill;
    Color table = dark ? mix(fill, COLOR_BLACK, 0.52) : fill;
    memset(palette, 0, sizeof(Color) * THEME_COLOR_COUNT);
    palette[THEME_ACCENT] = fill;
    palette[THEME_ACCENT_HOVER] = color_with_alpha(fill, 0xE6);
    palette[THEME_ACCENT_PRESSED] = color_with_alpha(fill, 0xCC);
    palette[THEME_ACCENT_TEXT] = color_ensure_contrast(fill, background, 4.5, dark);
    palette[THEME_ACCENT_SUBTLE] = color_with_alpha(fill, dark ? 0x2E : 0x1F);
    palette[THEME_ACCENT_FAINT] = color_with_alpha(fill, dark ? 0x18 : 0x10);
    palette[THEME_CARD_BACK_FILL] = back;
    palette[THEME_CARD_BACK_LINE] = mix(back, COLOR_BLACK, 0.25);
    palette[THEME_STACK_EDGE] = back;
    palette[THEME_SHEET_BORDER] = back;
    palette[THEME_TABLE_FILL] = table;
    for (size_t index = 0; index < sizeof(neutral_palette) / sizeof(neutral_palette[0]); index++)
        palette[neutral_palette[index].id] = color_from_argb(dark ? neutral_palette[index].dark : neutral_palette[index].light);
    // 强调色由用户自选，可能很暗也可能很亮：白字够清楚就用白字，和 Windows 一样，太亮的强调色上换成黑字。
    Color ink = color_from_argb(NEUTRAL_INK);
    palette[THEME_TEXT_ON_ACCENT] = color_prefers_dark_text(fill) ? ink : COLOR_WHITE;
    palette[THEME_TABLE_TEXT] = color_prefers_dark_text(table) ? ink : dark ? palette[THEME_TEXT_PRIMARY] : COLOR_WHITE;
    palette[THEME_SYSTEM_ACCENT] = accent.base;
    palette[THEME_TRANSPARENT] = COLOR_TRANSPARENT;
}

void theme_build_patterns(bool dark, const Color palette[THEME_COLOR_COUNT], ThemePattern patterns[THEME_PATTERN_COUNT])
{
    Color back = palette[THEME_CARD_BACK_FILL];
    // 牌背：斜格纹；设置页四周那圈边框和翻牌途中的轮廓都用它。牌背露出的窄边改用斜条纹，几像素宽也看得清。
    patterns[THEME_PATTERN_SHEET_BORDER] = (ThemePattern){ THEME_PATTERN_LATTICE, back, color_with_alpha(COLOR_WHITE, dark ? 0x1F : 0x29), 8 };
    patterns[THEME_PATTERN_STACK_EDGE] = (ThemePattern){ THEME_PATTERN_STRIPES, back, mix(back, COLOR_WHITE, dark ? 0.15 : 0.3), 0 };
    patterns[THEME_PATTERN_TABLE] = (ThemePattern){ THEME_PATTERN_LATTICE, palette[THEME_TABLE_FILL], color_with_alpha(COLOR_WHITE, dark ? 0x0F : 0x12), 12 };
}

// ───────────── 当前主题 ─────────────

typedef struct Listener
{
    ThemeChangedHandler handler;
    void *context;
} Listener;

static int current_mode = THEME_SYSTEM;
static bool follow_system_accent;
static bool has_custom_accent;
static Color custom_accent;
static bool current_is_dark;
static AccentPalette current_accent;
static Color current_palette[THEME_COLOR_COUNT];
static ThemePattern current_patterns[THEME_PATTERN_COUNT];
static bool palette_ready;
static Listener *listeners;
static int listener_count;

static const wchar_t *const accent_presets[] = {
    L"#8764B8", L"#C239B3", L"#E74856", L"#CA5010", L"#498205", L"#038387", L"#68768A"
};

int theme_accent_preset_count(void)
{
    return (int)(sizeof(accent_presets) / sizeof(accent_presets[0]));
}

const wchar_t *theme_accent_preset(int index)
{
    return accent_presets[index];
}

static bool read_system_dark_mode(void)
{
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, PERSONALIZE_KEY, L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS) return false;
    return value == 0;
}

static AccentPalette read_system_accent(void)
{
    uint8_t bytes[64];
    DWORD size = sizeof(bytes);
    AccentPalette palette;
    if (RegGetValueW(HKEY_CURRENT_USER, ACCENT_KEY, L"AccentPalette", RRF_RT_REG_BINARY, NULL, bytes, &size) == ERROR_SUCCESS &&
        accent_palette_parse(bytes, size, &palette))
        return palette;
    return accent_palette_default();
}

bool theme_resolve_is_dark(int mode, bool system_is_dark)
{
    if (mode == THEME_DARK) return true;
    if (mode == THEME_LIGHT) return false;
    return system_is_dark;
}

void theme_refresh(void)
{
    AccentPalette system = read_system_accent();
    AccentPalette accent = follow_system_accent ? system : accent_palette_from_color(has_custom_accent ? custom_accent : THEME_COBALT);
    bool dark = theme_resolve_is_dark(current_mode, read_system_dark_mode());

    Color palette[THEME_COLOR_COUNT];
    theme_build_palette(dark, accent, palette);
    palette[THEME_SYSTEM_ACCENT] = system.base;
    // Windows 的任何设置变化都会走到这里，颜色没变时不用通知。
    if (palette_ready && dark == current_is_dark && memcmp(palette, current_palette, sizeof(palette)) == 0) return;
    memcpy(current_palette, palette, sizeof(palette));
    theme_build_patterns(dark, current_palette, current_patterns);
    current_is_dark = dark;
    current_accent = accent;
    palette_ready = true;

    int count = listener_count;
    if (count == 0) return;
    Listener *copy = xalloc((size_t)count * sizeof(Listener));
    memcpy(copy, listeners, (size_t)count * sizeof(Listener));
    for (int index = 0; index < count; index++) copy[index].handler(copy[index].context);
    free(copy);
}

void theme_set_mode(int mode, const wchar_t *accent)
{
    current_mode = mode;
    follow_system_accent = accent && wcscmp(accent, THEME_SYSTEM_ACCENT_VALUE) == 0;
    COLORREF rgb;
    has_custom_accent = accent_color_parse(accent, &rgb);
    if (has_custom_accent) custom_accent = COLOR_RGB(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
    theme_refresh();
}

void theme_initialize(int mode, const wchar_t *accent)
{
    theme_set_mode(mode, accent);
}

bool theme_is_dark(void)
{
    if (!palette_ready) theme_refresh();
    return current_is_dark;
}

Color theme_color(ThemeColor id)
{
    if (!palette_ready) theme_refresh();
    return id >= 0 && id < THEME_COLOR_COUNT ? current_palette[id] : COLOR_TRANSPARENT;
}

ThemePattern theme_pattern(ThemePatternId id)
{
    if (!palette_ready) theme_refresh();
    return id >= 0 && id < THEME_PATTERN_COUNT ? current_patterns[id] : (ThemePattern){ THEME_PATTERN_LATTICE, COLOR_TRANSPARENT, COLOR_TRANSPARENT, 8 };
}

AccentPalette theme_accent(void)
{
    if (!palette_ready) theme_refresh();
    return current_accent;
}

void theme_add_listener(ThemeChangedHandler handler, void *context)
{
    listeners = xrealloc(listeners, (size_t)(listener_count + 1) * sizeof(Listener));
    listeners[listener_count++] = (Listener){ handler, context };
}

void theme_remove_listener(ThemeChangedHandler handler, void *context)
{
    for (int index = 0; index < listener_count; index++)
    {
        if (listeners[index].handler != handler || listeners[index].context != context) continue;
        memmove(&listeners[index], &listeners[index + 1], (size_t)(listener_count - index - 1) * sizeof(Listener));
        listener_count--;
        return;
    }
}
