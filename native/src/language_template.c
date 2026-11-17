// 翻译模板：把全部界面文字的原文写成一个空白的语言包。和 C# 版的 LanguageTemplate 对应：
// 逐条取一遍文字（带参数的用几组有代表性的参数各取一次），取的时候记下中英文原文或模板。
#include "lang.h"
#include "json.h"
#include "settings.h"
#include "strings.h"

#include <stdlib.h>
#include <string.h>

typedef struct Collected
{
    StrList chinese;
    StrList english;
} Collected;

static void collect(const wchar_t *chinese, const wchar_t *english, void *context)
{
    Collected *collected = context;
    for (int index = 0; index < collected->chinese.count; index++)
        if (wcscmp(collected->chinese.items[index], chinese) == 0 && wcscmp(collected->english.items[index], english) == 0) return;
    strlist_add(&collected->chinese, chinese);
    strlist_add(&collected->english, english);
}

#define TAKE(call) free(call)

static void call_parameterized(void)
{
    // 数量取 0、1、2（单复数和“没有”的分支），开关和枚举取全部。
    for (int count = 0; count <= 2; count++)
    {
        TAKE(str_item_count(count));
        TAKE(str_added(count));
        TAKE(str_file_count(count));
        TAKE(str_folder_count(count));
        TAKE(str_text_count(count));
        TAKE(str_link_count(count));
        TAKE(str_folder_items(count));
        TAKE(str_text_detail(count));
        TAKE(str_skipped_duplicates(count));
        TAKE(str_unavailable_items(count));
        TAKE(str_removed_items(count));
        TAKE(str_converted_to_files(count));
        TAKE(str_copied_files(count));
        TAKE(str_copied_paths(count));
        TAKE(str_reading_files(count));
        TAKE(str_with_failed_attachments(L"x", count));
        TAKE(str_copied_to(count, L"x"));
        TAKE(str_moved_to(count, L"x"));
        TAKE(str_copied_partially(count, count));
        TAKE(str_moved_partially(count, count));
        TAKE(str_move_left_behind(count));
        TAKE(str_images_converted(count, L"x"));
        TAKE(str_images_compressed(count, L"x"));
        TAKE(str_images_skipped_same_format(count, L"x"));
        TAKE(str_images_skipped_small(count));
        TAKE(str_images_failed(count));
        TAKE(str_opened_with(count, L"x"));
        TAKE(str_opened_properties_for(count));
        TAKE(str_saved_to_downloads(count));
        TAKE(str_history_entry_title(L"x", count));
        TAKE(str_restored(count, count));
        TAKE(str_merged_shelves(count));
        TAKE(str_history_count(count));
        TAKE(str_history_card_detail(count, L"x"));
        TAKE(str_sensitivity_detail(count, count));
        TAKE(str_calibration_progress(count, count));
    }
    TAKE(str_file_type(L"x"));
    TAKE(str_shelf_window_title(L"x"));
    TAKE(str_quick_action_tooltip(L"x", L"x"));
    TAKE(str_image_size_text(500));
    TAKE(str_copy_to_folder_title(L"x"));
    TAKE(str_move_to_folder_title(L"x"));
    TAKE(str_compress_images_title(L"x"));
    TAKE(str_convert_images_title(L"x"));
    TAKE(str_open_with_title(L"x"));
    TAKE(str_copy_to_folder_description(L"x"));
    TAKE(str_move_to_folder_description(L"x"));
    TAKE(str_compress_images_description(L"x"));
    TAKE(str_convert_images_description(L"x"));
    TAKE(str_open_with_description(L"x"));
    TAKE(str_folder_missing(L"x"));
    TAKE(str_program_missing(L"x"));
    TAKE(str_open_with_failed(L"x"));
    TAKE(str_created(L"x"));
    TAKE(str_extracted_to(L"x"));
    TAKE(str_images_already_format(L"x"));
    TAKE(str_images_already_small(L"x"));
    TAKE(str_history_yesterday(L"x"));
    TAKE(str_history_date(1, 1));
    TAKE(str_version(L"x"));
    TAKE(str_update_latest(L"x"));
    TAKE(str_update_available(L"x"));
    for (int trigger = TRIGGER_SHAKE; trigger <= TRIGGER_BOTH; trigger++)
    {
        TAKE(str_status_running_detail(trigger, NULL));
        TAKE(str_status_running_detail(trigger, L"x"));
        for (int mode = TRANSFER_MOVE; mode <= TRANSFER_COPY; mode++) TAKE(str_shelf_summary(trigger, mode));
    }
    for (int language = LANGUAGE_SYSTEM; language <= LANGUAGE_ENGLISH; language++)
    {
        TAKE(str_general_summary(language, false, NULL));
        TAKE(str_general_summary(language, true, L"x"));
    }
    StrList none = {0}, some = {0};
    strlist_add(&some, L"x");
    strlist_add(&some, L"y");
    TAKE(str_actions_summary(&none));
    TAKE(str_actions_summary(&some));
    strlist_clear(&some);
    for (int theme = THEME_SYSTEM; theme <= THEME_DARK; theme++)
    {
        TAKE(str_appearance_summary(theme, false));
        TAKE(str_appearance_summary(theme, true));
    }
    TAKE(str_confirm_delete_action(L"x"));
    TAKE(str_image_size_invalid(1, 2));
    TAKE(str_onboarding_actions_body(NULL));
    TAKE(str_onboarding_actions_body(L"x"));
}

wchar_t *language_template_build(bool from_chinese)
{
    Collected collected = {0};
    language_set_collector(collect, &collected);
    for (int id = 0; id < STR_COUNT; id++) S((StringId)id);
    call_parameterized();
    language_set_collector(NULL, NULL);

    // 同一条原文在几处出现时只列一次。
    StrList *keys = from_chinese ? &collected.chinese : &collected.english;
    StrList *others = from_chinese ? &collected.english : &collected.chinese;
    StrList written = {0};
    JsonWriter writer = {0};
    json_write_begin_object(&writer, NULL);
    json_write_string(&writer, L"name", L"");
    json_write_string(&writer, L"from", from_chinese ? L"zh" : L"en");
    json_write_begin_object(&writer, L"strings");
    for (int index = 0; index < keys->count; index++)
    {
        if (strlist_index_of(&written, keys->items[index], false) >= 0) continue;
        strlist_add(&written, keys->items[index]);
        json_write_string(&writer, keys->items[index], L"");
    }
    json_write_end_object(&writer);
    json_write_begin_object(&writer, L"reference");
    strlist_clear(&written);
    for (int index = 0; index < keys->count; index++)
    {
        if (strlist_index_of(&written, keys->items[index], false) >= 0) continue;
        strlist_add(&written, keys->items[index]);
        json_write_string(&writer, keys->items[index], others->items[index]);
    }
    json_write_end_object(&writer);
    json_write_end_object(&writer);
    strlist_clear(&written);
    strlist_clear(&collected.chinese);
    strlist_clear(&collected.english);
    return json_write_finish(&writer);
}
