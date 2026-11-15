// 后台任务：在线程池里做费时的事（读磁盘、复制附件、取缩略图），做完回到界面线程收尾。
// 和 C# 版里 await Task.Run(...) 之后接着写的那部分对应。
#pragma once

#include "base.h"

typedef void (*TaskWork)(void *data);
typedef void (*TaskDone)(void *data);

/// 启动时在界面线程调用一次：建一个只收消息的窗口，后台做完的任务经它回到界面线程。
bool task_initialize(void);
void task_shutdown(void);

/// 在线程池里调用 work(data)，做完后在界面线程调用 done(data)。work 可以是 NULL（只是稍后在界面线程调用 done）。
/// 程序退出时还没回来的任务不再调用 done，data 随进程一起释放。
void task_run(TaskWork work, TaskDone done, void *data);

/// 在界面线程稍后调用 done(data)，排在已有的消息后面。可以在任何线程调用。
void task_post(TaskDone done, void *data);

/// 过 milliseconds 毫秒后在界面线程调用 done(data)。
void task_delay(unsigned milliseconds, TaskDone done, void *data);

/// 在专门的图片处理线程上（单线程套间，COM 已初始化）按顺序调用 work，做完回到界面线程调用 done。
/// 取缩略图、编码图片都在这里做：外壳的缩略图组件要求单线程套间，排成一队也不会同时占满处理器。
void task_run_imaging(TaskWork work, TaskDone done, void *data);
