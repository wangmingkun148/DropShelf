# 生成便携版 artifacts\win-x64\DropShelf.exe 和安装包 artifacts\installer\DropShelf-Setup-<版本号>.exe。
# 程序是 C 版（native 目录，见 native\build.ps1）。
# 这里只编译不跑测试，测试用 .\native\build.ps1 单独运行。
param(
    [string]$InnoCompilerPath = ""
)

$ErrorActionPreference = "Stop"

# 可选的代码签名：设置了环境变量 DROPSHELF_SIGN_COMMAND 时，用它给主程序、卸载程序和安装包签名，
# 命令里的 {file} 换成要签名的文件，例如
#   signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a "{file}"
# 命令按 PowerShell 的写法执行：工具的路径里有空格时写成 & "C:\路径\signtool.exe" sign … "{file}"。
# 没有设置时不签名，其余照常。详见 docs/publishing.md。
function Invoke-Sign([string]$File) {
    if ([string]::IsNullOrWhiteSpace($env:DROPSHELF_SIGN_COMMAND)) { return }
    if (-not (Test-Path -LiteralPath $File)) { throw "找不到要签名的文件：$File" }
    Write-Host "签名：$File"
    $global:LASTEXITCODE = 0
    Invoke-Expression $env:DROPSHELF_SIGN_COMMAND.Replace("{file}", $File)
    if ($LASTEXITCODE -ne 0) { throw "签名失败：$File" }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$portableOutput = Join-Path $projectRoot "artifacts\win-x64"
$installerScript = Join-Path $PSScriptRoot "DropShelf.iss"
$uninstallLauncherScript = Join-Path $PSScriptRoot "UninstallLauncher.iss"

# C 版只有一个 DropShelf.exe：运行库静态链接，不依赖 .NET 或另外安装的 VC++ 运行库。
# 版本号由 CMake 从 native\CMakeLists.txt 读出，和安装包的一致。
& (Join-Path $projectRoot "native\build.ps1") -NoTests
New-Item -ItemType Directory -Force -Path $portableOutput | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot "native\build\Release\DropShelf.exe") -Destination (Join-Path $portableOutput "DropShelf.exe") -Force
Invoke-Sign (Join-Path $portableOutput "DropShelf.exe")

if ([string]::IsNullOrWhiteSpace($InnoCompilerPath)) {
    $compilerCandidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 7\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path ${env:ProgramFiles} "Inno Setup 7\ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe")
    )
    # 也查找安装在自定义位置的 Inno Setup。
    $uninstallKeys = @(
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    $compilerCandidates += Get-ItemProperty $uninstallKeys -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -like "Inno Setup*" -and $_.InstallLocation } |
        ForEach-Object { Join-Path $_.InstallLocation "ISCC.exe" }
    $InnoCompilerPath = $compilerCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}

if ([string]::IsNullOrWhiteSpace($InnoCompilerPath) -or
    -not (Test-Path -LiteralPath $InnoCompilerPath)) {
    throw "未找到 Inno Setup 编译器。便携版已生成在 $portableOutput。"
}

& $InnoCompilerPath $uninstallLauncherScript
if ($LASTEXITCODE -ne 0) { throw "无法生成卸载程序。" }
Invoke-Sign (Join-Path $portableOutput "卸载 DropShelf.exe")

& $InnoCompilerPath $installerScript
if ($LASTEXITCODE -ne 0) { throw "无法生成安装包。" }
Get-ChildItem (Join-Path $projectRoot "artifacts\installer") -Filter "DropShelf-Setup-*.exe" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
    ForEach-Object { Invoke-Sign $_.FullName }
