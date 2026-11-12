param(
    [string]$DotnetPath = "dotnet",
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
$appProject = Join-Path $projectRoot "src\DropShelf\DropShelf.csproj"
$portableOutput = Join-Path $projectRoot "artifacts\win-x64"
$installerScript = Join-Path $PSScriptRoot "DropShelf.iss"
$uninstallLauncherScript = Join-Path $PSScriptRoot "UninstallLauncher.iss"

& $DotnetPath restore $appProject -r win-x64 -p:PublishReadyToRun=true -p:NuGetAudit=false
if ($LASTEXITCODE -ne 0) { throw "无法准备发布文件。" }

# 自包含单文件：目标电脑无需安装 .NET。
# 预编译（ReadyToRun）让启动和第一次弹出置物架时不必等待即时编译；
# 不压缩单文件，程序集可直接从文件映射而不必解压到内存，占用更少。安装包本身仍会压缩。
& $DotnetPath publish $appProject -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:PublishReadyToRun=true -p:DebugType=none `
    --no-restore -o $portableOutput
if ($LASTEXITCODE -ne 0) { throw "无法生成 DropShelf.exe。" }
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
