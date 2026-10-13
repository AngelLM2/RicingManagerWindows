; Ricing Manager for Windows — the per-user installer (Inno Setup 6; docs/04 M8, ADR-0015).
; Built by scripts\package.ps1, which passes the version and the folders:
;   ISCC /DAppVersion=1.0.0 /DBinDir=<build>\bin /DRicesDir=<build>\rices /DSourceDir=<repo> /DOutDir=<out>
;
; No admin, ever: everything goes to %LOCALAPPDATA% and HKCU. Installing over an older version stops
; the core first and starts it again after (the data folder is migrated by the core itself).
; Uninstalling stops the core (the bar gives Windows' taskbar back as it ends), offers to restore the
; original look, removes the start-with-Windows entry and asks whether to keep your data.
; Silent uninstall: /RESTORE=1|0 (default 1) and /KEEPDATA=1|0 (default 1) answer both questions.

#ifndef AppVersion
  #error AppVersion is passed by scripts\package.ps1
#endif

#define AppName "Ricing Manager"
#define AppPublisher "Ricing Manager for Windows contributors"
#define AppUrl "https://github.com/AngelLM2/RicingManagerWindows"
#define DataFolder "{localappdata}\RicingManagerWindows"

[Setup]
AppId={{6B1E3C2A-8F4D-4E7B-9A51-2D7C0F3E9B84}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
LicenseFile={#SourceDir}\LICENSE
OutputDir={#OutDir}
OutputBaseFilename=RicingManager-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\rmw-settings.exe
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[CustomMessages]
en.RestoreQuestion=Restore the Windows look you had before Ricing Manager (wallpaper, colors, cursors, taskbar)?%n%nWindows' own taskbar comes back either way.
ptbr.RestoreQuestion=Restaurar o visual do Windows de antes do Ricing Manager (papel de parede, cores, cursores, barra de tarefas)?%n%nA barra de tarefas do Windows volta de qualquer jeito.
en.KeepDataQuestion=Keep your rices, settings and history (%1)?%n%nChoose No to remove them too.
ptbr.KeepDataQuestion=Manter seus rices, configurações e histórico (%1)?%n%nEscolha Não para removê-los também.
en.DataLeft=Some files in %1 were in use and stayed. Delete that folder after restarting Windows.
ptbr.DataLeft=Alguns arquivos em %1 estavam em uso e ficaram. Apague essa pasta depois de reiniciar o Windows.
en.StoreKept=Your rice is still on: its pictures, pointers and sounds stay in %1 (Windows uses them), with what Ricing Manager needs to bring your own Windows back. Everything else was removed. To remove that folder too, install Ricing Manager again and restore the original Windows first.
ptbr.StoreKept=O seu rice continua aplicado: as imagens, ponteiros e sons dele ficam em %1 (o Windows usa esses arquivos), com o que o Ricing Manager precisa para trazer o seu Windows de volta. O resto foi removido. Para remover essa pasta também, instale o Ricing Manager de novo e restaure o Windows original antes.
en.OpenSettings=Open Ricing Manager
ptbr.OpenSettings=Abrir o Ricing Manager
en.OfficialRices=Add the official rices to your library
ptbr.OfficialRices=Adicionar os rices oficiais à sua biblioteca

[Tasks]
Name: "rices"; Description: "{cm:OfficialRices}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BinDir}\rmw-core.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw-settings.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw-bar.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw-plugin-host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw-wallpaper.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw-tiler.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\rmw-elevated.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\slint_cpp.dll"; DestDir: "{app}"; Flags: ignoreversion
; The Visual C++ runtime next to the programs (app-local: no admin, no system-wide install).
Source: "{#BinDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\docs\user-guide.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\plugins\examples\*"; DestDir: "{app}\plugins\examples"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\plugins\template\*"; DestDir: "{app}\plugins\template"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\docs\plugins\*"; DestDir: "{app}\docs\plugins"; Flags: ignoreversion recursesubdirs createallsubdirs
; The official rices, into the library. They are yours from then on: never replaced (your edits stay)
; and never removed by uninstalling unless you choose to remove your data.
; (RicesDir is a build folder: what CMake keeps there stays out.)
Source: "{#RicesDir}\*"; DestDir: "{#DataFolder}\rices"; Tasks: rices; Flags: onlyifdoesntexist uninsneveruninstall recursesubdirs createallsubdirs; Excludes: "*.rice,\CMakeFiles,\cmake_install.cmake,*.pdb,*.obj,*.ilk"

[InstallDelete]
; Up to v1.8.0 those build leftovers went into the rices folder too.
Type: filesandordirs; Name: "{#DataFolder}\rices\CMakeFiles"
Type: files; Name: "{#DataFolder}\rices\cmake_install.cmake"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\rmw-settings.exe"; Comment: "Customize Windows: rices, wallpapers, bars, plugins"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\rmw-settings.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\rmw-settings.exe"; Description: "{cm:OpenSettings}"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Files the programs leave next to themselves (crash dumps are under the data folder).
Type: filesandordirs; Name: "{app}\plugins"
Type: filesandordirs; Name: "{app}\docs"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'RicingManagerWindows';

var
  WasStartingWithWindows: Boolean;

function RunQuiet(const Exe, Params: String; Wait: TExecWait): Integer;
var
  Code: Integer;
begin
  Result := -1;
  if FileExists(Exe) then
  begin
    if Exec(Exe, Params, '', SW_HIDE, Wait, Code) then
      Result := Code;
  end;
end;

// Installing over an older version: stop the core (and so its workers) before files are replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  WasStartingWithWindows := RegValueExists(HKCU, RunKey, RunValue);
  RunQuiet(ExpandConstant('{app}\rmw.exe'), 'shutdown --wait', ewWaitUntilTerminated);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // It started with Windows before the update: start it again now (it stays only if needed).
  if (CurStep = ssPostInstall) and WasStartingWithWindows then
    RunQuiet(ExpandConstant('{app}\rmw-core.exe'), '--startup', ewNoWait);
end;

function Answer(const Question, Param: String): Boolean;
begin
  if UninstallSilent then
    Result := ExpandConstant('{param:' + Param + '|1}') <> '0'
  else
    Result := MsgBox(Question, mbConfirmation, MB_YESNO) = IDYES;
end;

// Everything in the data folder but the store: the files a rice still on screen uses (Windows points at them).
procedure RemoveAllButTheStore(const Data: String);
var
  Found: TFindRec;
  Path: String;
begin
  if not FindFirst(Data + '\*', Found) then
    exit;
  try
    repeat
      if (Found.Name <> '.') and (Found.Name <> '..') and (CompareText(Found.Name, 'store') <> 0) then
      begin
        Path := Data + '\' + Found.Name;
        if (Found.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          DelTree(Path, True, True, True)
        else
          DeleteFile(Path);
      end;
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Rmw, Data: String;
  Restored: Boolean;
begin
  if CurUninstallStep <> usUninstall then
    exit;
  Rmw := ExpandConstant('{app}\rmw.exe');
  Data := ExpandConstant('{#DataFolder}');
  Restored := False;
  if Answer(CustomMessage('RestoreQuestion'), 'RESTORE') then
    Restored := RunQuiet(Rmw, 'restore --original --wait', ewWaitUntilTerminated) = 0
  else
    // The rice stays, but its bar goes with the app: Windows' taskbar comes back as it was (not auto-hidden).
    RunQuiet(Rmw, 'restore --original --module taskbar --wait', ewWaitUntilTerminated);
  // Not at the next sign-in either; then the core goes, and the bar hands the taskbar back.
  RunQuiet(Rmw, 'autostart off', ewWaitUntilTerminated);
  RegDeleteValue(HKCU, RunKey, RunValue);
  RunQuiet(Rmw, 'shutdown --wait', ewWaitUntilTerminated);
  if DirExists(Data) and not Answer(FmtMessage(CustomMessage('KeepDataQuestion'), [Data]), 'KEEPDATA') then
  begin
    // Everything stopped above (rmw shutdown --wait ends what did not): nothing holds a file.
    if Restored then
    begin
      DelTree(Data, True, True, True);
      // The copy of the original kept next to the folder: the person's Windows is back, it is not needed.
      DeleteFile(Data + '.original.json');
      if DirExists(Data) and not UninstallSilent then
        MsgBox(FmtMessage(CustomMessage('DataLeft'), [Data]), mbInformation, MB_OK);
    end
    else
    begin
      // Not restored: the pictures, pointers and sounds on screen are in the store, and the copy of the
      // original (next to the folder) can still bring the person's Windows back after a new install.
      RemoveAllButTheStore(Data);
      if not UninstallSilent then
        MsgBox(FmtMessage(CustomMessage('StoreKept'), [Data + '\store']), mbInformation, MB_OK);
    end;
  end;
end;
