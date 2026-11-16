// 主窗口里的三个对话框（Windows 11 对话框样式，盖在半透明的遮罩上）：添加或编辑自定义操作、确认删除、晃动示范校准。
// 和 C# 版的 CustomActionEditorViewModel、ShakeCalibrationViewModel、MainWindow.Calibration.cs 以及 MainWindow.xaml 里的对话框对应。
#include "main_window_internal.h"
#include "file_dialog.h"
#include "gesture.h"
#include "quick_action.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

typedef enum DialogKind { DIALOG_NONE, DIALOG_EDITOR, DIALOG_CONFIRM, DIALOG_CALIBRATION } DialogKind;

/// 一个对话框：上面是内容，下面一条底色不同的按钮栏。
typedef struct Dialog
{
    UiElement base;
    float width;
    UiElement *body, *footer;
} Dialog;

typedef struct Overlay
{
    UiElement base;
    DialogKind kind;
    Dialog *dialog;
    Anim opacity, scale;
    bool closing;
} Overlay;

static UiElement *layer;
static Overlay *overlays[4];
static DialogKind open_kind;

// ───────────── 对话框和遮罩 ─────────────

static GfxSize dialog_measure(UiElement *element, float available)
{
    Dialog *dialog = (Dialog *)element;
    float width = dialog->width < available ? dialog->width : available;
    ui_measure(dialog->body, width);
    ui_measure(dialog->footer, width);
    return (GfxSize){ width, dialog->body->desired.height + dialog->footer->desired.height };
}

static void dialog_arrange(UiElement *element)
{
    Dialog *dialog = (Dialog *)element;
    GfxRect bounds = element->bounds;
    float split = bounds.top + dialog->body->desired.height;
    ui_arrange(dialog->body, (GfxRect){ bounds.left, bounds.top, bounds.right, split });
    ui_arrange(dialog->footer, (GfxRect){ bounds.left, split, bounds.right, bounds.bottom });
}

static void dialog_paint(UiElement *element, double time)
{
    (void)time;
    Dialog *dialog = (Dialog *)element;
    GfxRect bounds = element->bounds;
    gfx_shadow(bounds, 8, 32, 0, 8, COLOR_ARGB(56, 0, 0, 0));
    gfx_fill_rounded(bounds, 8, theme_color(THEME_FLYOUT));
    GfxRect footer = dialog->footer->bounds;
    gfx_fill_rounded_corners((GfxRect){ bounds.left + 1, footer.top, bounds.right - 1, bounds.bottom - 1 }, 0, 7, theme_color(THEME_WINDOW_BACKGROUND));
    gfx_fill_rect((GfxRect){ bounds.left + 1, footer.top, bounds.right - 1, footer.top + 1 }, theme_color(THEME_DIVIDER));
    gfx_stroke_rounded(bounds, 8, theme_color(THEME_FLYOUT_BORDER), 1);
}

static const UiElementType dialog_type = { dialog_measure, dialog_arrange, dialog_paint, NULL, NULL, false };

static Dialog *dialog_new(float width)
{
    Dialog *dialog = (Dialog *)ui_element_new(&dialog_type, sizeof(Dialog));
    dialog->width = width;
    dialog->body = ui_stack(false, 0);
    ui_stack_set_padding(dialog->body, 24, 22, 24, 24);
    dialog->footer = ui_stack(true, 0);
    ui_stack_set_padding(dialog->footer, 24, 20, 24, 20);
    ui_add(&dialog->base, dialog->body);
    ui_add(&dialog->base, dialog->footer);
    return dialog;
}

static GfxSize overlay_measure(UiElement *element, float available)
{
    Overlay *overlay = (Overlay *)element;
    ui_measure(&overlay->dialog->base, available - 48);
    return (GfxSize){ available, 0 };
}

static void overlay_arrange(UiElement *element)
{
    Overlay *overlay = (Overlay *)element;
    GfxRect bounds = element->bounds;
    GfxSize size = overlay->dialog->base.desired;
    float left = (bounds.left + bounds.right - size.width) / 2, top = (bounds.top + bounds.bottom - size.height) / 2;
    if (top < bounds.top + 24) top = bounds.top + 24;
    ui_arrange(&overlay->dialog->base, (GfxRect){ left, top, left + size.width, top + size.height });
}

static void overlay_paint(UiElement *element, double time)
{
    (void)time;
    Overlay *overlay = (Overlay *)element;
    gfx_fill_rect(element->bounds, theme_color(THEME_SMOKE));
    // 确认框从略小处放大到原尺寸。
    float scale = (float)overlay->scale.value;
    GfxRect box = overlay->dialog->base.bounds;
    overlay->dialog->base.transformed = scale != 1;
    overlay->dialog->base.transform = gfx_matrix_scale(scale, scale, (box.left + box.right) / 2, (box.top + box.bottom) / 2);
}

static void overlay_mouse(UiElement *element, const UiMouseEvent *event)
{
    // 遮罩挡住下面的内容；点在遮罩上不关闭对话框（和 C# 版一样）。
    (void)element;
    (void)event;
}

static const UiElementType overlay_type = { overlay_measure, overlay_arrange, overlay_paint, overlay_mouse, NULL, true, NULL, NULL, NULL, false, NULL, IDC_ARROW, true };

static Overlay *overlay_new(DialogKind kind, float width)
{
    Overlay *overlay = (Overlay *)ui_element_new(&overlay_type, sizeof(Overlay));
    overlay->kind = kind;
    overlay->dialog = dialog_new(width);
    overlay->base.hidden = true;
    anim_set(&overlay->opacity, 1);
    anim_set(&overlay->scale, 1);
    ui_add(&overlay->base, &overlay->dialog->base);
    return overlay;
}

static void show_overlay(DialogKind kind, bool animate)
{
    Overlay *overlay = overlays[kind];
    open_kind = kind;
    overlay->base.hidden = false;
    overlay->closing = false;
    if (animate && anim_enabled())
    {
        anim_set(&overlay->opacity, 0);
        anim_start(&overlay->opacity, 1, 0.15, EASE_OF(EASE_LINEAR));
        anim_set(&overlay->scale, 0.96);
        anim_start(&overlay->scale, 1, MOTION_SECONDS, MOTION_EASE);
    }
    else
    {
        anim_set(&overlay->opacity, 1);
        anim_set(&overlay->scale, 1);
    }
    ui_window_set_focus(M->window, NULL);
    main_relayout();
    main_request_frame();
    ui_window_update_hover(M->window);
}

static void hide_overlay(DialogKind kind, bool animate)
{
    Overlay *overlay = overlays[kind];
    if (overlay->base.hidden) return;
    if (open_kind == kind) open_kind = DIALOG_NONE;
    ui_window_set_focus(M->window, NULL);
    if (animate && anim_enabled())
    {
        overlay->closing = true;
        anim_start(&overlay->opacity, 0, 0.12, EASE_OF(EASE_LINEAR));
        overlay->base.hit_through = true;
        main_request_frame();
        return;
    }
    overlay->base.hidden = true;
    main_relayout();
    ui_window_update_hover(M->window);
}

bool dialogs_open(void)
{
    return open_kind != DIALOG_NONE;
}

static UiElement *field_label(UiElement *parent, StringId text, float top)
{
    UiElement *label = ui_label(UI_STR(text), UI_TEXT_BODY);
    ui_label_set_size(label, 13);
    ui_set_margin(label, 0, top, 0, 6);
    ui_add(parent, label);
    return label;
}

static UiElement *footer_buttons(Dialog *dialog, float width, int count)
{
    UiElement *spacer = ui_spacer();
    ui_add(dialog->footer, spacer);
    UiElement *grid = ui_grid(count, 8, 0);
    grid->width = width;
    ui_add(dialog->footer, grid);
    return grid;
}

// ───────────── 添加或编辑自定义操作 ─────────────

static struct
{
    CustomAction original;
    bool is_new;
    bool show_errors;
    UiElement *title;
    UiElement *kind;
    UiElement *folder_section, *size_section, *format_section, *program_section;
    UiElement *folder, *folder_mode, *size, *format, *program, *arguments, *name;
    UiElement *error_row, *error_text;
    UiElement *save;
} E;

static const int kind_order[4] = { CUSTOM_ACTION_FOLDER, CUSTOM_ACTION_COMPRESS_IMAGE, CUSTOM_ACTION_CONVERT_IMAGE, CUSTOM_ACTION_OPEN_WITH };

static int editor_kind(void)
{
    // 分段选择没有读取的接口，按显示的分区判断。
    if (!E.folder_section->hidden) return CUSTOM_ACTION_FOLDER;
    if (!E.size_section->hidden) return CUSTOM_ACTION_COMPRESS_IMAGE;
    if (!E.format_section->hidden) return CUSTOM_ACTION_CONVERT_IMAGE;
    return CUSTOM_ACTION_OPEN_WITH;
}

static int folder_mode_value = TRANSFER_MOVE;
static int format_value = IMAGE_FORMAT_JPEG;

static bool parse_size(int *size)
{
    wchar_t *text = wcs_trim_dup(ui_textbox_text(E.size));
    bool ok = *text != 0;
    long value = 0;
    for (const wchar_t *cursor = text; ok && *cursor; cursor++)
    {
        if (*cursor < L'0' || *cursor > L'9' || value > 100000000) ok = false;
        else value = value * 10 + (*cursor - L'0');
    }
    free(text);
    if (!ok || value < CUSTOM_ACTION_MINIMUM_IMAGE_KB || value > CUSTOM_ACTION_MAXIMUM_IMAGE_KB) return false;
    *size = (int)value;
    return true;
}

static wchar_t *trimmed_program(void)
{
    wchar_t *text = wcs_trim_dup(ui_textbox_text(E.program));
    // 去掉两边的引号。
    size_t length = wcslen(text);
    size_t start = 0;
    while (start < length && text[start] == L'"') start++;
    while (length > start && text[length - 1] == L'"') length--;
    wchar_t *result = wcs_dup_n(text + start, length - start);
    free(text);
    return result;
}

/// 按对话框里填的内容生成操作；include_name 为 false 时名称留空（算出自动生成的名称）。
static void build_action(CustomAction *action, bool include_name)
{
    custom_action_clone(action, &E.original);
    int kind = editor_kind();
    action->kind = kind;
    free(action->name);
    action->name = NULL;
    if (include_name && !wcs_is_blank(ui_textbox_text(E.name))) action->name = wcs_trim_dup(ui_textbox_text(E.name));
    free(action->folder);
    action->folder = kind == CUSTOM_ACTION_FOLDER ? wcs_trim_dup(ui_textbox_text(E.folder)) : NULL;
    action->folder_mode = folder_mode_value;
    int size;
    action->image_size_kb = parse_size(&size) ? size : CUSTOM_ACTION_DEFAULT_IMAGE_KB;
    action->image_format = format_value;
    free(action->program_path);
    action->program_path = kind == CUSTOM_ACTION_OPEN_WITH ? trimmed_program() : NULL;
    free(action->arguments);
    action->arguments = kind == CUSTOM_ACTION_OPEN_WITH && !wcs_is_blank(ui_textbox_text(E.arguments)) ? wcs_trim_dup(ui_textbox_text(E.arguments)) : NULL;
}

/// 填得不对时的提示；没问题时返回 NULL。调用方 free。
static wchar_t *validate(void)
{
    switch (editor_kind())
    {
        case CUSTOM_ACTION_FOLDER:
        {
            if (wcs_is_blank(ui_textbox_text(E.folder))) return wcs_dup(S(STR_ChooseFolderFirst));
            wchar_t *folder = wcs_trim_dup(ui_textbox_text(E.folder));
            bool exists = path_is_directory(folder);
            free(folder);
            return exists ? NULL : wcs_dup(S(STR_FolderNotFoundShort));
        }
        case CUSTOM_ACTION_COMPRESS_IMAGE:
        {
            int size;
            return parse_size(&size) ? NULL : str_image_size_invalid(CUSTOM_ACTION_MINIMUM_IMAGE_KB, CUSTOM_ACTION_MAXIMUM_IMAGE_KB);
        }
        case CUSTOM_ACTION_OPEN_WITH:
        {
            if (wcs_is_blank(ui_textbox_text(E.program))) return wcs_dup(S(STR_ChooseProgramFirst));
            wchar_t *program = trimmed_program();
            bool exists = path_exists(program) && !path_is_directory(program);
            free(program);
            return exists ? NULL : wcs_dup(S(STR_ProgramNotFoundShort));
        }
        default:
            return NULL;
    }
}

/// 名称留空时使用的名称显示成输入框的提示文字；第一次点保存之后才显示错误，避免刚打开对话框就是一片红字。
static void editor_refresh(void)
{
    if (!E.name) return;
    CustomAction action;
    build_action(&action, false);
    QuickAction quick = { action.id, QUICK_ACTION_CUSTOM, true, action };
    wchar_t *placeholder = quick_action_label(&quick);
    ui_textbox_set_placeholder(E.name, UI_LIT(placeholder));
    free(placeholder);
    custom_action_free(&action);
    ui_label_set_text(E.title, UI_STR(E.is_new ? STR_AddCustomAction : STR_EditCustomAction));
    wchar_t *error = E.show_errors ? validate() : NULL;
    E.error_row->hidden = error == NULL;
    if (error) ui_label_set_text(E.error_text, UI_LIT(error));
    free(error);
    main_relayout();
}

static void show_kind(int kind)
{
    E.folder_section->hidden = kind != CUSTOM_ACTION_FOLDER;
    E.size_section->hidden = kind != CUSTOM_ACTION_COMPRESS_IMAGE;
    E.format_section->hidden = kind != CUSTOM_ACTION_CONVERT_IMAGE;
    E.program_section->hidden = kind != CUSTOM_ACTION_OPEN_WITH;
}

static void on_kind(int index, void *context)
{
    (void)context;
    show_kind(kind_order[index]);
    editor_refresh();
}

static void on_folder_mode(int index, void *context) { (void)context; folder_mode_value = index; editor_refresh(); }
static void on_format(int index, void *context) { (void)context; format_value = index; editor_refresh(); }
static void on_field(const wchar_t *text, void *context) { (void)text; (void)context; editor_refresh(); }

static void change_save_action(AppSettings *settings, void *context)
{
    const CustomAction *action = context;
    for (int index = 0; index < settings->custom_action_count; index++)
    {
        if (!wcs_equal(settings->custom_actions[index].id, action->id)) continue;
        custom_action_free(&settings->custom_actions[index]);
        custom_action_clone(&settings->custom_actions[index], action);
        return;
    }
    settings->custom_actions = xrealloc(settings->custom_actions, (size_t)(settings->custom_action_count + 1) * sizeof(CustomAction));
    custom_action_clone(&settings->custom_actions[settings->custom_action_count++], action);
}

static void close_editor(void)
{
    hide_overlay(DIALOG_EDITOR, false);
    custom_action_free(&E.original);
    memset(&E.original, 0, sizeof(E.original));
}

static void on_save(void *context)
{
    (void)context;
    E.show_errors = true;
    wchar_t *error = validate();
    if (error)
    {
        free(error);
        editor_refresh();
        return;
    }
    CustomAction action;
    build_action(&action, true);
    app_update_settings(change_save_action, &action);
    custom_action_free(&action);
    close_editor();
}

static void on_cancel_editor(void *context)
{
    (void)context;
    close_editor();
}

static void on_browse_folder(void *context)
{
    (void)context;
    wchar_t *current = wcs_trim_dup(ui_textbox_text(E.folder));
    wchar_t *folder = file_dialog_folder(ui_window_handle(M->window), S(STR_ChooseTargetFolder), path_is_directory(current) ? current : NULL);
    if (folder)
    {
        ui_textbox_set_text(E.folder, folder);
        editor_refresh();
    }
    free(folder);
    free(current);
}

static void on_browse_program(void *context)
{
    (void)context;
    wchar_t *current = trimmed_program();
    wchar_t *directory = path_exists(current) ? path_directory(current) : NULL;
    wchar_t *program = file_dialog_open(ui_window_handle(M->window), S(STR_ChooseProgram), S(STR_ProgramFilter), directory);
    if (program)
    {
        ui_textbox_set_text(E.program, program);
        editor_refresh();
    }
    free(program);
    free(directory);
    free(current);
}

static UiElement *field_row(UiElement *textbox, UiElement *button)
{
    UiElement *row = ui_stack(true, 8);
    textbox->grow = 1;
    ui_add(row, textbox);
    if (button) ui_add(row, button);
    return row;
}

static void build_editor(void)
{
    Overlay *overlay = overlay_new(DIALOG_EDITOR, 560);
    overlays[DIALOG_EDITOR] = overlay;
    UiElement *body = overlay->dialog->body;
    E.title = ui_label(UI_STR(STR_AddCustomAction), UI_TEXT_SUBTITLE);
    ui_add(body, E.title);
    field_label(body, STR_ActionKind, 18);
    UiText kinds[4] = { UI_STR(STR_KindFolder), UI_STR(STR_KindCompressImage), UI_STR(STR_KindConvertImage), UI_STR(STR_KindOpenWith) };
    E.kind = ui_segmented(kinds, 4, 0, on_kind, NULL);
    E.kind->cross = UI_CROSS_START;
    ui_add(body, E.kind);

    // 文件夹
    E.folder_section = ui_stack(false, 0);
    field_label(E.folder_section, STR_FolderLabel, 16);
    E.folder = ui_textbox(L"", UI_NO_TEXT, 0, on_field, on_save, NULL);
    ui_add(E.folder_section, field_row(E.folder, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Browse), on_browse_folder, NULL)));
    field_label(E.folder_section, STR_FolderModeLabel, 16);
    UiText modes[2] = { UI_STR(STR_TransferMove), UI_STR(STR_TransferCopy) };
    E.folder_mode = ui_segmented(modes, 2, 0, on_folder_mode, NULL);
    E.folder_mode->cross = UI_CROSS_START;
    ui_add(E.folder_section, E.folder_mode);
    ui_add(body, E.folder_section);

    // 压缩图片
    E.size_section = ui_stack(false, 0);
    field_label(E.size_section, STR_ImageSizeLabel, 16);
    UiElement *size_row = ui_stack(true, 0);
    E.size = ui_textbox(L"", UI_NO_TEXT, 9, on_field, on_save, NULL);
    E.size->width = 120;
    ui_add(size_row, E.size);
    UiElement *unit = ui_label(UI_LIT(L"KB"), UI_TEXT_BODY);
    unit->cross = UI_CROSS_CENTER;
    ui_set_margin(unit, 10, 0, 0, 1);
    ui_add(size_row, unit);
    ui_add(E.size_section, size_row);
    UiElement *size_hint = ui_label(UI_STR(STR_ImageSizeHint), UI_TEXT_CAPTION);
    ui_set_margin(size_hint, 0, 6, 0, 0);
    ui_add(E.size_section, size_hint);
    ui_add(body, E.size_section);

    // 转换图片
    E.format_section = ui_stack(false, 0);
    field_label(E.format_section, STR_ImageFormatLabel, 16);
    UiText formats[2] = { UI_LIT(L"JPG"), UI_LIT(L"PNG") };
    E.format = ui_segmented(formats, 2, 0, on_format, NULL);
    E.format->cross = UI_CROSS_START;
    ui_add(E.format_section, E.format);
    UiElement *format_hint = ui_label(UI_STR(STR_ImageFormatHint), UI_TEXT_CAPTION);
    ui_set_margin(format_hint, 0, 6, 0, 0);
    ui_add(E.format_section, format_hint);
    ui_add(body, E.format_section);

    // 用程序打开
    E.program_section = ui_stack(false, 0);
    field_label(E.program_section, STR_ProgramLabel, 16);
    E.program = ui_textbox(L"", UI_NO_TEXT, 0, on_field, on_save, NULL);
    ui_add(E.program_section, field_row(E.program, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Browse), on_browse_program, NULL)));
    field_label(E.program_section, STR_ArgumentsLabel, 16);
    E.arguments = ui_textbox(L"", UI_LIT(L"{files}"), 0, on_field, on_save, NULL);
    ui_add(E.program_section, E.arguments);
    UiElement *arguments_hint = ui_label(UI_STR(STR_ArgumentsHint), UI_TEXT_CAPTION);
    ui_set_margin(arguments_hint, 0, 6, 0, 0);
    ui_add(E.program_section, arguments_hint);
    ui_add(body, E.program_section);

    field_label(body, STR_NameLabel, 16);
    E.name = ui_textbox(L"", UI_NO_TEXT, CUSTOM_ACTION_MAXIMUM_NAME, on_field, on_save, NULL);
    ui_add(body, E.name);

    E.error_row = ui_stack(true, 0);
    ui_set_margin(E.error_row, 0, 14, 0, 0);
    UiElement *error_glyph = ui_label(UI_LIT(GLYPH_ERROR), UI_TEXT_GLYPH);
    ui_label_set_size(error_glyph, 13);
    ui_label_set_color(error_glyph, THEME_CRITICAL);
    error_glyph->cross = UI_CROSS_START;
    ui_set_margin(error_glyph, 0, 2, 8, 0);
    ui_add(E.error_row, error_glyph);
    E.error_text = ui_label(UI_NO_TEXT, UI_TEXT_BODY);
    ui_label_set_size(E.error_text, 13);
    ui_label_set_color(E.error_text, THEME_CRITICAL);
    E.error_text->grow = 1;
    ui_add(E.error_row, E.error_text);
    E.error_row->hidden = true;
    ui_add(body, E.error_row);

    UiElement *buttons = footer_buttons(overlay->dialog, 260, 2);
    E.save = ui_button(UI_BUTTON_ACCENT, UI_STR(STR_Save), on_save, NULL);
    ui_add(buttons, E.save);
    ui_add(buttons, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Cancel), on_cancel_editor, NULL));
}

void dialogs_edit_action(const CustomAction *action)
{
    if (!layer) return;
    if (!action && app_settings()->custom_action_count >= QUICK_ACTION_MAXIMUM_CUSTOM) return;
    custom_action_free(&E.original);
    if (action) custom_action_clone(&E.original, action);
    else
    {
        custom_action_init(&E.original);
        E.original.kind = CUSTOM_ACTION_FOLDER;
    }
    E.is_new = action == NULL;
    E.show_errors = false;
    const CustomAction *source = &E.original;
    int kind_index = 0;
    for (int index = 0; index < 4; index++)
        if (kind_order[index] == source->kind) kind_index = index;
    ui_segmented_set_selected(E.kind, kind_index);
    show_kind(source->kind);
    ui_textbox_set_text(E.folder, source->folder ? source->folder : L"");
    folder_mode_value = source->folder_mode;
    ui_segmented_set_selected(E.folder_mode, source->folder_mode);
    wchar_t size[16];
    swprintf(size, 16, L"%d", source->image_size_kb);
    ui_textbox_set_text(E.size, size);
    format_value = source->image_format;
    ui_segmented_set_selected(E.format, source->image_format);
    ui_textbox_set_text(E.program, source->program_path ? source->program_path : L"");
    ui_textbox_set_text(E.arguments, source->arguments ? source->arguments : L"");
    ui_textbox_set_text(E.name, source->name ? source->name : L"");
    editor_refresh();
    show_overlay(DIALOG_EDITOR, false);
}

// ───────────── 确认删除 ─────────────

static wchar_t *pending_delete;
static UiElement *confirm_message;

static void close_confirm(void)
{
    free(pending_delete);
    pending_delete = NULL;
    hide_overlay(DIALOG_CONFIRM, true);
}

static void on_confirm_delete(void *context)
{
    (void)context;
    if (pending_delete) actions_delete_custom(pending_delete);
    close_confirm();
}

static void on_confirm_cancel(void *context)
{
    (void)context;
    close_confirm();
}

static void build_confirm(void)
{
    Overlay *overlay = overlay_new(DIALOG_CONFIRM, 400);
    overlays[DIALOG_CONFIRM] = overlay;
    UiElement *body = overlay->dialog->body;
    ui_add(body, ui_label(UI_STR(STR_ConfirmDeleteTitle), UI_TEXT_SUBTITLE));
    confirm_message = ui_label(UI_NO_TEXT, UI_TEXT_BODY);
    ui_label_style(confirm_message)->line_height = 20;
    ui_set_margin(confirm_message, 0, 12, 0, 0);
    ui_add(body, confirm_message);
    UiElement *buttons = footer_buttons(overlay->dialog, 260, 2);
    ui_add(buttons, ui_button(UI_BUTTON_ACCENT, UI_STR(STR_DeleteTooltip), on_confirm_delete, NULL));
    ui_add(buttons, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Cancel), on_confirm_cancel, NULL));
}

/// 和编辑对话框相同样式的确认框：遮罩淡入，对话框从略小处放大到原尺寸。
void dialogs_confirm_delete(const CustomAction *action, const wchar_t *title)
{
    if (!layer || !action) return;
    free(pending_delete);
    pending_delete = wcs_dup(action->id);
    wchar_t *message = str_confirm_delete_action(title);
    ui_label_set_text(confirm_message, UI_LIT(message));
    free(message);
    show_overlay(DIALOG_CONFIRM, true);
}

// ───────────── 晃动示范校准 ─────────────

static struct
{
    int levels[CALIBRATION_DEMONSTRATIONS];
    int done;
    bool too_weak;
    UiElement *pad, *result, *slider, *result_text, *status, *dots[3];
    UiElement *apply, *retry;
    ShakeSample *stroke;
    int stroke_count, stroke_capacity;
    bool stroking;
    float origin_x, origin_y;
    Anim chip_x, chip_y;
    float follow_x, follow_y;
} C;

static bool calibration_complete(void)
{
    return C.done >= CALIBRATION_DEMONSTRATIONS;
}

static int calibration_recommended(void)
{
    return calibration_complete() ? shake_calibration_recommend(C.levels, C.done) : SHAKE_BALANCED;
}

static void calibration_refresh(void)
{
    if (!C.pad) return;
    bool complete = calibration_complete();
    C.pad->hidden = complete;
    C.result->hidden = !complete;
    for (int index = 0; index < 3; index++) C.dots[index]->disabled = C.done <= index;
    if (complete)
    {
        ui_slider_set_value(C.slider, calibration_recommended());
        int reversals = 0, distance = 0;
        shake_describe(calibration_recommended(), &reversals, &distance);
        wchar_t *text = str_sensitivity_detail(reversals, distance);
        ui_label_set_text(C.result_text, UI_LIT(text));
        free(text);
        ui_label_set_text(C.status, UI_STR(STR_CalibrationDone));
    }
    else if (C.too_weak) ui_label_set_text(C.status, UI_STR(STR_CalibrationTooWeak));
    else if (C.done == 0) ui_label_set_text(C.status, UI_STR(STR_CalibrationStart));
    else
    {
        wchar_t *text = str_calibration_progress(C.done, CALIBRATION_DEMONSTRATIONS - C.done);
        ui_label_set_text(C.status, UI_LIT(text));
        free(text);
    }
    ui_set_disabled(C.apply, !complete);
    ui_set_disabled(C.retry, !(C.done > 0 || C.too_weak));
    main_relayout();
}

static long long now_milliseconds(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (long long)(counter.QuadPart * 1000 / frequency.QuadPart);
}

static void add_sample(void)
{
    POINT cursor;
    GetCursorPos(&cursor);
    if (C.stroke_count == C.stroke_capacity)
    {
        C.stroke_capacity = C.stroke_capacity ? C.stroke_capacity * 2 : 256;
        C.stroke = xrealloc(C.stroke, (size_t)C.stroke_capacity * sizeof(ShakeSample));
    }
    C.stroke[C.stroke_count++] = (ShakeSample){ cursor.x, cursor.y, now_milliseconds() };
}

bool dialogs_calibration_submit(const ShakeSample *samples, int count)
{
    // 晃得太轻、最灵敏也触发不了时不算数。
    if (calibration_complete()) return false;
    int level = shake_calibration_steadiest_level(samples, count);
    C.too_weak = level < 0;
    if (level >= 0) C.levels[C.done++] = level;
    calibration_refresh();
    return level >= 0;
}

static void end_stroke(void)
{
    if (!C.stroking) return;
    C.stroking = false;
    // 松手后弹回中间。
    anim_set(&C.chip_x, C.follow_x);
    anim_set(&C.chip_y, C.follow_y);
    anim_start(&C.chip_x, 0, 0.26, EASE_BACK_OF(0.4));
    anim_start(&C.chip_y, 0, 0.26, EASE_BACK_OF(0.4));
    main_request_frame();
    dialogs_calibration_submit(C.stroke, C.stroke_count);
    C.stroke_count = 0;
}

static GfxSize pad_measure(UiElement *element, float available)
{
    (void)element;
    return (GfxSize){ available, 170 };
}

static void pad_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    gfx_fill_rounded(bounds, 8, theme_color(THEME_CONTROL_ALT));
    gfx_stroke_rounded(bounds, 8, theme_color(THEME_DIVIDER), 1);
    gfx_push_layer(bounds, 8, 1);
    float dx = C.stroking ? C.follow_x : (float)C.chip_x.value, dy = C.stroking ? C.follow_y : (float)C.chip_y.value;
    float cx = (bounds.left + bounds.right) / 2 + dx, cy = (bounds.top + bounds.bottom) / 2 + dy;
    GfxRect chip = { cx - 46, cy - 46, cx + 46, cy + 46 };
    gfx_fill_rounded(chip, 10, theme_color(THEME_SHELF_TILE));
    gfx_stroke_rounded(chip, 10, theme_color(THEME_SHELF_TILE_BORDER), 1);
    GfxTextStyle glyph = { GFX_FONT_ICON, 30, 400, false, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    GfxTextStyle hint = { GFX_FONT_TEXT, 11, 400, true, false, GFX_ALIGN_CENTER, GFX_ALIGN_START };
    float glyph_height = gfx_text_measure(GLYPH_DOCUMENT, &glyph, 0).height;
    float hint_height = gfx_text_measure(S(STR_CalibrationPadHint), &hint, 92 - 8).height;
    float top = cy - (glyph_height + 8 + hint_height) / 2;
    Color secondary = theme_color(THEME_TEXT_SECONDARY);
    gfx_text_draw(GLYPH_DOCUMENT, (GfxRect){ chip.left, top, chip.right, top + glyph_height }, &glyph, secondary);
    gfx_text_draw(S(STR_CalibrationPadHint), (GfxRect){ chip.left + 4, top + glyph_height + 8, chip.right - 4, top + glyph_height + 8 + hint_height }, &hint,
        secondary);
    gfx_pop_layer();
}

static void pad_mouse(UiElement *element, const UiMouseEvent *event)
{
    (void)element;
    switch (event->kind)
    {
        case UI_MOUSE_DOWN:
            if (event->right_button || calibration_complete()) return;
            C.stroking = true;
            C.stroke_count = 0;
            C.origin_x = event->x;
            C.origin_y = event->y;
            C.follow_x = C.follow_y = 0;
            anim_set(&C.chip_x, 0);
            anim_set(&C.chip_y, 0);
            add_sample();
            break;
        case UI_MOUSE_MOVE:
            if (!C.stroking) return;
            add_sample();
            // 那张“文件”跟着光标走，像真的在拖。
            C.follow_x = event->x - C.origin_x;
            C.follow_y = event->y - C.origin_y;
            main_repaint();
            break;
        case UI_MOUSE_UP:
            end_stroke();
            break;
        default:
            break;
    }
}

static const UiElementType pad_type = { pad_measure, NULL, pad_paint, pad_mouse, NULL, true };

static GfxSize dot_measure(UiElement *element, float available)
{
    (void)element;
    (void)available;
    return (GfxSize){ 8, 8 };
}

static void dot_paint(UiElement *element, double time)
{
    (void)time;
    GfxRect bounds = element->bounds;
    bool lit = !element->disabled;
    Color color = theme_color(lit ? THEME_ACCENT : THEME_CONTROL_STRONG_BORDER);
    if (!lit) color.a = (uint8_t)(color.a * 0.4f);
    gfx_fill_ellipse((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2, 4, 4, color);
}

static const UiElementType dot_type = { dot_measure, NULL, dot_paint, NULL, NULL, false };

static void close_calibration(void)
{
    C.stroking = false;
    hide_overlay(DIALOG_CALIBRATION, false);
}

static void on_apply_calibration(void *context)
{
    (void)context;
    if (!calibration_complete()) return;
    int level = calibration_recommended();
    close_calibration();
    pages_apply_calibration(level);
}

static void on_retry_calibration(void *context)
{
    (void)context;
    C.done = 0;
    C.too_weak = false;
    calibration_refresh();
}

static void on_cancel_calibration(void *context)
{
    (void)context;
    close_calibration();
}

static void build_calibration(void)
{
    Overlay *overlay = overlay_new(DIALOG_CALIBRATION, 480);
    overlays[DIALOG_CALIBRATION] = overlay;
    UiElement *body = overlay->dialog->body;
    ui_stack_set_padding(body, 24, 22, 24, 22);
    ui_add(body, ui_label(UI_STR(STR_CalibrationTitle), UI_TEXT_SUBTITLE));
    UiElement *intro = ui_label(UI_STR(STR_CalibrationIntro), UI_TEXT_BODY);
    ui_label_set_size(intro, 13);
    ui_label_set_color(intro, THEME_TEXT_SECONDARY);
    ui_set_margin(intro, 0, 10, 0, 0);
    ui_add(body, intro);
    C.pad = ui_element_new(&pad_type, sizeof(UiElement));
    ui_set_margin(C.pad, 0, 16, 0, 0);
    ui_add(body, C.pad);

    // 结果：滑块上标出建议的位置，下面是这个位置需要晃多远、掉头几次。
    C.result = ui_stack(false, 0);
    ui_set_margin(C.result, 0, 20, 0, 0);
    C.slider = ui_slider(0, 100, SHAKE_BALANCED, 1, NULL, NULL);
    ui_slider_set_read_only(C.slider, true);
    ui_add(C.result, C.slider);
    UiElement *ends = ui_stack(true, 0);
    ui_set_margin(ends, 0, -3, 0, 0);
    UiElement *steady = ui_label(UI_STR(STR_SensitivitySteady), UI_TEXT_CAPTION);
    ui_label_style(steady)->wrap = false;
    steady->grow = 1;
    ui_add(ends, steady);
    UiElement *responsive = ui_label(UI_STR(STR_SensitivityResponsive), UI_TEXT_CAPTION);
    ui_label_style(responsive)->wrap = false;
    ui_add(ends, responsive);
    ui_add(C.result, ends);
    C.result_text = ui_label(UI_NO_TEXT, UI_TEXT_BODY);
    ui_label_set_size(C.result_text, 13);
    ui_set_margin(C.result_text, 0, 12, 0, 0);
    ui_add(C.result, C.result_text);
    ui_add(body, C.result);

    // 进度：三个点，记下一次亮一个。
    UiElement *progress = ui_stack(true, 0);
    ui_set_margin(progress, 0, 16, 0, 0);
    for (int index = 0; index < 3; index++)
    {
        C.dots[index] = ui_element_new(&dot_type, sizeof(UiElement));
        C.dots[index]->cross = UI_CROSS_CENTER;
        ui_set_margin(C.dots[index], 0, 0, 6, 0);
        ui_add(progress, C.dots[index]);
    }
    C.status = ui_label(UI_NO_TEXT, UI_TEXT_BODY);
    ui_label_set_size(C.status, 13);
    ui_label_set_color(C.status, THEME_TEXT_SECONDARY);
    C.status->cross = UI_CROSS_CENTER;
    C.status->grow = 1;
    ui_set_margin(C.status, 6, 0, 0, 1);
    ui_add(progress, C.status);
    ui_add(body, progress);

    UiElement *buttons = footer_buttons(overlay->dialog, 390, 3);
    C.apply = ui_button(UI_BUTTON_ACCENT, UI_STR(STR_CalibrationApply), on_apply_calibration, NULL);
    C.retry = ui_button(UI_BUTTON_STANDARD, UI_STR(STR_CalibrationRetry), on_retry_calibration, NULL);
    ui_add(buttons, C.apply);
    ui_add(buttons, C.retry);
    ui_add(buttons, ui_button(UI_BUTTON_STANDARD, UI_STR(STR_Cancel), on_cancel_calibration, NULL));
    anim_set(&C.chip_x, 0);
    anim_set(&C.chip_y, 0);
}

void dialogs_open_calibration(void)
{
    if (!layer) return;
    C.done = 0;
    C.too_weak = false;
    C.stroking = false;
    calibration_refresh();
    show_overlay(DIALOG_CALIBRATION, false);
}

// ───────────── 公共 ─────────────

static void layer_arrange(UiElement *element)
{
    for (int index = 0; index < element->child_count; index++) ui_arrange(element->children[index], element->bounds);
}

static GfxSize layer_measure(UiElement *element, float available)
{
    for (int index = 0; index < element->child_count; index++) ui_measure(element->children[index], available);
    return (GfxSize){ available, 0 };
}

static const UiElementType layer_type = { layer_measure, layer_arrange, NULL, NULL, NULL, false };

UiElement *dialogs_create(void)
{
    layer = ui_element_new(&layer_type, sizeof(UiElement));
    open_kind = DIALOG_NONE;
    build_calibration();
    build_editor();
    build_confirm();
    ui_add(layer, &overlays[DIALOG_CALIBRATION]->base);
    ui_add(layer, &overlays[DIALOG_EDITOR]->base);
    ui_add(layer, &overlays[DIALOG_CONFIRM]->base);
    return layer;
}

bool dialogs_escape(void)
{
    switch (open_kind)
    {
        case DIALOG_CONFIRM: close_confirm(); return true;
        case DIALOG_EDITOR: close_editor(); return true;
        case DIALOG_CALIBRATION: close_calibration(); return true;
        default: return false;
    }
}

void dialogs_refresh(void)
{
    if (!layer) return;
    if (!overlays[DIALOG_EDITOR]->base.hidden) editor_refresh();
    if (!overlays[DIALOG_CALIBRATION]->base.hidden) calibration_refresh();
}

void dialogs_animate(double time)
{
    if (!layer) return;
    bool running = anim_update(&C.chip_x, time) | anim_update(&C.chip_y, time);
    for (int kind = DIALOG_EDITOR; kind <= DIALOG_CALIBRATION; kind++)
    {
        Overlay *overlay = overlays[kind];
        running |= anim_update(&overlay->opacity, time);
        running |= anim_update(&overlay->scale, time);
        overlay->base.opacity = (float)overlay->opacity.value;
        if (overlay->closing && !overlay->opacity.running)
        {
            overlay->closing = false;
            overlay->base.hidden = true;
            overlay->base.hit_through = false;
            main_relayout();
        }
    }
    if (running) main_request_frame();
}

bool dialogs_mouse(const UiMouseEvent *event)
{
    (void)event;
    return false;
}

void dialogs_close(void)
{
    custom_action_free(&E.original);
    memset(&E, 0, sizeof(E));
    free(pending_delete);
    pending_delete = NULL;
    free(C.stroke);
    memset(&C, 0, sizeof(C));
    confirm_message = NULL;
    layer = NULL;
    memset(overlays, 0, sizeof(overlays));
    open_kind = DIALOG_NONE;
}

// ───────────── 测试用 ─────────────

UiElement *dialogs_part(DialogPart part)
{
    switch (part)
    {
        case DIALOG_PART_EDITOR_KIND: return E.kind;
        case DIALOG_PART_EDITOR_SIZE: return E.size;
        case DIALOG_PART_EDITOR_NAME: return E.name;
        case DIALOG_PART_EDITOR_SAVE: return E.save;
        case DIALOG_PART_EDITOR_ERROR: return E.error_row;
        case DIALOG_PART_CALIBRATION_STATUS: return C.status;
        case DIALOG_PART_CALIBRATION_APPLY: return C.apply;
        case DIALOG_PART_CALIBRATION_RETRY: return C.retry;
    }
    return NULL;
}

int dialogs_calibration_done(void)
{
    return C.done;
}

int dialogs_calibration_recommended(void)
{
    return calibration_recommended();
}
