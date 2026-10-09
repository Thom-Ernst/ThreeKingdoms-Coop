#ifndef Version
  #error Build through Build-Installer.ps1
#endif
#if Variant == "Debug"
  #define ProductName "Three Kingdoms Coop Developer Build (Debug)"
  #define FileSuffix "-debug"
#else
  #define ProductName "Three Kingdoms Coop"
  #define FileSuffix ""
#endif
[Setup]
AppId={code:InstallId}
AppName={#ProductName}
AppVersion={#Version}
AppVerName={#ProductName} {#Version}
AppPublisher=Three Kingdoms Coop contributors
VersionInfoVersion={#NumericVersion}
VersionInfoDescription={#ProductName} {#Version} - DLL {#DllStamp}
AppComments=DLL source: {#DllStamp}
DefaultDirName={code:DefaultGameDir}
AppendDefaultDirName=no
UsePreviousAppDir=no
UsePreviousLanguage=no
DisableDirPage=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
DirExistsWarning=no
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#Output}
OutputBaseFilename=TW3K-Coop-Setup-{#Version}{#FileSuffix}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallFilesDir={app}\.tw3k-coop-uninstall
; An older Setup may have logged a local data pack. Appending that log would
; delete a later Workshop/manual copy even though today's helper owns no packs.
; The helper handles root ownership across both variants; use only today's log.
UninstallLogMode=overwrite
UninstallDisplayName={#ProductName} {#Version}
CloseApplications=no
RestartApplications=no
SetupLogging=yes

[Files]
Source: "{#Stage}\InstallerHelper.exe"; DestDir: "{app}\.tw3k-coop-uninstall"; Flags: ignoreversion
Source: "{#Stage}\InstallerHelper.exe"; Flags: dontcopy
Source: "{#Stage}\tw3k_coop.dll"; Flags: dontcopy
Source: "{#Stage}\amd_ags_x64_proxy.dll"; Flags: dontcopy
#if Variant == "Debug"
Source: "{#Stage}\TW3K-Coop-Control.ps1"; Flags: dontcopy
Source: "{#Stage}\TW3K-Coop-Recovery.cmd"; Flags: dontcopy
#endif

[Run]
Filename: "steam://run/779340"; Description: "Start the launcher (tick tw3k_coop under Mods)"; Flags: shellexec postinstall skipifsilent unchecked runasoriginaluser
Filename: "steam://url/CommunityFilePage/{#WorkshopItemId}"; Description: "Open the Workshop page to subscribe to tw3k_coop"; Flags: shellexec postinstall skipifsilent unchecked runasoriginaluser; Check: HasWorkshopItem

[Code]
var
  Helper, ResultPath, FinishMessage: String;
  Candidates: TArrayOfString;
  PickPage: TInputOptionWizardPage;
  FinishMemo: TNewMemo;
  DeleteLogs: Boolean;

function Quote(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function InstallId(Param: String): String;
begin
  Result := 'TW3K-Coop-65FBEE59-07CD-4DA0-BEF0-6E47FD5C94A8';
  if ExpandConstant('{param:FIXTUREROOT|}') <> '' then Result := Result + '-' + ExpandConstant('{param:FIXTUREID|offline}');
end;

function HasWorkshopItem: Boolean;
begin
  Result := '{#WorkshopItemId}' <> '';
end;

function FixtureOptions: String;
begin
  Result := ' ' + Quote('fixture=' + ExpandConstant('{param:FIXTUREROOT|}'));
end;

function RunHelper(Operation, Options: String): Boolean;
var ExitCode: Integer;
begin
  DeleteFile(ResultPath);
  Result := Exec(Helper, Operation + ' ' + Quote('result=' + ResultPath) + Options,
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  Result := Result and (ExitCode = 0);
  FinishMessage := GetIniString('result', 'message', 'Installer helper could not run. Check the setup log.', ResultPath);
  Log(FinishMessage);
end;

function DefaultGameDir(Param: String): String;
begin
  if GetArrayLength(Candidates) = 1 then Result := Candidates[0]
  else Result := ExpandConstant('{sd}\SteamLibrary\steamapps\common\Total War THREE KINGDOMS');
end;

procedure InitializeWizard;
var I, Count: Integer; HintLabel: TNewStaticText;
begin
  ExtractTemporaryFile('InstallerHelper.exe');
  Helper := ExpandConstant('{tmp}\InstallerHelper.exe');
  ResultPath := ExpandConstant('{tmp}\tw3k-result.ini');
  if not RunHelper('detect', ' ' + Quote('steam=' + ExpandConstant('{param:STEAMROOT|}')) +
    ' ' + Quote('uninstalllocation=' + ExpandConstant('{param:UNINSTALLLOCATION|}')) + FixtureOptions) then
    Log('Detection failed; the player must choose the game folder.');
  Count := GetIniInt('result', 'count', 0, 0, 1024, ResultPath);
  SetArrayLength(Candidates, Count);
  for I := 0 to Count-1 do Candidates[I] := GetIniString('result', 'path' + IntToStr(I), '', ResultPath);
  if (Count = 1) and (ExpandConstant('{param:DIR|}') = '') then WizardForm.DirEdit.Text := Candidates[0];
  PickPage := CreateInputOptionPage(wpWelcome, 'Choose your Steam installation',
    'Several game installations were found.', 'Choose the installation you play. You can change the folder on the next page.', True, False);
  for I := 0 to Count-1 do PickPage.Add(Candidates[I]);
  if Count > 0 then PickPage.SelectedValueIndex := 0;
  WizardForm.WelcomeLabel2.Caption := 'Install {#ProductName} {#Version}.' + #13#10 +
    'DLL source: {#DllStamp}' + #13#10#13#10 +
    'Close the game and the Creative Assembly launcher before continuing. Setup installs the mod and keeps your original game DLL for uninstall. Your saves are untouched.';
  WizardForm.SelectDirLabel.Caption := 'Choose the folder containing Three_Kingdoms.exe.';
  HintLabel := TNewStaticText.Create(WizardForm);
  HintLabel.Parent := WizardForm.SelectDirPage;
  HintLabel.Left := WizardForm.DirEdit.Left;
  HintLabel.Top := WizardForm.DirEdit.Top + WizardForm.DirEdit.Height + ScaleY(16);
  HintLabel.Width := WizardForm.SelectDirBrowseLabel.Width;
  HintLabel.Height := ScaleY(42); HintLabel.AutoSize := False; HintLabel.WordWrap := True;
  HintLabel.Caption := 'If it was not found automatically: Steam > right-click the game > Manage > Browse local files.';
  // The retirement list can be long; a fixed Inno FinishedLabel silently clips repair advice.
  FinishMemo := TNewMemo.Create(WizardForm);
  FinishMemo.Parent := WizardForm.FinishedPage;
  FinishMemo.Left := WizardForm.FinishedLabel.Left;
  FinishMemo.Top := WizardForm.FinishedLabel.Top;
  FinishMemo.Width := WizardForm.FinishedLabel.Width;
  FinishMemo.Height := ScaleY(170);
  FinishMemo.ReadOnly := True; FinishMemo.WordWrap := True; FinishMemo.ScrollBars := ssVertical;
  WizardForm.FinishedLabel.Visible := False;
  WizardForm.RunList.Top := FinishMemo.Top + FinishMemo.Height + ScaleY(10);
  WizardForm.RunList.Height := ScaleY(60);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = PickPage.ID) and (GetArrayLength(Candidates) < 2);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = PickPage.ID then WizardForm.DirEdit.Text := Candidates[PickPage.SelectedValueIndex];
  if CurPageID = wpSelectDir then begin
    // Silent mode must reach PrepareToInstall's nonzero refusal, never a hidden modal box.
    if WizardSilent then exit;
    Result := FileExists(ExpandConstant('{app}\Three_Kingdoms.exe'));
    if not Result then SuppressibleMsgBox('Choose the folder containing Three_Kingdoms.exe. In Steam: Manage > Browse local files.', mbError, MB_OK, IDOK);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if WizardSilent and (GetArrayLength(Candidates) > 1) and (ExpandConstant('{param:DIR|}') = '') then begin
    Result := 'Several installations were found. Specify /DIR or run the interactive wizard to choose one.'; exit;
  end;
  // Inno displays Retry on a PrepareToInstall error; silently it returns a nonzero exit.
  if not RunHelper('check', ' ' + Quote('game=' + ExpandConstant('{app}')) + FixtureOptions) then begin
    Result := FinishMessage; exit;
  end;
  ExtractTemporaryFile('tw3k_coop.dll');
  ExtractTemporaryFile('amd_ags_x64_proxy.dll');
#if Variant == "Debug"
  ExtractTemporaryFile('TW3K-Coop-Control.ps1');
  ExtractTemporaryFile('TW3K-Coop-Recovery.cmd');
#endif
  if not RunHelper('install', ' ' + Quote('game=' + ExpandConstant('{app}')) +
    ' ' + Quote('source=' + ExpandConstant('{tmp}')) + ' ' + Quote('workshop={#WorkshopItemId}') + FixtureOptions +
    ' ' + Quote('fail=' + ExpandConstant('{param:TESTFAIL|}'))) then Result := FinishMessage;
end;

procedure CurPageChanged(CurPageID: Integer);
var VisibleText: String;
begin
  if CurPageID = wpFinished then begin
    // Inno sizes its run list when entering this page; apply our bounds afterwards.
    WizardForm.RunList.Top := FinishMemo.Top + FinishMemo.Height + ScaleY(10);
    WizardForm.RunList.Height := ScaleY(60);
    VisibleText := FinishMessage;
    StringChangeEx(VisibleText, ' | ', #13#10#13#10, True);
    FinishMemo.Text := VisibleText;
  end;
end;

function InitializeUninstall: Boolean;
var Form: TSetupForm; Check: TNewCheckBox; OkButton, CancelButton: TNewButton;
begin
  Result := True;
  DeleteLogs := ExpandConstant('{param:REMOVELOGS|0}') = '1';
  if UninstallSilent then exit;
  Form := CreateCustomForm(ScaleX(480), ScaleY(130), False, True);
  try
    Form.Caption := 'Remove {#ProductName}';
    Form.ClientWidth := ScaleX(480); Form.ClientHeight := ScaleY(130);
    Check := TNewCheckBox.Create(Form); Check.Parent := Form;
    Check.Left := ScaleX(16); Check.Top := ScaleY(24); Check.Width := ScaleX(448);
    Check.Caption := 'Also remove tw3k_coop_*.log files (saves are always kept)';
    Check.Checked := DeleteLogs;
    OkButton := TNewButton.Create(Form); OkButton.Parent := Form;
    OkButton.Left := ScaleX(270); OkButton.Top := ScaleY(80); OkButton.Caption := 'Continue'; OkButton.ModalResult := mrOk;
    CancelButton := TNewButton.Create(Form); CancelButton.Parent := Form;
    CancelButton.Left := ScaleX(370); CancelButton.Top := ScaleY(80); CancelButton.Caption := 'Cancel'; CancelButton.ModalResult := mrCancel;
    Result := Form.ShowModal = mrOk; DeleteLogs := Check.Checked;
  finally Form.Free; end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var LogOption: String;
begin
  if CurUninstallStep = usUninstall then begin
    Helper := ExpandConstant('{app}\.tw3k-coop-uninstall\InstallerHelper.exe');
    ResultPath := ExpandConstant('{tmp}\tw3k-uninstall-result.ini');
    LogOption := 'no';
    if DeleteLogs then LogOption := 'yes';
    while not RunHelper('uninstall', ' ' + Quote('game=' + ExpandConstant('{app}')) +
      ' ' + Quote('logs=' + LogOption) + FixtureOptions) do begin
      if UninstallSilent then RaiseException(FinishMessage);
      if MsgBox(FinishMessage, mbError, MB_RETRYCANCEL) <> IDRETRY then RaiseException(FinishMessage);
    end;
  end;
end;
