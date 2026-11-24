// 主题色：跟随 Windows 的深浅色，或固定为浅色、深色；强调色跟随 Windows（纸牌风格默认是钴蓝）或自选。
// 两种风格（简洁、纸牌）各有一套颜色，切换风格就是换一套颜色，再通知各个窗口重画。
// 界面用到的颜色都按名称（ThemeColor）从这里取，主题切换后重新取就是新的颜色。名称和取值与 C# 版相同。
#pragma once

#include "base.h"

/// 一种颜色，各分量 0–255，没有预乘透明度。
typedef struct Color
{
    uint8_t a, r, g, b;
} Color;

#define COLOR_RGB(r, g, b) ((Color){ 255, (r), (g), (b) })
#define COLOR_ARGB(a, r, g, b) ((Color){ (a), (r), (g), (b) })
#define COLOR_WHITE COLOR_RGB(255, 255, 255)
#define COLOR_BLACK COLOR_RGB(0, 0, 0)
#define COLOR_TRANSPARENT COLOR_ARGB(0, 0, 0, 0)

bool color_equal(Color first, Color second);
Color color_with_alpha(Color color, uint8_t alpha);

/// 两种颜色按 amount（0–1）混合 RGB，结果不透明。
Color color_mix(Color from, Color to, double amount);

/// 两种颜色之间按 amount（0–1）过渡，透明度一起过渡。
Color color_lerp(Color from, Color to, double amount);

/// top 盖在 bottom 上面（普通的透明叠加）得到的颜色。
Color color_over(Color top, Color bottom);

/// 强调色的一组深浅：Windows 提供的，或由一个颜色生成的。
typedef struct AccentPalette
{
    Color light3, light2, light1, base, dark1, dark2, dark3;
} AccentPalette;

AccentPalette accent_palette_default(void);

/// 解析注册表 AccentPalette：依次为 Light3、Light2、Light1、强调色、Dark1、Dark2、Dark3，每项 RGBA 四字节。不完整时返回 false。
bool accent_palette_parse(const uint8_t *bytes, size_t length, AccentPalette *palette);

/// 由一个颜色生成整组深浅：向白色或黑色逐级混合。
AccentPalette accent_palette_from_color(Color color);

typedef enum ThemeColor
{
    THEME_ACCENT,
    THEME_ACCENT_HOVER,
    THEME_ACCENT_PRESSED,
    THEME_ACCENT_TEXT,
    THEME_ACCENT_SUBTLE,
    THEME_ACCENT_FAINT,
    THEME_STACK_EDGE,           // 简洁：卡片背面的底色；纸牌：牌背露出的窄边（斜条纹）的底色，图案见 theme_pattern
    THEME_SHEET_GLOW,           // 简洁：卡片背面左上角稍浓的一块
    THEME_SHEET_BORDER,         // 简洁：卡片背面的边框；纸牌：牌背（斜格纹）的底色，图案见 theme_pattern
    THEME_CARD_BACK_FILL,
    THEME_CARD_BACK_LINE,
    THEME_TABLE_FILL,           // 牌桌的底色，图案见 theme_pattern
    THEME_TABLE_TEXT,           // 牌桌上的文字
    THEME_TEXT_PRIMARY,
    THEME_TEXT_SECONDARY,
    THEME_TEXT_TERTIARY,
    THEME_TEXT_DISABLED,
    THEME_TEXT_ON_ACCENT,
    THEME_WINDOW_BACKGROUND,
    THEME_LAYER,
    THEME_CARD,
    THEME_CARD_HOVER,
    THEME_CARD_BORDER,
    THEME_TABLE_CARD_BORDER,
    THEME_CONTROL,
    THEME_CONTROL_HOVER,
    THEME_CONTROL_PRESSED,
    THEME_CONTROL_BORDER,
    THEME_CONTROL_STRONG_BORDER,
    THEME_CONTROL_ALT,
    THEME_SUBTLE_HOVER,
    THEME_SUBTLE_PRESSED,
    THEME_SUBTLE_SELECTED,
    THEME_DIVIDER,
    THEME_FLYOUT,
    THEME_FLYOUT_BORDER,
    THEME_SETTING_ROW,
    THEME_SETTING_ROW_BORDER,
    THEME_SHELF_SOLID,
    THEME_SHELF_ACRYLIC_TINT,
    THEME_SHELF_BORDER,
    THEME_SHELF_TILE,
    THEME_SHELF_TILE_BORDER,
    THEME_SHELF_UNDER_CARD_BORDER,
    THEME_ART_FRAME,
    THEME_SCROLL_THUMB,
    THEME_SUCCESS,
    THEME_CAUTION,
    THEME_CRITICAL,
    THEME_SMOKE,
    THEME_SYSTEM_ACCENT,        // Windows 当前的强调色，用于“跟随 Windows”色块的预览
    THEME_TRANSPARENT,
    THEME_COLOR_COUNT
} ThemeColor;

/// 界面的风格：简洁（和 Windows 11 一样的卡片和系统材质）或纸牌（牌面、牌背和牌桌）。
typedef enum Look
{
    LOOK_SIMPLE,
    LOOK_CARDS
} Look;

/// 按风格、深浅色和强调色算出全部颜色（THEME_SYSTEM_ACCENT 除外，它由调用方另填）。
void theme_build_palette(Look look, bool dark, AccentPalette accent, Color palette[THEME_COLOR_COUNT]);

/// 强调色设置为这个值时跟随 Windows；为 NULL 时简洁风格也跟随 Windows，纸牌风格用钴蓝。
#define THEME_SYSTEM_ACCENT_VALUE L"system"

/// 纸牌风格的默认强调色：牌背和牌桌的钴蓝。
#define THEME_COBALT COLOR_RGB(0x2B, 0x4E, 0xFF)

/// 牌背、牌桌这些铺满图案的地方（纸牌风格）：底色加上一层线条。
typedef enum ThemePatternKind
{
    THEME_PATTERN_LATTICE,      // 斜格纹：每格画一个叉，平铺后连成菱形格子
    THEME_PATTERN_STRIPES       // 45° 的细条纹，两种颜色各 2 像素宽
} ThemePatternKind;

typedef struct ThemePattern
{
    ThemePatternKind kind;
    Color fill;
    Color line;
    float size;                 // 斜格纹一格的边长（DIP）
} ThemePattern;

typedef enum ThemePatternId
{
    THEME_PATTERN_SHEET_BORDER, // 牌背：设置页四周那圈边框、翻牌途中的轮廓
    THEME_PATTERN_STACK_EDGE,   // 牌背露出的窄边
    THEME_PATTERN_TABLE,        // 牌桌
    THEME_PATTERN_COUNT
} ThemePatternId;

/// 按深浅色算出的图案（纸牌风格用）。
void theme_build_patterns(bool dark, const Color palette[THEME_COLOR_COUNT], ThemePattern patterns[THEME_PATTERN_COUNT]);
ThemePattern theme_pattern(ThemePatternId id);

/// 启动时调用一次：读取 Windows 的深浅色和强调色。mode 是 AppTheme，accent 是 #RRGGBB、THEME_SYSTEM_ACCENT_VALUE 或 NULL。
void theme_initialize(int mode, const wchar_t *accent, Look look);

/// 换风格：换一套颜色并通知各个窗口。
void theme_set_look(Look look);
Look theme_look(void);
/// 现在是纸牌风格（各处按风格画法不同时用）。
bool theme_is_cards(void);

/// 程序图标的资源号：简洁是 1，纸牌是 2（见 res/DropShelf.rc）。exe 文件本身的图标总是第一个。
int theme_icon_resource(void);

/// 切换主题或强调色。
void theme_set_mode(int mode, const wchar_t *accent);

/// Windows 的深浅色或强调色变了（收到 WM_SETTINGCHANGE 的 ImmersiveColorSet）：重新读取。
void theme_refresh(void);

bool theme_is_dark(void);
Color theme_color(ThemeColor id);
AccentPalette theme_accent(void);
bool theme_resolve_is_dark(int mode, bool system_is_dark);

/// 可选的强调色（#RRGGBB），取自 Windows 的强调色色板；纸牌风格不含 Windows 默认的蓝色（它有自己的钴蓝）。
int theme_accent_preset_count(void);
const wchar_t *theme_accent_preset(int index);

/// 主题变化后调用 handler，已显示的窗口需要重画。
typedef void (*ThemeChangedHandler)(void *context);
void theme_add_listener(ThemeChangedHandler handler, void *context);
void theme_remove_listener(ThemeChangedHandler handler, void *context);

// ───────────── 对比度 ─────────────

double color_relative_luminance(Color color);
double color_contrast_ratio(Color first, Color second);

/// 强调色由用户自选，可能很暗也可能很亮：这个底色上用黑字的对比度更高时返回 true。
bool color_prefers_dark_text(Color fill);
/// 纸牌风格：白字够清楚就用白字，和 Windows 一样；白字的对比度不到 4.5 时返回 true，改用黑字。
bool color_prefers_dark_text_cards(Color fill);

/// 把颜色逐步向白色（lighten）或黑色调整，直到与背景达到所需对比度。
Color color_ensure_contrast(Color color, Color background, double ratio, bool lighten);
