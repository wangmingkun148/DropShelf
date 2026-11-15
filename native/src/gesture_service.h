// 在独立线程上用原始输入（Raw Input）监听按住鼠标时的晃动，以及拖到屏幕边缘后的停留。
// 原始输入由系统异步投递，不像低级鼠标钩子那样每次移动都要等本程序处理完才放行，
// 因此本程序繁忙、被系统降频或回收内存时都不会拖慢鼠标。
// 识别到手势后发消息给界面线程，由界面线程用探针窗口确认是否真的在拖放。
#pragma once

#include "base.h"
#include "gesture.h"

typedef enum GestureEvent
{
    GESTURE_SHAKE = 1,          // 识别到晃动
    GESTURE_EDGE_DWELL,         // 拖到屏幕边缘并停留了设定的时间
    GESTURE_RELEASED            // 识别到手势后，按住的鼠标键被松开
} GestureEvent;

/// 开始监听。识别到手势时向 window 投递 message：wparam 是 gesture_event_pack 的结果，lparam 是光标位置。
bool gesture_service_start(HWND window, UINT message, int sensitivity);
void gesture_service_stop(void);
bool gesture_service_is_running(void);

void gesture_service_set_sensitivity(int sensitivity);

/// 选择用晃动、屏幕边缘停留或两者来呼出置物架（TRIGGER_*）。
void gesture_service_set_trigger(int trigger);
void gesture_service_set_edge_dwell(int dwell);

/// 屏蔽的应用在前台时暂停识别晃动和边缘停留。
void gesture_service_set_paused(bool paused);

/// 消息参数的拆解。光标位置是屏幕物理像素，可能是负数。
GestureEvent gesture_message_event(WPARAM wparam);
ScreenEdge gesture_message_edge(WPARAM wparam);
POINT gesture_message_point(LPARAM lparam);
