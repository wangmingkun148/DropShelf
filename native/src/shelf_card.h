// 置物架出场和离场时替它表演的那张牌，和 C# style 版的 ShelfCardWindow、ShelfMotion 里出场离场的部分对应。
// 晃动呼出时一张牌背从光标处翻过来；从边缘呼出时一张牌从那条边被甩上桌面。两种各有几个大体相似、细节不同的版本，
// 每次随机挑一个且不和上一次相同，看多了也不腻。关闭时，边缘呼出的甩回那条边，其余的翻回背面收走。
#pragma once

#include "anim.h"
#include "gesture.h"
#include "gfx.h"

/// 牌在某一时刻的姿态：中心相对落点的偏移 x、y（DIP），缩放，平面内的转角（度），绕翻转轴转过的角度 turn（0 正面朝外，180 背面朝外）和透明度。
/// ease 是从这一帧到下一帧的缓动，has_ease 为 false 时匀速。
typedef struct CardKey
{
    double ms, x, y, scale, angle, turn, opacity;
    bool has_ease;
    Ease ease;
} CardKey;

#define CARD_MOTION_MAX_KEYS 6

/// 一段牌的动画：按时间排好的关键帧。horizontal_axis 时绕横轴上下翻，否则绕竖轴左右翻；开头 fade_in_ms 毫秒内淡入。
typedef struct CardMotion
{
    CardKey keys[CARD_MOTION_MAX_KEYS];
    int count;
    bool horizontal_axis;
    double fade_in_ms;
} CardMotion;

double card_motion_duration(const CardMotion *motion);
/// ms 时刻的姿态：在前后两帧之间按前一帧的缓动插值。
CardKey card_motion_at(const CardMotion *motion, double ms);

/// 晃动呼出：牌背从光标处冒出来，一边放大、一边飞向落点、一边翻面，最后略微超出原大再收回。cursor 是光标相对落点中心的位置（DIP）。
CardMotion card_motion_shake_in(double cursor_x, double cursor_y);
/// 从边缘呼出：牌带着倾角从屏幕外滑进来，边走边回正，冲过落点一点再停稳。target 是落定时的位置，work 是工作区，都是屏幕像素；
/// dpi_scale 是那里的显示缩放比例。
CardMotion card_motion_edge_in(ScreenEdge edge, RECT target, RECT work, double dpi_scale);
/// 点开观察文件夹探出的角：牌从角的位置（from，屏幕像素）、大小（DIP）和转角出发，往离开边缘 edge 的方向滚着转一整圈，
/// 一边变大一边飞到 target，冲过一点再停稳。dpi_scale 是那里的显示缩放比例。
CardMotion card_motion_peek_pop(RECT target, double dpi_scale, POINT from, double from_width, double from_height, double from_angle, ScreenEdge edge);
/// 关闭：从边缘呼出的（edge 不是 EDGE_NONE）先往里收一下再甩回那条边；其余的翻回背面、缩小淡出，翻的方向和出场时一致。
CardMotion card_motion_exit(ScreenEdge edge, bool horizontal_axis, RECT target, RECT work, double dpi_scale);

typedef struct ShelfCard ShelfCard;
typedef void (*ShelfCardHandler)(void *context);

/// 取一张空闲的牌，没有就新建；建不了窗口时返回 NULL。
ShelfCard *shelf_card_rent(void);
/// 空闲时先备好一张，呼出置物架时不必再等。
void shelf_card_prewarm(void);
/// 播放一段动画，face 是牌的正面（拿着它的一份引用直到播完）。target 是牌落定（关键帧各项取默认值）时在屏幕上的位置，
/// area 是窗口能占用的范围，都是屏幕像素；dpi_scale 是那里的显示缩放比例。
/// 牌先隐身画好第一帧再现身，同时调用 shown，然后才开始计时；播完调用 finished，之后还停留两帧盖住置物架现身前的那一帧，再隐藏放回去。
void shelf_card_play(ShelfCard *card, RECT target, RECT area, double dpi_scale, GfxImage *face, const CardMotion *motion,
    ShelfCardHandler shown, ShelfCardHandler finished, void *context);
/// 换风格的翻牌（多米诺里的一张）：停 delay_ms 毫秒后，用 duration_ms 毫秒绕竖轴翻过来，翻到侧面时略微抬起。
CardMotion card_motion_look_flip(double delay_ms, double duration_ms);
/// 换风格时盖住一个窗口的那张牌：正面 front 是窗口原来的样子，摆在 from（屏幕像素）；停 delay_ms 毫秒后翻过来。
/// 背面（新风格的样子）和翻过来后的位置由 shelf_card_set_back 给出，翻到一半还没给时背面画成牌背。shown、finished 同 shelf_card_play。
void shelf_card_flip(ShelfCard *card, RECT from, double dpi_scale, GfxImage *front, double delay_ms, double duration_ms, ShelfCardHandler shown,
    ShelfCardHandler finished, void *context);
void shelf_card_set_back(ShelfCard *card, GfxImage *back, RECT to, double dpi_scale);
/// 立即停下并隐藏，不再调用 shown 和 finished。
void shelf_card_cancel(ShelfCard *card);

/// 隐身：窗口照常显示和绘制，只是不画到屏幕上；取消隐身后立即以画好的样子出现。
void window_set_cloaked(HWND window, bool cloaked);
