<#
.SYNOPSIS
    Exercises the compiled installer and uninstaller only against disposable offline fixtures.
.DESCRIPTION
    Fake Steam libraries, fake originals and a sleeping fixture EXE cover detection and the proxy
    state machine. Never discovers or writes a real game folder: every Setup/helper invocation
    carries /FIXTUREROOT, and /STEAMROOT suppresses registry and drive discovery. Each fixture has
    a unique Inno AppId so a public/rig installation's Apps & features entry cannot be overwritten.
    The fake running-game test is a tiny sleeping managed process, not the game. Logs are retained
    Variant-specific DLL/client hashes, finish advice, both switch directions and failed
    switches are checked with -OtherInstaller supplying the opposite variant. Logs are retained
    under ResultDir. Windows PowerShell 5.1 and pwsh 7 run the same compiled EXE tests.
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Test-Installer.ps1 -Installer out/installer/TW3K-Coop-Setup-0.1.0.exe -OtherInstaller out/installer/TW3K-Coop-Setup-0.1.0-debug.exe
#>
#Requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Installer, [Parameter(Mandatory)][string]$OtherInstaller, [string]$ResultDir)
$ErrorActionPreference = 'Stop'
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$manifest = Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($Installer, '.hashes.json')) | ConvertFrom-Json
$OtherInstaller = (Resolve-Path -LiteralPath $OtherInstaller).Path
$otherManifest = Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($OtherInstaller, '.hashes.json')) | ConvertFrom-Json
if ($manifest.variant -notin @('Release', 'Debug') -or $otherManifest.variant -notin @('Release', 'Debug') -or $manifest.variant -eq $otherManifest.variant) { throw 'Tests require one Release and one Debug installer.' }
$helper = Join-Path (Split-Path $Installer) "stage/$($manifest.variant)/InstallerHelper.exe"
if (-not (Test-Path -LiteralPath $helper)) { throw 'Build stage helper missing; run Build-Installer.ps1 first.' }
if (-not $ResultDir) { $ResultDir = Join-Path (Split-Path $Installer) ('tests-PS' + $PSVersionTable.PSVersion.Major) }
New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null
$ResultDir = (Resolve-Path -LiteralPath $ResultDir).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('TW3K-Coop-Installer-Test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$script:checks = 0
$script:sequence = 0
$script:installed = New-Object System.Collections.ArrayList
function Assert([bool]$Condition, [string]$Name) {
    if (-not $Condition) { Write-Host "FAIL $Name"; throw $Name }
    $script:checks++; Write-Host "ok $Name"
}
function Write-Fixture([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
function Game([string]$Library, [string]$Name) {
    $game = Join-Path $Library "steamapps/common/$Name"
    Write-Fixture (Join-Path $game 'Three_Kingdoms.exe') 'offline fixture, not the game'
    Write-Fixture (Join-Path $game 'amd_ags_x64.dll') "original AMD fixture $Name"
    Write-Fixture (Join-Path $Library 'steamapps/appmanifest_779340.acf') ('"AppState" { "appid" "779340" "installdir" "' + $Name + '" }')
    return $game
}
function Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Run-FixtureExe([string]$File, [string]$Arguments) {
    # ★ -Wait tracks the Inno bootstrapper's process TREE. WaitForExit on the parent alone
    # returns while the temporary uninstaller still holds its log and is deleting files.
    return (Start-Process -FilePath $File -ArgumentList $Arguments -PassThru -Wait -WindowStyle Hidden)
}
function Detect([string]$Steam, [string]$Stale='') {
    $script:sequence++
    $ini=Join-Path $ResultDir "detect-$script:sequence.ini"
    & $helper detect "steam=$Steam" "fixture=$root" "uninstalllocation=$Stale" "result=$ini" | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'detection helper completed'
    return [IO.File]::ReadAllText($ini)
}
function Setup([string]$Steam, [string]$Target, [bool]$Success=$true, [string]$Extra='', [bool]$AutoDir=$false, [string]$SetupInstaller=$Installer) {
    if (-not ([IO.Path]::GetFullPath($Target).StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase))) { throw 'Test target outside fixture root.' }
    $script:sequence++
    $log = Join-Path $ResultDir "setup-$script:sequence.log"
    $id = ([IO.Path]::GetFileName($root) + '-' + [IO.Path]::GetFileName($Target)) -replace '[^A-Za-z0-9_-]', '-'
    $dirArg = "/DIR=`"$Target`""; if ($AutoDir) { $dirArg = '' }
    $args = "/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART $dirArg /LOG=`"$log`" /STEAMROOT=`"$Steam`" /FIXTUREROOT=`"$root`" /FIXTUREID=$id $Extra"
    $process = Run-FixtureExe $SetupInstaller $args
    Assert (($process.ExitCode -eq 0) -eq $Success) "Setup expected success=$Success (exit $($process.ExitCode))"
    if ($Success -and -not $script:installed.Contains($Target)) { [void]$script:installed.Add($Target) }
    return [IO.File]::ReadAllText($log)
}
function Uninstall([string]$Target, [bool]$Logs=$false, [bool]$Success=$true) {
    $script:sequence++
    $log=Join-Path $ResultDir "uninstall-$script:sequence.log"
    $exe=Join-Path $Target '.tw3k-coop-uninstall/unins000.exe'
    $extra=''; if ($Logs) { $extra='/REMOVELOGS=1' }
    $process=Run-FixtureExe $exe "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$log`" /FIXTUREROOT=`"$root`" $extra"
    Assert (($process.ExitCode -eq 0) -eq $Success) "uninstall expected success=$Success (exit $($process.ExitCode))"
    if ($Success) { [void]$script:installed.Remove($Target) }
    return [IO.File]::ReadAllText($log)
}
function Assert-Variant([string]$Target, $Expected, [string]$Log='') {
    Assert ((Sha (Join-Path $Target ".tw3k-coop-uninstall/InstallerHelper.exe")) -eq $Expected.helperSha256) "$($Expected.variant) extracted helper matches audited build input"
    Assert ((Sha (Join-Path $Target 'tw3k_coop.dll')) -eq $Expected.inputs.'tw3k_coop.dll') "$($Expected.variant) DLL installed under proxy filename"
    foreach ($client in @('TW3K-Coop-Control.ps1', 'TW3K-Coop-Recovery.cmd')) {
        $path=Join-Path $Target $client
        Assert ((Test-Path -LiteralPath $path) -eq ($Expected.variant -eq 'Debug')) "$($Expected.variant) client presence: $client"
        if ($Expected.variant -eq 'Debug') { Assert ((Sha $path) -eq $Expected.inputs.$client) "debug client verified: $client" }
    }
    if ($Log) {
        Assert ($Log.Contains('TW3K-Coop-Recovery.cmd') -eq ($Expected.variant -eq 'Debug')) "$($Expected.variant) finish recovery advice"
        $label='Player build (RELEASE)'; if ($Expected.variant -eq 'Debug') { $label='Developer build (DEBUG)' }
        Assert ($Log.Contains($label)) "$($Expected.variant) finish label"
    }
}
try {
    $none = Join-Path $root 'empty-steam'
    Write-Fixture (Join-Path $none 'steamapps/libraryfolders.vdf') '"libraryfolders" { }'
    Assert ((Detect $none) -match 'count=0') 'no game detected'
    $one = Join-Path $root 'Steam Root with spaces'
    $game = Game $one 'Fixture One'
    $original=Sha (Join-Path $game 'amd_ags_x64.dll')
    $stale = Game (Join-Path $root 'old-library') 'Old Folder'
    $detected = Detect $one $stale
    Assert ($detected.Contains('count=1') -and $detected.Contains($game) -and -not $detected.Contains($stale)) 'manifest beats valid stale uninstall location'
    Assert ((Detect $none (Join-Path $root 'missing-old-folder')) -match 'count=0') 'stale uninstall location without exe rejected'
    $secondLib=Join-Path $root 'Second Library'
    $second=Game $secondLib 'Fixture Two'
    $escaped=$secondLib.Replace('\','\\')
    Write-Fixture (Join-Path $one 'steamapps/libraryfolders.vdf') ('"libraryfolders" { "0" { "path" "' + $one.Replace('\','\\') + '" } "1" { "path" "' + $escaped + '" "apps" { "779340" "1" } } }')
    $multi=Detect $one
    Assert ($multi.Contains('count=2') -and $multi.Contains($game) -and $multi.Contains($second)) 'several modern Steam libraries detected'
    Write-Fixture (Join-Path $one 'steamapps/libraryfolders.vdf') ('"libraryfolders" { "1" "' + $escaped + '" }')
    Assert ((Detect $one).Contains('count=2')) 'legacy libraryfolders format detected'
    $log=Setup $one $game $false '' $true
    Assert ($log.Contains('Several installations were found')) 'silent several installations requires explicit selection'
    Write-Fixture (Join-Path $one 'steamapps/libraryfolders.vdf') '"libraryfolders" { }'
    $retired=@('coop_4player_ui.pack','coop_gift_panel_LIVE.pack','coop_gift_panel.pack','coop_skip_battle_prompts.pack','coop_dilemma_pump.pack','coop_gift_panel_DIAG.pack','coop_gift_panel_PROBE.pack','tw3k_coop.pack','skip_intro_movies.pack','reveal_starting_map.pack')
    foreach ($file in $retired) { Write-Fixture (Join-Path $game "data/$file") 'retired fixture' }
    $legacy=@('injector.exe','Install-TwProxy.ps1','Invoke-TwControl.ps1','Watch-TwInject.ps1','amd_ags_x64_proxy.dll')
    foreach ($file in $legacy) { Write-Fixture (Join-Path $game $file) 'legacy fixture' }
    Write-Fixture (Join-Path $game 'unrelated.ps1') 'keep unrelated script'
    Write-Fixture (Join-Path $game 'data/unrelated.pack') 'keep unrelated pack'
    Write-Fixture (Join-Path $game 'used_mods.txt') 'mod "tw3k_coop.pack";'
    Write-Fixture (Join-Path $game 'save_games/sentinel.save') 'save must stay'
    $workshopFile = ''
    if ($manifest.workshopItemId) {
        $workshopFile=Join-Path $one ("steamapps/workshop/content/779340/{0}/workshop-sentinel.pack" -f $manifest.workshopItemId)
        Write-Fixture $workshopFile 'Workshop content must stay'
    }
    # Reproduce an earlier Setup's appended uninstall ownership, not only clean installs.
    # Its tracked data pack would delete a new manual/Workshop copy on later uninstall.
    $legacyId = (([IO.Path]::GetFileName($root) + '-' + [IO.Path]::GetFileName($game)) -replace '[^A-Za-z0-9_-]', '-')
    $legacyPack=Join-Path $ResultDir 'legacy-pack.txt'
    Write-Fixture $legacyPack 'old installer pack fixture'
    $legacyIss=Join-Path $ResultDir 'legacy-pack.iss'
    $legacySpec=@"
[Setup]
AppId=TW3K-Coop-65FBEE59-07CD-4DA0-BEF0-6E47FD5C94A8-$legacyId
AppName=Offline legacy pack fixture
AppVersion=0.0.1
DefaultDirName=$game
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallFilesDir={app}\.tw3k-coop-uninstall
OutputDir=$ResultDir
OutputBaseFilename=legacy-pack-setup
[Files]
Source: "$legacyPack"; DestDir: "{app}\data"; DestName: "tw3k_coop.pack"
"@
    Write-Fixture $legacyIss $legacySpec
    $iscc=Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe'
    & $iscc $legacyIss | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'legacy pack installer fixture compiled'
    $oldSetup=Run-FixtureExe (Join-Path $ResultDir 'legacy-pack-setup.exe') "/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=`"$game`""
    Assert ($oldSetup.ExitCode -eq 0) 'legacy pack uninstall ownership seeded'
    $log=Setup $one $game $true '' $true
    Assert ((Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $original) 'fresh install preserves original byte-for-byte'
    Assert ((Sha (Join-Path $game 'amd_ags_x64.dll')) -eq (Sha (Join-Path (Split-Path $Installer) "stage/$($manifest.variant)/amd_ags_x64_proxy.dll"))) 'installed proxy matches build'
    Assert ($log.Contains('already enabled')) 'finish text recognizes enabled pack'
    if ($workshopFile) { Assert ($log.Contains('already downloaded')) 'finish text recognizes downloaded Workshop item' }
    else { Assert ($log.Contains('Subscribe to tw3k_coop in Steam Workshop')) 'unknown Workshop ID displays subscription instructions' }
    foreach ($file in $retired) { Assert (-not (Test-Path -LiteralPath (Join-Path $game "data/$file"))) "retired $file removed"; Assert ($log.Contains($file)) "retirement $file reported" }
    foreach ($file in $legacy) { Assert (-not (Test-Path -LiteralPath (Join-Path $game $file))) "legacy $file removed"; Assert ($log.Contains($file)) "legacy $file reported" }
    Assert ((Get-Content -Raw (Join-Path $game 'unrelated.ps1')) -eq 'keep unrelated script') 'unrelated root script untouched'
    Assert (-not (Test-Path -LiteralPath (Join-Path $game 'injector.exe'))) 'injector not shipped'
    Assert (-not (Test-Path -LiteralPath (Join-Path $game 'data/tw3k_coop.pack'))) 'Workshop pack not installed locally'
    Assert-Variant $game $manifest $log
    $null=Setup $one $game
    Assert ((Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $original) 'rerun preserves original before variant switch'
    $null=Setup $one $game $false '/TESTFAIL=after-cleanup' $false $OtherInstaller
    Assert-Variant $game $manifest
    Assert ((Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $original) 'failed variant switch preserves original'
    $switchLog=Setup $one $game $true '' $false $OtherInstaller
    Assert-Variant $game $otherManifest $switchLog
    Assert ((Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $original) 'variant switch preserves original'
    $null=Setup $one $game $false '/TESTFAIL=after-cleanup' $false $Installer
    Assert-Variant $game $otherManifest
    $switchLog=Setup $one $game
    Assert-Variant $game $manifest $switchLog
    Assert ((Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $original) 'switch back preserves original'
    $null=Setup $one $game
    Assert ((Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $original) 'rerun never overwrites saved original with proxy'
    Write-Fixture (Join-Path $game 'amd_ags_x64.dll') 'new Steam AMD original'
    $updated=Sha (Join-Path $game 'amd_ags_x64.dll')
    $log=Setup $one $game
    Assert ($log.Contains('Steam update') -and (Sha (Join-Path $game 'amd_ags_x64_orig.dll')) -eq $updated) 'Steam update current original wins'
    Write-Fixture (Join-Path $game 'tw3k_coop_keep.log') 'log kept'
    Write-Fixture (Join-Path $game 'data/tw3k_coop.pack') 'manually installed Workshop stand-in'
    $manualPack=Sha (Join-Path $game 'data/tw3k_coop.pack')
    $null=Uninstall $game
    Assert ((Sha (Join-Path $game 'data/tw3k_coop.pack')) -eq $manualPack) 'uninstall preserves pack added after install byte-for-byte'
    Assert ((Sha (Join-Path $game 'amd_ags_x64.dll')) -eq $updated) 'uninstall restores latest original byte-identically'
    Assert ((Test-Path -LiteralPath (Join-Path $game 'tw3k_coop_keep.log')) -and (Test-Path -LiteralPath (Join-Path $game 'save_games/sentinel.save'))) 'uninstall keeps logs and saves'
    Assert (-not (Test-Path -LiteralPath (Join-Path $game 'tw3k_coop.dll')) -and -not (Test-Path -LiteralPath (Join-Path $game 'TW3K-Coop-Control.ps1')) -and -not (Test-Path -LiteralPath (Join-Path $game 'TW3K-Coop-Recovery.cmd'))) 'uninstall removes mod and helpers'
    Assert (Test-Path -LiteralPath (Join-Path $game 'data/unrelated.pack')) 'unrelated pack untouched'
    if ($workshopFile) { Assert ((Get-Content -Raw -LiteralPath $workshopFile) -eq 'Workshop content must stay') 'uninstall preserves Workshop content byte-for-byte' }
    $null=Setup $one $game
    Write-Fixture (Join-Path $game 'amd_ags_x64.dll') 'Steam original after install'
    $steamNow=Sha (Join-Path $game 'amd_ags_x64.dll')
    $null=Uninstall $game $true
    Assert ((Sha (Join-Path $game 'amd_ags_x64.dll')) -eq $steamNow) 'uninstall after Steam update preserves current original'
    Assert (-not (Test-Path -LiteralPath (Join-Path $game 'tw3k_coop_keep.log'))) 'explicit remove logs works'
    $rollback=Game (Join-Path $root 'rollback-lib') 'Rollback'
    $rollbackHash=Sha (Join-Path $rollback 'amd_ags_x64.dll')
    Write-Fixture (Join-Path $rollback 'data/coop_gift_panel.pack') 'keep on failure'
    foreach ($file in $legacy) { Write-Fixture (Join-Path $rollback $file) 'rollback legacy bytes' }
    $null=Setup $none $rollback $false '/TESTFAIL=after-cleanup'
    foreach ($file in $legacy) { Assert ((Get-Content -Raw (Join-Path $rollback $file)) -eq 'rollback legacy bytes') "failed cleanup restores $file" }
    $null=Setup $none $rollback $false '/TESTFAIL=after-copy'
    Assert ((Sha (Join-Path $rollback 'amd_ags_x64.dll')) -eq $rollbackHash -and -not (Test-Path -LiteralPath (Join-Path $rollback 'amd_ags_x64_orig.dll'))) 'failed fresh install rolls back working DLL'
    Assert (Test-Path -LiteralPath (Join-Path $rollback 'data/coop_gift_panel.pack')) 'failed install keeps retired packs'
    $null=Setup $none $rollback
    Write-Fixture (Join-Path $rollback 'amd_ags_x64.dll') 'Steam updated rollback original'
    $rollbackUpdated=Sha (Join-Path $rollback 'amd_ags_x64.dll')
    $null=Setup $none $rollback $false '/TESTFAIL=after-copy'
    Assert ((Sha (Join-Path $rollback 'amd_ags_x64.dll')) -eq $rollbackUpdated -and (Sha (Join-Path $rollback 'amd_ags_x64_orig.dll')) -eq $rollbackHash) 'failed Steam repair restores both prior originals'
    $null=Uninstall $rollback
    $bad=Game (Join-Path $root 'invalid-lib') 'Invalid'
    Remove-Item -LiteralPath (Join-Path $bad 'amd_ags_x64.dll')
    $null=Setup $none $bad $false
    Assert (-not (Test-Path -LiteralPath (Join-Path $bad 'tw3k_coop.dll'))) 'missing original refused before mod install'
    $proxySource=Join-Path (Split-Path $Installer) "stage/$($manifest.variant)/amd_ags_x64_proxy.dll"
    Copy-Item -LiteralPath $proxySource -Destination (Join-Path $bad 'amd_ags_x64.dll')
    $log=Setup $none $bad $false
    Assert ($log.Contains('Saved original is missing')) 'already ours without original refuses safely'
    Write-Fixture (Join-Path $bad 'amd_ags_x64.dll') 'fixture original'
    Copy-Item -LiteralPath $proxySource -Destination (Join-Path $bad 'amd_ags_x64_orig.dll')
    $log=Setup $none $bad $false
    Assert ($log.Contains('another proxy')) 'proxy in saved-original slot refused'
    $notGame=Join-Path $root 'not-a-game'; New-Item -ItemType Directory -Path $notGame | Out-Null
    $log=Setup $none $notGame $false
    Assert ($log.Contains('Three_Kingdoms.exe')) 'chosen directory without game executable refused'
    $crash=Game (Join-Path $root 'crash-lib') 'CrashRecovery'
    $crashOriginal=Sha (Join-Path $crash 'amd_ags_x64.dll')
    $journalDir=Join-Path $crash '.tw3k-coop-transaction'; New-Item -ItemType Directory -Path $journalDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $crash 'amd_ags_x64.dll') -Destination (Join-Path $journalDir '0')
    Write-Fixture (Join-Path $journalDir 'journal') ("amd_ags_x64.dll|$crashOriginal`r`namd_ags_x64_orig.dll|absent`r`n")
    Copy-Item -LiteralPath (Join-Path $crash 'amd_ags_x64.dll') -Destination (Join-Path $crash 'amd_ags_x64_orig.dll')
    Copy-Item -LiteralPath $proxySource -Destination (Join-Path $crash 'amd_ags_x64.dll') -Force
    $null=Setup $none $crash
    Assert ((Sha (Join-Path $crash 'amd_ags_x64_orig.dll')) -eq $crashOriginal -and -not (Test-Path -LiteralPath $journalDir)) 'interrupted transaction journal recovered then install succeeds'
    $null=Uninstall $crash
    $lock=Game (Join-Path $root 'locked-lib') 'Locked'
    $stream=[IO.File]::Open((Join-Path $lock 'Three_Kingdoms.exe'),'Open','Read','None')
    try { $log=Setup $none $lock $false; Assert (-not (Test-Path -LiteralPath (Join-Path $lock 'amd_ags_x64_orig.dll'))) 'locked game refuses with no mutations' } finally { $stream.Dispose() }
    # This is ONLY a sleeping fixture process; never starts Total War or Steam.
    $source=Join-Path $root 'Sleep.cs'
    Write-Fixture $source 'class Fixture { static void Main() { System.Threading.Thread.Sleep(120000); } }'
    $fake=Join-Path $root 'Three_Kingdoms.exe'
    & (Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe') /nologo "/out:$fake" $source
    if ($LASTEXITCODE -ne 0) { throw 'Fixture process compilation failed.' }
    $proc=Start-Process -FilePath $fake -PassThru -WindowStyle Hidden
    try { $log=Setup $none $lock $false; Assert ($log.Contains('is running')) 'running fake game process clearly refused' } finally { Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue }
    $fakeLauncher=Join-Path $root 'launcher.exe'; Copy-Item -LiteralPath $fake -Destination $fakeLauncher
    $proc=Start-Process -FilePath $fakeLauncher -PassThru -WindowStyle Hidden
    try { $log=Setup $none $lock $false; Assert ($log.Contains('launcher is running')) 'running fake CA launcher clearly refused' } finally { Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue }
    $null=Setup $none $lock
    $lockedProxy=Sha (Join-Path $lock 'amd_ags_x64.dll')
    $proc=Start-Process -FilePath $fake -PassThru -WindowStyle Hidden
    try {
        $log=Uninstall $lock $false $false
        Assert ((Sha (Join-Path $lock 'amd_ags_x64.dll')) -eq $lockedProxy -and (Test-Path -LiteralPath (Join-Path $lock 'tw3k_coop.dll'))) 'uninstall refuses running game without deleting mod'
    } finally { Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue }
    $null=Uninstall $lock
    Write-Host "ok PASS $script:checks checks; PowerShell $($PSVersionTable.PSVersion); logs $ResultDir"
    "PASS $script:checks checks; PS $($PSVersionTable.PSVersion); fixture $root" | Set-Content -LiteralPath (Join-Path $ResultDir 'summary.txt')
} catch {
    Write-Host "FAIL $($_.Exception.Message)"
    "FAIL $($_.Exception.Message); fixture $root" | Set-Content -LiteralPath (Join-Path $ResultDir 'summary.txt')
    exit 1
} finally {
    # Failed tests retain fixtures for diagnosis, but clean up every successful Inno registration.
    foreach ($target in @($script:installed)) { try { $null=Uninstall $target } catch { Write-Warning "Fixture cleanup failed: $_" } }
}
