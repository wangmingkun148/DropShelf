// 使用电池时，Windows 会把看起来在后台的进程调到能效核、按低频运行。置物架不抢焦点，拖文件时前台一直是别的程序，
// 常被这样对待，弹出和动画因此卡顿。有窗口显示或按住鼠标（随时可能晃动）时声明需要正常性能；
// 都没有时交还系统决定，空闲时不多耗电。系统不支持时调用失败，保持原样。
#pragma once

#include "base.h"

/// 可在任意线程调用；任何一方（holder 只用来区分是谁）需要时整个进程按正常性能运行。
void power_hold(const void *holder, bool active);

/// 当前是否声明了正常性能，供诊断日志使用。
bool power_is_high(void);

/// 当前线程只做不急的后台工作：始终节能运行，把性能核和功耗让给界面。
void power_background_thread(void);

/// 当前线程不再单独指定，跟随进程。
void power_follow_process(void);
