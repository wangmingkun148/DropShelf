cmake_minimum_required(VERSION 3.20)

# 版本号只写在 C# 工程里（见仓库根目录的 CLAUDE.md），这里读出来用，不用多维护一处。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../src/DropShelf/DropShelf.csproj" DROPSHELF_VERSION_LINE REGEX "<Version>")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" DROPSHELF_VERSION "${DROPSHELF_VERSION_LINE}")
if(NOT DROPSHELF_VERSION)
    message(FATAL_ERROR "没有在 DropShelf.csproj 里找到版本号")
endif()

project(DropShelf VERSION ${DROPSHELF_VERSION} LANGUAGES C CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "DropShelf 只支持用 MSVC 编译")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
# 静态链接运行库：发布的程序不依赖另外安装的 VC++ 运行库。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 第三方：miniz 处理 ZIP（MIT 许可，原样放在 third_party/miniz）。只用到压缩包读写，警告不归我们管。
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES _CRT_SECURE_NO_WARNINGS)
target_compile_options(miniz PRIVATE /W0 /utf-8)

add_library(dropshelf_core STATIC
    src/about_window.c
    src/anim.c
    src/app.c
    src/archive.c
    src/attachment_cache.c
    src/base.c
    src/clipboard.c
    src/drop_data.c
    src/drop_read.c
    src/file_ops.c
    src/file_dialog.c
    src/file_identity.c
    src/foreground.c
    src/gesture.c
    src/gesture_service.c
    src/gfx.cpp
    src/hotkey.c
    src/history_menu.c
    src/image_tools.c
    src/json.c
    src/lang.c
    src/language_template.c
    src/log.c
    src/main_actions.c
    src/main_dialogs.c
    src/main_fan.c
    src/main_flip.c
    src/main_pages.c
    src/main_window.c
    src/onboarding_window.c
    src/power.c
    src/probe.c
    src/quick_action.c
    src/settings.c
    src/shelf_drag.c
    src/shelf_history.c
    src/shelf_input.c
    src/shelf_item.c
    src/shelf_merge.c
    src/shelf_model.c
    src/shelf_motion.c
    src/shelf_window.c
    src/shelves.c
    src/single_instance.c
    src/startup.c
    src/strings.c
    src/task.c
    src/theme.c
    src/thumbnail.c
    src/tray.c
    src/ui_controls.c
    src/ui_inputs.c
    src/ui_menu.c
    src/ui_window.c
    src/update_check.c
)
target_include_directories(dropshelf_core PUBLIC src)
target_compile_definitions(dropshelf_core PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DROPSHELF_VERSION=L"${DROPSHELF_VERSION}"
)
# 源文件是 UTF-8，里面直接写中文。gfx.cpp 是唯一的 C++ 文件，不用异常和运行时类型信息。
target_compile_options(dropshelf_core PUBLIC /utf-8 /W4 /wd4100)
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
target_link_libraries(dropshelf_core PUBLIC miniz user32 gdi32 comctl32 version shell32 advapi32 ole32 imm32 winhttp dwmapi shcore shlwapi windowscodecs d2d1 d3d11 dxgi dwrite dcomp dxguid)

configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

# 清单与 C# 版共用一份，版本号不用多改一处。
add_executable(DropShelf WIN32
    src/main.c
    res/DropShelf.rc
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/DropShelf/app.manifest"
)
target_include_directories(DropShelf PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(DropShelf PRIVATE dropshelf_core)
# 清单里已经写了权限级别，不让链接器再生成一份。
target_link_options(DropShelf PRIVATE /MANIFESTUAC:NO)
# 提示框等系统控件用新版的样式（C# 版不需要这一项，所以没有写在共用的清单里）。
target_link_options(DropShelf PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

enable_testing()
add_executable(DropShelfTests tests/tests.c)
target_link_libraries(DropShelfTests PRIVATE dropshelf_core)
add_test(NAME DropShelfTests COMMAND DropShelfTests)
