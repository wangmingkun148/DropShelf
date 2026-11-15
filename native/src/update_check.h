// 检查更新。DropShelf 平时不联网，只有在“关于”里点了“检查更新”才访问一次 GitHub 的发布列表，
// 请求里只有程序名称和版本号；不自动下载，也不自动安装。和 C# 版的 UpdateChecker 对应。
#pragma once

#include "base.h"

#define UPDATE_RELEASES_PAGE L"https://github.com/wangmingkun148/dropshelf/releases"

typedef enum UpdateOutcome
{
    UPDATE_LATEST,          // 已经是最新
    UPDATE_NEWER,           // 有新版本
    UPDATE_ERROR            // 连不上、超时或返回的内容看不懂
} UpdateOutcome;

typedef struct UpdateInfo
{
    UpdateOutcome outcome;
    int major, minor, patch;
    wchar_t *url;           // 新版本的下载页
} UpdateInfo;

/// 在后台访问发布列表，做完在界面线程调用 done；info 在 done 之后释放。
void update_check_start(void (*done)(const UpdateInfo *info, void *context), void *context);

/// 从发布列表里找比 current 新的最新正式版本：只看以 v 开头的标签，草稿和预发布不算。
/// 返回 UPDATE_NEWER 时 info 里是版本和下载页（url 由调用方 free）。内容不是发布列表时返回 UPDATE_ERROR。
UpdateOutcome update_find_newer(const wchar_t *json, int major, int minor, int patch, UpdateInfo *info);

/// v2.8.5 → 2.8.5；前缀不对或不是版本号时返回 false。
bool update_parse_tag(const wchar_t *tag, int *major, int *minor, int *patch);
