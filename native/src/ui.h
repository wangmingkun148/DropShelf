// 界面底座：窗口（ui_window.c）和布局、控件（ui_controls.c）。
// 窗口里的内容是一棵元素的树：容器负责排版，控件负责画自己和响应鼠标。坐标都是 DIP，相对窗口客户区的左上角。
#pragma once

#include "anim.h"
#include "gfx.h"

typedef struct UiWindow UiWindow;
typedef struct UiElement UiElement;

// ───────────── 鼠标和键盘 ─────────────

typedef enum UiMouseKind
{
    UI_MOUSE_ENTER,
    UI_MOUSE_LEAVE,
    UI_MOUSE_MOVE,
    UI_MOUSE_DOWN,
    UI_MOUSE_UP,        // 在按下的元素上松开；inside 表示松开时光标还在它上面（也就是一次点击）
    UI_MOUSE_WHEEL
} UiMouseKind;

typedef struct UiMouseEvent
{
    UiMouseKind kind;
    float x, y;
    bool right_button;
    bool inside;
    float wheel;        // 滚轮转动的格数，向上为正
} UiMouseEvent;

// ───────────── 窗口 ─────────────

typedef enum UiBackdrop
{
    UI_BACKDROP_NONE = 1,
    UI_BACKDROP_MICA = 2,
    UI_BACKDROP_ACRYLIC = 3
} UiBackdrop;

typedef struct UiWindowOptions
{
    const wchar_t *title;
    float width, height;            // 客户区大小
    float minimum_width, minimum_height;
    bool popup;                     // 没有边框和标题栏、不在任务栏显示、保持在最上面（菜单、置物架）
    bool no_activate;               // 显示和点击时不抢占当前程序的焦点
    bool resizable;
    UiBackdrop backdrop;            // 系统材质；系统不支持或低性能模式下改用 background 纯色
    ThemeColor background;          // 没有材质时的背景色；popup 窗口不画背景
    float caption_height;           // 顶部这么高的范围可以拖动窗口（没有控件的地方）
    HICON icon;
    /// 置物架这样的窗口：没有任何系统边框（整个窗口都是客户区），但保留系统的圆角、阴影和材质；
    /// 不在任务栏和 Alt+Tab 中显示，可以拖动、点击后成为当前窗口。拖动窗口由调用方自己处理（ui_window_begin_move）。
    bool frameless;

    void *context;                  // 原样传给下面的回调
    /// 在元素之前画（背景、自绘的内容）。time 是这一帧的时刻，交给 anim_update。
    void (*paint)(UiWindow *window, void *context, double time);
    /// 鼠标事件先交给这里，返回 true 表示已处理，不再交给元素。
    bool (*mouse)(UiWindow *window, void *context, const UiMouseEvent *event);
    /// 按键（虚拟键码），返回 true 表示已处理。
    bool (*key)(UiWindow *window, void *context, unsigned key);
    /// 窗口成为或不再是当前窗口。
    void (*activated)(UiWindow *window, void *context, bool active);
    /// 窗口位置或大小变了（用户拖动、最大化）。
    void (*moved)(UiWindow *window, void *context);
    /// 窗口已经关闭并销毁，window 之后不能再用。
    void (*closed)(UiWindow *window, void *context);
    /// 每一帧开始画之前调用：推进动画、改窗口的位置和大小。这之后才按窗口现在的大小排版和画。
    void (*animate)(UiWindow *window, void *context, double time);
    /// 光标在窗口里时用的光标（IDC_HAND 等）；返回 NULL 时按控件决定。
    LPCWSTR (*cursor)(UiWindow *window, void *context, float x, float y);
    /// 其他窗口消息先交给这里；返回 true 表示已处理，result 是返回值。
    bool (*message)(UiWindow *window, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
} UiWindowOptions;

/// 创建窗口（还没有显示）。失败时返回 NULL。
UiWindow *ui_window_create(const UiWindowOptions *options);
HWND ui_window_handle(UiWindow *window);

/// 设置窗口里的元素树，窗口拥有它；原来的那棵被销毁。
void ui_window_set_root(UiWindow *window, UiElement *root);
UiElement *ui_window_root(UiWindow *window);

void ui_window_show(UiWindow *window);
void ui_window_close(UiWindow *window);

/// 内容变了：下一次刷新时重新排版并重画。
void ui_window_invalidate(UiWindow *window);

/// 画的过程中调用：还有动画在进行，下一帧接着画。
void ui_window_request_frame(UiWindow *window);

/// 窗口所在显示器的缩放比例（DPI / 96）。
float ui_window_scale(UiWindow *window);

/// 客户区的大小（DIP）。
GfxSize ui_window_size(UiWindow *window);

/// 窗口是不是当前窗口（标题栏按钮据此变淡）。
bool ui_window_is_active(UiWindow *window);

/// 系统材质是否已经启用；没有启用时窗口用纯色背景。
bool ui_window_has_backdrop(UiWindow *window);

/// 以屏幕像素放置窗口：x、y 是左上角，width、height 是客户区大小（DIP）；width 或 height 为 0 时不改大小。
void ui_window_place(UiWindow *window, int x, int y, float width, float height);

/// 把窗口放到光标所在显示器的工作区中央。
void ui_window_center(UiWindow *window);

/// 显示窗口；activate 为 false 时不抢当前程序的焦点。
void ui_window_show_with(UiWindow *window, bool activate);

/// 保持在其他窗口上方。
void ui_window_set_topmost(UiWindow *window, bool topmost);

/// Windows 11 上窗口边框的颜色；color 为 NULL 时恢复系统默认。
void ui_window_set_border_color(UiWindow *window, const Color *color);

/// 按住鼠标后调用：由系统接着拖动窗口，直到松开鼠标才返回（和 WPF 的 DragMove 相同）。
void ui_window_begin_move(UiWindow *window);

/// 光标停在窗口上时显示的提示；NULL 表示不显示。换了文字时先收起旧的。
void ui_window_set_tooltip(UiWindow *window, const wchar_t *text);

/// 所有窗口都关掉（退出时用）。
void ui_close_all_windows(void);

/// 诊断和测试用：把窗口现在的样子画进一张 PNG 图片（没有系统材质，背景是纯色）。
bool ui_window_snapshot(UiWindow *window, const wchar_t *path);

/// 在屏幕外把窗口画一遍再扔掉（窗口可以是隐藏的）：字体、文字排版和绘制效果提前准备好，第一次显示时更快。
void ui_window_warm(UiWindow *window);

/// 把所有窗口各存一张图片到 directory：window-1.png、window-2.png……（最新打开的排在前面）。
void ui_snapshot_windows(const wchar_t *directory);

/// 低性能模式的设置变了：各窗口重新决定用不用系统材质。
void ui_refresh_window_appearance(void);

// ───────────── 元素 ─────────────

typedef enum UiCross
{
    UI_CROSS_STRETCH,   // 在容器的另一个方向上占满
    UI_CROSS_START,
    UI_CROSS_CENTER,
    UI_CROSS_END
} UiCross;

/// 一种元素的行为。没有的函数填 NULL。
typedef struct UiElementType
{
    /// 可用宽度为 available 时想要多大（不含外边距）。
    GfxSize (*measure)(UiElement *element, float available);
    /// bounds 已经定了，安排各个子元素的位置。
    void (*arrange)(UiElement *element);
    void (*paint)(UiElement *element, double time);
    void (*mouse)(UiElement *element, const UiMouseEvent *event);
    void (*destroy)(UiElement *element);
    bool interactive;   // 接收鼠标事件；否则事件交给上一级
} UiElementType;

/// 所有元素开头的公共部分。具体的控件把它放在自己结构的第一个成员。
struct UiElement
{
    const UiElementType *type;
    UiWindow *window;
    UiElement *parent;
    UiElement **children;
    int child_count;

    GfxRect bounds;             // 排版的结果
    GfxSize desired;            // 上一次量出来的大小
    float margin_left, margin_top, margin_right, margin_bottom;
    float width, height;        // 固定的大小，0 表示按内容
    float minimum_width, minimum_height;
    float grow;                 // 在容器的排列方向上分走多少份剩余空间，0 表示不分
    UiCross cross;
    bool hidden;                // 不显示也不占位置
    bool disabled;
    bool hovered;
    bool pressed;
};

/// 新建一个元素：size 是整个结构的大小（开头是 UiElement）。
UiElement *ui_element_new(const UiElementType *type, size_t size);

/// 连同子元素一起销毁。
void ui_element_destroy(UiElement *element);
void ui_add(UiElement *parent, UiElement *child);
void ui_set_margin(UiElement *element, float left, float top, float right, float bottom);
void ui_set_disabled(UiElement *element, bool disabled);

/// 元素的内容变了：所在的窗口重新排版并重画。
void ui_invalidate(UiElement *element);

/// 以下三个由窗口调用。
GfxSize ui_measure(UiElement *element, float available);
void ui_arrange(UiElement *element, GfxRect bounds);
void ui_paint(UiElement *element, double time);

/// (x, y) 处最里层的能接收鼠标的元素；没有时返回 NULL。
UiElement *ui_hit_test(UiElement *element, float x, float y);

// ───────────── 控件上的文字 ─────────────

/// 一段文字：文字表里的一条（切换语言后自动跟着变），或一段固定的文字（会被复制）。
typedef struct UiText
{
    int string;                 // StringId，-1 表示用 literal
    const wchar_t *literal;
} UiText;

#define UI_STR(id) ((UiText){ (id), NULL })
#define UI_LIT(text) ((UiText){ -1, (text) })
#define UI_NO_TEXT ((UiText){ -1, NULL })

// ───────────── 容器 ─────────────

/// 把子元素横着或竖着排成一列，相邻的隔开 spacing。
UiElement *ui_stack(bool horizontal, float spacing);
void ui_stack_set_padding(UiElement *stack, float left, float top, float right, float bottom);

/// 给容器加上背景、边框（1 DIP）和圆角；不要的颜色传 THEME_TRANSPARENT。
void ui_stack_set_background(UiElement *stack, ThemeColor fill, ThemeColor border, float radius);

/// 占位：在容器的排列方向上把剩余空间占满。
UiElement *ui_spacer(void);

// ───────────── 文字 ─────────────

typedef enum UiTextKind
{
    UI_TEXT_TITLE,          // 28，半粗
    UI_TEXT_SUBTITLE,       // 20，半粗
    UI_TEXT_BODY_STRONG,    // 14，半粗
    UI_TEXT_BODY,           // 14
    UI_TEXT_CAPTION,        // 12，次要颜色
    UI_TEXT_GLYPH           // 图标字形，16
} UiTextKind;

UiElement *ui_label(UiText text, UiTextKind kind);
void ui_label_set_text(UiElement *label, UiText text);
void ui_label_set_color(UiElement *label, ThemeColor color);
void ui_label_set_size(UiElement *label, float size);

// ───────────── 按钮 ─────────────

typedef enum UiButtonKind
{
    UI_BUTTON_STANDARD,
    UI_BUTTON_ACCENT,       // 强调色的主要按钮
    UI_BUTTON_SUBTLE,       // 平时没有底色
    UI_BUTTON_ICON,         // 32×32，内容是一个图标字形
    UI_BUTTON_CAPTION,      // 标题栏右上角的最小化、最大化按钮
    UI_BUTTON_CAPTION_CLOSE // 标题栏的关闭按钮，悬停时变红
} UiButtonKind;

typedef void (*UiClickHandler)(void *context);

UiElement *ui_button(UiButtonKind kind, UiText text, UiClickHandler clicked, void *context);
void ui_button_set_text(UiElement *button, UiText text);

// ───────────── 开关 ─────────────

typedef void (*UiToggleHandler)(bool checked, void *context);

UiElement *ui_toggle(bool checked, UiToggleHandler changed, void *context);

/// 改成 checked（带滑动的动画），不触发 changed。
void ui_toggle_set_checked(UiElement *toggle, bool checked);

// ───────────── 分段选择 ─────────────

typedef void (*UiSelectHandler)(int index, void *context);

UiElement *ui_segmented(const UiText *items, int count, int selected, UiSelectHandler changed, void *context);
void ui_segmented_set_selected(UiElement *segmented, int selected);

// ───────────── 设置卡片 ─────────────

/// 一行设置：左边图标，中间标题和说明，右边一个控件。glyph、control 可以是 NULL，description 可以是 UI_NO_TEXT。
/// description_label 不为 NULL 时带回说明文字的元素，之后可以用 ui_label_set_text 改它。
UiElement *ui_settings_card(const wchar_t *glyph, UiText header, UiText description, UiElement *control, UiElement **description_label);

// ───────────── 菜单（ui_menu.c） ─────────────

typedef struct UiMenuItem
{
    int id;                     // 选中后传给 chosen；分隔线不用填
    const wchar_t *text;
    const wchar_t *glyph;       // 左边的图标字形，可以是 NULL
    const wchar_t *gesture;     // 右边的快捷键文字，可以是 NULL
    bool separator;
    bool disabled;
    bool checked;               // 显示对勾（代替图标）
    const wchar_t *tooltip;     // 停住时显示的提示，可以是 NULL
    float max_width;            // 文字最多这么宽（DIP），放不下时末尾显示省略号；0 表示不限
    const struct UiMenuItem *children;  // 子菜单（只有一层）：悬停一会儿或点按时在旁边展开，选中的项同样交给 chosen
    int child_count;
} UiMenuItem;

typedef void (*UiMenuHandler)(int id, void *context);

/// 在屏幕上的 (x, y) 处弹出菜单（一般是光标的位置），放不下时自动换到另一侧。
/// 选了一项后调用 chosen；点到别处或按 Esc 时直接关闭。items 里的文字会被复制。
void ui_menu_show(const UiMenuItem *items, int count, int x, int y, UiMenuHandler chosen, void *context);

/// 关掉正在显示的菜单。
void ui_menu_dismiss(void);
bool ui_menu_is_open(void);
