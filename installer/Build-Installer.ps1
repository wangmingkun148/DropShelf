param(
    [string]$DotnetPath = "dotnet",
    [string]$InnoCompilerPath = ""
)

$ErrorActionPreference = "Stop"
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

& $InnoCompilerPath $installerScript
if ($LASTEXITCODE -ne 0) { throw "无法生成安装包。" }
