# 编译 C 版 DropShelf：找到 Visual Studio（或 Build Tools）自带的 MSVC、CMake 和 Ninja，生成到 native\build。
# 用法：  .\native\build.ps1            编译发布版并运行测试
#         .\native\build.ps1 -Debug     编译调试版
#         .\native\build.ps1 -NoTests   不运行测试
param(
    [switch]$Debug,
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$configuration = if ($Debug) { "Debug" } else { "Release" }
$build = Join-Path $root "build\$configuration"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "未找到 vswhere.exe，请先安装 Visual Studio 或 Build Tools（需要勾选 C++ 桌面开发）。" }
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { throw "未找到带 C++ 编译器的 Visual Studio 或 Build Tools。" }
$vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"

# 在同一个 cmd 里先载入编译环境，再配置和编译。
$commands = @(
    "`"$vcvars`" >nul 2>&1",
    "cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$configuration",
    "cmake --build `"$build`""
)
if (-not $NoTests) { $commands += "ctest --test-dir `"$build`" --output-on-failure" }
cmd /c ($commands -join " && ")
if ($LASTEXITCODE -ne 0) { throw "编译或测试失败（退出码 $LASTEXITCODE）。" }
Write-Host "已生成 $build\DropShelf.exe"
