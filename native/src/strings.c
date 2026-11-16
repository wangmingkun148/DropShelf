#include "strings.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct StringSource
{
    const wchar_t *chinese;
    const wchar_t *english;
} StringSource;

static const StringSource sources[STR_COUNT] = {
#define STR(name, chinese, english) { chinese, english },
#include "strings.def"
#undef STR
};

const wchar_t *S(StringId id)
{
    if (id < 0 || id >= STR_COUNT) return L"";
    return tr(sources[id].chinese, sources[id].english);
}

void string_sources(StringId id, const wchar_t **chinese, const wchar_t **english)
{
    *chinese = sources[id].chinese;
    *english = sources[id].english;
}

// ───────────── 带参数的文字 ─────────────
// 中文和英文各有自己的模板和参数：英文里的数量连同单复数一起作为一个参数（"3 items"），
// 所以英文模板是 "Added {0}"，语言包对照英文翻译时参数就是 "3 items"。与 C# 版的模板相同。

wchar_t *str_item_count(int count)
{
    wchar_t *counted = english_count(count, L"item", L"items");
    LocFormat chinese = { L"{0} 个项目", 1, { ARG_INT(count) } };
    LocFormat english = { L"{0}", 1, { ARG_TEXT(counted) } };
    wchar_t *text = trf(&chinese, &english);
    free(counted);
    return text;
}

/// 中文模板里直接填数量，英文模板里填“数量 + 单复数”。
static wchar_t *counted(const wchar_t *chinese_template, const wchar_t *english_template, int count, const wchar_t *one, const wchar_t *many)
{
    wchar_t *english_value = english_count(count, one, many);
    LocFormat chinese = { chinese_template, 1, { ARG_INT(count) } };
    LocFormat english = { english_template, 1, { ARG_TEXT(english_value) } };
    wchar_t *text = trf(&chinese, &english);
    free(english_value);
    return text;
}

/// 中英文都直接填数量（英文不分单复数的几条）。
static wchar_t *numbered(const wchar_t *chinese_template, const wchar_t *english_template, int count)
{
    LocFormat chinese = { chinese_template, 1, { ARG_INT(count) } };
    LocFormat english = { english_template, 1, { ARG_INT(count) } };
    return trf(&chinese, &english);
}

wchar_t *str_added(int count) { return counted(L"已添加 {0} 项", L"Added {0}", count, L"item", L"items"); }
wchar_t *str_file_count(int count) { return counted(L"{0} 个文件", L"{0}", count, L"file", L"files"); }
wchar_t *str_folder_count(int count) { return counted(L"{0} 个文件夹", L"{0}", count, L"folder", L"folders"); }
wchar_t *str_text_count(int count) { return counted(L"{0} 段文本", L"{0}", count, L"snippet", L"snippets"); }
wchar_t *str_link_count(int count) { return counted(L"{0} 个链接", L"{0}", count, L"link", L"links"); }
wchar_t *str_folder_items(int count) { return counted(L"{0} 项", L"{0}", count, L"item", L"items"); }
wchar_t *str_text_detail(int characters) { return counted(L"文本 · {0} 个字符", L"Text · {0}", characters, L"character", L"characters"); }
wchar_t *str_skipped_duplicates(int count) { return counted(L"，忽略 {0} 个重复项", L", skipped {0}", count, L"duplicate", L"duplicates"); }
wchar_t *str_unavailable_items(int count) { return numbered(L"，{0} 项无法访问", L", {0} couldn't be accessed", count); }
wchar_t *str_removed_items(int count) { return counted(L"已移除 {0} 项", L"Removed {0}", count, L"item", L"items"); }
wchar_t *str_converted_to_files(int count) { return counted(L"已转成 {0} 个文件", L"Turned into {0}", count, L"file", L"files"); }
wchar_t *str_copied_files(int count) { return counted(L"已复制 {0} 个文件", L"Copied {0}", count, L"file", L"files"); }
wchar_t *str_copied_paths(int count) { return numbered(L"已复制 {0} 个路径", L"Copied {0} paths", count); }
wchar_t *str_reading_files(int count) { return numbered(L"正在读取 {0} 个文件…", L"Reading {0} files…", count); }

wchar_t *str_file_type(const wchar_t *extension)
{
    LocFormat chinese = { L"{0} 文件", 1, { ARG_TEXT(extension) } };
    LocFormat english = { L"{0} File", 1, { ARG_TEXT(extension) } };
    return trf(&chinese, &english);
}

wchar_t *str_shelf_window_title(const wchar_t *title)
{
    LocFormat chinese = { L"DropShelf 置物架 - {0}", 1, { ARG_TEXT(title) } };
    LocFormat english = { L"DropShelf shelf - {0}", 1, { ARG_TEXT(title) } };
    return trf(&chinese, &english);
}

// ───────────── 快速操作 ─────────────

/// 中英文各填一段文字。
static wchar_t *with_text(const wchar_t *chinese_template, const wchar_t *english_template, const wchar_t *text)
{
    LocFormat chinese = { chinese_template, 1, { ARG_TEXT(text) } };
    LocFormat english = { english_template, 1, { ARG_TEXT(text) } };
    return trf(&chinese, &english);
}

/// 数量（英文带单复数）和一段文字。
static wchar_t *counted_with_text(const wchar_t *chinese_template, const wchar_t *english_template, int count, const wchar_t *one,
    const wchar_t *many, const wchar_t *text)
{
    wchar_t *english_value = english_count(count, one, many);
    LocFormat chinese = { chinese_template, 2, { ARG_INT(count), ARG_TEXT(text) } };
    LocFormat english = { english_template, 2, { ARG_TEXT(english_value), ARG_TEXT(text) } };
    wchar_t *result = trf(&chinese, &english);
    free(english_value);
    return result;
}

/// 两个数量：前一个英文带单复数，后一个直接填。
static wchar_t *counted_pair(const wchar_t *chinese_template, const wchar_t *english_template, int count, int other)
{
    wchar_t *english_value = english_count(count, L"item", L"items");
    LocFormat chinese = { chinese_template, 2, { ARG_INT(count), ARG_INT(other) } };
    LocFormat english = { english_template, 2, { ARG_TEXT(english_value), ARG_INT(other) } };
    wchar_t *result = trf(&chinese, &english);
    free(english_value);
    return result;
}

wchar_t *str_quick_action_tooltip(const wchar_t *label, const wchar_t *description)
{
    LocFormat chinese = { L"{0}：{1}", 2, { ARG_TEXT(label), ARG_TEXT(description) } };
    LocFormat english = { L"{0}: {1}", 2, { ARG_TEXT(label), ARG_TEXT(description) } };
    return trf(&chinese, &english);
}

wchar_t *str_image_size_text(int kilobytes)
{
    wchar_t text[32];
    if (kilobytes >= 1024 && kilobytes % 1024 == 0) swprintf(text, 32, L"%d MB", kilobytes / 1024);
    else swprintf(text, 32, L"%d KB", kilobytes);
    return wcs_dup(text);
}

wchar_t *str_copy_to_folder_title(const wchar_t *folder) { return with_text(L"复制到“{0}”", L"Copy to {0}", folder); }
wchar_t *str_move_to_folder_title(const wchar_t *folder) { return with_text(L"移动到“{0}”", L"Move to {0}", folder); }
wchar_t *str_compress_images_title(const wchar_t *size) { return with_text(L"压缩图片到 {0}", L"Compress images to {0}", size); }
wchar_t *str_convert_images_title(const wchar_t *format) { return with_text(L"图片转成 {0}", L"Convert images to {0}", format); }
wchar_t *str_open_with_title(const wchar_t *program) { return with_text(L"用 {0} 打开", L"Open with {0}", program); }
wchar_t *str_copy_to_folder_description(const wchar_t *folder) { return with_text(L"复制到 {0}", L"Copies items to {0}", folder); }
wchar_t *str_move_to_folder_description(const wchar_t *folder) { return with_text(L"移动到 {0}", L"Moves items to {0}", folder); }
wchar_t *str_compress_images_description(const wchar_t *size)
{
    return with_text(L"压缩为不超过 {0} 的 JPG，原图保持不变", L"Saves a JPG copy of {0} or less. The original stays unchanged", size);
}
wchar_t *str_convert_images_description(const wchar_t *format)
{
    return with_text(L"另存为 {0}，原图保持不变", L"Saves a {0} copy. The original stays unchanged", format);
}
wchar_t *str_open_with_description(const wchar_t *program) { return with_text(L"用 {0} 打开", L"Opens with {0}", program); }

wchar_t *str_folder_missing(const wchar_t *name) { return with_text(L"找不到文件夹“{0}”", L"Couldn't find the folder “{0}”", name); }
wchar_t *str_program_missing(const wchar_t *name) { return with_text(L"找不到程序“{0}”", L"Couldn't find {0}", name); }
wchar_t *str_open_with_failed(const wchar_t *name) { return with_text(L"{0} 没能打开这些文件", L"{0} couldn't open these files", name); }
wchar_t *str_created(const wchar_t *name) { return with_text(L"已创建 {0}", L"Created {0}", name); }
wchar_t *str_extracted_to(const wchar_t *name) { return with_text(L"已解压到 {0}", L"Extracted to {0}", name); }
wchar_t *str_images_already_format(const wchar_t *format) { return with_text(L"图片已经是 {0}，无需转换", L"The images are already {0}", format); }
wchar_t *str_images_already_small(const wchar_t *size) { return with_text(L"图片已小于 {0}，无需压缩", L"The images are already under {0}", size); }

wchar_t *str_copied_to(int count, const wchar_t *folder)
{
    return counted_with_text(L"已复制 {0} 项到 {1}", L"Copied {0} to {1}", count, L"item", L"items", folder);
}

wchar_t *str_moved_to(int count, const wchar_t *folder)
{
    return counted_with_text(L"已移动 {0} 项到 {1}", L"Moved {0} to {1}", count, L"item", L"items", folder);
}

wchar_t *str_copied_partially(int copied, int failed) { return counted_pair(L"已复制 {0} 项，{1} 项未能复制", L"Copied {0}; {1} couldn't be copied", copied, failed); }
wchar_t *str_moved_partially(int moved, int failed) { return counted_pair(L"已移动 {0} 项，{1} 项未能移动", L"Moved {0}; {1} couldn't be moved", moved, failed); }

wchar_t *str_images_converted(int count, const wchar_t *format)
{
    return counted_with_text(L"已把 {0} 张图片转成 {1}", L"Converted {0} to {1}", count, L"image", L"images", format);
}

wchar_t *str_images_compressed(int count, const wchar_t *size)
{
    return counted_with_text(L"已压缩 {0} 张图片到 {1} 以内", L"Compressed {0} to under {1}", count, L"image", L"images", size);
}

wchar_t *str_images_skipped_same_format(int count, const wchar_t *format)
{
    LocFormat chinese = { L"，{0} 张本来就是 {1}", 2, { ARG_INT(count), ARG_TEXT(format) } };
    LocFormat english = { L", {0} already {1}", 2, { ARG_INT(count), ARG_TEXT(format) } };
    return trf(&chinese, &english);
}

wchar_t *str_images_skipped_small(int count)
{
    return numbered(L"，{0} 张本来就符合", count == 1 ? L", {0} already fits" : L", {0} already fit", count);
}

wchar_t *str_images_failed(int count) { return numbered(L"，{0} 张无法读取", L", {0} couldn't be read", count); }

wchar_t *str_opened_with(int count, const wchar_t *name)
{
    wchar_t *items = english_count(count, L"item", L"items");
    LocFormat chinese = { L"已用 {0} 打开 {1} 项", 2, { ARG_TEXT(name), ARG_INT(count) } };
    LocFormat english = { L"Opened {0} with {1}", 2, { ARG_TEXT(items), ARG_TEXT(name) } };
    wchar_t *text = trf(&chinese, &english);
    free(items);
    return text;
}

wchar_t *str_opened_properties_for(int count) { return numbered(L"已打开 {0} 个项目的属性", L"Opened properties for {0} items", count); }
wchar_t *str_saved_to_downloads(int count) { return counted(L"已保存 {0} 项到下载文件夹", L"Saved {0} to Downloads", count, L"item", L"items"); }

// ───────────── 最近关闭的置物架、合并 ─────────────

wchar_t *str_history_entry_title(const wchar_t *first, int count)
{
    LocFormat chinese = { L"{0} 等 {1} 项", 2, { ARG_TEXT(first), ARG_INT(count) } };
    LocFormat english = { L"{0} and {1} more", 2, { ARG_TEXT(first), ARG_INT(count - 1) } };
    return trf(&chinese, &english);
}

wchar_t *str_history_yesterday(const wchar_t *time) { return with_text(L"昨天 {0}", L"Yesterday {0}", time); }

wchar_t *str_history_day(int days_ago, int month, int day)
{
    if (days_ago == 0) return wcs_dup(tr(L"今天", L"Today"));
    if (days_ago == 1) return wcs_dup(tr(L"昨天", L"Yesterday"));
    return str_history_date(month, day);
}

wchar_t *str_history_date(int month, int day)
{
    static const wchar_t *const months[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    wchar_t english_text[16];
    swprintf(english_text, ARRAYSIZE(english_text), L"%ls %d", months[(month - 1) % 12], day);
    // 英文是现成的日期（C# 版按 en-US 的 "MMM d" 写好再交给语言包），中文按月和日填进模板。
    LocFormat chinese = { L"{0}月{1}日", 2, { ARG_INT(month), ARG_INT(day) } };
    LocFormat english = { english_text, 0, { ARG_INT(0) } };
    return trf(&chinese, &english);
}

wchar_t *str_restored(int added, int unavailable)
{
    wchar_t chinese_part[48] = L"", english_part[64] = L"";
    if (unavailable > 0)
    {
        swprintf(chinese_part, ARRAYSIZE(chinese_part), L"，%d 项已删除或移动", unavailable);
        swprintf(english_part, ARRAYSIZE(english_part), L", %d moved or deleted", unavailable);
    }
    wchar_t *items = english_count(added, L"item", L"items");
    LocFormat chinese = { L"已恢复 {0} 项{1}", 2, { ARG_INT(added), ARG_TEXT(chinese_part) } };
    LocFormat english = { L"Reopened {0}{1}", 2, { ARG_TEXT(items), ARG_TEXT(english_part) } };
    wchar_t *text = trf(&chinese, &english);
    free(items);
    return text;
}

wchar_t *str_merged_shelves(int count) { return counted(L"已合并 {0} 项", L"Merged {0}", count, L"item", L"items"); }

// ───────────── 主窗口 ─────────────

wchar_t *str_version(const wchar_t *version) { return with_text(L"版本 {0}", L"Version {0}", version); }
wchar_t *str_update_latest(const wchar_t *version) { return with_text(L"已经是最新版本（{0}）", L"You have the latest version ({0})", version); }
wchar_t *str_update_available(const wchar_t *version) { return with_text(L"有新版本 {0}", L"Version {0} is available", version); }

wchar_t *str_status_running_detail(int trigger, const wchar_t *hotkey)
{
    // 与 C# 版一样，括号里的快捷键一句是填进模板的参数。
    wchar_t chinese_tail[96] = L"", english_tail[128] = L"";
    const wchar_t *chinese_template, *english_template;
    switch (trigger)
    {
        case TRIGGER_SHAKE:
            chinese_template = L"拖住文件并快速晃动鼠标{0}";
            english_template = L"Drag a file and shake the mouse{0}";
            if (hotkey) swprintf(chinese_tail, ARRAYSIZE(chinese_tail), L"，或按 %ls 打开置物架", hotkey);
            if (hotkey) swprintf(english_tail, ARRAYSIZE(english_tail), L", or press %ls to open a shelf", hotkey);
            break;
        case TRIGGER_EDGE:
            chinese_template = L"把文件拖到屏幕边缘停一下{0}";
            english_template = L"Drag a file to the edge of the screen and pause{0}";
            if (hotkey) swprintf(chinese_tail, ARRAYSIZE(chinese_tail), L"，或按 %ls 打开置物架", hotkey);
            if (hotkey) swprintf(english_tail, ARRAYSIZE(english_tail), L", or press %ls to open a shelf", hotkey);
            break;
        default:
            chinese_template = L"拖住文件晃一晃，或拖到屏幕边缘停一下{0}";
            english_template = L"Drag a file and shake it, or pause at the screen edge{0}";
            if (hotkey) swprintf(chinese_tail, ARRAYSIZE(chinese_tail), L"；也可以按 %ls", hotkey);
            if (hotkey) swprintf(english_tail, ARRAYSIZE(english_tail), L". You can also press %ls", hotkey);
            break;
    }
    LocFormat chinese = { chinese_template, 1, { ARG_TEXT(chinese_tail) } };
    LocFormat english = { english_template, 1, { ARG_TEXT(english_tail) } };
    return trf(&chinese, &english);
}

static void join_part(WStr *text, const wchar_t *part)
{
    if (text->length > 0) wstr_append(text, L" · ");
    wstr_append(text, part);
}

wchar_t *str_general_summary(int language, bool start_with_windows, const wchar_t *hotkey)
{
    WStr text = {0};
    join_part(&text, language == LANGUAGE_CHINESE ? L"中文" : language == LANGUAGE_ENGLISH ? L"English" : tr(L"语言跟随系统", L"System language"));
    join_part(&text, start_with_windows ? tr(L"开机启动", L"Starts with Windows") : tr(L"不开机启动", L"Doesn't start with Windows"));
    join_part(&text, hotkey && *hotkey ? hotkey : S(STR_HotkeyNotSet));
    return wstr_take(&text);
}

wchar_t *str_shelf_summary(int trigger, int drag_out_mode)
{
    WStr text = {0};
    join_part(&text, trigger == TRIGGER_SHAKE ? tr(L"晃动呼出", L"Shake to open")
                   : trigger == TRIGGER_EDGE ? tr(L"屏幕边缘呼出", L"Pause at edge to open")
                                             : tr(L"晃动或屏幕边缘呼出", L"Shake or pause at edge to open"));
    join_part(&text, drag_out_mode == TRANSFER_COPY ? tr(L"拖出时复制", L"Drag out to copy") : tr(L"拖出时移动", L"Drag out to move"));
    return wstr_take(&text);
}

wchar_t *str_actions_summary(const StrList *labels)
{
    if (!labels || labels->count == 0) return wcs_dup(tr(L"不显示快速操作", L"Quick actions are off"));
    WStr text = {0};
    const wchar_t *separator = tr(L"、", L", ");
    for (int index = 0; index < labels->count; index++)
    {
        if (index > 0) wstr_append(&text, separator);
        wstr_append(&text, labels->items[index]);
    }
    return wstr_take(&text);
}

wchar_t *str_appearance_summary(int theme, const wchar_t *accent)
{
    WStr text = {0};
    join_part(&text, theme == THEME_LIGHT ? S(STR_ThemeLight) : theme == THEME_DARK ? S(STR_ThemeDark) : tr(L"主题跟随系统", L"System theme"));
    join_part(&text, !accent ? S(STR_AccentCobalt) : wcs_equal(accent, L"system") ? tr(L"Windows 强调色", L"Windows accent color") :
                     tr(L"自选强调色", L"Custom accent color"));
    return wstr_take(&text);
}

wchar_t *str_history_count(int count)
{
    return counted(L"已保留 {0} 个最近关闭的置物架", L"{0} kept", count, L"recently closed shelf", L"recently closed shelves");
}

wchar_t *str_history_card_detail(int count, const wchar_t *time)
{
    // C# 版直接拼起来，不经过语言包。
    wchar_t *items = str_item_count(count);
    WStr text = {0};
    wstr_appendf(&text, L"%ls · %ls", items, time ? time : L"");
    free(items);
    return wstr_take(&text);
}

wchar_t *str_sensitivity_detail(int reversals, int distance)
{
    LocFormat chinese = { L"来回晃动约 {0} 像素、掉头 {1} 次即可弹出", 2, { ARG_INT(distance), ARG_INT(reversals) } };
    LocFormat english = { L"Opens when you shake about {0} px back and forth, changing direction {1} times", 2, { ARG_INT(distance), ARG_INT(reversals) } };
    return trf(&chinese, &english);
}

wchar_t *str_calibration_progress(int done, int left)
{
    LocFormat chinese = { L"第 {0} 次记下了，再来 {1} 次", 2, { ARG_INT(done), ARG_INT(left) } };
    LocFormat english = { L"Got shake {0}. {1} more to go", 2, { ARG_INT(done), ARG_INT(left) } };
    return trf(&chinese, &english);
}

wchar_t *str_confirm_delete_action(const wchar_t *title)
{
    return with_text(L"“{0}”将从快速操作中移除，此操作无法撤销。", L"“{0}” will be removed from your quick actions. This can't be undone.", title);
}

wchar_t *str_image_size_invalid(int minimum_value, int maximum_value)
{
    LocFormat chinese = { L"请输入 {0} 到 {1} 之间的整数", 2, { ARG_INT(minimum_value), ARG_INT(maximum_value) } };
    LocFormat english = { L"Enter a whole number from {0} to {1}", 2, { ARG_INT(minimum_value), ARG_INT(maximum_value) } };
    return trf(&chinese, &english);
}

wchar_t *str_onboarding_actions_body(const wchar_t *hotkey)
{
    if (!hotkey)
        return wcs_dup(tr(L"把文件拖到置物架底部，会展开快速操作，可以直接压缩、复制路径、移动或查看属性。这些操作可以在设置中更换，也能添加自己的操作。在设置中设定快捷键后，还能随时打开空置物架。",
            L"Drag files to the bottom of the shelf to reveal quick actions: compress them, copy their paths, move them or view their properties. You can change these actions or add your own in Settings, and set a keyboard shortcut there to open an empty shelf at any time."));
    return with_text(L"把文件拖到置物架底部，会展开快速操作，可以直接压缩、复制路径、移动或查看属性。这些操作可以在设置中更换，也能添加自己的操作。按 {0} 能随时打开空置物架。",
        L"Drag files to the bottom of the shelf to reveal quick actions: compress them, copy their paths, move them or view their properties. You can change these actions or add your own in Settings. Press {0} to open an empty shelf at any time.",
        hotkey);
}

wchar_t *str_with_failed_attachments(const wchar_t *message, int failed)
{
    wchar_t *attachments = english_count(failed, L"attachment", L"attachments");
    LocFormat chinese = { L"{0}，{1} 个附件接收失败", 2, { ARG_TEXT(message), ARG_INT(failed) } };
    LocFormat english = { L"{0}; {1} couldn't be received", 2, { ARG_TEXT(message), ARG_TEXT(attachments) } };
    wchar_t *text = trf(&chinese, &english);
    free(attachments);
    return text;
}
