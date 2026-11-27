// X11（XWayland）应用发起的拖放：Hyprland 只把 Wayland 源的拖动转给 X11 应用，
// 反方向没有实现——X11 源拖动时光标下的原生 Wayland 窗口（探针、置物架）收不到任何事件。
// 这个模块在探针/置物架的位置放一个看不见的 X11 窗口，用 Xdnd 协议把拖动接住，
// 读出的文字、文件或图片交给置物架，和 Wayland 源走同一套处理。
#pragma once

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
/// 坐标以置物架左上角为原点，和 GTK 拖放回调里拿到的一样。drop 返回置物架有没有真的收下。
XdndSink *xdnd_add_sink(void *context,
    void (*enter)(void *context, XdndKind kind, double x, double y),
    void (*motion)(void *context, XdndKind kind, double x, double y),
    void (*leave)(void *context),
    bool (*drop)(void *context, const XdndPayload *payload, double x, double y),
    bool (*frame)(void *context, int *x, int *y, int *width, int *height));
void xdnd_remove_sink(XdndSink *sink);

// ───────────── 从置物架拖到 XWayland 程序 ─────────────

/// 单张图片拖出时连图片内容一起给对方（聊天程序只把图片格式当图片收）；再大就只给文件，别让一次拖出搬几十兆。
#define XDND_MAXIMUM_IMAGE_BYTES ((size_t)32 * 1024 * 1024)

/// 读取单张拖出图片，超限或不支持时返回 NULL。调用方释放 GBytes。
GBytes *xdnd_read_drag_image(const char *path, const char **mime);

/// 把这次要拖出的东西交给 X11 拖动源：paths 是 UTF-8 的文件路径（没有文件时传 NULL），
/// text 是纯文字；html 是一份 HTML（混合拖出时把文字和图片放在一起），没有时传 NULL。
/// move_allowed 表示置物架那边按“移动”算；image 和 image_mime 可为空。
void xdnd_source_set_payload(const char *const *paths, int count, const char *text, const char *html, bool move_allowed,
    GBytes *image, const char *image_mime);

/// 再排一批：这次拖动真正放下以后，自动按排队的顺序一批一批地送给同一个窗口
/// （每一批都被对方收下才接着送下一批）。paths 是这一批的文件，text 是这一批的纯文字，内容会被复制。
/// 没排上（空的，或者排满了）返回 false。
bool xdnd_source_add_batch(const char *const *paths, int count, const char *text);

/// 把文字和文件路径里的图片合成一份 HTML：文字按段落排，图片用 file:// 地址。
/// 什么都没排出来时返回 NULL。调用方负责 free。
char *xdnd_source_html_build(const char *text, const char *const *paths, int count);

/// 从置物架往外拖的时候看一眼光标底下：停在 XWayland 程序上够久、而且真能找到它的 X 窗口时返回 true。
/// 只在这时候才值得把 GTK 的 Wayland 拖动换成自己的 Xdnd。
bool xdnd_source_should_take_over(void);

/// 这次拖出只按复制（复制模式，或者文字、链接这种本来就不能移走的）：可以保留 Hyprland 的旧 Xdnd，
/// 只接管它的 selection 数据，免得目标程序同时见到新旧两个拖动源。
bool xdnd_source_copy_only(void);

/// 保留 Hyprland 的旧 Xdnd，只临时接管它的 selection 数据。失败时返回 false，交给自建拖动源。
bool xdnd_source_assist_begin(void);

/// GTK 拖动结束后调用：等接收方把数据取完，再交回 selection。
void xdnd_source_assist_finish(void);

/// 接管这次拖出（先把 GTK 的拖动取消掉再调用）。真正开始发 Xdnd 要等一小会儿。
bool xdnd_source_begin(void);

/// 上一次拖出改由自己的 X11 拖动源送了吗。送了的话 parts 里第 n 位表示第 n 批（0 是跟着拖动走的那批，
/// 之后按 xdnd_source_add_batch 的顺序）被对方用 XdndFinished 确认收下了。没接管时 GTK 报的动作就是整次的结果。
bool xdnd_source_delivered(uint32_t *parts);

/// 自己正在（或者马上要）当 X11 拖动源。
bool xdnd_source_active(void);

/// 取消接管；收尾会照常通知回去。
void xdnd_source_abort(void);