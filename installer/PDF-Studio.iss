#define AppName "PDF Studio"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define AppPublisher "PDF Studio"
#define AppExeName "PDF-Studio.exe"

[Setup]
AppId={{93EE0B2C-A613-4F93-9F98-147911D5F525}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\PDF-Studio
DefaultGroupName=PDF Studio
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=PDF-Studio-Setup-{#AppVersion}-x86
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\{#AppExeName}
SetupIconFile=..\assets\icon.ico
UsePreviousAppDir=yes
CloseApplications=yes
RestartApplications=no
Uninstallable=yes

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительные ярлыки:"; Flags: unchecked
Name: "pdfassoc"; Description: "Добавить PDF Студию в список программ для PDF"; GroupDescription: "Ассоциации файлов:"; Flags: unchecked

[Files]
Source: "..\release\PDF-Studio.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\PDF Студия"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\PDF Студия"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon
Name: "{group}\Удалить PDF Студию"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\Classes\PDFStudio.Document"; ValueType: string; ValueName: ""; ValueData: "PDF document (PDF Studio)"; Flags: uninsdeletekey; Tasks: pdfassoc
Root: HKCU; Subkey: "Software\Classes\PDFStudio.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExeName},0"; Tasks: pdfassoc
Root: HKCU; Subkey: "Software\Classes\PDFStudio.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""; Tasks: pdfassoc
Root: HKCU; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: none; ValueName: "PDFStudio.Document"; Flags: uninsdeletevalue; Tasks: pdfassoc
Root: HKCU; Subkey: "Software\Classes\Applications\PDF-Studio.exe\SupportedTypes"; ValueType: string; ValueName: ".pdf"; ValueData: ""; Flags: uninsdeletekeyifempty; Tasks: pdfassoc
Root: HKCU; Subkey: "Software\Classes\Applications\PDF-Studio.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""; Flags: uninsdeletekeyifempty; Tasks: pdfassoc

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Запустить PDF Студию"; Flags: postinstall nowait skipifsilent

[Code]
const
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;

procedure SHChangeNotify(wEventId: Integer; uFlags: Cardinal; dwItem1: Integer; dwItem2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    begin
      SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
      if WizardIsTaskSelected('pdfassoc') then
        MsgBox('PDF Студия добавлена в список программ для PDF. В Windows 8 и новее назначьте программу по умолчанию в разделе Параметры > Приложения > Приложения по умолчанию.', mbInformation, MB_OK);
    end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
end;
