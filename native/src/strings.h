// 界面文字。没有参数的文字列在 strings.def 里，用 S(STR_名称) 读取；带参数的文字是下面的函数。
// 都按当前语言（或语言包）返回，见 lang.h。
#pragma once

#include "lang.h"

typedef enum StringId
{
#define STR(name, chinese, english) STR_##name,
#include "strings.def"
#undef STR
    STR_COUNT
} StringId;

/// 当前语言下的文字。返回的指针在语言包重新加载之前有效，不要 free。
const wchar_t *S(StringId id);

/// 一条文字的中文和英文原文；导出翻译模板时用。
void string_sources(StringId id, const wchar_t **chinese, const wchar_t **english);

// ───────────── 带参数的文字 ─────────────
// 返回的字符串由调用方 free。其余带参数的文字在移植到用它们的界面时再加进来。

wchar_t *str_item_count(int count);     // 3 个项目 / 3 items
wchar_t *str_added(int count);          // 已添加 3 项 / Added 3 items
wchar_t *str_file_count(int count);     // 3 个文件
wchar_t *str_folder_count(int count);   // 3 个文件夹
wchar_t *str_text_count(int count);     // 3 段文本
wchar_t *str_link_count(int count);     // 3 个链接
wchar_t *str_folder_items(int count);   // 3 项
wchar_t *str_text_detail(int characters);               // 文本 · 12 个字符
wchar_t *str_file_type(const wchar_t *extension);       // PDF 文件
wchar_t *str_shelf_window_title(const wchar_t *title);  // DropShelf 置物架 - 3 个项目
wchar_t *str_skipped_duplicates(int count);             // ，忽略 2 个重复项
wchar_t *str_unavailable_items(int count);              // ，2 项无法访问
wchar_t *str_removed_items(int count);                  // 已移除 3 项
wchar_t *str_converted_to_files(int count);             // 已转成 2 个文件
wchar_t *str_copied_files(int count);                   // 已复制 3 个文件
wchar_t *str_copied_paths(int count);                   // 已复制 3 个路径
wchar_t *str_reading_files(int count);                  // 正在读取 30 个文件…
wchar_t *str_with_failed_attachments(const wchar_t *message, int failed);

// 快速操作
wchar_t *str_quick_action_tooltip(const wchar_t *label, const wchar_t *description);   // 压缩：压缩为 ZIP…
wchar_t *str_image_size_text(int kilobytes);                    // 500 KB / 2 MB（不分语言）
wchar_t *str_copy_to_folder_title(const wchar_t *folder);       // 复制到“截图”
wchar_t *str_move_to_folder_title(const wchar_t *folder);
wchar_t *str_compress_images_title(const wchar_t *size);
wchar_t *str_convert_images_title(const wchar_t *format);
wchar_t *str_open_with_title(const wchar_t *program);
wchar_t *str_copy_to_folder_description(const wchar_t *folder);
wchar_t *str_move_to_folder_description(const wchar_t *folder);
wchar_t *str_compress_images_description(const wchar_t *size);
wchar_t *str_convert_images_description(const wchar_t *format);
wchar_t *str_open_with_description(const wchar_t *program);
wchar_t *str_folder_missing(const wchar_t *name);
wchar_t *str_program_missing(const wchar_t *name);
wchar_t *str_open_with_failed(const wchar_t *name);
wchar_t *str_created(const wchar_t *name);                      // 已创建 资料.zip
wchar_t *str_extracted_to(const wchar_t *name);
wchar_t *str_images_already_format(const wchar_t *format);
wchar_t *str_images_already_small(const wchar_t *size);
wchar_t *str_copied_to(int count, const wchar_t *folder);       // 已复制 3 项到 截图
wchar_t *str_moved_to(int count, const wchar_t *folder);
wchar_t *str_copied_partially(int copied, int failed);
wchar_t *str_moved_partially(int moved, int failed);
wchar_t *str_images_converted(int count, const wchar_t *format);
wchar_t *str_images_compressed(int count, const wchar_t *size);
wchar_t *str_images_skipped_same_format(int count, const wchar_t *format);  // ，2 张本来就是 PNG
wchar_t *str_images_skipped_small(int count);                   // ，2 张本来就符合
wchar_t *str_images_failed(int count);                          // ，1 张无法读取
wchar_t *str_opened_with(int count, const wchar_t *name);       // 已用 画图 打开 2 项
wchar_t *str_opened_properties_for(int count);
wchar_t *str_saved_to_downloads(int count);

// 最近关闭的置物架、合并
wchar_t *str_history_entry_title(const wchar_t *first, int count);   // 报告.pdf 等 3 项
wchar_t *str_history_yesterday(const wchar_t *time);                // 昨天 14:05
/// 历史牌右上角时间下面的一行：今天、昨天或日期。days_ago 是关闭那天离今天几天。
wchar_t *str_history_day(int days_ago, int month, int day);
wchar_t *str_history_date(int month, int day);                      // 11月5日、Nov 5
wchar_t *str_restored(int added, int unavailable);                  // 已恢复 3 项，1 项已删除或移动
wchar_t *str_merged_shelves(int count);                             // 已合并 3 项

// 主窗口
wchar_t *str_version(const wchar_t *version);                       // 版本 2.8.2
wchar_t *str_update_latest(const wchar_t *version);
wchar_t *str_update_available(const wchar_t *version);
/// 状态的说明；hotkey 为 NULL 时不提快捷键。trigger 是 AutoOpenTrigger。
wchar_t *str_status_running_detail(int trigger, const wchar_t *hotkey);
/// 概览页卡片上的当前设置，如“中文 · 开机启动 · Ctrl+Shift+Space”。hotkey 为 NULL 时是“未设置”。
wchar_t *str_general_summary(int language, bool start_with_windows, const wchar_t *hotkey);
wchar_t *str_shelf_summary(int trigger, int drag_out_mode);
wchar_t *str_actions_summary(const StrList *labels);
/// accent 为 NULL 时是钴蓝，"system" 时跟随 Windows，其余是自选的颜色。
wchar_t *str_appearance_summary(int theme, const wchar_t *accent);
wchar_t *str_history_count(int count);                              // 已保留 3 个最近关闭的置物架
wchar_t *str_history_card_detail(int count, const wchar_t *time);  // 3 个项目 · 14:05
wchar_t *str_sensitivity_detail(int reversals, int distance);
wchar_t *str_calibration_progress(int done, int left);
wchar_t *str_confirm_delete_action(const wchar_t *title);
wchar_t *str_image_size_invalid(int minimum, int maximum);
/// 引导最后一步的说明；hotkey 为 NULL 时提示去设置里设定快捷键。
wchar_t *str_onboarding_actions_body(const wchar_t *hotkey);
