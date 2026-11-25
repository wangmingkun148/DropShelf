cmake_minimum_required(VERSION 3.20)

# 版本号以 native/CMakeLists.txt 为准（CLAUDE.md 的规则）。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../native/CMakeLists.txt" DROPSHELF_NATIVE_CMAKE)
string(REGEX MATCH "project\\(DropShelf VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" DROPSHELF_VERSION_MATCH "${DROPSHELF_NATIVE_CMAKE}")
set(DROPSHELF_VERSION "${CMAKE_MATCH_1}")
if(NOT DROPSHELF_VERSION)
    set(DROPSHELF_VERSION "0.0.0")
endif()

project(DropShelfLinux VERSION ${DROPSHELF_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
pkg_check_modules(GDK_PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

set(DROPSHELF_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../native/src")
set(DROPSHELF_LINUX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../native/platform/linux")

# 平台无关的业务模型、界面元素树，加上各自的 Linux 后端。
# 共享的 native/src 编译进 dropshelf_core，系统服务由 native/platform/linux 提供。
add_library(dropshelf_core STATIC
    "${DROPSHELF_SOURCE_DIR}/json.c"
    "${DROPSHELF_SOURCE_DIR}/gesture.c"
    "${DROPSHELF_SOURCE_DIR}/image_quality.c"
    "${DROPSHELF_SOURCE_DIR}/hotkey.c"
    "${DROPSHELF_SOURCE_DIR}/settings.c"
    "${DROPSHELF_SOURCE_DIR}/strings.c"
    "${DROPSHELF_SOURCE_DIR}/lang.c"
    "${DROPSHELF_SOURCE_DIR}/theme.c"
    "${DROPSHELF_SOURCE_DIR}/anim.c"
    "${DROPSHELF_SOURCE_DIR}/ui_controls.c"
    "${DROPSHELF_SOURCE_DIR}/ui_inputs.c"
    "${DROPSHELF_SOURCE_DIR}/ui_menu.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_item.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_model.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_history.c"
    "${DROPSHELF_SOURCE_DIR}/shelves.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_card.c"
    "${DROPSHELF_SOURCE_DIR}/card_face.c"
    "${DROPSHELF_SOURCE_DIR}/look_dispatch.c"
    "${DROPSHELF_SOURCE_DIR}/look_switch.c"
    "${DROPSHELF_SOURCE_DIR}/history_menu.c"
    "${DROPSHELF_SOURCE_DIR}/file_identity.c"
    "${DROPSHELF_SOURCE_DIR}/attachment_cache.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_peek.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_merge.c"
    "${DROPSHELF_SOURCE_DIR}/onboarding_window.c"
    "${DROPSHELF_SOURCE_DIR}/language_template.c"
    "${DROPSHELF_SOURCE_DIR}/update_check.c"
    "${DROPSHELF_SOURCE_DIR}/../third_party/miniz/miniz.c"
    "${DROPSHELF_SOURCE_DIR}/main_window.c"
    "${DROPSHELF_SOURCE_DIR}/main_flip.c"
    "${DROPSHELF_SOURCE_DIR}/main_fan.c"
    "${DROPSHELF_SOURCE_DIR}/main_dialogs.c"
    "${DROPSHELF_SOURCE_DIR}/main_actions.c"
    "${DROPSHELF_SOURCE_DIR}/about_window.c"
    "${DROPSHELF_LINUX_DIR}/app_linux.c"
    "${DROPSHELF_LINUX_DIR}/clipboard_linux.c"
    "${DROPSHELF_LINUX_DIR}/drop_read_linux.c"
    "${DROPSHELF_LINUX_DIR}/thumbnail_linux.c"
    "${DROPSHELF_LINUX_DIR}/services_linux.c"
    "${DROPSHELF_LINUX_DIR}/file_dialog_linux.c"
    "${DROPSHELF_LINUX_DIR}/startup_linux.c"
    "${DROPSHELF_LINUX_DIR}/archive_linux.c"
    "${DROPSHELF_LINUX_DIR}/image_tools_linux.c"
    "${DROPSHELF_LINUX_DIR}/properties_linux.c"
    "${DROPSHELF_LINUX_DIR}/task_linux.c"
    "${DROPSHELF_LINUX_DIR}/base_linux.c"
    "${DROPSHELF_LINUX_DIR}/shell_linux.c"
    "${DROPSHELF_LINUX_DIR}/wide_linux.c"
    "${DROPSHELF_LINUX_DIR}/log_linux.c"
    "${DROPSHELF_LINUX_DIR}/monitor_linux.c"
    "${DROPSHELF_LINUX_DIR}/hyprland_linux.c"
    "${DROPSHELF_LINUX_DIR}/gesture_service_linux.c"
    "${DROPSHELF_LINUX_DIR}/foreground_linux.c"
    "${DROPSHELF_LINUX_DIR}/probe_linux.c"
    "${DROPSHELF_LINUX_DIR}/hotkey_linux.c"
    "${DROPSHELF_LINUX_DIR}/tray_linux.c"
    "${DROPSHELF_LINUX_DIR}/folder_watch_linux.c"
    "${DROPSHELF_LINUX_DIR}/anim_linux.c"
    "${DROPSHELF_LINUX_DIR}/theme_linux.c"
    "${DROPSHELF_LINUX_DIR}/shortcut_icons_linux.c"
    "${DROPSHELF_LINUX_DIR}/gfx_gtk.c"
    "${DROPSHELF_LINUX_DIR}/ui_window_gtk.c"
)

# 编译选项单独放在一个接口库里：下面两种风格的源文件也用同样的选项。
add_library(dropshelf_flags INTERFACE)
target_include_directories(dropshelf_flags INTERFACE
    "${DROPSHELF_SOURCE_DIR}"
    "${DROPSHELF_LINUX_DIR}"
    "${DROPSHELF_SOURCE_DIR}/../third_party/miniz"
)
target_compile_definitions(dropshelf_flags INTERFACE _GNU_SOURCE DROPSHELF_VERSION=L\"${DROPSHELF_VERSION}\")
target_compile_options(dropshelf_flags INTERFACE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(dropshelf_flags INTERFACE Threads::Threads m PkgConfig::GTK4 PkgConfig::LAYER_SHELL PkgConfig::GDK_PIXBUF PkgConfig::CURL PkgConfig::FONTCONFIG)
target_link_libraries(dropshelf_core PUBLIC dropshelf_flags)

# 置物架和设置页在两种风格（简洁、纸牌）下差别很大：这几个文件各编译两遍，纸牌那一遍定义 LOOK_VARIANT_CARDS，
# 对外的函数分别加上 simple_、cards_ 前缀（见 look_variant.h），由 look_dispatch.c 按当前的风格调用其中一份。
set(LOOK_SOURCES
    "${DROPSHELF_SOURCE_DIR}/shelf_window.c"
    "${DROPSHELF_SOURCE_DIR}/shelf_input.c"
    "${DROPSHELF_SOURCE_DIR}/main_pages.c")
add_library(look_simple OBJECT ${LOOK_SOURCES} "${DROPSHELF_SOURCE_DIR}/shelf_motion.c")
add_library(look_cards OBJECT ${LOOK_SOURCES} "${DROPSHELF_SOURCE_DIR}/shelf_motion_cards.c")
target_compile_definitions(look_cards PRIVATE LOOK_VARIANT_CARDS)
foreach(look look_simple look_cards)
    target_link_libraries(${look} PRIVATE dropshelf_flags)
    target_sources(dropshelf_core PRIVATE $<TARGET_OBJECTS:${look}>)
endforeach()

# 图标字体放在可执行文件旁边，运行时由 gfx 交给 fontconfig；安装和便携版放在 share/dropshelf/fonts。
configure_file("${DROPSHELF_LINUX_DIR}/fonts/DropShelfIcons.ttf" "${CMAKE_CURRENT_BINARY_DIR}/DropShelfIcons.ttf" COPYONLY)

add_executable(dropshelf "${DROPSHELF_LINUX_DIR}/main_linux.c")
target_compile_definitions(dropshelf PRIVATE DROPSHELF_VERSION_UTF8="${DROPSHELF_VERSION}")
target_compile_options(dropshelf PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(dropshelf PRIVATE dropshelf_core)

include(GNUInstallDirs)
install(TARGETS dropshelf RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/io.github.wangmingkun148.DropShelf.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/io.github.wangmingkun148.DropShelf.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/icons/hicolor" DESTINATION "${CMAKE_INSTALL_DATADIR}/icons")
install(FILES "${DROPSHELF_LINUX_DIR}/fonts/DropShelfIcons.ttf" DESTINATION "${CMAKE_INSTALL_DATADIR}/dropshelf/fonts")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dropshelf")
install(FILES "${DROPSHELF_LINUX_DIR}/fonts/DropShelfIcons-LICENSE.txt" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dropshelf")

enable_testing()

add_test(NAME version_check COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/.. -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/version_check.cmake)

add_executable(core_tests tests/core_tests.c)
target_link_libraries(core_tests PRIVATE dropshelf_core)
target_compile_options(core_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME core_tests COMMAND core_tests)

add_executable(archive_tests tests/archive_tests.c)
target_link_libraries(archive_tests PRIVATE dropshelf_core)
target_compile_options(archive_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME archive_tests COMMAND archive_tests)

add_executable(image_tests tests/image_tests.c)
target_link_libraries(image_tests PRIVATE dropshelf_core)
target_compile_options(image_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME image_tests COMMAND image_tests)

add_executable(quick_action_tests tests/quick_action_tests.c)
target_link_libraries(quick_action_tests PRIVATE dropshelf_core)
target_compile_options(quick_action_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME quick_action_tests COMMAND quick_action_tests)

add_executable(ui_tests tests/ui_tests.c)
target_link_libraries(ui_tests PRIVATE dropshelf_core)
target_compile_options(ui_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME ui_tests COMMAND ui_tests)