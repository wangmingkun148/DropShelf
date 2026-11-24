// 置物架窗口内部共用的定义：shelf_window.c（窗口、排版、绘制）、shelf_input.c（鼠标、键盘、拖入拖出、提示）、
// shelf_motion.c（动画）、shelves.c（管理多个置物架）。
#pragma once

#define COBJMACROS
#include "app.h"
#include "shelf_window.h"
#include "shelf_drag.h"
#include "shelf_model.h"
#include "strings.h"
#include "ui.h"

// ───────────── 尺寸（DIP），与 C# 版的 XAML 相同 ─────────────

#define SHELF_HEADER_HEIGHT 40.0f
#define SHELF_CAPTION_HEIGHT 38.0f      // 堆叠下面两行文字的高度（估算窗口大小用）
#define SHELF_ROW_HEIGHT 46.0f          // 估算窗口大小用的行高
#define SHELF_ROW_PITCH 44.0f           // 列表里一行实际占的高度：内容 32 + 上下内边距 5 + 上下外边距 1
#define SHELF_CANVAS_WIDTH 160.0f
#define SHELF_CANVAS_HEIGHT 116.0f
#define SHELF_CARD_SIZE 84.0f
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
    GfxRect toast, toast_glyph, toast_text, toast_action;
    bool scrollable;
    GfxRect scroll_track, scroll_thumb;
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
    double pop_duration, pop_fade, pop_start_scale;
    Ease pop_ease;
    float pop_shift_x, pop_shift_y;     // 低性能模式：内容在窗口里移回原处
    bool sliding;
    MotionClock slide_clock;
    int slide_from_x, slide_from_y, slide_to_x, slide_to_y, slide_x, slide_y;
    double slide_duration;
    Ease slide_ease;
    bool slide_keep_in_work_area;
    bool leaving;               // 正在播放关闭的动画
    MotionClock leave_clock;
    float leave_from_opacity, leave_from_scale, leave_shift_x, leave_shift_y, leave_from_dx, leave_from_dy;

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
    ShelfItem **rows_before;    // 移除前各行和它们的位置，补位时用
    float *rows_before_y;
    int rows_before_count;
    bool hold_size;             // 列表移除后、补位之前不改窗口大小
    bool card_posed[3];
    CardPose card_pose[3];
    bool mark_override;
    int mark_count;
    float mark_opacity;         // 角标和堆叠下面的文字（视图切换时淡入淡出）
    MotionClock badge_clock;
    bool badge_popping;
    float badge_scale;

    // 视图
    bool shown_expanded;        // 正在显示的是列表（数量变化的刷新之后才跟上 model->expanded）
    bool switching;             // 堆叠和列表之间切换中：两个都显示，堆叠在上面
    bool stack_pinned, list_pinned;
    GfxSize stack_pin, list_pin;
    int scroll_first;           // 列表最上面一行的序号
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
void shelf_refresh_views(ShelfWindow *shelf);
void shelf_request_refresh(ShelfWindow *shelf);
int shelf_visible_rows(ShelfWindow *shelf);
void shelf_scroll_into_view(ShelfWindow *shelf, int index);
void shelf_set_scroll(ShelfWindow *shelf, int first);

/// 列表第 index 行（容器，含圆角底色）在窗口里的位置；按现在的滚动位置算。
GfxRect shelf_row_rect(ShelfWindow *shelf, int index);
/// 行首图标的位置。
GfxRect shelf_row_thumb_rect(ShelfWindow *shelf, int index);
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

/// 拖进来时内容区淡出，放置区域淡入；只改透明度，不重新排版。
void shelf_show_drop_overlay(ShelfWindow *shelf);

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
void shelf_animate_in(ShelfWindow *shelf, POINT cursor);
void shelf_start_leave(ShelfWindow *shelf);
void shelf_stop_slide(ShelfWindow *shelf);
void shelf_stop_pop(ShelfWindow *shelf);
void shelf_finish_motion(ShelfWindow *shelf);
void shelf_stop_motion(ShelfWindow *shelf);
bool shelf_play_landing(ShelfWindow *shelf, float x, float y, int added);
bool shelf_play_list_landing(ShelfWindow *shelf, float x, float y, int added, double lead);
bool shelf_play_return(ShelfWindow *shelf, float x, float y, ShelfItem **items, int count, double lead);
double shelf_grow_for_rows(ShelfWindow *shelf);
void shelf_remove_with_motion(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool animate);
void shelf_animate_view_switch(ShelfWindow *shelf);
void shelf_finish_after(ShelfWindow *shelf, double end);
void shelf_card_landed_mark(ShelfWindow *shelf, int count);
void shelf_dim_dragged(ShelfWindow *shelf, ShelfItem **items, int count);
void shelf_restore_dimmed(ShelfWindow *shelf);
/// 立即移除（不播放动画）并做后续的事：手动移除时提示可以撤销，拖出后移空且设置为自动关闭时关闭置物架。
void shelf_perform_remove(ShelfWindow *shelf, ShelfItem **items, int count, RemoveKind kind, bool expanded_before);

// ───────────── shelves.c ─────────────

void shelves_on_closed(ShelfWindow *shelf);

/// 打开着的置物架，顺着 next 往下走。
ShelfWindow *shelves_first(void);

/// 光标是不是在某个置物架上（屏幕像素）。
bool shelves_contain_point(POINT point);

/// 在 source 旁边打开一个新置物架放这些文件（快速操作生成的）；source 为 NULL 或已关闭时在光标处打开。
void shelves_open_beside(ShelfWindow *source, const StrList *paths);
