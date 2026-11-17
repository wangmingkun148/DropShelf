// 一个置物架的内容和对内容的操作，和 C# 版的 ShelfViewModel 对应；窗口在 shelf_window.c。
// 只在界面线程上使用。后台的工作（统计文件夹、取缩略图、检查文件还在不在）拿着引用，置物架关闭后回来的结果直接丢掉。
#pragma once

#include "drop_read.h"
#include "quick_action.h"
#include "shelf_drag.h"
#include "shelf_item.h"

typedef struct ShelfModel ShelfModel;

/// 置物架告诉窗口的事。
typedef enum ShelfChange
{
    SHELF_CHANGED_ITEMS,        // 增加、移除、替换了项目（数量可能没变）
    SHELF_CHANGED_EXPANDED,     // 展开或收起了列表
    SHELF_CHANGED_BUSY,         // 开始或结束忙碌
    SHELF_CHANGED_ITEM_TEXT     // 某个项目的说明、缩略图或可用性变了，只需重画
} ShelfChange;

typedef struct ShelfModelEvents
{
    void *context;
    void (*changed)(void *context, ShelfChange change);
    void (*notify)(void *context, const wchar_t *message, bool success);
    /// 拖出之后、以及稍后确认文件已被移走时，交给窗口移除项目（窗口先播放移除的动画）。为 NULL 时直接移除。
    void (*remove_visibly)(void *context, ShelfItem **items, int count);
    /// 快速操作生成了新文件（如压缩包），而这个置物架上已经有东西：交给窗口放进旁边的新置物架。
    void (*produced)(void *context, const StrList *paths);
} ShelfModelEvents;

struct ShelfModel
{
    LONG references;
    wchar_t *id;
    ShelfItem **items;          // 从旧到新；堆叠最上面是最新放入的
    int count, capacity;
    bool expanded;
    bool pinned;                // 置顶，默认开启
    bool busy;
    wchar_t *busy_text;
    bool opened_by_gesture;     // 晃动呼出：松开鼠标时仍为空就自动关闭
    bool has_interacted;        // 点击过、切换过置顶或放入过内容，空置物架也不再自动关闭
    bool dragging_out;
    int thumbnail_size;         // 缩略图的像素大小，由窗口按堆叠卡片的实际显示大小设置
    int visible_from, visible_to;   // 列表里显示出来的行（含），没有展开时为 -1
    bool released;
    volatile LONG lifetime_over;    // 关闭后变为 1，后台的工作据此尽快停下

    StrList caches;             // 持有的临时附件缓存
    bool delay_cache_cleanup;   // 临时附件已交给别的程序：关闭后稍晚再清理
    wchar_t *own_cache;         // 文本和链接转成的文件放在这里
    ShelfItem **drag_items;     // 和文件一起拖出时替文本、链接写好的文件
    wchar_t **drag_paths;
    int drag_count;
    ShelfItem **leaving;        // 拖出后正等着确认已被移走的项目
    int leaving_count;
    bool checking_availability;

    ShelfModelEvents events;
};

ShelfModel *shelf_model_new(const ShelfModelEvents *events);
ShelfModel *shelf_model_retain(ShelfModel *model);
void shelf_model_release(ShelfModel *model);

bool shelf_model_contains(const ShelfModel *model, const ShelfItem *item);
int shelf_model_index_of(const ShelfModel *model, const ShelfItem *item);
bool shelf_model_all_items_in(const ShelfModel *model, ShelfItem *const *items, int count);

/// 第 position 新的项目（1 是最新的）；不够时返回 NULL。
ShelfItem *shelf_model_from_end(const ShelfModel *model, int position);

void shelf_model_set_expanded(ShelfModel *model, bool expanded);
void shelf_model_set_busy(ShelfModel *model, const wchar_t *text);    // text 为 NULL 时结束忙碌

// ───────────── 放入 ─────────────

typedef struct ShelfAddSummary
{
    int added, duplicates, unavailable;
} ShelfAddSummary;

/// 放入读取结果：同一路径（不分大小写）或同样内容的不重复放，读不到的文件不放。结果里的缓存由置物架接管。
ShelfAddSummary shelf_model_add(ShelfModel *model, DropReadResult *result);

/// 放入后的说明，例如“已添加 3 项，忽略 1 个重复项”。调用方负责 free。
wchar_t *shelf_add_summary_message(ShelfAddSummary summary);

/// 当场放入磁盘上的文件（快速操作刚生成的），在界面线程上读文件信息。
ShelfAddSummary shelf_model_add_files(ShelfModel *model, const StrList *paths);

// ───────────── 快速操作 ─────────────

/// 快速操作做完（置物架可能已经关闭，model->released 为 true）。result 在回调之后释放。
typedef void (*ShelfActionFinished)(ShelfModel *model, const QuickActionResult *result, void *context);

/// 对置物架上的项目执行快速操作：需要文件的操作先把文本和链接转成文件；耗时的操作显示进度，“移动到”先选文件夹。
/// 做完后更新项目（移动后的新位置、处理后的新文件）、提示结果，再调用 finished。
void shelf_model_run_action(ShelfModel *model, const QuickAction *action, ShelfItem *const *targets, int count, HWND owner,
    ShelfActionFinished finished, void *context);

/// 对直接拖到快速操作上的内容执行。读取结果里的缓存由置物架接管，result 由调用方释放。
void shelf_model_run_action_on_dropped(ShelfModel *model, const QuickAction *action, DropReadResult *result, HWND owner,
    ShelfActionFinished finished, void *context);

// ───────────── 合并和拆分 ─────────────

/// 接收另一个置物架交来的项目（合并、拆分、架子之间拖动）：放进来的是副本，文本、链接、缩略图和已经统计好的文件夹信息都保留；
/// 这边已经有的相同文件或内容不重复放。临时附件所在的缓存由两边共同持有，对方关闭后文件还在。
/// 对方随后自己把原来的项目移除。返回放进来的数量（放在最后）。
int shelf_model_adopt(ShelfModel *model, ShelfItem *const *items, int count, ShelfModel *from);

// ───────────── 跟着改名和移动 ─────────────

/// 当场找回的项目最多这么多，多了交给后台的检查。
#define SHELF_RELINK_LIMIT 16

/// 拖出、打开、复制或交给快速操作之前：这几个项目里路径找不到的，当场按文件标识找回来，免得刚在别处改了名就拖不出去。
void shelf_model_relink(ShelfModel *model, ShelfItem *const *items, int count);

// ───────────── 移除和撤销 ─────────────

/// 一次移除的记录：移除的项目和它们原来的位置（从前到后），撤销时按原位放回。
typedef struct ShelfRemoval
{
    ShelfItem **items;
    int *indices;
    int count;
} ShelfRemoval;

/// 移除项目；一个也没移除时返回 NULL。返回的记录拿着项目的引用，用 shelf_removal_free 释放。
ShelfRemoval *shelf_model_remove(ShelfModel *model, ShelfItem *const *items, int count);
void shelf_removal_free(ShelfRemoval *removal);

/// 撤销移除：按原来的位置放回，这期间又放上来的相同文件或内容不重复放。返回放回了几项，放回的项目写进 restored（可以是 NULL，调用方 free 数组）。
int shelf_model_restore(ShelfModel *model, const ShelfRemoval *removal, ShelfItem ***restored);

// ───────────── 文本和链接当文件用 ─────────────

/// 把文本和链接原地转成文件（文本存成 .txt，网页链接存成 .url），位置不变。返回转好了几个。
int shelf_model_convert_to_files(ShelfModel *model, ShelfItem *const *items, int count);

/// 和文件一起拖出时，替文本和链接各写一个文件跟着走，置物架上的项目不变。写不了时 paths 为空。
void shelf_model_write_for_drag(ShelfModel *model, ShelfItem *const *items, int count, StrList *paths);

/// 写成文件时的文件名，例如“会议纪要.txt”、“example.com.url”。调用方负责 free。
wchar_t *shelf_item_file_name(const ShelfItem *item);

// ───────────── 拖出、打开、复制 ─────────────

/// 处理拖出的结果；dragged 是实际交给目标程序的项目。
void shelf_model_apply_drag_out(ShelfModel *model, ShelfItem *const *dragged, int count, DragOutResult result, bool remove_copied);

/// 临时附件已交给其他程序（拖出、复制、用程序打开），关闭置物架后稍晚再清理。
void shelf_model_keep_temporary_files(ShelfModel *model);

bool shelf_model_copy(ShelfModel *model, ShelfItem *const *items, int count);
bool shelf_model_copy_paths(ShelfModel *model, ShelfItem *const *items, int count);
void shelf_model_open(ShelfModel *model, ShelfItem *item);
void shelf_model_reveal(ShelfModel *model, ShelfItem *const *items, int count);

/// 在后台检查磁盘上的项目是否还在，找不到的按文件标识找它现在的位置，查完回到界面线程更新。上一次还没查完时跳过。
void shelf_model_refresh_availability_in_background(ShelfModel *model);

// ───────────── 显示 ─────────────

/// 标题：空的时候是“置物架”，否则是“N 个项目”。调用方负责 free。
wchar_t *shelf_model_title(const ShelfModel *model);

/// 堆叠下面的两行：一项时是它的名称和说明；多项时按类别列出各有几项、文件和文件夹的总大小。调用方负责 free。
wchar_t *shelf_model_stack_caption(ShelfModel *model);
wchar_t *shelf_model_stack_detail(ShelfModel *model);

/// 只有堆叠顶部三张和列表里显示出来的行持有缩略图：其余的取消并释放，内存不随项目数量增长。
void shelf_model_update_thumbnails(ShelfModel *model);

/// 关闭置物架时调用：取消后台的工作，清理临时附件，放掉全部项目。
void shelf_model_release_resources(ShelfModel *model);
