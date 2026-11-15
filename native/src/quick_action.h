// 置物架底部的快速操作：内置操作和用户添加的自定义操作，以及执行它们。
// 和 C# 版的 QuickActionInfo、QuickActions、QuickActionService 对应。只在界面线程上调用。
#pragma once

#include "settings.h"
#include "strings.h"

/// 一个可放到置物架上的快速操作。名称和说明按当前界面语言生成。
typedef struct QuickAction
{
    wchar_t *id;                // 保存在设置中的标识：内置操作为类型名称，自定义操作为它的 Id
    QuickActionType type;       // 自定义操作是 QUICK_ACTION_CUSTOM
    bool is_custom;
    CustomAction custom;        // is_custom 时有效（副本）
} QuickAction;

typedef struct QuickActionList
{
    QuickAction *items;
    int count;
} QuickActionList;

/// 置物架上实际显示的操作，按从左到右的顺序。
QuickActionList quick_actions_visible(const AppSettings *settings);
void quick_action_list_free(QuickActionList *list);
bool quick_action_list_same(const QuickActionList *first, const QuickActionList *second);

void quick_action_clone(QuickAction *target, const QuickAction *source);
void quick_action_free(QuickAction *action);

const wchar_t *quick_action_glyph(const QuickAction *action);

// 以下文字由调用方 free。
wchar_t *quick_action_label(const QuickAction *action);         // 置物架按钮上的简短名称
wchar_t *quick_action_title(const QuickAction *action);         // 设置页、菜单和提示中的完整名称
wchar_t *quick_action_description(const QuickAction *action);
wchar_t *quick_action_busy_text(const QuickAction *action);
wchar_t *quick_action_tooltip(const QuickAction *action);       // 名称：说明

/// 可能耗时较长的操作在处理期间显示进度。
bool quick_action_shows_progress(const QuickAction *action);

/// 需要文件才能做的操作（压缩、移动或复制到文件夹、交给程序打开）：置物架上的文本和链接先转成文件再一起处理。
bool quick_action_takes_text(const QuickAction *action);

/// 先弹出文件夹选择（“移动到”）。
bool quick_action_asks_for_folder(const QuickAction *action);

// ───────────── 执行 ─────────────

typedef struct QuickActionResult
{
    bool succeeded;
    bool cancelled;             // 用户在选择文件夹时取消，不需要提示失败
    bool produced_new_items;    // 生成了新文件（如压缩包），放进新的置物架方便继续拖走
    bool keeps_temporary_files; // 其他程序稍后才会读取文件（属性窗口、用程序打开），临时附件晚一些再清理
    wchar_t *message;
    StrList outputs;
    StrList move_sources, move_destinations;    // 移动后的新位置，用于更新置物架中的引用
    StrList replaced_sources, replaced_outputs; // 处理后的新文件替换置物架中的原项目（原文件保留在磁盘上）
} QuickActionResult;

void quick_action_result_set(QuickActionResult *result, bool succeeded, wchar_t *message);     // 收下 message
void quick_action_result_fail(QuickActionResult *result, StringId message);
void quick_action_result_free(QuickActionResult *result);

typedef struct QuickActionRequest
{
    QuickAction action;         // 副本，执行完释放
    StrList paths;
    bool has_temporary_files;
    HWND owner;
    wchar_t *folder;            // “移动到”选好的文件夹；NULL 表示取消了
} QuickActionRequest;

/// 做完后在界面线程调用；result 由 done 之后自动释放。
typedef void (*QuickActionDone)(QuickActionResult *result, void *context);

/// 执行快速操作。耗时的部分在后台，做完回到界面线程调用 done。request 的内容被收下（调用后清空）。
void quick_action_execute(QuickActionRequest *request, QuickActionDone done, void *context);

/// 生成程序参数：{files} 替换为加上引号的全部路径；没有 {files} 时把路径放在最后。调用方负责 free。
wchar_t *quick_action_build_arguments(const wchar_t *template_text, const StrList *paths);

/// 程序显示的名称：文件说明（不长的话），否则是文件名。调用方负责 free。
wchar_t *program_display_name(const wchar_t *path);

/// 在资源管理器中选中这些项目（按所在文件夹分组，最多打开 6 个窗口）。
bool shell_open_folder_and_select(const StrList *paths);

/// 选择文件夹的对话框；取消时返回 NULL。调用方负责 free。
wchar_t *pick_folder(HWND owner, const wchar_t *title);
