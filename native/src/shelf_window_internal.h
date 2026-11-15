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

// 计时器
#define SHELF_TIMER_LEAVE 1             // 拖动离开后稍等再确认
#define SHELF_TIMER_TOAST 2
#define SHELF_TIMER_BUSY 3
#define SHELF_TIMER_CLOSE_EMPTY 4

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

/// 光标是不是在某个置物架上（屏幕像素）。
bool shelves_contain_point(POINT point);
