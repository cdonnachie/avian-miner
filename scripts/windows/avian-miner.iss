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

{ Reads one `set "NAME=value"` back out of an existing settings.cmd, which
  is where the last install left the address and this one is about to write
  over it. The unquoted form is accepted too, since the file says it can be
  edited by hand. '' if the file, or the line, is not there. }
function SettingFromFile(const Name: String): String;
var
  Lines: TArrayOfString;
  I, P: Integer;
  Line, Rest, Key: String;
begin
  Result := '';
  if not LoadStringsFromFile(
       ExpandConstant('{commonappdata}\avian-miner\settings.cmd'), Lines) then
    Exit;
  Key := Uppercase(Name) + '=';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    Rest := '';
    if Uppercase(Copy(Line, 1, 5)) = 'SET "' then
      Rest := Trim(Copy(Line, 6, Length(Line)))
    else if Uppercase(Copy(Line, 1, 4)) = 'SET ' then
      Rest := Trim(Copy(Line, 5, Length(Line)));
    if (Rest <> '') and (Uppercase(Copy(Rest, 1, Length(Key))) = Key) then
    begin
      Rest := Copy(Rest, Length(Key) + 1, Length(Rest));
      P := Length(Rest);
      if (P > 0) and (Rest[P] = '"') then
        Rest := Copy(Rest, 1, P - 1);
      { the last line wins, which is what cmd would do with the file }
      Result := Trim(Rest);
    end;
  end;
end;

procedure InitializeWizard;
var
  Wallet, Worker, Note: String;
begin
  { /WALLET=... and /WORKER=... on the command line fill the page in, which
    is what a silent install (/VERYSILENT) has instead of a wizard. Failing
    that, an upgrade reads back what the last install wrote, so an address
    is typed once per machine rather than once per release. That also stops
    a silent upgrade with no parameters from writing an empty WALLET over a
    good one, which is what it did before. }
  Wallet := ExpandConstant('{param:WALLET|}');
  if Wallet = '' then
    Wallet := SettingFromFile('WALLET');
  Worker := ExpandConstant('{param:WORKER|}');
  if Worker = '' then
    Worker := SettingFromFile('WORKER');
  if Worker = '' then
    Worker := GetComputerNameString;

  if Wallet <> '' then
    Note := 'Filled in from the settings.cmd already on this machine. '
  else
    Note := '';
  WalletPage := CreateInputQueryPage(wpSelectDir,
    'Your Avian address', 'Where the rewards go',
    'The address mined coins are paid to, and a name for this machine on the ' +
    'pool. ' + Note + 'Both are kept in ' + ExpandConstant('{commonappdata}') +
    '\avian-miner\settings.cmd and can be changed there later.');
  WalletPage.Add('Avian address (avn1...):', False);
  WalletPage.Add('Worker name:', False);
  WalletPage.Values[0] := Wallet;
  WalletPage.Values[1] := Worker;
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
        MsgBox('Installed, but this package came without the two PawnIO ' +
               'modules the sensors service loads, so it has nothing to read ' +
               'yet. Copy AMDFamily17.bin and IntelMSR.bin, from the zip at ' +
               'https://github.com/namazso/PawnIO.Modules/releases, into ' +
               ExpandConstant('{app}') + '. The service looks for them every ' +
               'thirty seconds and needs no restart; everything else works now.',
               mbInformation, MB_OK);
    end;
  end;
end;
