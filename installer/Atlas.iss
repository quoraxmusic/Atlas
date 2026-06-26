; Atlas Windows Installer
; Compiled by the AtlasInstaller CMake target.
;
; Required defines passed via /D on the ISCC command line:
;   AppVersion  - e.g. 1.2.0
;   SourceRoot  - absolute path to the repository root
;   BuildDir    - absolute path to the CMake build directory

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppName=Atlas
AppVersion={#AppVersion}
AppPublisher=Jayladder Music
AppId={{6F2A4C81-D739-4B5E-A3F0-8C1E927B40D2}
DefaultDirName={autopf}\Alessio\Atlas
DisableDirPage=yes
LicenseFile={#SourceRoot}\LICENSE
OutputDir={#SourceRoot}\dist
OutputBaseFilename=Atlas-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableReadyPage=no
ArchitecturesInstallIn64BitMode=x64compatible

[Types]
Name: "full";   Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 Plugin"; Types: full custom

[Dirs]
Name: "{userdocs}\Alessio Plugins\Atlas\Factory\Presets";    Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\Factory\Wavetables"; Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\Factory\Samples";    Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\Factory\LFOs";       Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\Factory\FX";         Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\User\Presets";       Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\User\Wavetables";    Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\User\Samples";       Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\User\LFOs";          Flags: uninsneveruninstall
Name: "{userdocs}\Alessio Plugins\Atlas\User\FX";            Flags: uninsneveruninstall

[Files]
Source: "{#BuildDir}\Atlas_artefacts\Release\VST3\Atlas.vst3\*"; DestDir: "{code:GetVST3Dir}\Atlas.vst3"; Flags: recursesubdirs createallsubdirs; Components: vst3
Source: "{#SourceRoot}\installer\accessible-layout-guide.html"; DestDir: "{userdocs}\Alessio Plugins\Atlas"; DestName: "Accessible Layout Guide.html"; Flags: onlyifdoesntexist

[Code]
var
  VST3DirPage: TInputDirWizardPage;

function GetVST3Dir(Param: String): String;
begin
  Result := VST3DirPage.Values[0];
end;

procedure InitializeWizard;
begin
  VST3DirPage := CreateInputDirPage(
    wpSelectComponents,
    'Select VST3 Install Location',
    'Where should the VST3 plugin be installed?',
    'Select the folder in which Atlas.vst3 will be installed, then click Next.',
    False,
    '');
  VST3DirPage.Add('');
  VST3DirPage.Values[0] := ExpandConstant('{commonpf}\Common Files\VST3');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = VST3DirPage.ID then
    Result := not WizardIsComponentSelected('vst3');
end;
