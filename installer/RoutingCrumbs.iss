; Routing Crumbs — Windows installer (Inno Setup 6, free & open source)
; https://jrsoftware.org/isinfo.php
;
; Build from repo root after a Release build:
;   cmake --build build --config Release --target installer
; Or manually:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\RoutingCrumbs.iss

#ifndef MyBuildDir
  #define MyBuildDir "..\build\Release"
#endif

#include "ci-version.iss"

#define MyAppName "Routing Crumbs"
#define MyAppPublisher "Routing Crumbs"
#define MyAppExeName "RoutingCrumbs.exe"

[Setup]
AppId={{A7B3C4D5-E6F7-4890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=RoutingCrumbs-Setup-{#MyAppVersion}
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=
InfoBeforeFile=
ChangesAssociations=no
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyBuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""RoutingCrumbs"" /F"; Flags: runhidden; RunOnceId: "RemoveAutostartTask"

