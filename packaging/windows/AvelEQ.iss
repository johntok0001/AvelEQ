; Inno Setup script for Avel EQ (Windows, VST3).
; Build:  iscc /DAppVersion=1.0.0 packaging\windows\AvelEQ.iss
; Expects the plugin at build\AvelEQ_artefacts\Release\VST3\Avel EQ.vst3

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#define Root "..\.."

[Setup]
AppId={{7DEC0CA2-F2A5-42A4-835C-5517ADF906BB}
AppName=Avel EQ
AppVersion={#AppVersion}
AppVerName=Avel EQ {#AppVersion}
AppPublisher=Avel
DefaultDirName={commoncf64}\VST3\Avel EQ.vst3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallFilesDir={autopf}\Avel\Avel EQ
UninstallDisplayName=Avel EQ {#AppVersion} (VST3)
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Root}\dist
OutputBaseFilename=AvelEQ-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Messages]
en.WelcomeLabel2=This will install Avel EQ {#AppVersion} (VST3) into%n%nC:\Program Files\Common Files\VST3%n%nClose your DAW before continuing.
ru.WelcomeLabel2=Будет установлен Avel EQ {#AppVersion} (VST3) в папку%n%nC:\Program Files\Common Files\VST3%n%nПеред продолжением закройте DAW.
en.FinishedLabel=Avel EQ is installed. Start your DAW and rescan plugins (REAPER: Preferences > Plug-ins > VST > Re-scan).
ru.FinishedLabel=Avel EQ установлен. Запустите DAW и пересканируйте плагины (REAPER: Preferences > Plug-ins > VST > Re-scan).

[InstallDelete]
; remove files of an older version inside the bundle
Type: filesandordirs; Name: "{commoncf64}\VST3\Avel EQ.vst3"

[Files]
Source: "{#Root}\build\AvelEQ_artefacts\Release\VST3\Avel EQ.vst3\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Root}\README.md"; DestDir: "{autopf}\Avel\Avel EQ"; DestName: "README.txt"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Avel EQ.vst3"
