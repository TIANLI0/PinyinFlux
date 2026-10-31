; SPDX-License-Identifier: MIT
; PinyinFlux installer (Inno Setup 7). Build with build\build-installer.ps1.
;
; What gets installed: the settings app (PinyinFlux.exe, WinUI 3, self-contained) and the resident
; native core and injection DLL. Installation is per user. The input host may require the core
; to be restarted with UAC from the settings app before native integration becomes available.
;
; Enabling (postinstall checkbox, or "安装并启动" in the app) runs PinyinFluxCore.exe --install:
; Native mode does not create a certificate or install a PAC. Legacy proxy mode retains both.
;
; Upgrade ("覆盖升级"):
;   * Fixed AppId; a running core is stopped (--stop restores the proxy setting), the previous version
;     is uninstalled silently with /UPGRADE=1, and the core is started again afterwards if it was enabled.
;   * The upgrade uninstall keeps the certificate, proxy backup and settings, so nothing is re-trusted.
;
; Uninstall: PinyinFluxCore.exe --uninstall restores the original proxy settings, deletes the
; certificate and its private key, and removes autostart. Settings and logs in
; %LOCALAPPDATA%\PinyinFlux are deleted only if the user agrees.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef AppVersionNumeric
  #define AppVersionNumeric "0.1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\artifacts\PinyinFlux-win-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\artifacts\installer"
#endif
#define AppName "PinyinFlux"
#define AppExe "PinyinFlux.exe"
#define CoreExe "PinyinFluxCore.exe"
#define AppIdGuid "7C1F4E52-3B8A-4D6E-9A21-5F0B8C3D2E91"

[Setup]
AppId={{{#AppIdGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Tianli
AppPublisherURL=https://github.com/TIANLI0/PinyinFlux
AppSupportURL=https://github.com/TIANLI0/PinyinFlux/issues
AppUpdatesURL=https://github.com/TIANLI0/PinyinFlux/releases
AppCopyright=Copyright (c) 2026 Tianli (MIT)
VersionInfoVersion={#AppVersionNumeric}
VersionInfoProductVersion={#AppVersionNumeric}
VersionInfoDescription={#AppName} Setup
; Per-user install (no administrator rights): %LOCALAPPDATA%\Programs\PinyinFlux.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
UsePreviousAppDir=yes
UsePreviousTasks=yes
UsePreviousLanguage=yes
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
LicenseFile=..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=PinyinFlux-Setup-{#AppVersion}-win-x64
SetupIconFile=..\src\PinyinFlux.App\Assets\PinyinFlux.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=auto

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimplified.TaskDesktopIcon=创建桌面快捷方式
chinesesimplified.TaskGroupOptions=其他选项：
chinesesimplified.EnableNow=启用原生云候选实验模式（可能需要管理员权限）
chinesesimplified.LaunchApp=打开 PinyinFlux 设置
chinesesimplified.DeleteUserData=是否同时删除 PinyinFlux 的设置、日志和代理设置备份？%n%n（位于 %1）
english.TaskDesktopIcon=Create a desktop shortcut
english.TaskGroupOptions=Additional options:
english.EnableNow=Enable experimental native cloud candidates (may require administrator privileges)
english.LaunchApp=Open PinyinFlux settings
english.DeleteUserData=Also delete PinyinFlux's settings, logs and proxy-setting backup?%n%n(located in %1)

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktopIcon}"; GroupDescription: "{cm:TaskGroupOptions}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; First install: offer to enable right away, set per-user autostart and start the configured mode.
Filename: "{app}\{#CoreExe}"; Parameters: "--install"; Description: "{cm:EnableNow}"; Flags: postinstall runhidden waituntilterminated skipifsilent; Check: not WasEnabled
; Upgrade: the core was stopped before copying files. --install starts it again and points autostart at
; this copy. Native injection may still need a later explicit UAC restart.
Filename: "{app}\{#CoreExe}"; Parameters: "--install"; Flags: runhidden waituntilterminated; Check: WasEnabled
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + '{#AppIdGuid}' + '}_is1';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

var
  Enabled: Boolean;

function IsUpgradeUninstall(): Boolean;
begin
  Result := ExpandConstant('{param:UPGRADE|0}') = '1';
end;

// Enabled = the core's autostart entry exists (written by --install, removed by --uninstall).
function WasEnabled(): Boolean;
begin
  Result := Enabled;
end;

function GetPreviousUninstaller(): String;
var
  Value: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, UninstallKey, 'UninstallString', Value) then
    Result := RemoveQuotes(Value);
end;

function GetPreviousAppDir(): String;
var
  Value: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, UninstallKey, 'InstallLocation', Value) then
    Result := RemoveBackslashUnlessRoot(Value);
end;

procedure RunCore(const Dir, Verb: String);
var
  ResultCode: Integer;
begin
  if (Dir <> '') and FileExists(AddBackslash(Dir) + '{#CoreExe}') then
    Exec(AddBackslash(Dir) + '{#CoreExe}', Verb, Dir, SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Uninstaller, PreviousDir: String;
  ResultCode: Integer;
begin
  Result := '';
  Enabled := RegValueExists(HKCU, RunKey, '{#AppName}');
  PreviousDir := GetPreviousAppDir();
  // --stop waits for the core to exit and restores the proxy setting, so its files can be replaced.
  RunCore(PreviousDir, '--stop');
  Uninstaller := GetPreviousUninstaller();
  if (Uninstaller <> '') and FileExists(Uninstaller) then
  begin
    // Remove the previous version's files so no stale binaries remain. Certificate and settings are kept.
    Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /UPGRADE=1', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    // Before files are deleted: an upgrade only stops the core; a real uninstall reverts everything
    // (original proxy settings, certificate + private key, autostart).
    if IsUpgradeUninstall() then
      RunCore(ExpandConstant('{app}'), '--stop')
    else
      RunCore(ExpandConstant('{app}'), '--uninstall');
  end;

  if CurUninstallStep <> usPostUninstall then Exit;
  if IsUpgradeUninstall() then Exit;

  // Belt and braces: the autostart entry must never outlive the program.
  RegDeleteValue(HKCU, RunKey, '{#AppName}');

  DataDir := ExpandConstant('{localappdata}\{#AppName}');
  if DirExists(DataDir) and not UninstallSilent() then
  begin
    if MsgBox(FmtMessage(CustomMessage('DeleteUserData'), [DataDir]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(DataDir, True, True, True);
  end;
end;
