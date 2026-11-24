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
    return color_contrast_ratio(fill, COLOR_BLACK) > color_contrast_ratio(fill, COLOR_WHITE);
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

static const NeutralColor neutral_palette[] = {
    { THEME_TEXT_PRIMARY, 0xE4000000, 0xFFFFFFFF },
    { THEME_TEXT_SECONDARY, 0x9E000000, 0xC5FFFFFF },
    { THEME_TEXT_TERTIARY, 0x72000000, 0x87FFFFFF },
    { THEME_TEXT_DISABLED, 0x5C000000, 0x5DFFFFFF },
    { THEME_WINDOW_BACKGROUND, 0xFFF9F7F1, 0xFF202020 },
    { THEME_LAYER, 0xCCFFFDF8, 0x4C3A3A3A },
    { THEME_CARD, 0xB3FFFFFF, 0x0DFFFFFF },
    { THEME_CARD_HOVER, 0xE6FFFFFF, 0x14FFFFFF },
    { THEME_CARD_BORDER, 0x14000000, 0x33000000 },
    { THEME_CONTROL, 0xB3FFFFFF, 0x0FFFFFFF },
    { THEME_CONTROL_HOVER, 0x80F9F9F9, 0x15FFFFFF },
    { THEME_CONTROL_PRESSED, 0x4DF9F9F9, 0x08FFFFFF },
    { THEME_CONTROL_BORDER, 0x1F000000, 0x17FFFFFF },
    { THEME_CONTROL_STRONG_BORDER, 0x72000000, 0x8BFFFFFF },
    { THEME_CONTROL_ALT, 0x06000000, 0x19000000 },
    { THEME_SUBTLE_HOVER, 0x09000000, 0x0FFFFFFF },
    { THEME_SUBTLE_PRESSED, 0x06000000, 0x0AFFFFFF },
    { THEME_SUBTLE_SELECTED, 0x0D000000, 0x12FFFFFF },
    { THEME_DIVIDER, 0x0F000000, 0x15FFFFFF },
    { THEME_FLYOUT, 0xFFFDFCF8, 0xFF2C2C2C },
    { THEME_FLYOUT_BORDER, 0x1A000000, 0x26FFFFFF },
    { THEME_SHELF_SOLID, 0xFFFBF9F3, 0xFF2B2B2B },
    { THEME_SHELF_ACRYLIC_TINT, 0xB3FBF9F3, 0xB32A2A2A },
    { THEME_SHELF_TILE, 0xFFFFFFFF, 0xFF4D4D4D },
    { THEME_SHELF_TILE_BORDER, 0x17000000, 0x2EFFFFFF },
    { THEME_SCROLL_THUMB, 0x72000000, 0x8BFFFFFF },
    { THEME_SUCCESS, 0xFF0F7B0F, 0xFF6CCB5F },
    { THEME_CAUTION, 0xFF9D5D00, 0xFFFCE100 },
    { THEME_CRITICAL, 0xFFC42B1C, 0xFFFF99A4 },
    { THEME_SMOKE, 0xCCFBF9F3, 0xCC2B2B2B },
};

static Color color_from_argb(uint32_t value)
{
    return COLOR_ARGB((uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value);
}

/// 先垫一层透明度为 shade 的黑，再铺一层透明度为 alpha 的 color，合成为一种半透明色。
static Color tint(Color color, double alpha, double shade)
{
    double total = 1 - (1 - shade) * (1 - alpha);
    double k = alpha / total;
    return COLOR_ARGB((uint8_t)rint(total * 255), (uint8_t)rint(color.r * k), (uint8_t)rint(color.g * k),
        (uint8_t)rint(color.b * k));
}

void theme_build_palette(bool dark, AccentPalette accent, Color palette[THEME_COLOR_COUNT])
{
    Color fill = dark ? accent.light2 : accent.dark1;
    Color background = dark ? COLOR_RGB(0x20, 0x20, 0x20) : COLOR_RGB(0xF9, 0xF7, 0xF1);
    memset(palette, 0, sizeof(Color) * THEME_COLOR_COUNT);
    palette[THEME_ACCENT] = fill;
    palette[THEME_ACCENT_HOVER] = color_with_alpha(fill, 0xE6);
    palette[THEME_ACCENT_PRESSED] = color_with_alpha(fill, 0xCC);
    palette[THEME_ACCENT_TEXT] = color_ensure_contrast(dark ? accent.light3 : accent.dark2, background, 4.5, dark);
    palette[THEME_ACCENT_SUBTLE] = color_with_alpha(fill, dark ? 0x2E : 0x1F);
    palette[THEME_ACCENT_FAINT] = color_with_alpha(fill, dark ? 0x18 : 0x10);
    // 设置页是翻过来的卡片背面：整张铺一层很淡的强调色，左上角稍浓；深色主题下面再垫一层淡淡的黑，里面的设置卡片才显得出来。
    palette[THEME_STACK_EDGE] = tint(fill, 0.05, dark ? 0.12 : 0);
    palette[THEME_SHEET_GLOW] = tint(fill, 0.10, dark ? 0.12 : 0);
    palette[THEME_SHEET_BORDER] = color_with_alpha(fill, 0x29);
    for (size_t index = 0; index < sizeof(neutral_palette) / sizeof(neutral_palette[0]); index++)
        palette[neutral_palette[index].id] = color_from_argb(dark ? neutral_palette[index].dark : neutral_palette[index].light);
    // 按强调色的实际亮度选择对比度更高的黑字或白字。
    palette[THEME_TEXT_ON_ACCENT] = color_prefers_dark_text(fill) ? COLOR_ARGB(0xE4, 0, 0, 0) : COLOR_WHITE;
    palette[THEME_SYSTEM_ACCENT] = accent.base;
    palette[THEME_TRANSPARENT] = COLOR_TRANSPARENT;
}

// ───────────── 当前主题 ─────────────

typedef struct Listener
{
    ThemeChangedHandler handler;
    void *context;
} Listener;

static int current_mode = THEME_SYSTEM;
static bool has_custom_accent;
static Color custom_accent;
static bool current_is_dark;
static AccentPalette current_accent;
static Color current_palette[THEME_COLOR_COUNT];
static bool palette_ready;
static Listener *listeners;
static int listener_count;

static const wchar_t *const accent_presets[] = {
    L"#0078D4", L"#8764B8", L"#C239B3", L"#E74856", L"#CA5010", L"#498205", L"#038387", L"#68768A"
};

int theme_accent_preset_count(void)
{
    return (int)(sizeof(accent_presets) / sizeof(accent_presets[0]));
}

const wchar_t *theme_accent_preset(int index)
{
    return accent_presets[index];
}

#ifdef _WIN32
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
#else
// Linux 侧从 GTK 设置和 xdg-desktop-portal 的 color-scheme / accent-color 读取。
bool theme_linux_system_is_dark(void);
AccentPalette theme_linux_system_accent(void);

static bool read_system_dark_mode(void)
{
    return theme_linux_system_is_dark();
}

static AccentPalette read_system_accent(void)
{
    return theme_linux_system_accent();
}
#endif

bool theme_resolve_is_dark(int mode, bool system_is_dark)
{
    if (mode == THEME_DARK) return true;
    if (mode == THEME_LIGHT) return false;
    return system_is_dark;
}

void theme_refresh(void)
{
    AccentPalette system = read_system_accent();
    AccentPalette accent = has_custom_accent ? accent_palette_from_color(custom_accent) : system;
    bool dark = theme_resolve_is_dark(current_mode, read_system_dark_mode());

    Color palette[THEME_COLOR_COUNT];
    theme_build_palette(dark, accent, palette);
    palette[THEME_SYSTEM_ACCENT] = system.base;
    // Windows 的任何设置变化都会走到这里，颜色没变时不用通知。
    if (palette_ready && dark == current_is_dark && memcmp(palette, current_palette, sizeof(palette)) == 0) return;
    memcpy(current_palette, palette, sizeof(palette));
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
