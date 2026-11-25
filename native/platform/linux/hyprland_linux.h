// Hyprland 的 IPC：全局光标位置和显示器的位置、工作区。
// 别的合成器上没有这个 socket，调用方要自己退回估算值（光标取显示器中间，工作区等于整个屏幕）。
#pragma once

#include "base.h"

typedef struct HyprMonitor
{
    wchar_t *name;                  // 连接名，例如 DP-3
    RECT rect;                      // 逻辑坐标（已按缩放和旋转折算）
    int reserved_left, reserved_top, reserved_right, reserved_bottom;
    int scale;
} HyprMonitor;

/// 这个会话里能不能连上 Hyprland 的 IPC。
bool hyprland_available(void);

/// 直接发一条 IPC 命令，返回 UTF-8 的回复（调用方 free）。失败时返回 NULL。
char *hyprland_request(const char *command);

/// Hyprland 的事件流套接字（.socket2.sock）路径，用来收 activewindow 这类事件。
bool hyprland_event_path(char *out, size_t size);

/// 全局光标位置（逻辑坐标）。拿不到时返回 false。
bool hyprland_cursor_position(int *x, int *y);

/// 当前的显示器列表，最多 max 个，返回个数。out 里名字要由 hyprland_monitors_free 释放。
int hyprland_monitors(HyprMonitor *out, int max);
void hyprland_monitors_free(HyprMonitor *monitors, int count);

/// 纯解析，不访问 IPC，测试用。格式是 hyprctl -j monitors 的输出。
int hyprland_parse_monitors(const wchar_t *json, HyprMonitor *out, int max);

typedef struct HyprClientAt
{
    bool found;                 // 光标底下有没有窗口
    bool xwayland;              // 这个窗口是不是 XWayland 的程序
    int pid;                    // 窗口所属进程；为 0 表示 Hyprland 没报
    RECT rect;                  // 窗口在逻辑坐标里的位置
    wchar_t title[256];         // 窗口标题：同一个程序开着几个窗口时，靠它认出是哪一个
} HyprClientAt;

/// 光标（逻辑坐标）底下最上面的窗口：只算当前工作区里可见的窗口。
/// 光标下面是 Wayland 原生窗口时 xwayland 为 false——这种时候拖动归合成器和 GTK 管。
bool hyprland_client_at(int x, int y, HyprClientAt *out);

// ───────────── 快捷键 ─────────────
// Hyprland 的全局快捷键协议只让程序“登记”快捷键，按键仍要用户在配置里绑；
// 为了让设置里的快捷键直接可用，这里在运行时用 keyword bind 把组合键绑到一条命令，不改配置文件。

typedef enum HyprBindState
{
    HYPR_BIND_FREE,     // 这个组合键没人用
    HYPR_BIND_OURS,     // 已经绑到了我们自己（上一次运行留下的，或者用户写进了配置）
    HYPR_BIND_OTHER,    // 被别的功能占着
} HyprBindState;

/// 组合键现在的去向。modmask 是 Hyprland 的位：SHIFT 1、CTRL 4、ALT 8、SUPER 64。
HyprBindState hyprland_bind_state(unsigned modmask, const char *key, const char *command);

/// 把组合键绑到 command（经 sh -c 执行）。调用前先用 hyprland_bind_state 确认没人占用。
bool hyprland_bind_hotkey(unsigned modmask, const char *modifiers, const char *key, const char *command);

/// 去掉以前运行时留下的、描述是 DropShelf 的绑定（上次没正常退出时会留下）。用户写进配置的会随重载回来。
void hyprland_clear_own_binds(void);

/// 撤掉 hyprland_bind_hotkey 加的绑定。
bool hyprland_unbind_hotkey(const char *modifiers, const char *key);

// ───────────── 窗口规则 ─────────────

/// 让 Hyprland 把 app_id 对应的普通窗口浮动显示。
/// 主窗口、引导和“关于”都是固定布局的 Windows 式窗口，在平铺合成器里被平铺会挤成一列；
/// 规则只加到当前会话里，不改配置文件，重载配置就消失。
bool hyprland_float_windows(const char *app_id);