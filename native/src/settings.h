// 设置：%LOCALAPPDATA%\DropShelf\settings.json，文件格式与 C# 版相同，两个版本可以读写同一个文件。
#pragma once

#include "base.h"
#include "hotkey.h"

#define SETTINGS_CURRENT_VERSION 3

// 以下各组取值的名称就是设置文件里写的文字（去掉前缀），顺序与 C# 版的枚举相同。

/// 拖动时用哪种方式自动弹出置物架。
typedef enum AutoOpenTrigger { TRIGGER_SHAKE, TRIGGER_EDGE, TRIGGER_BOTH } AutoOpenTrigger;

/// 拖到屏幕边缘后需要停留多久才弹出置物架。
typedef enum EdgeDwellTime { DWELL_SHORT, DWELL_MEDIUM, DWELL_LONG } EdgeDwellTime;

typedef enum AppTheme { THEME_SYSTEM, THEME_LIGHT, THEME_DARK } AppTheme;
typedef enum AppLanguage { LANGUAGE_SYSTEM, LANGUAGE_CHINESE, LANGUAGE_ENGLISH } AppLanguage;
typedef enum QuickShelfSize { SHELF_SIZE_SMALL, SHELF_SIZE_MEDIUM, SHELF_SIZE_LARGE } QuickShelfSize;

/// 移动或复制：用于拖出置物架，也用于“发送到文件夹”的自定义操作。
typedef enum TransferMode { TRANSFER_MOVE, TRANSFER_COPY } TransferMode;

/// 置物架底部显示几个快速操作。两个时与四个占用相同空间，每个按钮更大。
typedef enum QuickActionLayout { LAYOUT_OFF = 0, LAYOUT_TWO = 2, LAYOUT_FOUR = 4 } QuickActionLayout;

/// 低性能模式什么时候启用：自动是指使用电池或系统开启节能模式时。
typedef enum PerformanceMode { PERFORMANCE_AUTO, PERFORMANCE_NORMAL, PERFORMANCE_LOW } PerformanceMode;

typedef enum QuickActionType
{
    QUICK_ACTION_NONE,
    QUICK_ACTION_PROPERTIES,
    QUICK_ACTION_COMPRESS_ZIP,
    QUICK_ACTION_COPY_FULL_PATH,
    QUICK_ACTION_CONVERT_PNG,       // 已移除，只在迁移旧设置时出现
    QUICK_ACTION_CONVERT_JPEG,      // 同上
    QUICK_ACTION_REVEAL,
    QUICK_ACTION_MOVE_TO,
    QUICK_ACTION_CUSTOM
} QuickActionType;

typedef enum CustomActionKind
{
    CUSTOM_ACTION_FOLDER,           // 复制或移动到固定的文件夹
    CUSTOM_ACTION_COMPRESS_IMAGE,   // 把图片压缩为不超过指定大小的 JPG
    CUSTOM_ACTION_OPEN_WITH,        // 用指定程序打开
    CUSTOM_ACTION_CONVERT_IMAGE     // 把图片另存为 JPG 或 PNG
} CustomActionKind;

typedef enum ImageFormat { IMAGE_FORMAT_JPEG, IMAGE_FORMAT_PNG } ImageFormat;

/// 2.5 及以前的三档晃动灵敏度，仅用于迁移旧设置。
typedef enum GestureSensitivity { SENSITIVITY_RESPONSIVE, SENSITIVITY_BALANCED, SENSITIVITY_STEADY } GestureSensitivity;

/// 晃动灵敏度 0–100：0 最稳健，100 最灵敏，50 为默认。
#define SHAKE_STEADY 0
#define SHAKE_BALANCED 50
#define SHAKE_RESPONSIVE 100

#define CUSTOM_ACTION_MINIMUM_IMAGE_KB 10
#define CUSTOM_ACTION_MAXIMUM_IMAGE_KB (100 * 1024)
#define CUSTOM_ACTION_DEFAULT_IMAGE_KB 500
#define CUSTOM_ACTION_MAXIMUM_NAME 40

/// 设置中始终保存四个位置，切换为两个时保留后两个的选择。
#define QUICK_ACTION_SLOTS 4
#define QUICK_ACTION_MAXIMUM_CUSTOM 20

/// 用户添加的快速操作。只保存操作本身的设置，名称留空时按内容自动生成。
typedef struct CustomAction
{
    wchar_t *id;
    int kind;
    wchar_t *name;
    wchar_t *folder;
    int folder_mode;
    int image_size_kb;
    int image_format;
    wchar_t *program_path;
    wchar_t *arguments;
} CustomAction;

void custom_action_init(CustomAction *action);          // 默认值，带一个新的标识
void custom_action_free(CustomAction *action);
void custom_action_clone(CustomAction *target, const CustomAction *source);
bool custom_action_same(const CustomAction *first, const CustomAction *second);

/// 缺少必需内容（文件夹或程序）的操作无法执行。
bool custom_action_is_complete(const CustomAction *action);

typedef struct AppSettings
{
    int version;
    double window_left;             // 没有记录过位置时是 NaN
    double window_top;
    double window_width;
    double window_height;

    int auto_open_trigger;
    int shake_sensitivity;
    int edge_dwell;

    /// 这些程序（文件名，如 Photoshop.exe）在前台时不自动弹出置物架，也不响应快捷键。
    StrList blocked_apps;

    int theme;
    wchar_t *accent_color;          // 自选的强调色（#RRGGBB）；"system" 时跟随 Windows，NULL 时是默认的钴蓝
    int language;
    wchar_t *language_pack;         // 选用的语言包（文件名，不含扩展名）；NULL 时不使用
    bool start_with_windows;
    int quick_shelf_size;
    int drag_out_mode;              // 从置物架拖出文件时默认移动还是复制
    int quick_action_layout;

    /// 四个位置上的快速操作标识，按从左到右的顺序；只显示两个时使用前两个。
    StrList quick_action_slots;
    CustomAction *custom_actions;
    int custom_action_count;

    bool close_when_empty;          // 全部拖出或移除后关闭置物架
    int performance_mode;
    wchar_t *hotkey;
    bool has_completed_first_run;
    bool show_getting_started;      // 概览页的“快速上手”被关闭后不再显示

    // 以下只在读取旧版本的设置文件时用到，settings_normalize 之后都已迁移，不再写回文件。
    bool has_quick_action_slots;    // 文件里有没有写 QuickActionSlots；没有时按旧版本的设置迁移
    bool has_custom_actions;        // 文件里有没有写 CustomActions；没有时提供一个示例操作
    bool has_legacy_sensitivity;    // 2.5 及以前的三档灵敏度
    int legacy_sensitivity;
    bool has_legacy_actions;        // 2.0 的快速操作列表
    int *legacy_actions;
    int legacy_action_count;
    int legacy_action;              // 1.0 只能选择一个快速操作
    bool legacy_low_performance;    // 2.7.1 及以前低性能模式是一个开关
} AppSettings;

void settings_init(AppSettings *settings);              // 全部取默认值（还没有整理）
void settings_free(AppSettings *settings);
void settings_clone(AppSettings *target, const AppSettings *source);

/// 把读到的设置整理成有效的：迁移旧版本、去掉无效的取值、补齐缺少的内容。
void settings_normalize(AppSettings *settings);

/// 读设置文件的内容；settings 需要先 settings_init。格式不对时返回 false，settings 的内容不可再用（仍需 free）。
bool settings_parse(const wchar_t *json, AppSettings *settings);
wchar_t *settings_serialize(const AppSettings *settings);

/// 设置文件的位置；环境变量 DROPSHELF_SETTINGS_PATH 可以改到别处。调用方负责 free。
wchar_t *settings_path(void);

/// 设置、最近关闭的置物架等数据所在的文件夹。调用方负责 free。
wchar_t *settings_data_directory(void);

/// 读取并整理；文件不存在或读取失败时使用默认值。
void settings_load(AppSettings *settings);

/// 保存并等写完；退出时用。平时用 settings_save_in_background。
void settings_save(const AppSettings *settings);

/// 在后台保存此刻设置的副本，连续修改时只写最新的内容。
void settings_save_in_background(const AppSettings *settings);

/// 等后台的保存写完。
void settings_flush(void);

HotkeyGesture settings_hotkey(const AppSettings *settings);

/// #RRGGBB 写法的颜色；不是这种写法时返回 false。
bool accent_color_parse(const wchar_t *text, COLORREF *color);

int shake_sensitivity_from_legacy(int legacy);
bool auto_open_uses_shake(int trigger);
bool auto_open_uses_edge(int trigger);
int edge_dwell_milliseconds(int dwell);

// ───────────── 快速操作 ─────────────

/// 内置操作的个数，以及按设置页中的排列顺序取其中一个的类型和标识（标识就是类型名称）。
int quick_action_builtin_count(void);
QuickActionType quick_action_builtin_type(int index);
const wchar_t *quick_action_builtin_id(int index);
bool quick_action_is_builtin_id(const wchar_t *id);

/// 标识对应一个内置操作或 customs 里的自定义操作。
bool quick_action_exists(const wchar_t *id, const CustomAction *customs, int custom_count);

/// 整理为四个互不重复的有效操作；缺少的位置依次用默认操作补齐。
void quick_action_normalize_slots(StrList *slots, const CustomAction *customs, int custom_count);
