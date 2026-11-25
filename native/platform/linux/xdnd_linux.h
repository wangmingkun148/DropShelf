// X11（XWayland）应用发起的拖放：Hyprland 只把 Wayland 源的拖动转给 X11 应用，
// 反方向没有实现——X11 源拖动时光标下的原生 Wayland 窗口（探针、置物架）收不到任何事件。
// 这个模块在探针/置物架的位置放一个看不见的 X11 窗口，用 Xdnd 协议把拖动接住，
// 读出的文字、文件或图片交给置物架，和 Wayland 源走同一套处理。
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum XdndKind
{
    XDND_KIND_NONE,
    XDND_KIND_FILES,        // text/uri-list
    XDND_KIND_TEXT,         // text/plain 一类
    XDND_KIND_IMAGE         // image/png、image/jpeg
} XdndKind;

/// 松开时读到的数据。text 和 image 都只在回调期间有效。
typedef struct XdndPayload
{
    XdndKind kind;
    const char *text;           // 文字；FILES 时是 text/uri-list 原文
    const void *image;          // 图片原始数据（PNG/JPEG）
    size_t image_size;
    const char *image_mime;     // image/png 或 image/jpeg
} XdndPayload;

/// 登记过的置物架；注销时原样传回。
typedef struct XdndSink XdndSink;

/// X11 应用正拖着东西的时候返回 true。这段时间开的层壳窗口不能要键盘：
/// 置物架一挂在键盘按需的层上，Hyprland 就把键盘从 XWayland 收走，X 那边的拖动会被直接结束。
bool xdnd_drag_in_progress(void);

/// 连上 X 并建好窗口。没有 DISPLAY 或者连不上时返回 false（其它功能照常）。
/// detected：拖动真的落到接放窗口上时调用一次，用来打开置物架（和探针确认一样）。
bool xdnd_start(void (*detected)(void));
void xdnd_stop(void);

/// 手势识别到晃动或边缘停留：跟着光标放一个探针大小的接放窗口，等 X11 应用把拖动送上来。
/// 置物架打开以后窗口会改盖在置物架上，不用再跟着光标。
void xdnd_probe_show(void);
void xdnd_probe_hide(void);

/// 置物架登记成拖放目标。x、y、width、height 是接放窗口要盖住的位置和大小
/// （和 Hyprland 的布局坐标、光标坐标同一套）；置物架不可见时 frame 返回 false。
/// 坐标以置物架左上角为原点，和 GTK 拖放回调里拿到的一样。
/// want_move：这次拖入按“移动”收尾吗（跟着置物架顶部的复制/移动开关走）；
/// 返回 true 时来源程序在松手后会删掉原文，和真的移动一样。
XdndSink *xdnd_add_sink(void *context,
    void (*enter)(void *context, XdndKind kind, double x, double y),
    void (*motion)(void *context, XdndKind kind, double x, double y),
    void (*leave)(void *context),
    void (*drop)(void *context, const XdndPayload *payload, double x, double y),
    bool (*frame)(void *context, int *x, int *y, int *width, int *height),
    bool (*want_move)(void *context));
void xdnd_remove_sink(XdndSink *sink);