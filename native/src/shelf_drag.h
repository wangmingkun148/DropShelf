// 从置物架拖出，以及拖入时的系统反馈。和 C# 版的 ShelfDragService、DropTargetHelper 对应。
// 拖出用资源管理器同款的数据对象：目标程序得到完整的文件信息，并显示带缩略图和数量的拖动图像。
#pragma once

#include "drop_data.h"
#include "shelf_item.h"

typedef enum DragOutResult
{
    DRAG_OUT_CANCELLED,
    DRAG_OUT_COPIED,
    DRAG_OUT_MOVED,
    DRAG_OUT_LINKED
} DragOutResult;

/// 允许的拖放效果。移动模式下仍允许复制（聊天软件和浏览器只接受复制）；复制模式下不允许移动；临时附件不提供快捷方式。
DWORD drag_allowed_effects(bool move_mode, bool has_temporary_files);

/// 按拖放结果判断项目的去向。资源管理器在同一磁盘“优化移动”时会返回“无操作”，只能从逻辑结果或源文件是否消失来判断已经移动。
/// performed、logical 为 -1 表示没有提供。
DragOutResult drag_resolve(HRESULT result, DWORD effect, int performed, int logical, bool source_missing);

typedef struct DragOutRequest
{
    HWND owner;
    ShelfItem **items;          // 要拖出的项目（文件已经确认还在）
    int count;
    const StrList *extra_files; // 一起选中的文本、链接写成的文件，跟在文件后面；可以是 NULL
    const wchar_t *shelf_id;
    bool move_mode;
    float scale;                // 显示缩放比例，拖动图像按它绘制
} DragOutRequest;

typedef struct DragOutOutcome
{
    DragOutResult result;
    bool refused;               // 松手的地方没有接收（不是按 Esc 取消的）：拖出文本时用来提示可以转成文件
} DragOutOutcome;

/// 资源管理器同款的数据对象（拖出、多个文件的属性窗口用）；解析不了的路径（个别文件刚被删）时返回 NULL。
IDataObject *shell_data_object_create(const StrList *paths);

/// 在后台先把这些路径解析好，之后拖出时不用在按下时等。
void shell_paths_prefetch(const StrList *paths);

/// 开始拖出，直到松手才返回。有文件时只拖出文件（和 extra_files），否则拖出文字和链接。
DragOutOutcome drag_out(const DragOutRequest *request);

// ───────────── 拖入时的系统反馈 ─────────────

typedef enum DropImageType
{
    DROP_IMAGE_INVALID = -1,
    DROP_IMAGE_NONE = 0,
    DROP_IMAGE_COPY = 1,
    DROP_IMAGE_MOVE = 2,
    DROP_IMAGE_LINK = 4,
    DROP_IMAGE_LABEL = 6,
    DROP_IMAGE_WARNING = 7
} DropImageType;

/// 拖进窗口时让 Windows 继续显示来源程序的拖动图像，并在光标旁显示说明。
typedef struct DropTargetHelper DropTargetHelper;

DropTargetHelper *drop_helper_create(void);
void drop_helper_destroy(DropTargetHelper *helper);
void drop_helper_enter(DropTargetHelper *helper, HWND window, IDataObject *data, DWORD effect);
void drop_helper_over(DropTargetHelper *helper, DWORD effect);
void drop_helper_leave(DropTargetHelper *helper);
void drop_helper_drop(DropTargetHelper *helper, IDataObject *data, DWORD effect);

/// 光标旁的说明。message 为空字符串时不显示文字。
void drop_set_description(IDataObject *data, DropImageType type, const wchar_t *message);

// ───────────── 数据对象 ─────────────

bool data_set_bytes(IDataObject *data, CLIPFORMAT format, const void *bytes, size_t size);
bool data_set_unicode(IDataObject *data, const wchar_t *format, const wchar_t *text);

/// 读一个四字节的整数（Performed DropEffect 等）；没有时返回 -1。
int data_get_int32(IDataObject *data, const wchar_t *format);
