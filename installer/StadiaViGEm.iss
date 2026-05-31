; Stadia ViGEm — Windows installer (ViGEmBus + HidHide + app)
; Build: ISCC.exe installer\StadiaViGEm.iss

#define MyAppName "Stadia ViGEm"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "lukasfuscic19"
#define MyAppURL "https://github.com/lukasfuscic19/StadiaEmu"
#define MyAppExeName "stadia-vigem-x64.exe"

[Setup]
AppId={{A7B3C9D1-5E2F-4A8B-9C0D-1E2F3A4B5C6D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={localappdata}\Programs\StadiaViGEm
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename=StadiaViGEm-Setup-x64
SetupIconFile=..\stadia-vigem\res\icon.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\bin\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "..\bin\{#MyAppExeName}"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\Install.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\stadia-vigem\res\icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\Install.ps1"" -Silent"; \
  StatusMsg: "Installing ViGEmBus, HidHide, and application..."; \
  Flags: runhidden waituntilterminated
Filename: "{app}\bin\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\bin"
Type: files; Name: "{app}\Install.log"
