; The voicepack's installer: SpokenPlayersVoicepack-Setup.exe. tools\Build-Installer.ps1 compiles it,
; passing Version, Dist (engine\dist), Build (its folder with voices.iss and the engine's certificate)
; and Thumbprint (the certificate's).
;
; The setup itself holds the engine, espeak-ng's data and each voice's small files; the models
; (~70 MB each) are downloaded from Hugging Face (Source, at the commit Revision) for the game chosen
; first (retail, WoW Forever or both: their races), or taken from a voices\ folder beside the setup
; when there is one. Updating is running a newer setup: it downloads only the models that changed,
; and the game chosen last time is chosen again.

#include "common.iss"
#define Clsid "{{055bf9e8-68b3-4cb2-8090-50895f4bcc68}"
#define Tokens "SOFTWARE\Microsoft\Speech\Voices\Tokens"
; The races, each model's files and its voice's registry key, as the build writes them (and Models,
; Components: the lists of them; each game's races, models and download size; Source and Revision).
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
WelcomeLabel2=The players around you, spoken: this installs the race voices for the Spoken Players addon, a voice for every race and body type, as Windows voices that World of Warcraft speaks with.%n%nNext you choose the game you play: only its voices are downloaded, from Hugging Face.
SelectComponentsDesc=Which races should get their voices?
SelectComponentsLabel2=The races of the game you chose are ticked. Untick a race to leave it out, or tick one to add it. Races left out speak with the voices Windows comes with.
FinishedHeadingLabel=Your group has a voice
FinishedLabel=Start World of Warcraft with the Spoken Players addon and type /sp in game.%n%nWoW looks for voices when it starts: restart it if it was running.

[Types]
; The one type: the races are the game's (the game page) or picked one by one.
Name: "custom"; Description: "The races chosen"; Flags: iscustom

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
; The game chosen (retail, forever or all) and whether its races were then picked one by one, chosen
; again when the setup runs again.
Root: HKLM; Subkey: "SOFTWARE\Spoken Players"; ValueType: string; ValueName: "Game"; ValueData: "{code:ChosenGame}"
Root: HKLM; Subkey: "SOFTWARE\Spoken Players"; ValueType: dword; ValueName: "Adjusted"; ValueData: "{code:ChosenAdjusted}"
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
  GamePage: TWizardPage;
  GameButtons: array of TNewRadioButton;  { retail, WoW Forever, both }
  AdjustBox: TNewCheckBox;  { then the races one by one, on the components page }
  SourceLabel: TNewStaticText;  { on the installing page: where the model being installed comes from }
  PreviousGame: String;  { the game chosen last time ('retail', 'forever', 'all'), '' before }
  PreviousAdjusted: Cardinal;  { 1: its races were then picked one by one }

{ The games, in the order of the game page: their key (kept in the registry), title, what they get,
  and their races (components). }
function GameKey(Index: Integer): String;
begin
  case Index of
    0: Result := 'retail';
    1: Result := 'forever';
  else
    Result := 'all';
  end;
end;

function GameTitle(Index: Integer): String;
begin
  case Index of
    0: Result := 'Retail only';
    1: Result := 'WoW Forever only';
  else
    Result := 'Both: retail and WoW Forever';
  end;
end;

function GameText(Index: Integer): String;
begin
  case Index of
    0: Result := 'Every race of retail WoW, and the raid voices (not WoW Forever''s {#RetailLeftOut}): ' +
                 '{#RetailModels} voice models, {#RetailSize} to download.';
    1: Result := 'WoW Forever''s races ({#ForeverRaces}), and the raid voices: ' +
                 '{#ForeverModels} voice models, {#ForeverSize} to download.';
  else
    Result := 'Every voice, for every race of both games: {#AllModels} voice models, {#AllSize} to download.';
  end;
end;

function GameComponents(Index: Integer): String;
begin
  case Index of
    0: Result := '{#RetailComponents}';
    1: Result := '{#ForeverComponents}';
  else
    Result := '{#AllComponents}';
  end;
end;

{ The game chosen on the game page: -1 while none is (no game is chosen for the player). }
function ChosenIndex: Integer;
var
  I: Integer;
begin
  Result := -1;
  for I := 0 to GetArrayLength(GameButtons) - 1 do
    if GameButtons[I].Checked then Result := I;
end;

{ For the registry: the game chosen, or last time's in a silent install, which shows no page. }
function ChosenGame(Param: String): String;
begin
  if ChosenIndex >= 0 then Result := GameKey(ChosenIndex) else Result := PreviousGame;
end;

function ChosenAdjusted(Param: String): String;
begin
  if AdjustBox.Checked then Result := '1' else Result := '0';
end;

procedure GameClicked(Sender: TObject);
begin
  WizardForm.NextButton.Enabled := ChosenIndex >= 0;
end;

procedure OpenSource(Sender: TObject);
var
  Code: Integer;
begin
  ShellExecAsOriginalUser('open', '{#Source}', '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

function AddText(Left, Top: Integer; Text: String): TNewStaticText;
begin
  Result := TNewStaticText.Create(GamePage);
  Result.Parent := GamePage.Surface;
  Result.Left := Left;
  Result.Top := Top;
  Result.Width := GamePage.SurfaceWidth - Left;
  Result.WordWrap := True;
  Result.Caption := Text;
end;

{ The setup's first choice, after the welcome: which game the player plays. Nothing is chosen for
  them (Next waits for a choice), but last time's choice when the setup runs again. The races it
  gets can then be changed one by one, on the components page, else skipped. }
procedure CreateGamePage;
var
  I, Top: Integer;
  Text, Link: TNewStaticText;
begin
  GamePage := CreateCustomPage(wpWelcome, 'Which World of Warcraft do you play?',
    'Choose one: only the voices your game speaks with are downloaded.');
  SetArrayLength(GameButtons, 3);
  Top := 0;
  for I := 0 to 2 do begin
    GameButtons[I] := TNewRadioButton.Create(GamePage);
    GameButtons[I].Parent := GamePage.Surface;
    GameButtons[I].Top := Top;
    GameButtons[I].Width := GamePage.SurfaceWidth;
    GameButtons[I].Height := ScaleY(17);
    GameButtons[I].Font.Style := [fsBold];
    GameButtons[I].Caption := GameTitle(I);
    if GameKey(I) = PreviousGame then begin
      GameButtons[I].Caption := GameTitle(I) + '  (as last time)';
      GameButtons[I].Checked := True;
    end;
    GameButtons[I].OnClick := @GameClicked;
    Text := AddText(ScaleX(18), Top + ScaleY(19), GameText(I));
    Top := Text.Top + Text.Height + ScaleY(10);
  end;
  AdjustBox := TNewCheckBox.Create(GamePage);
  AdjustBox.Parent := GamePage.Surface;
  AdjustBox.Top := Top;
  AdjustBox.Width := GamePage.SurfaceWidth;
  AdjustBox.Height := ScaleY(17);
  AdjustBox.Caption := 'Then let me untick or add races one by one';
  AdjustBox.Checked := PreviousAdjusted = 1;
  Text := AddText(0, AdjustBox.Top + AdjustBox.Height + ScaleY(12), 'The voices are downloaded from Hugging Face:');
  Link := AddText(0, Text.Top + Text.Height + ScaleY(2), Copy('{#Source}', Length('https://') + 1, Length('{#Source}')));
  Link.Font.Style := [fsUnderline];
  Link.Cursor := crHand;
  Link.OnClick := @OpenSource;
end;

{ On the installing page, under the progress bar: where the model being installed comes from. }
procedure CreateSourceLabel;
begin
  SourceLabel := TNewStaticText.Create(WizardForm);
  SourceLabel.Parent := WizardForm.InstallingPage;
  SourceLabel.Left := WizardForm.ProgressGauge.Left;
  SourceLabel.Top := WizardForm.ProgressGauge.Top + WizardForm.ProgressGauge.Height + ScaleY(16);
  SourceLabel.Width := WizardForm.ProgressGauge.Width;
  SourceLabel.AutoSize := False;
  SourceLabel.Height := ScaleY(80);
  SourceLabel.WordWrap := True;
  SourceLabel.Caption := '';
end;

{ Where a model is downloaded from: its file on Hugging Face, at the commit the setup was built with. }
function ModelUrl(Model: String): String;
begin
  Result := '{#Source}/resolve/{#Revision}/' + Model + '/model.onnx';
end;

{ The value of Key in a list of 'key=value,...' ('' when it has none). }
function ListValue(List, Key: String): String;
var
  At: Integer;
begin
  Result := '';
  List := ',' + List + ',';
  At := Pos(',' + Key + '=', List);
  if At = 0 then Exit;
  Delete(List, 1, At + Length(Key) + 1);
  Result := Copy(List, 1, Pos(',', List) - 1);
end;

{ Before a model is installed: its voice ("Orc Male") and where it comes from, the whole address of
  its download (Model: its folder on Hugging Face), or the voices folder beside the setup (Model ''). }
procedure ModelFetching(Voice, Model: String);
begin
  if Model = '' then
    SourceLabel.Caption := 'Installing the ' + Voice + ' voice from the voices folder beside this setup.'
  else begin
    { The address on three lines, broken after a slash: one long line would be cut off. }
    SourceLabel.Caption := 'Downloading the ' + Voice + ' voice from' + #13#10 +
      '{#Source}/resolve/' + #13#10 + '{#Revision}/' + Model + '/model.onnx';
    Log('Downloading ' + ModelUrl(Model));
  end;
end;

procedure InitializeWizard;
begin
  RegQueryStringValue(HKLM64, 'SOFTWARE\Spoken Players', 'Game', PreviousGame);
  RegQueryDWordValue(HKLM64, 'SOFTWARE\Spoken Players', 'Adjusted', PreviousAdjusted);
  CreateGamePage;
  CreateSourceLabel;
end;

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

{ The races of the game chosen that the setup before did not offer (added since), ticked: run again
  with the races picked one by one, those of last time stay as Setup remembers them. }
procedure SelectRacesAddedSince(Races: String);
var
  Known, Race: String;
  Comma: Integer;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\Spoken Players', 'Races', Known) then Known := '';
  Races := Races + ',';
  while Races <> '' do begin
    Comma := Pos(',', Races);
    Race := Copy(Races, 1, Comma - 1);
    Delete(Races, 1, Comma);
    if Pos(',' + Race + ',', ',' + Known + ',') = 0 then WizardSelectComponents(Race);
  end;
end;

{ The game chosen: its races ticked (all others not), unless they were picked one by one last time for
  the same game and are again. Install clicked: WoW closed, then the voices already there checked,
  before anything is installed. }
function NextButtonClick(CurPageID: Integer): Boolean;
var
  Game: Integer;
begin
  Result := True;
  if CurPageID = GamePage.ID then begin
    Game := ChosenIndex;
    if Game < 0 then begin
      { A silent install shows no page: the races of last time, as Setup remembers them (or those
        it was given), else every one. }
      Result := WizardSilent;
      if Result and (WizardSelectedComponents(False) = '') then WizardSelectComponents('{#AllComponents}');
      Exit;
    end;
    if AdjustBox.Checked and (PreviousAdjusted = 1) and (GameKey(Game) = PreviousGame) then
      SelectRacesAddedSince(GameComponents(Game))
    else begin
      WizardSelectComponents('{#NoComponents}');
      WizardSelectComponents(GameComponents(Game));
    end;
  end else if CurPageID = wpReady then begin
    Result := EngineFree;
    if Result then CheckInstalledModels;
  end;
end;

{ The races one by one only when asked for on the game page. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpSelectComponents) and not AdjustBox.Checked;
end;

{ The Ready page: the game, then every file the races chosen download, by its whole address (those
  installed already and unchanged are kept, not downloaded), then the races. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
                         MemoGroupInfo, MemoTasksInfo: String): String;
var
  Pairs, Pair, Model, Downloads, Kept, Beside: String;
  Comma, Equals: Integer;
begin
  Result := '';
  if MemoDirInfo <> '' then Result := MemoDirInfo + NewLine + NewLine;
  if ChosenIndex >= 0 then
    Result := Result + 'Game:' + NewLine + Space + GameTitle(ChosenIndex) + NewLine + NewLine;
  Downloads := '';
  Kept := '';
  Beside := '';
  Pairs := '{#ModelComponents},';
  while Pairs <> '' do begin
    Comma := Pos(',', Pairs);
    Pair := Copy(Pairs, 1, Comma - 1);
    Delete(Pairs, 1, Comma);
    Equals := Pos('=', Pair);
    Model := Copy(Pair, 1, Equals - 1);
    if WizardIsComponentSelected(Copy(Pair, Equals + 1, Length(Pair))) then begin
      if CompareText(SavedHash(Model), ListValue('{#ModelHashes}', Model)) = 0 then
        Kept := Kept + Space + Model + NewLine
      else if Pos(',' + Model + ',', ',{#LocalModels},') > 0 then
        Beside := Beside + Space + Model + NewLine
      else
        { The model first: the addresses differ only at their end, past the page's edge. }
        Downloads := Downloads + Space + Model + ':  ' + ModelUrl(Model) + NewLine;
    end;
  end;
  if Downloads <> '' then
    Result := Result + 'Downloads, from Hugging Face:' + NewLine + Downloads + NewLine;
  if Beside <> '' then
    Result := Result + 'From the voices folder beside this setup:' + NewLine + Beside + NewLine;
  if Kept <> '' then
    Result := Result + 'Installed already and unchanged, kept:' + NewLine + Kept + NewLine;
  Result := Result + MemoComponentsInfo;
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
  end else if CurStep = ssPostInstall then
    SourceLabel.Caption := '';
end;

{ Next waits on the game page until a game is chosen. }
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = GamePage.ID then WizardForm.NextButton.Enabled := ChosenIndex >= 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then RemoveSpokenVoices;
end;
