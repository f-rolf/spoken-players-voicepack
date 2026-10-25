; The voicepack's installer: SpokenPlayersVoicepack-Setup.exe. tools\Build-Installer.ps1 compiles it,
; passing Version, Url, Dist (engine\dist), Build (its folder with voices.iss and the engine's
; certificate) and Thumbprint (the certificate's).
;
; The setup itself holds the engine, espeak-ng's data and each voice's small files; the models
; (~70 MB each) are downloaded for the races chosen, from Url's voices\, or taken from a voices\ folder
; beside the setup when there is one. Updating is running a newer setup: it downloads only the models
; that changed, and offers the races added since already chosen.

#include "common.iss"
#define Clsid "{{055bf9e8-68b3-4cb2-8090-50895f4bcc68}"
#define Tokens "SOFTWARE\Microsoft\Speech\Voices\Tokens"
; The races, each model's files and its voice's registry key, as the build writes them (and Models,
; Components: the lists of them).
#include Build + "\voices.iss"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#Version}
AppPublisher={#Publisher}
VersionInfoVersion={#Version}
UninstallDisplayName={#AppName} (WoW voices)
UninstallDisplayIcon={uninstallexe}
DefaultDirName={autopf}\Spoken Players
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; WoW keeps the engine loaded: PrepareToInstall asks to close it, rather than Windows' Restart Manager.
CloseApplications=no
OutputBaseFilename={#SetupName}
SetupIconFile=art\icon.ico
WizardStyle=modern dark includetitlebar
WizardImageFile=art\wizard.png
WizardSmallImageFile=art\wizard-small.png
Compression=lzma2/ultra64
SolidCompression=yes

[Messages]
SetupAppTitle={#AppName}
SetupWindowTitle={#AppName}
WelcomeLabel1=Spoken Players Voicepack
WelcomeLabel2=The players around you, spoken: this installs the race voices for the Spoken Players addon, a voice for every race and body type, as Windows voices that World of Warcraft speaks with.%n%nOnly the races you choose are downloaded.
SelectComponentsDesc=Which races should get their voices?
SelectComponentsLabel2=Choose the races to download. Races you leave out speak with the voices Windows comes with; run this setup again to add them later.
FinishedHeadingLabel=Your group has a voice
FinishedLabel=Start World of Warcraft with the Spoken Players addon and type /sp in game.%n%nWoW looks for voices when it starts: restart it if it was running.

[Types]
Name: "full"; Description: "Every race"
Name: "custom"; Description: "Only the races I choose"; Flags: iscustom

[Files]
Source: "{#Dist}\spoken_voice.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\espeak-ng-data\*"; DestDir: "{app}\espeak-ng-data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Build}\spoken_voice_engine.cer"; DestDir: "{app}"; Flags: ignoreversion
; The engine's license (GPL-3.0, as espeak-ng's, which is built into it) and those of the rest of
; what is built into it, beside it.
Source: "..\LICENSE"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion

[Registry]
; The engine (a COM class), for the voices' tokens to point at; the tokens are in voices.iss.
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#Clsid}"; ValueType: string; ValueName: ""; ValueData: "Spoken Voice Engine"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#Clsid}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\spoken_voice.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#Clsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Both"
; The races this setup offered, so that a newer one knows which of its races were added since.
Root: HKLM; Subkey: "SOFTWARE\Spoken Players"; ValueType: string; ValueName: "Races"; ValueData: "{#Components}"; Flags: uninsdeletekey
; The PowerShell setup's entry in the installed apps, before this installer replaced it.
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SpokenGroup"; ValueType: none; Flags: deletekey

[InstallDelete]
; What the PowerShell setup left, were it installed in the same folder (else RemoveOldSetup takes its
; folder, Spoken Group).
Type: files; Name: "{app}\setup.ps1"
Type: files; Name: "{app}\Install-SpokenVoices.ps1"
Type: files; Name: "{app}\Uninstall Spoken Group.cmd"
Type: files; Name: "{app}\manifest.json"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\voices"

[Run]
; WoW only loads a signed engine. Its certificate is self-made (its private key was deleted when it
; signed the engine, so it vouches for this one file), hence trusted here.
Filename: "{sys}\certutil.exe"; Parameters: "-f -addstore Root ""{app}\spoken_voice_engine.cer"""; StatusMsg: "Trusting the voice engine..."; Flags: runhidden
Filename: "{sys}\certutil.exe"; Parameters: "-f -addstore TrustedPublisher ""{app}\spoken_voice_engine.cer"""; StatusMsg: "Trusting the voice engine..."; Flags: runhidden

[UninstallRun]
Filename: "{sys}\certutil.exe"; Parameters: "-delstore Root {#Thumbprint}"; RunOnceId: "UntrustRoot"; Flags: runhidden
Filename: "{sys}\certutil.exe"; Parameters: "-delstore TrustedPublisher {#Thumbprint}"; RunOnceId: "UntrustPublisher"; Flags: runhidden


[Code]
var
  Installed: TStringList;  { 'model=SHA-256 of its installed model.onnx' ('-': none), worked out once }

function CreateFileW(Name: String; Access, Share, Security, Disposition, Flags, Template: Cardinal): Integer;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(Handle: Integer): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function ModelPath(Model: String): String;
begin
  Result := ExpandConstant('{app}\voices\' + Model + '\model.onnx');
end;

{ The hash an installed model was saved with (model.onnx.sha256 beside it, '<sha256> <size>', written
  when it was installed or last checked), if its size still matches: '' when there is none. Reading it
  spares hashing 3.5 GB of models each time the setup runs. }
function SavedHash(Model: String): String;
var
  Text: AnsiString;
  Space, Size: Integer;
begin
  Result := '';
  if not LoadStringFromFile(ModelPath(Model) + '.sha256', Text) then Exit;
  Text := Trim(Text);
  Space := Pos(' ', Text);
  if (Space = 0) or not FileSize(ModelPath(Model), Size) then Exit;
  if Copy(Text, Space + 1, Length(Text)) = IntToStr(Size) then Result := Copy(Text, 1, Space - 1);
end;

procedure SaveHash(Model, Hash: String);
var
  Size: Integer;
begin
  if FileSize(ModelPath(Model), Size) then
    SaveStringToFile(ModelPath(Model) + '.sha256', Hash + ' ' + IntToStr(Size), False);
end;

procedure Remember(Model, Hash: String);
var
  I: Integer;
begin
  if Installed = nil then Installed := TStringList.Create;
  for I := Installed.Count - 1 downto 0 do
    if Copy(Installed[I], 1, Length(Model) + 1) = Model + '=' then Installed.Delete(I);
  Installed.Add(Model + '=' + Hash);
end;

function InstalledHash(Model: String): String;
var
  I: Integer;
begin
  if Installed = nil then Installed := TStringList.Create;
  for I := 0 to Installed.Count - 1 do
    if Copy(Installed[I], 1, Length(Model) + 1) = Model + '=' then begin
      Result := Copy(Installed[I], Length(Model) + 2, Length(Installed[I]));
      Exit;
    end;
  Result := '-';
  if FileExists(ModelPath(Model)) then begin
    Result := SavedHash(Model);
    if Result = '' then
      try
        Result := GetSHA256OfFile(ModelPath(Model));
        SaveHash(Model, Result);
      except
        Log(GetExceptionMessage);
        Result := '-';
      end;
  end;
  Remember(Model, Result);
end;

{ The installed models of the races chosen that have no saved hash yet (an install by an older setup),
  hashed before installing, a model at a time on a progress page, rather than all at once in the
  middle of installing (half a minute and more without a word). }
procedure CheckInstalledModels;
var
  Pairs, Pair, Model, Component: String;
  Unknown: TStringList;
  Comma, Equals, I: Integer;
  Page: TOutputProgressWizardPage;
begin
  Unknown := TStringList.Create;
  try
    Pairs := '{#ModelComponents},';
    while Pairs <> '' do begin
      Comma := Pos(',', Pairs);
      Pair := Copy(Pairs, 1, Comma - 1);
      Delete(Pairs, 1, Comma);
      Equals := Pos('=', Pair);
      Model := Copy(Pair, 1, Equals - 1);
      Component := Copy(Pair, Equals + 1, Length(Pair));
      if WizardIsComponentSelected(Component) and FileExists(ModelPath(Model)) and (SavedHash(Model) = '') then
        Unknown.Add(Model);
    end;
    if Unknown.Count = 0 then Exit;
    Page := CreateOutputProgressPage('Checking the installed voices',
      'Only the voices that changed since are downloaded again.');
    Page.Show;
    try
      for I := 0 to Unknown.Count - 1 do begin
        Page.SetText('Checking ' + Unknown[I] + '...', IntToStr(I + 1) + ' of ' + IntToStr(Unknown.Count));
        Page.SetProgress(I, Unknown.Count);
        InstalledHash(Unknown[I]);
      end;
      Page.SetProgress(Unknown.Count, Unknown.Count);
    finally
      Page.Hide;
    end;
  finally
    Unknown.Free;
  end;
end;

{ After a model is installed: its hash saved beside it, for the next setup. }
procedure ModelInstalled(Model, Hash: String);
begin
  SaveHash(Model, Hash);
  Remember(Model, Hash);
end;

{ A model is fetched only when the installed one is another (or none): from voices\ beside the setup
  if it is there, else downloaded. }
function ModelFromDisk(Model, Hash, FileName: String): Boolean;
begin
  Result := (CompareText(InstalledHash(Model), Hash) <> 0) and FileExists(ExpandConstant('{src}\voices\') + FileName);
end;

function ModelFromWeb(Model, Hash, FileName: String): Boolean;
begin
  Result := (CompareText(InstalledHash(Model), Hash) <> 0) and not FileExists(ExpandConstant('{src}\voices\') + FileName);
end;

{ Whether a program has the engine loaded (WoW, while it runs): Windows lets no one open a DLL for
  writing while it is loaded somewhere. Asked at once, of the file itself. }
function EngineInUse: Boolean;
var
  Handle: Integer;
begin
  Result := False;
  if not FileExists(ExpandConstant('{app}\spoken_voice.dll')) then Exit;
  { GENERIC_WRITE, any sharing, OPEN_EXISTING }
  Handle := CreateFileW(ExpandConstant('{app}\spoken_voice.dll'), $40000000, 7, 0, 3, 0, 0);
  if Handle = -1 then
    Result := DLLGetLastError <> 2  { not ERROR_FILE_NOT_FOUND: in use (or locked) }
  else
    CloseHandle(Handle);
end;

function EngineFree: Boolean;
begin
  Result := True;
  while EngineInUse do
    if SuppressibleMsgBox('Please close World of Warcraft first: it has the voices loaded (or another program ' +
                          'speaking with them does).', mbInformation, MB_OKCANCEL, IDCANCEL) = IDCANCEL then begin
      Result := False;
      Exit;
    end;
end;

{ Install clicked: WoW closed, then the voices already there checked, before anything is installed. }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then begin
    Result := EngineFree;
    if Result then CheckInstalledModels;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if EngineFree then Result := ''
  else Result := 'World of Warcraft still has the voices loaded. Close it, then run this setup again.';
end;

function InitializeUninstall: Boolean;
begin
  Result := EngineFree;
end;

{ Every Spoken voice Windows has, gone: each install writes those of the chosen races anew, so none
  linger for a race left out or a model that is no more (and none of an older, PowerShell install). }
procedure RemoveSpokenVoices;
var
  Names: TArrayOfString;
  I: Integer;
  Marked: Cardinal;
begin
  if not RegGetSubkeyNames(HKLM64, '{#Tokens}', Names) then Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
    if RegQueryDWordValue(HKLM64, '{#Tokens}\' + Names[I], 'SpokenGroupVoice', Marked) and (Marked = 1) then
      RegDeleteKeyIncludingSubkeys(HKLM64, '{#Tokens}\' + Names[I]);
end;

{ Model folders this setup does not know (an older pack's), gone; voices.iss's [InstallDelete] takes
  the races left out. }
procedure RemoveUnknownModels;
var
  Found: TFindRec;
  Voices: String;
begin
  Voices := ExpandConstant('{app}\voices\');
  if not FindFirst(Voices + '*', Found) then Exit;
  try
    repeat
      if (Found.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0) and (Found.Name <> '.') and (Found.Name <> '..')
         and (Pos(',' + Found.Name + ',', ',{#Models},') = 0) then
        DelTree(Voices + Found.Name, True, True, True);
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
end;

{ The PowerShell setup's install, from before the rename (Spoken: Group), in a folder of its own: gone,
  voices and all. Known by the setup.ps1 it kept there. }
procedure RemoveOldSetup;
var
  Old: String;
begin
  Old := ExpandConstant('{commonpf64}\Spoken Group');
  if (CompareText(Old, ExpandConstant('{app}')) <> 0) and FileExists(Old + '\setup.ps1') then
    DelTree(Old, True, True, True);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    RemoveSpokenVoices;
    RemoveUnknownModels;
    RemoveOldSetup;
  end;
end;

{ Run again (an update), the races chosen last time stay chosen and the rest not, as Setup remembers;
  a race this setup offers that the one before did not (added since) starts chosen, as for a first
  install. }
procedure CurPageChanged(CurPageID: Integer);
var
  Offered, Known, Race: String;
  Comma: Integer;
begin
  if (CurPageID <> wpSelectComponents) or not RegQueryStringValue(HKLM64, 'SOFTWARE\Spoken Players', 'Races', Known) then
    Exit;
  Offered := '{#Components},';
  while Offered <> '' do begin
    Comma := Pos(',', Offered);
    Race := Copy(Offered, 1, Comma - 1);
    Delete(Offered, 1, Comma);
    if Pos(',' + Race + ',', ',' + Known + ',') = 0 then WizardSelectComponents(Race);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then RemoveSpokenVoices;
end;
