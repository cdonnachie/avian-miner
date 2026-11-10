; The Windows installer, for Inno Setup 6. Built by scripts/release.sh
; windows, after the zip, from the same release directory:
;
;   ISCC /DAppVersion=1.2.2 /DNumericVersion=1.2.2 ^
;        /DSourceDir=<release dir> /DOutputDir=<out dir> avian-miner.iss
;
; Silent: setup.exe /VERYSILENT /WALLET=avn1... [/WORKER=name] [/TASKS=sensors]
;
; What it adds over the zip: it asks for the Avian address once and writes
; it where the mine-*.bat files look, installs the CPU sensors service with
; the elevation it already has and removes it again on uninstall, puts the
; three miners and the dashboard in the Start menu, and can start one of
; them at login. The zip stays for anyone who wants a folder instead.
;
; Program Files is read-only to a user, which is why nothing in {app} is
; ever written to: settings go to ProgramData, the service's log too, and
; the batch files read rather than carry the address.

#ifndef AppVersion
  #error AppVersion is passed on the command line by release.sh
#endif
#ifndef NumericVersion
  #define NumericVersion AppVersion
#endif
#ifndef SourceDir
  #error SourceDir is passed on the command line by release.sh
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{A4F0C9D2-7B3E-4E61-9D8A-2C5F1B7E6A10}
AppName=Avian Miner
AppVersion={#AppVersion}
VersionInfoVersion={#NumericVersion}
AppPublisher=The avian-miner developers
AppPublisherURL=https://github.com/cdonnachie/cpuminer-avian
AppSupportURL=https://github.com/cdonnachie/cpuminer-avian/issues
DefaultDirName={autopf}\avian-miner
DefaultGroupName=Avian Miner
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=avian-miner-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Avian Miner {#AppVersion}
UninstallDisplayIcon={app}\avian-miner.exe
; A running miner holds its exe open; the Restart Manager prompt is how an
; upgrade asks for it to be closed rather than failing on the copy.
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "sensors"; Description: "Install the CPU temperature and power service (reads through PawnIO, from pawnio.eu)"
Name: "autostart"; Description: "Start mining when Windows starts"; Flags: unchecked
Name: "autostart\cpu"; Description: "MinotaurX on the CPU"; Flags: exclusive
Name: "autostart\gpu"; Description: "X16RT on the GPU"; Flags: exclusive unchecked
Name: "autostart\both"; Description: "Both, the GPU miner on the last core"; Flags: exclusive unchecked

[Icons]
Name: "{group}\Mine MinotaurX (CPU)"; Filename: "{app}\mine-minotaurx.bat"; WorkingDir: "{app}"
Name: "{group}\Mine X16RT (GPU)"; Filename: "{app}\mine-x16rt.bat"; WorkingDir: "{app}"
Name: "{group}\Mine both"; Filename: "{app}\mine-both.bat"; WorkingDir: "{app}"
Name: "{group}\Dashboard"; Filename: "http://localhost:4049/"
Name: "{group}\Manual"; Filename: "{app}\README.md"
Name: "{group}\Uninstall Avian Miner"; Filename: "{uninstallexe}"
Name: "{commonstartup}\Avian Miner"; Filename: "{app}\mine-minotaurx.bat"; WorkingDir: "{app}"; Tasks: autostart\cpu
Name: "{commonstartup}\Avian Miner"; Filename: "{app}\mine-x16rt.bat"; WorkingDir: "{app}"; Tasks: autostart\gpu
Name: "{commonstartup}\Avian Miner"; Filename: "{app}\mine-both.bat"; WorkingDir: "{app}"; Tasks: autostart\both

[Run]
Filename: "{app}\avian-sensors.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated; Tasks: sensors; StatusMsg: "Installing the sensors service..."

[UninstallRun]
Filename: "{app}\avian-sensors.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "sensors"

[UninstallDelete]
; The log, not the settings: an address someone typed is theirs to keep for
; the next install.
Type: files; Name: "{commonappdata}\avian-miner\avian-sensors.log"

[Code]
var
  WalletPage: TInputQueryWizardPage;

procedure InitializeWizard;
begin
  WalletPage := CreateInputQueryPage(wpSelectDir,
    'Your Avian address', 'Where the rewards go',
    'The address mined coins are paid to, and a name for this machine on the ' +
    'pool. Both are kept in ' + ExpandConstant('{commonappdata}') +
    '\avian-miner\settings.cmd and can be changed there later.');
  WalletPage.Add('Avian address (avn1...):', False);
  WalletPage.Add('Worker name:', False);
  { /WALLET=... and /WORKER=... on the command line fill the page in, which
    is what a silent install (/VERYSILENT) has instead of a wizard. }
  WalletPage.Values[0] := ExpandConstant('{param:WALLET|}');
  WalletPage.Values[1] := ExpandConstant('{param:WORKER|}');
  if WalletPage.Values[1] = '' then
    WalletPage.Values[1] := GetComputerNameString;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = WalletPage.ID) and (Trim(WalletPage.Values[0]) = '') then
  begin
    MsgBox('An address is needed: without one the miner has nowhere to send ' +
           'what it finds.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Exe, Dir: String;
  Code: Integer;
  Lines: TArrayOfString;
begin
  if CurStep = ssInstall then
  begin
    { An earlier install's service holds its exe open and is registered
      against it; stop and remove it before the file is replaced. The
      sensors task puts it back afterwards. }
    Exe := ExpandConstant('{app}\avian-sensors.exe');
    if FileExists(Exe) then
      Exec(Exe, '--uninstall', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;

  if CurStep = ssPostInstall then
  begin
    Dir := ExpandConstant('{commonappdata}\avian-miner');
    ForceDirectories(Dir);
    SetArrayLength(Lines, 3);
    Lines[0] := '@rem Written by the Avian Miner installer; the mine-*.bat files read it.';
    Lines[1] := 'set "WALLET=' + Trim(WalletPage.Values[0]) + '"';
    Lines[2] := 'set "WORKER=' + Trim(WalletPage.Values[1]) + '"';
    SaveStringsToFile(Dir + '\settings.cmd', Lines, False);

    if WizardIsTaskSelected('sensors') then
    begin
      if not RegKeyExists(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO') then
        MsgBox('The sensors service is installed, but PawnIO is not, so it has ' +
               'nothing to read yet. Install PawnIO from https://pawnio.eu; the ' +
               'service looks for it every thirty seconds and needs no restart.',
               mbInformation, MB_OK);
      if not FileExists(ExpandConstant('{app}\AMDFamily17.bin')) or
         not FileExists(ExpandConstant('{app}\IntelMSR.bin')) then
        MsgBox('This package carries no PawnIO modules. The service needs ' +
               'AMDFamily17.bin and IntelMSR.bin beside it, the signed builds ' +
               'from https://github.com/namazso/PawnIO.Modules/releases.',
               mbInformation, MB_OK);
    end;
  end;
end;
