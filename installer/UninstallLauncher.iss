#define AppName "DropShelf"
#define AppVersion "2.6.7"

[Setup]
AppName={#AppName} 卸载程序
AppVersion={#AppVersion}
VersionInfoVersion=2.6.7.0
VersionInfoDescription=卸载 {#AppName}
DefaultDirName={tmp}
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
OutputDir=..\artifacts\win-x64
OutputBaseFilename=卸载 DropShelf
SetupIconFile=..\src\DropShelf\Assets\DropShelf.ico
Compression=lzma2/fast
SolidCompression=no
WizardStyle=modern
DisableWelcomePage=yes
DisableReadyPage=yes
DisableFinishedPage=yes

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Code]
function InitializeSetup(): Boolean;
var
  Uninstaller: String;
  UninstallParameters: String;
  ResultCode: Integer;
begin
  Result := False;
  Uninstaller := AddBackslash(ExpandConstant('{src}')) + 'unins000.exe';

  if not FileExists(Uninstaller) then
    Uninstaller := ExpandConstant('{localappdata}\Programs\DropShelf\unins000.exe');

  if not FileExists(Uninstaller) then
  begin
    MsgBox('没有找到已安装的 DropShelf。便携版不需要卸载：请先完全退出程序，再删除整个便携版文件夹。',
      mbInformation, MB_OK);
    Exit;
  end;

  UninstallParameters := '';
  if WizardSilent then
    UninstallParameters := '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART';

  if not ShellExec('', Uninstaller, UninstallParameters, ExtractFileDir(Uninstaller), SW_SHOWNORMAL,
    ewNoWait, ResultCode) then
    MsgBox('无法启动 DropShelf 卸载程序，请稍后重试。', mbError, MB_OK);
end;
