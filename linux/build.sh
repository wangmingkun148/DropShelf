#!/usr/bin/env bash
# 配置、编译并运行 Linux 侧的测试。窗口和 Wayland 集成在后续里程碑加入。
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build="$root/build"

cmake -S "$root" -B "$build" -G Ninja
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure