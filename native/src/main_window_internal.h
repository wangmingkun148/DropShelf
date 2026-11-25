// 主窗口各部分之间共用的东西。主窗口分成几个文件：
//   main_window.c   窗口、标题栏、排版、左侧状态栏、键盘
//   main_pages.c    概览页和四个设置页的内容
//   main_flip.c     点开卡片翻成设置页、牌堆露出的边、从牌边抽出另一页
//   main_fan.c      概览页底部最近关闭的置物架（一手牌），以及把牌拖出窗口
//   main_actions.c  快速操作页：拖动更换操作、拖到底部删除
//   main_dialogs.c  添加或编辑自定义操作、确认删除、晃动示范校准三个对话框
// 与 C# 版的 MainWindow（及其各个分部文件）、SettingsViewModel 对应。
#pragma once

#include "main_window.h"
#include "anim.h"
#include "app.h"
#include "gesture.h"
#include "settings.h"
#include "strings.h"
#include "ui.h"

typedef enum MainPage
{
    PAGE_OVERVIEW = -1,
    PAGE_GENERAL,
    PAGE_SHELF,
    PAGE_ACTIONS,
    PAGE_APPEARANCE,
    PAGE_COUNT
} MainPage;

#define CAPTION_HEIGHT 48.0f
#define PAGE_MARGIN 36.0f           // 页面内容的左右和下边距
#define PAGE_TOP 26.0f
#define PAGE_MAX_WIDTH 860.0f
#define SIDE_PANEL_WIDTH 300.0f
#define SHEET_PADDING 24.0f         // 设置页那张牌比页面内容两侧各宽出这么多
#define SHEET_TOP 14.0f
#define STACK_ROOM 64.0f            // 牌底下留给其余几张牌的边
#define SHEET_MAX_WIDTH 908.0f
#define SHEET_RADIUS 12.0f
/// 纸牌：设置页四周牌背纹样的宽度：翻牌、抽牌途中由背面的轮廓画出这一圈。
#define SHEET_BAND 10.0f
#define TILE_RADIUS 8.0f
#define MOTION_SECONDS 0.34
#define MOTION_EASE EASE_OF(EASE_CUBIC_OUT)

// 图标字形，与 C# 版 Tokens.xaml 里的相同。
#define GLYPH_CLOSE L"\xE711"
#define GLYPH_INFO L"\xE946"
#define GLYPH_COPY L"\xE8C8"
#define GLYPH_ZIP L"\xF012"
#define GLYPH_FOLDER_OPEN L"\xE838"
#define GLYPH_FOLDER L"\xE8B7"
#define GLYPH_DOCUMENT L"\xE8A5"
#define GLYPH_PICTURE L"\xE8B9"
#define GLYPH_TEXT L"\xE8D2"
#define GLYPH_SETTINGS L"\xE713"
#define GLYPH_COLOR L"\xE790"
#define GLYPH_STYLE L"\xE771"
#define GLYPH_LIGHTNING L"\xE945"
#define GLYPH_KEYBOARD L"\xE765"
#define GLYPH_POWER L"\xE7E8"
#define GLYPH_MOUSE L"\xE962"
#define GLYPH_MOVE L"\xE7C2"
#define GLYPH_VIEW_ALL L"\xE8A9"
#define GLYPH_LIST L"\xE8FD"
#define GLYPH_MORE L"\xE712"
#define GLYPH_PINNED L"\xE840"
#define GLYPH_CHECK L"\xE73E"
#define GLYPH_ERROR L"\xE783"
#define GLYPH_ADD L"\xE710"
#define GLYPH_DELETE L"\xE74D"
#define GLYPH_DOWNLOAD L"\xE896"
#define GLYPH_MOVE_TO_FOLDER L"\xE8DE"
#define GLYPH_LIBRARY L"\xE8F1"
#define GLYPH_CLEAR L"\xE894"
#define GLYPH_HELP L"\xE897"
#define GLYPH_COMPLETED L"\xE930"
#define GLYPH_SHIELD L"\xEA18"
#define GLYPH_EXPAND L"\xE740"
#define GLYPH_BRUSH L"\xE771"
#define GLYPH_BACK L"\xE72B"
#define GLYPH_LOCK L"\xE72E"
#define GLYPH_LANGUAGE L"\xE775"
#define GLYPH_HISTORY L"\xE81C"
#define GLYPH_EDIT L"\xE70F"
#define GLYPH_TIMER L"\xE823"
#define GLYPH_SWAP L"\xE8AB"
#define GLYPH_VIEW L"\xE890"

/// 一个设置页：一张圆角的牌，内容在牌里滚动。
typedef struct PageView
{
    UiElement *host;        // 翻牌时整页的变换和裁剪放在它上面（坐标就是窗口的）
    UiElement *sheet;       // 那张牌：底色、边框，里面是滚动区
    UiElement *scroll;
    UiElement *content;
    UiElement *header;      // 页面顶部的返回按钮和标题；状态栏显示时移到状态栏里
    StringId title;
    const wchar_t *glyph;
    bool sheet_border_hidden;   // 翻牌中边框改由翻牌画
} PageView;

typedef struct MainShell
{
    UiWindow *window;
    UiElement *root;
    GfxRect stage;          // 标题栏下面的内容区
    float page_left;        // 页面内容的左边距（状态栏显示时加宽）
    float page_shift;       // 状态栏出现或收起时页面横向的平移（动画中）

    PageView pages[PAGE_COUNT];
    UiElement *overview_scroll;
    UiElement *overview_page;   // 概览页的内容（翻牌时下沉淡出的就是它）
    UiElement *overview_header;
    UiElement *overview_body;
    UiElement *tile_grid;
    UiElement *tiles[PAGE_COUNT];
    UiElement *history_section;
    UiElement *fan;
    UiElement *stack_layer;     // 设置页底下露出的其余几张牌的边
    UiElement *side_panel;
    UiElement *version_button;
    UiElement *dialogs;         // 三个对话框的遮罩层

    int page;                   // 当前页，PAGE_OVERVIEW 是概览
    bool side_panel_shown;
    bool side_panel_moving;     // 状态栏出现或收起的动画还没结束：这时不翻牌
} MainShell;

extern MainShell main_shell;
#define M (&main_shell)

// ───────────── main_window.c ─────────────

/// 页面那张牌静止时的位置。
GfxRect main_sheet_area(void);
/// 页面内容（概览页）静止时的左右边。
void main_content_columns(float *left, float *right);
/// 内容区的底色：窗口的底色上盖一层内容区的颜色。
Color main_stage_background(void);
void main_request_frame(void);
void main_repaint(void);
void main_relayout(void);
/// 状态栏顶部的页面标题换成 page（PAGE_OVERVIEW 是概览）。
void main_set_panel_header(int page, bool animate);
/// 概览页卡片和页面的名称、图标。
StringId main_page_title(int page);
const wchar_t *main_page_glyph(int page);
/// 打开置物架：主窗口开着时放在它旁边。返回打开的置物架（可能是 NULL）。
struct ShelfWindow *main_open_shelf(void);

// ───────────── main_pages.c ─────────────

/// 建好概览页和四个设置页，放进 M。
void pages_build(void);
/// 设置变了：开关、选项、说明文字都按现在的设置更新。
void pages_refresh(void);
void pages_animate(double time);
/// 概览页卡片的高度：窗口较高时拉长卡片填满页面，避免纵向留白。viewport 是概览页可见范围的高度。
/// 卡片高度变了时返回 true，调用方要重新量一次概览页。
bool pages_update_card_height(float viewport, bool resized);
/// 画概览页的一张卡片：matrix 把卡片从排版的位置变到现在的样子（翻牌时）。
void pages_paint_tile_face(UiElement *tile, GfxMatrix matrix, float opacity);
/// 主窗口关闭：还没保存的灵敏度立即保存，记着的元素都清掉。
void pages_close(void);
/// 采用校准给出的灵敏度：立即保存，滑块随之移到新的位置。
void pages_apply_calibration(int level);

// ───────────── main_flip.c ─────────────

void flip_open(int page);
void flip_back(void);
/// 窗口大小、缩放或状态变了：翻牌不再播完，直接摆到终点。
void flip_cancel(void);
bool flip_morphing(void);
void flip_animate(double time);
/// 画内容区：概览页、牌的边、设置页和翻牌中的卡片。
void flip_paint_stage(double time);
/// 窗口的鼠标事件先交给这里（按住卡片或设置页空白处横向拖动翻牌），返回 true 表示已处理。
bool flip_mouse(const UiMouseEvent *event);
/// 按 Esc：拖动翻牌时退回起点。
bool flip_escape(void);
/// 页面切换后的显示：当前页可见，其余隐藏，牌的边摆好。
void flip_show_page(int page);
/// 设置页里牌的边的元素（放在根元素里，接收光标和点击）。
UiElement *flip_create_stack_layer(void);
/// 排版变了：牌的边按页面那张牌的新位置摆。
void flip_layout_edges(void);
/// 拖动翻牌时窗口失去鼠标（如切换了窗口）：退回起点。
void flip_capture_lost(void);
void flip_close(void);

// ───────────── main_fan.c ─────────────

UiElement *fan_create(void);
/// 最近关闭的置物架有变化：牌换成新的一组（沿用已有的牌）。
void fan_refresh(bool reuse);
void fan_animate(double time);
bool fan_dragging(void);
bool fan_escape(void);
/// 主窗口关闭：拖到窗口外的卡牌窗口一起关掉。
void fan_close(void);
/// 拖着卡牌在窗口里时画在最上层。
void fan_paint_ghost(double time);

// ───────────── main_actions.c ─────────────

/// 快速操作页的“当前操作”和“更多操作”两块，放进 content。
void actions_build(UiElement *content);
void actions_refresh(void);
bool actions_dragging(void);
bool actions_escape(void);
/// 拖动操作卡片时底部的光效和跟随光标的卡片。
void actions_paint_overlay(double time);
void actions_animate(double time);
void actions_close(void);
/// 把操作放到第 index 个位置；已在别的位置时两者互换。
void actions_assign_slot(int index, const wchar_t *id);
/// 删除自定义操作，它所在的位置换成一个没用到的内置操作。
void actions_delete_custom(const wchar_t *id);

// ───────────── main_dialogs.c ─────────────

UiElement *dialogs_create(void);
/// 添加（action 为 NULL）或编辑自定义操作。
void dialogs_edit_action(const CustomAction *action);
/// 确认删除自定义操作。
void dialogs_confirm_delete(const CustomAction *action, const wchar_t *title);
void dialogs_open_calibration(void);
bool dialogs_open(void);
bool dialogs_escape(void);
void dialogs_refresh(void);
void dialogs_animate(double time);
/// 窗口的鼠标事件：校准练习区按住拖动。
bool dialogs_mouse(const UiMouseEvent *event);
void dialogs_close(void);

// ───────────── 测试用 ─────────────
// 测试按用户看到的样子检查各部分：只读出现在的状态，或拿到控件后像用户一样操作它。

typedef enum PagesControl
{
    PAGES_GETTING_STARTED,
    PAGES_LANGUAGE,
    PAGES_PERFORMANCE,
    PAGES_PERFORMANCE_DESCRIPTION,
    PAGES_TRIGGER,
    PAGES_SENSITIVITY_CARD,
    PAGES_SENSITIVITY_SLIDER,
    PAGES_SENSITIVITY_DESCRIPTION,
    PAGES_EDGE_DWELL_CARD,
    PAGES_EDGE_DWELL,
    PAGES_SHELF_SIZE,
    PAGES_DRAG_OUT_MODE,
    PAGES_CLOSE_WHEN_EMPTY,
    PAGES_WATCH_LIST,
    PAGES_PEEK_SECONDS,
    PAGES_PEEK_SECONDS_CARD,
    PAGES_LAYOUT,
    PAGES_LOOK,
    PAGES_THEME_SYSTEM,
    PAGES_THEME_LIGHT,
    PAGES_THEME_DARK,
    PAGES_SWATCHES,
} PagesControl;

UiElement *pages_control(PagesControl control);
/// 概览页卡片上的摘要、是不是大卡片的样式。
const wchar_t *pages_tile_summary(int page);
bool pages_tile_is_tall(int page);
/// 概览页卡片现在的高度。
float pages_card_height(void);
/// 灵敏度滑块下三个字（稳健、均衡、灵敏）现在的大小比例。
float pages_sensitivity_label_scale(int index);

/// 快速操作页：置物架上第 index 个位置的操作和位置名称，更多操作里的操作。
int actions_slot_count(void);
const wchar_t *actions_slot_id(int index);
StringId actions_slot_position(int index);
int actions_more_count(void);
const wchar_t *actions_more_id(int index);

typedef enum DialogPart
{
    DIALOG_PART_EDITOR_KIND,
    DIALOG_PART_EDITOR_SIZE,
    DIALOG_PART_EDITOR_NAME,
    DIALOG_PART_EDITOR_SAVE,
    DIALOG_PART_EDITOR_ERROR,       // 错误提示那一行
    DIALOG_PART_CALIBRATION_STATUS,
    DIALOG_PART_CALIBRATION_APPLY,
    DIALOG_PART_CALIBRATION_RETRY,
} DialogPart;

UiElement *dialogs_part(DialogPart part);
/// 校准：交上一次示范（和在练习区里拖着晃一次一样），返回是否记下了。
bool dialogs_calibration_submit(const ShakeSample *samples, int count);
int dialogs_calibration_done(void);
int dialogs_calibration_recommended(void);

/// 最近关闭的一手牌：牌数、每张牌对应的记录、牌上的缩略图数、叠放次序、是否被选出；
/// identity 在牌沿用时保持不变，重新生成时换新。
int fan_card_count(void);
const struct ShelfHistoryEntry *fan_card_entry(int index);
int fan_card_thumbs(int index);
int fan_card_z(int index);
bool fan_card_hot(int index);
const void *fan_card_identity(int index);
