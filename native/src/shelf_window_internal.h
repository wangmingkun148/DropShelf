// 置物架窗口内部共用的定义：shelf_window.c（窗口、排版、绘制）、shelf_input.c（鼠标、键盘、拖入拖出、提示）、
// shelf_motion.c（动画）、shelves.c（管理多个置物架）。
// 两种风格的置物架：shelf_window.c、shelf_input.c 各编译两遍（LOOK_VARIANT_CARDS 定义与否），动画分成 shelf_motion.c（简洁）
// 和 shelf_motion_cards.c（纸牌），见 look_variant.h。结构体里两种风格的字段都在，各用各的；函数的声明按风格分开。
#pragma once

#define COBJMACROS
#include "app.h"
#include "card_face.h"
#include "shelf_window.h"
#include "shelf_drag.h"
#include "shelf_model.h"
#include "strings.h"
#include "ui.h"

// ───────────── 尺寸（DIP），与 C# 版的 XAML 相同 ─────────────

#define SHELF_HEADER_HEIGHT 40.0f
#define SHELF_CAPTION_HEIGHT 38.0f      // 堆叠下面两行文字的高度（估算窗口大小用）
#ifndef LOOK_VARIANT_CARDS
#define SHELF_ROW_HEIGHT 46.0f          // 估算窗口大小用的行高
#define SHELF_ROW_PITCH 44.0f           // 列表里一行实际占的高度：内容 32 + 上下内边距 5 + 上下外边距 1
#endif
#define SHELF_CANVAS_WIDTH 160.0f
#define SHELF_CANVAS_HEIGHT 116.0f
#ifdef LOOK_VARIANT_CARDS
/// 牌面的大小：堆叠里的牌、平铺里的牌和拖入时的虚线牌都是这么大。
#define SHELF_CARD_WIDTH 64.0f
#define SHELF_CARD_HEIGHT 86.0f
#define SHELF_CARD_RADIUS 5.0f
/// 平铺的格子：牌面在格子顶部居中，名字写在下面；行与行之间留 10，上下各留 8，第一行的牌悬停抬起时不会被裁掉。
#define SHELF_SLOT_WIDTH 70.0f
#define SHELF_SLOT_HEIGHT 120.0f
#define SHELF_ROW_GAP 10.0f
#define SHELF_GRID_INSET 8.0f
#define SHELF_ROW_PITCH (SHELF_SLOT_HEIGHT + SHELF_ROW_GAP)
#else
#define SHELF_CARD_SIZE 84.0f
#endif
#define SHELF_ACTION_BAR_HEIGHT 50.0f   // 底部快速操作按钮栏（估算窗口大小用）
#define SHELF_ACTION_TILE_HEIGHT 94.0f  // 拖入时展开的快速操作卡片
#define SHELF_MAX_ACTIONS 4

// 计时器
#define SHELF_TIMER_LEAVE 1             // 拖动离开后稍等再确认
#define SHELF_TIMER_TOAST 2
#define SHELF_TIMER_BUSY 3
#define SHELF_TIMER_CLOSE_EMPTY 4
#define SHELF_TIMER_MERGE 5             // 拖着标题栏停在另一个置物架上：停够了才可以合并

typedef enum ShelfEntranceKind
{
    ENTRANCE_PLAIN,             // 快捷键、托盘菜单等主动打开：轻微浮现
    ENTRANCE_SHAKE,             // 晃动呼出：从光标处弹出
    ENTRANCE_EDGE               // 拖到屏幕边缘呼出：从该边缘滑入
} ShelfEntranceKind;

/// 牌在堆叠画布上的姿态：中心位置、角度、缩放和透明度。
typedef struct CardPose
{
    float x, y, angle, scale, opacity;
} CardPose;

/// 按真实时间推进、一帧最多前进一点的时钟，见 frame_clock_step。
typedef struct MotionClock
{
    double elapsed;
    double lag;
    double last;
    bool has_last;
} MotionClock;

typedef struct Tween Tween;
typedef void (*TweenApply)(ShelfWindow *shelf, Tween *tween, float t);

/// 一段逐帧动画：从 start 起等 delay，再在 duration 内推进；apply 收到缓动后的进度 0–1。
struct Tween
{
    double start, delay, duration;
    TweenApply apply;
    TweenApply completed;
    int card;                   // 堆叠里的第几张牌，-1 表示不是牌
    ShelfItem *item;            // 拿着引用
    CardPose from, to;
    float control_x, control_y;
    float a, b;
    int number;
    bool flag;
    int window_from[4];         // 窗口大小过渡：起点的左、上、宽、高（屏幕像素）
    int window_to[4];
    int window_last[4];         // 上一帧已经设到的位置和大小，没变时不再移动窗口
};

/// 一段动画结束（或被打断）时要做的收尾，可以叠加。
enum
{
    CLEANUP_REST_CARDS = 1 << 0,        // 牌回到原位
    CLEANUP_STACK_MARK = 1 << 1,        // 角标和堆叠下面的文字恢复
    CLEANUP_ITEM_MOTION = 1 << 2,       // 列表各行恢复原样
    CLEANUP_UNPIN = 1 << 3,             // 堆叠和列表恢复自动大小
    CLEANUP_UPDATE_VIEWS = 1 << 4,      // 按现在的状态显示堆叠或列表
    CLEANUP_WINDOW_SIZE = 1 << 5,       // 窗口定到最终大小
    CLEANUP_REMOVE = 1 << 6,            // 移除动画播完：真正移除
    CLEANUP_FAN = 1 << 7,               // 纸牌：不再停在拖入时摊开的样子
};

typedef enum RemoveKind
{
    REMOVE_VISIBLY,             // 拖出之后
    REMOVE_BY_USER              // 手动移除，之后可以撤销
} RemoveKind;

typedef enum ToastAction
{
    TOAST_ACTION_NONE,
    TOAST_ACTION_UNDO,
    TOAST_ACTION_CONVERT
} ToastAction;

/// 被另一个置物架拖到上面时这边显示的提示：没有、正在蓄力（松手不合并）、可以合并。
typedef enum MergeHint
{
    MERGE_HINT_NONE,
    MERGE_HINT_CHARGING,
    MERGE_HINT_ARMED
} MergeHint;

/// 鼠标能点到的地方。
typedef enum ShelfTarget
{
    TARGET_NONE,
    TARGET_HEADER,              // 标题栏空白处：拖动窗口
    TARGET_PIN,
    TARGET_MODE,
    TARGET_VIEW,
    TARGET_MORE,
    TARGET_CLOSE,
    TARGET_EMPTY,               // 空置物架：拖动窗口
    TARGET_STACK,
    TARGET_ROW,
    TARGET_ROW_REMOVE,
    TARGET_GRID,                // 纸牌：平铺里牌之间的空白
    TARGET_SCROLLBAR,
    TARGET_TOAST,
    TARGET_TOAST_ACTION,
    TARGET_ACTION,              // 底部的快速操作按钮（序号放在 row 里）
    TARGET_BLOCKED              // 正在处理的遮罩
} ShelfTarget;

/// 这一帧的排版结果（DIP，相对窗口左上角，还没有经过整体的缩放和平移）。
typedef struct ShelfLayout
{
    float width, height;
    GfxRect pin, mode, view, more, close, title;
    GfxRect body;
    GfxRect stack, list;        // 堆叠和列表各自占的范围（动画期间可能固定为别的大小）
    float canvas_x, canvas_y, canvas_scale;     // 堆叠画布左上角和缩放
    GfxRect caption, detail;
    GfxRect drop_zone;
    float drop_canvas_x, drop_canvas_y, drop_canvas_scale;     // 纸牌：放置区域里摊开的牌，176 × 108 的画布按比例缩进去
    GfxRect drop_text;
    GfxRect toast, toast_glyph, toast_text, toast_action;
    bool scrollable;
    GfxRect scroll_track, scroll_thumb;
    int columns;                // 纸牌：平铺一行放几张
    float extent;               // 纸牌：平铺的总高度
    GfxRect action_divider;
    GfxRect action_buttons[SHELF_MAX_ACTIONS];
    GfxRect tiles_host;         // 拖入时快速操作卡片那一行（随动画长高），卡片在里面显示
    GfxRect tiles[SHELF_MAX_ACTIONS];
} ShelfLayout;

struct ShelfWindow
{
    LONG references;
    UiWindow *ui;
    HWND handle;
    ShelfModel *model;
    bool closing, closed;
    ShelfWindow *next;

    // 设置
    int size;                   // QuickShelfSize
    bool move_mode;             // 拖出时移动（否则复制）

    // 出场和离场
    ShelfEntranceKind entrance;
    ScreenEdge edge;
    float surface_opacity, surface_scale, surface_dx, surface_dy;
    float origin_x, origin_y;   // 缩放的中心（相对窗口大小 0–1）
    bool popping;
    MotionClock pop_clock;
    double pop_duration, pop_fade, pop_start_scale;     // 简洁
    Ease pop_ease;
    float pop_shift_x, pop_shift_y;     // 低性能模式：内容在窗口里移回原处（纸牌只用 y：内容升起）
    bool sliding;
    MotionClock slide_clock;
    int slide_from_x, slide_from_y, slide_to_x, slide_to_y, slide_x, slide_y;
    double slide_duration;
    Ease slide_ease;
    bool slide_keep_in_work_area;
    // 简洁：关闭的动画
    bool leaving;               // 正在播放关闭的动画
    MotionClock leave_clock;
    float leave_from_opacity, leave_from_scale, leave_shift_x, leave_shift_y, leave_from_dx, leave_from_dy;
    /// 纸牌：正在替置物架出场或离场的那张牌。正常播完时出场的让置物架现身、离场的关闭窗口；出场途中置物架又被复用或要关闭时
    /// 牌立即撤下、置物架现身（shelf_finish_entrance）；窗口关闭时剩下的动画直接取消，不再回调置物架（shelf_cancel_card）。
    struct ShelfCard *card;
    bool flipped_on_horizontal_axis;    // 出场时那张牌是上下翻开的，关闭时按同样的方向翻回去

    // 内容的动画
    Tween *tweens;
    int tween_count, tween_capacity;
    bool motion_running;
    MotionClock motion_clock;
    unsigned cleanup;
    ShelfItem **removing;       // CLEANUP_REMOVE：要移除的项目
    int removing_count;
    RemoveKind remove_kind;
    bool remove_expanded;       // 移除时是不是展开着列表（决定之后怎么补位）
    bool remove_expanded_before;    // 手动移除前是不是展开着列表（撤销时回到列表）
    bool remove_remains;        // 移除后还剩项目：接着补位
    bool remove_closes;         // 全部移走且设置为自动关闭：移除后直接关闭
    ShelfItem *shown_before[3]; // 移除前堆叠上的三张，补位时用
    ShelfItem **rows_before;    // 移除前各行（纸牌：平铺里各张牌）和它们的位置，补位时用
    float *rows_before_x;       // 纸牌
    float *rows_before_y;
    int rows_before_count;
    bool hold_size;             // 简洁：列表移除后、补位之前不改窗口大小
    bool card_posed[3];
    CardPose card_pose[3];
    float mark_opacity;         // 角标（纸牌：牌角）和堆叠下面的文字（视图切换时淡入淡出）
    // 简洁：角标
    bool mark_override;
    int mark_count;
    MotionClock badge_clock;
    bool badge_popping;
    float badge_scale;
    // 纸牌：牌角和拖入时摊开的牌
    bool rank_hidden;           // 落下动画期间最上面那张还没落下，先藏起牌角
    bool rank_rolling;          // 牌角的数字从下面滚上来
    MotionClock rank_clock;
    float rank_shift, rank_opacity;
    FileSuit rank_suit;         // 牌角的花色；要把全部项目看一遍，只在项目变了以后重算
    bool rank_suit_known;
    bool fan_held;              // 松手后、读完拖入的内容之前，堆叠停在拖入时摊开的样子
    CardPose fan_poses[2];      // 摊开的两张牌在堆叠画布上的姿态
    bool fan_valid[2];

    // 视图
    bool shown_expanded;        // 正在显示的是列表（数量变化的刷新之后才跟上 model->expanded）
    bool switching;             // 堆叠和列表之间切换中：两个都显示，堆叠在上面
    bool stack_pinned, list_pinned;
    GfxSize stack_pin, list_pin;
    int scroll_first;           // 简洁：列表最上面一行的序号
    float scroll_offset;        // 纸牌：平铺滚过的距离（DIP）
    bool expanded_by_click;     // 纸牌：刚由单击牌堆摊开，紧接着的双击第二下不算数
    bool refresh_posted;
    bool count_changed;
    int shown_count;            // 上一次通知时的数量，变了才算“数量变化”
    bool landing;
    float landing_x, landing_y;
    int landing_added;
    bool returning;
    float returning_x, returning_y;
    ShelfItem **returning_items;
    int returning_count;
    ShelfLayout layout;
    unsigned label_generation;
    float mode_widths[2];       // DIP，复制和移动分别量一次。
    wchar_t *stack_caption, *stack_detail;    // 多个项目的汇总；单项直接用项目自己的文字。

    // 鼠标
    ShelfTarget hovered;
    int hovered_row;
    ShelfTarget pressed;
    int pressed_row;
    bool scroll_hovered;
    bool double_click;
    float press_x, press_y;
    bool stack_pressed, list_pressed, defer_selection;
    ShelfItem *pressed_item;
    int selection_anchor;
    bool scroll_dragging;
    float scroll_drag_offset;
    bool moving_window;

    // 拖入
    struct ShelfDropTarget
    {
        IDropTarget base;       // 系统拿到的是它的地址，回调里由它找回置物架
        ShelfWindow *shelf;
    } drop_target;
    DropTargetHelper *drop_helper;
    bool drop_registered;
#ifdef __linux__
    // X11（XWayland）应用的拖动落不到原生 Wayland 窗口上，这个接放窗口盖在置物架上接住它（见 xdnd_linux.h）。
    void *xdnd_sink;
#endif
    bool drag_inside, drag_from_self, image_left;
    DropContentKind drag_kind;
    DropImageType last_description_type;
    const wchar_t *last_description;
    bool has_last_description;
    ULONGLONG last_drag_over;
    LARGE_INTEGER drag_over_handled;
    DWORD drag_effects;
    bool overlay_visible;
    bool overlay_hit;
    Anim overlay_opacity;
    Anim fan;                   // 纸牌：拖进来时原来那一摞最上面两张摊开（0 到 1）
    int incoming;               // 纸牌：拖进来几个文件（角标上的“+N”），读不到文件列表时是 0
    Anim body_opacity;
    bool body_hidden_after_fade;
    StringId drop_zone_text;
    int receiving;

    // 拖出
    bool dragging_out;
    ShelfItem **dimmed;
    int dimmed_count;

    // 正在处理
    bool busy_shown;
    LARGE_INTEGER busy_shown_at;
    Anim busy_opacity;
    MotionClock ring_clock;

    // 提示
    wchar_t *toast_text;
    bool toast_success;
    bool toast_visible;
    Anim toast_opacity;
    Anim toast_offset;
    ToastAction toast_action;
    ShelfRemoval *undo;
    bool undo_expanded;
    bool close_after_toast;
    ShelfItem **convert_items;
    int convert_count;
    bool toast_timer_running;

    // 快速操作
    QuickActionList actions;
    wchar_t *action_labels[SHELF_MAX_ACTIONS];
    wchar_t *action_tooltips[SHELF_MAX_ACTIONS];
    bool tiles_expanded;        // 拖到底部时快速操作展开成卡片
    Anim tiles_height;          // 卡片那一行的高度：收起时和按钮栏一样高
    Anim tiles_opacity;
    Anim bar_opacity;           // 按钮栏：卡片展开时淡出
    Anim tile_highlight[SHELF_MAX_ACTIONS];     // 拖到这张卡片上：强调色的底、略微放大、文字变色（0–1）
    Anim frame_opacity;         // 放置区域的虚线框和底色：拖到快速操作上时变淡
    int hovered_action;         // 拖到了哪张卡片上，-1 是没有

    // 合并和拆分
    bool merged_away;           // 项目已经全部交给别的置物架，正在关闭：不记进最近关闭
    bool drag_from_shelf;       // 拖进来的是另一个置物架上的项目：松手后由那个置物架交过来，不读拖放数据
    bool merge_watch;           // 正拖着标题栏移动：留意光标下面是不是别的置物架
    ShelfWindow *merge_target;  // 光标下面的另一个置物架，已经停够了：松手就合并进去（拿着引用）
    ShelfWindow *merge_candidate;   // 光标刚到上面、还在蓄力的另一个置物架（拿着引用）
    MergeHint merge_hint;       // 被别的置物架拖到上面时这边的提示
    Anim merge_dim;             // 松手就合并时这边内容的透明度
    Anim merge_charge_opacity;  // 蓄力的底色：从中间涨满
    Anim merge_charge_scale;
    Anim drop_label_opacity;    // 放置区域里的图标和文字（蓄力时先不显示）
    Anim drop_zone_scale;       // 可以合并时放置区域轻轻弹一下
    int pulse_phase;            // 弹一下的两段：1 放大，2 回到原样

    // 拖入的数据（拖动经过期间拿着引用）和右键菜单针对的项目
    IDataObject *drag_data;
    ShelfItem **menu_items;
    int menu_count;
};

// ───────────── shelf_window.c ─────────────

ShelfWindow *shelf_window_create(void);
ShelfWindow *shelf_window_retain(ShelfWindow *shelf);
void shelf_window_release(ShelfWindow *shelf);
void shelf_window_show_at(ShelfWindow *shelf, POINT center, bool activate, ShelfEntranceKind entrance, ScreenEdge edge);
void shelf_window_apply_settings(ShelfWindow *shelf);
bool shelf_window_is_reusable(const ShelfWindow *shelf);
bool shelf_window_is_animating(const ShelfWindow *shelf);
bool shelf_window_is_visible(const ShelfWindow *shelf);
void shelf_window_close_now(ShelfWindow *shelf);
void shelf_window_close_animated(ShelfWindow *shelf);
float shelf_scale(const ShelfWindow *shelf);
void shelf_layout(ShelfWindow *shelf);
void shelf_repaint(ShelfWindow *shelf);
void shelf_update_window_size(ShelfWindow *shelf);
void shelf_target_size(ShelfWindow *shelf, bool expanded, float *width, float *height);
void shelf_keep_in_work_area(ShelfWindow *shelf);
void shelf_fit_resized(RECT rect, int width, int height, int *left, int *top);
void shelf_update_title(ShelfWindow *shelf);

/// 换风格：theme_set_look 之前由原来的风格调 leave（动画走到终点，收起提示），之后由新风格调 adopt（回调换成这一份，
/// 按这种风格的大小重新排版）。见 look_switch.c。
void shelf_window_leave_look(ShelfWindow *shelf);
void shelf_window_adopt_look(ShelfWindow *shelf);
void shelf_refresh_views(ShelfWindow *shelf);
void shelf_request_refresh(ShelfWindow *shelf);
#ifndef LOOK_VARIANT_CARDS
int shelf_visible_rows(ShelfWindow *shelf);
#endif
void shelf_scroll_into_view(ShelfWindow *shelf, int index);
#ifndef LOOK_VARIANT_CARDS
void shelf_set_scroll(ShelfWindow *shelf, int first);
#endif

#ifdef LOOK_VARIANT_CARDS
/// 平铺一行放几张：按平铺区域的宽度；平铺时窗口的宽度由设置的尺寸定（小、中、大一行 4、5、6 张）。
int shelf_grid_columns_for(float width);
int shelf_grid_columns(int size);
int shelf_grid_rows_for(int count, float width);
/// 放下 rows 行牌需要的高度，含上下的空白。
float shelf_grid_height(int rows);
int shelf_grid_columns_now(ShelfWindow *shelf);
/// 平铺里第 index 张牌的格子（70 × 120）和牌面（64 × 86）在窗口里的位置；按现在的滚动位置算。
GfxRect shelf_tile_rect(ShelfWindow *shelf, int index);
GfxRect shelf_tile_card_rect(ShelfWindow *shelf, int index);
/// 牌面（悬停时抬起后的位置）右上角的移除按钮：18 × 18 的圆，伸出牌角。
GfxRect shelf_tile_remove_rect(GfxRect face);
/// 平铺现在能看到的牌（上下各多一行）：first 到 last；没有时 last < first。
void shelf_visible_tiles(ShelfWindow *shelf, int *first, int *last);
/// 平铺滚到 offset（DIP），超出范围时取到边上。
void shelf_set_scroll_offset(ShelfWindow *shelf, float offset);
#else
/// 列表第 index 行（容器，含圆角底色）在窗口里的位置；按现在的滚动位置算。
GfxRect shelf_row_rect(ShelfWindow *shelf, int index);
/// 行首图标的位置。
GfxRect shelf_row_thumb_rect(ShelfWindow *shelf, int index);
#endif
/// 堆叠上第 card 张牌的原位。
CardPose shelf_card_home(int card);
/// 窗口里的点换到堆叠画布上，或者反过来。
void shelf_to_canvas(ShelfWindow *shelf, float x, float y, float *cx, float *cy);
/// 按设置的尺寸，收起时置物架有多宽。
float shelf_width_for(int size);
/// 重新生成快速操作的名称和提示（换了操作或界面语言）。
void shelf_rebuild_actions(ShelfWindow *shelf);

// ───────────── shelf_input.c ─────────────

bool shelf_on_mouse(UiWindow *window, void *context, const UiMouseEvent *event);
bool shelf_on_key(UiWindow *window, void *context, unsigned key);
LPCWSTR shelf_on_cursor(UiWindow *window, void *context, float x, float y);
bool shelf_on_message(UiWindow *window, void *context, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
void shelf_register_drop_target(ShelfWindow *shelf);
void shelf_revoke_drop_target(ShelfWindow *shelf);
void shelf_show_toast(ShelfWindow *shelf, const wchar_t *message, bool success, ToastAction action);
void shelf_hide_toast(ShelfWindow *shelf);
void shelf_update_busy_overlay(ShelfWindow *shelf, bool waited);
void shelf_remove_items(ShelfWindow *shelf, ShelfItem **items, int count);
void shelf_end_drag(ShelfWindow *shelf);
void shelf_close_if_still_empty(ShelfWindow *shelf);
void shelf_notify(void *context, const wchar_t *message, bool success);
void shelf_remove_visibly(void *context, ShelfItem **items, int count);
void shelf_add_paths(ShelfWindow *shelf, const StrList *paths);
/// 恢复最近关闭的置物架：在后台读文件信息，读完放入并提示恢复了几项。
void shelf_restore_paths(ShelfWindow *shelf, const StrList *paths);
/// 快速操作收起成按钮栏的高度（立即到位）。
void shelf_reset_tiles(ShelfWindow *shelf);
/// 拖到哪张快速操作卡片上（-1 是没有）：那张卡片铺上强调色、略微放大，放置区域变淡。force 时直接到位。
void shelf_set_hovered_action(ShelfWindow *shelf, int action, bool force);
/// 快速操作的按钮按下或执行完：生成的新文件交给旁边的新置物架等。
void shelf_produced(void *context, const StrList *paths);

#ifdef LOOK_VARIANT_CARDS
/// 拖进来时内容区淡出，原来那一摞摊开、放置区域淡入；只改透明度，不重新排版。
/// 角标上标出拖进来几个文件（从 data 读）；data 为 NULL 时（合并，没有拖放数据）标出 incoming。
void shelf_show_drop_overlay(ShelfWindow *shelf, IDataObject *data, int incoming);
#else
/// 拖进来时内容区淡出，放置区域淡入；只改透明度，不重新排版。
void shelf_show_drop_overlay(ShelfWindow *shelf);
#endif

// ───────────── shelf_merge.c ─────────────

/// 正在从某个置物架拖出的项目。放到另一个置物架或“新置物架”上时，对方不去读拖放数据，只在这里记下自己；
/// 拖动结束后由来源置物架把项目交过去：文本、链接、缩略图和临时附件都跟着走，是移动而不是再复制一份。同一时刻只有一次。
typedef struct ShelfDragSession
{
    ShelfWindow *source;
    ShelfWindow *target;        // 放到了这个置物架上（拿着引用）
    float drop_x, drop_y;       // 松手的位置（相对那个窗口，DIP）
    bool split;                 // 放到了“新置物架”上：拆成一个新的置物架
} ShelfDragSession;

void shelf_drag_session_begin(ShelfDragSession *session);
void shelf_drag_session_end(void);
/// 拖动数据是不是这一次拖出的（按来源置物架的标识判断）；不是时返回 NULL。
ShelfDragSession *shelf_drag_session_from(IDataObject *data);
void shelf_drag_session_drop_on(ShelfDragSession *session, ShelfWindow *target, float x, float y);

/// 新置物架出现的位置（屏幕像素）：贴着来源置物架的下边、左边对齐、宽度是收起时的宽度；下面放不下时放到上边。
typedef struct SplitPlace
{
    RECT rect;
    bool below;
} SplitPlace;

bool shelf_split_place(ShelfWindow *source, SplitPlace *place);
/// “新置物架”小窗口：从列表里只拖出一部分时贴着来源置物架伸出来，拖动结束就收起。收起时返回它刚才所在的位置。
void split_pad_show_for(ShelfWindow *source);
bool split_pad_dismiss(SplitPlace *place);
bool split_pad_is_shown(void);
void split_pad_close_shared(void);
/// Linux：拖出结束时问一下松手是不是落在“新置物架”上；问过就清掉。
bool split_pad_take_request(void);

/// 接收另一个置物架交来的项目，从 (x, y)（相对本窗口）飞进堆叠或列表。这边已经有的不重复放；一个也没放进来时提示。
int shelf_receive(ShelfWindow *shelf, ShelfItem **items, int count, ShelfModel *from, float x, float y);
/// 项目已经交给别的置物架，这边放手：全部交出去且设置为自动关闭时直接淡出关闭，否则播放移除动画。
void shelf_let_go(ShelfWindow *shelf, ShelfItem **items, int count);
/// 把一部分项目拆到 place 处的新置物架；from 是项目飞出的位置（屏幕像素），NULL 时从新置物架靠近这边的一侧。
void shelf_split_off(ShelfWindow *shelf, ShelfItem **items, int count, const SplitPlace *place, const POINT *from);
/// 右键菜单里的“移到新置物架”。
void shelf_split_off_from_menu(ShelfWindow *shelf, ShelfItem **items, int count);

/// 按住标题栏（或空置物架）拖动窗口，松开后才返回；途中拖到别的置物架上停一下再松手就合并进去。
void shelf_move_by_hand(ShelfWindow *shelf);
void shelf_merge_on_moved(ShelfWindow *shelf);
void shelf_merge_on_dwell(ShelfWindow *shelf);
void shelf_update_merge_pulse(ShelfWindow *shelf);
void shelf_reset_merge_charge(ShelfWindow *shelf);
/// 窗口关闭：放掉合并途中拿着的别的置物架。
void shelf_merge_forget(ShelfWindow *shelf);

// ───────────── shelf_motion.c ─────────────

bool clock_tick(MotionClock *clock, double time);
void shelf_animate(UiWindow *window, void *context, double time);
#ifdef LOOK_VARIANT_CARDS
/// 主动打开时的弹出动画：窗口从下方升起一点，内容淡入并放大到原尺寸。
void shelf_animate_in(ShelfWindow *shelf);
/// 晃动或从边缘呼出时的出场：截下置物架作为牌面，置物架隐身期间由牌表演，落定时置物架现身。截不下时直接现身。
/// 晃动时牌从光标处（cursor，屏幕像素）翻开。
void shelf_deal_in(ShelfWindow *shelf, POINT cursor);
/// 出场的牌还没落定时直接让置物架现身。
void shelf_finish_entrance(ShelfWindow *shelf);
/// 窗口关闭时撤下还在播放的牌。
void shelf_cancel_card(ShelfWindow *shelf);
/// 关闭：一张牌先盖住置物架，置物架隐身后牌再离场，播完才真正关闭窗口。截不下置物架时返回 false。
bool shelf_play_exit(ShelfWindow *shelf);
#else
void shelf_animate_in(ShelfWindow *shelf, POINT cursor);
void shelf_start_leave(ShelfWindow *shelf);
#endif
void shelf_stop_slide(ShelfWindow *shelf);
void shelf_stop_pop(ShelfWindow *shelf);
void shelf_finish_motion(ShelfWindow *shelf);
void shelf_stop_motion(ShelfWindow *shelf);
bool shelf_play_landing(ShelfWindow *shelf, float x, float y, int added);
#ifdef LOOK_VARIANT_CARDS
/// 平铺状态下拖入：新文件的牌从松手处划一道弧落到自己的格子，快落下时名字淡入。
bool shelf_play_grid_landing(ShelfWindow *shelf, float x, float y, int added);
/// 撤销移除之后放回：平铺里放回的牌从下面一点升回原来的格子；堆叠里放回的牌从“撤销”按钮处飞回去。
bool shelf_play_return(ShelfWindow *shelf, float x, float y, ShelfItem **items, int count);
#else
bool shelf_play_list_landing(ShelfWindow *shelf, float x, float y, int added, double lead);
bool shelf_play_return(ShelfWindow *shelf, float x, float y, ShelfItem **items, int count, double lead);
double shelf_grow_for_rows(ShelfWindow *shelf);
#endif
void shelf_remove_with_motion(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool animate);
void shelf_animate_view_switch(ShelfWindow *shelf);
void shelf_finish_after(ShelfWindow *shelf, double end);
#ifdef LOOK_VARIANT_CARDS
/// 新牌落进这一摞：牌角的数字从下面滚上来。
void shelf_roll_rank(ShelfWindow *shelf);
/// 拖入时摊开最上面两张（fan 从 0 动到 1）；没有动画效果时直接摊开。
void shelf_fan_out(ShelfWindow *shelf);
/// 松手时摊开的两张牌换到堆叠的画布上记下来（要在放置区域收起之前）；没有摊开的牌时返回 false。
bool shelf_take_fan(ShelfWindow *shelf);
/// 松手后堆叠停在 shelf_take_fan 记下的摊开的样子，等新牌落进中间再合拢；没有放进新项目时由 shelf_close_fan 直接合拢。
void shelf_hold_fan(ShelfWindow *shelf);
void shelf_close_fan(ShelfWindow *shelf);
/// 拖入时摊开的第 index 张牌（0 在左、1 在右）在堆叠画布上的姿态；progress 是摊开的程度。
CardPose shelf_fan_pose(int index, float progress);
#else
void shelf_card_landed_mark(ShelfWindow *shelf, int count);
#endif
void shelf_dim_dragged(ShelfWindow *shelf, ShelfItem **items, int count);
void shelf_restore_dimmed(ShelfWindow *shelf);
/// 立即移除（不播放动画）并做后续的事：手动移除时提示可以撤销，拖出后移空且设置为自动关闭时关闭置物架。
void shelf_perform_remove(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool expanded_before);

// ───────────── shelves.c ─────────────

void shelves_on_closed(ShelfWindow *shelf);

/// 打开着的置物架，顺着 next 往下走。
ShelfWindow *shelves_first(void);

/// 换风格：打开着的和待命的置物架先按原来的风格收尾（leave），换好颜色后再按新风格重新排版（adopt）。
void shelves_leave_look(void);
void shelves_adopt_look(void);

/// 光标是不是在某个置物架上（屏幕像素）。
bool shelves_contain_point(POINT point);

/// 在 source 旁边打开一个新置物架放这些文件（快速操作生成的）；source 为 NULL 或已关闭时在光标处打开。
void shelves_open_beside(ShelfWindow *source, const StrList *paths);
