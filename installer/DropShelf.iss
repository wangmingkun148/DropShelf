#define AppName "DropShelf"
#define AppVersion "2.6.7"
#define AppPublisher "DropShelf"
#define AppExeName "DropShelf.exe"
#define AppURL "https://github.com/wangmingkun148/dropshelf"

[Setup]
AppId={{8EC91768-8DF1-4F25-B31F-5F78FD3D6774}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases/latest
VersionInfoVersion=2.6.7.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription=DropShelf 安装程序
VersionInfoProductName={#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\artifacts\installer
OutputBaseFilename=DropShelf-Setup-{#AppVersion}
SetupIconFile=..\src\DropShelf\Assets\DropShelf.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
UninstallFilesDir={app}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "其他选项："; Flags: unchecked
Name: "sendto"; Description: "在右键“发送到”菜单中添加 DropShelf"; GroupDescription: "其他选项："

[Files]
Source: "..\artifacts\win-x64\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\win-x64\卸载 DropShelf.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\卸载 {#AppName}"; Filename: "{app}\卸载 DropShelf.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon
Name: "{usersendto}\DropShelf 置物架"; Filename: "{app}\{#AppExeName}"; Parameters: "--shelf"; Tasks: sendto

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "DropShelf"; ValueType: none; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Description: "启动 {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM {#AppExeName} /F"; Flags: runhidden waituntilterminated; RunOnceId: "StopDropShelf"
