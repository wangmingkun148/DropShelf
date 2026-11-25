cmake_minimum_required(VERSION 3.20)

# 版本号改动时同步 res/app.manifest 和 installer 下的两个 .iss 文件（见仓库根目录的 CLAUDE.md）。
project(DropShelf VERSION 3.1.0 LANGUAGES C CXX RC)

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
    src/card_face.c
    src/clipboard.c
    src/drop_data.c
    src/drop_read.c
    src/file_ops.c
    src/file_dialog.c
    src/file_identity.c
    src/folder_watch.c
    src/foreground.c
    src/gesture.c
    src/gesture_service.c
    src/gfx.cpp
    src/hotkey.c
    src/history_menu.c
    src/image_quality.c
    src/image_tools.c
    src/json.c
    src/lang.c
    src/look_dispatch.c
    src/look_switch.c
    src/language_template.c
    src/log.c
    src/main_actions.c
    src/main_dialogs.c
    src/main_fan.c
    src/main_flip.c
    src/main_window.c
    src/onboarding_window.c
    src/power.c
    src/probe.c
    src/quick_action.c
    src/settings.c
    src/shelf_card.c
    src/shelf_drag.c
    src/shelf_history.c
    src/shelf_item.c
    src/shelf_merge.c
    src/shelf_model.c
    src/shelf_peek.c
    src/shelves.c
    src/shortcut_icons.c
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
# 编译选项单独放在一个接口库里：下面两种风格的源文件也用同样的选项。
add_library(dropshelf_flags INTERFACE)
target_include_directories(dropshelf_flags INTERFACE src)
target_compile_definitions(dropshelf_flags INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    DROPSHELF_VERSION=L"${PROJECT_VERSION}"
)
# 源文件是 UTF-8，里面直接写中文。gfx.cpp 是唯一的 C++ 文件，不用异常和运行时类型信息。
target_compile_options(dropshelf_flags INTERFACE /utf-8 /W4 /wd4100)
target_link_libraries(dropshelf_core PUBLIC dropshelf_flags)

# 置物架和设置页在两种风格（简洁、纸牌）下差别很大：这几个文件各编译两遍，纸牌那一遍定义 LOOK_VARIANT_CARDS，
# 对外的函数分别加上 simple_、cards_ 前缀（见 look_variant.h），由 look_dispatch.c 按当前的风格调用其中一份。
set(LOOK_SOURCES src/shelf_window.c src/shelf_input.c src/main_pages.c)
add_library(look_simple OBJECT ${LOOK_SOURCES} src/shelf_motion.c)
add_library(look_cards OBJECT ${LOOK_SOURCES} src/shelf_motion_cards.c)
target_compile_definitions(look_cards PRIVATE LOOK_VARIANT_CARDS)
foreach(look look_simple look_cards)
    target_link_libraries(${look} PRIVATE dropshelf_flags)
    target_sources(dropshelf_core PRIVATE $<TARGET_OBJECTS:${look}>)
endforeach()
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
target_link_libraries(dropshelf_core PUBLIC miniz user32 gdi32 comctl32 version shell32 advapi32 ole32 imm32 winhttp dwmapi shcore shlwapi windowscodecs d2d1 d3d11 dxgi dwrite dcomp dxguid)

configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

add_executable(DropShelf WIN32
    src/main.c
    res/DropShelf.rc
    res/app.manifest
)
target_include_directories(DropShelf PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(DropShelf PRIVATE dropshelf_core)
# 清单里已经写了权限级别，不让链接器再生成一份。
target_link_options(DropShelf PRIVATE /MANIFESTUAC:NO)
# 提示框等系统控件用新版的样式。
target_link_options(DropShelf PRIVATE "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

enable_testing()
add_executable(DropShelfTests tests/tests.c)
target_link_libraries(DropShelfTests PRIVATE dropshelf_core)
# 测试要读仓库里的清单和安装脚本，核对四处版本号是否一致。
target_compile_definitions(DropShelfTests PRIVATE DROPSHELF_REPOSITORY_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME DropShelfTests COMMAND DropShelfTests)
