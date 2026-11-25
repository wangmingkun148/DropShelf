# 按 CLAUDE.md 的规则，四个地方的版本号必须一致：native/CMakeLists.txt、native/res/app.manifest、两个 Inno Setup 脚本。
# 便携包和 PKGBUILD 也顺带核对，免得发版时对不上。
# legacy/csharp 是停用的旧版，不再同步版本号，这里不查它。
if(NOT ROOT)
    message(FATAL_ERROR "没有给出 ROOT")
endif()

function(read_version FILE PATTERN OUT)
    file(READ "${FILE}" text)
    string(REGEX MATCH "${PATTERN}" match "${text}")
    if(NOT match)
        message(FATAL_ERROR "在 ${FILE} 里找不到版本号")
    endif()
    set(${OUT} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

read_version("${ROOT}/native/CMakeLists.txt" "project\\(DropShelf VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" version)
read_version("${ROOT}/native/res/app.manifest" "assemblyIdentity version=\"([0-9]+\\.[0-9]+\\.[0-9]+)" manifest)
read_version("${ROOT}/installer/DropShelf.iss" "#define AppVersion \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" installer)
read_version("${ROOT}/installer/UninstallLauncher.iss" "#define AppVersion \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" uninstaller)
read_version("${ROOT}/linux/PKGBUILD" "pkgver=([0-9]+\\.[0-9]+\\.[0-9]+)" pkgbuild)

set(expected "${version}")
foreach(pair "app.manifest=${manifest}" "installer/DropShelf.iss=${installer}" "installer/UninstallLauncher.iss=${uninstaller}" "linux/PKGBUILD=${pkgbuild}")
    string(FIND "${pair}" "=" equals)
    math(EXPR value_start "${equals} + 1")
    string(SUBSTRING "${pair}" 0 ${equals} name)
    string(SUBSTRING "${pair}" ${value_start} -1 value)
    if(NOT value STREQUAL expected)
        message(FATAL_ERROR "${name} 的版本号 ${value} 和 native/CMakeLists.txt 的 ${expected} 不一致")
    endif()
endforeach()

message(STATUS "版本号一致：${version}")