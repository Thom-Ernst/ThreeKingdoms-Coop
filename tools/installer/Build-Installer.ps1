<#
.SYNOPSIS
    Builds the public Windows DLL installer; the UI pack is distributed through Steam Workshop.
.DESCRIPTION
    One offline command, compatible with Windows PowerShell 5.1 and PowerShell 7. Inno Setup 6
    and the Windows .NET Framework compiler are required. No game directories are accessed.
    The DLL's embedded git stamp (not the checkout's HEAD) is shown in Setup and recorded beside
    hashes of every input and output. Existing runtime/ binaries are read, never rebuilt here.
    Release (default) ships no control clients. Debug selects tw3k_coop_debug.dll but installs
    it as tw3k_coop.dll and adds the local recovery client. DLL file properties must match
    the selected variant, including when an explicit -DllPath is supplied.
    Output is derived build data; do not commit it. Reproducible means the same documented inputs
    and build command; Inno/.NET timestamp fields do not promise byte-identical EXEs.
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Build-Installer.ps1 -Version 0.1.0
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:[.-][A-Za-z0-9.-]+)?$')][string]$Version = '0.1.0',
    [ValidateSet('Release', 'Debug')][string]$Variant = 'Release',
    [string]$DllPath,
    [ValidatePattern('^(?:[0-9]+)?$')][string]$WorkshopItemId = '',
    [string]$OutputDir,
    [string]$ISCC
)
$ErrorActionPreference = 'Stop'
# ValidateSet accepts case-insensitive input; Inno's preprocessor comparison is case-sensitive.
if ($Variant -eq 'Debug') { $Variant = 'Debug' } else { $Variant = 'Release' }
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $ISCC) { $ISCC = Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe' }
if (-not $DllPath) {
    $dllName = 'tw3k_coop.dll'
    if ($Variant -eq 'Debug') { $dllName = 'tw3k_coop_debug.dll' }
    $DllPath = Join-Path $repo "runtime/$dllName"
}
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
# ★ A renamed debug DLL must never become a player release through -DllPath.
$dllVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($DllPath)
if ($dllVersion.FileDescription -ne "Three Kingdoms Co-op $($Variant.ToUpperInvariant())" -or
    $dllVersion.IsDebug -ne ($Variant -eq 'Debug')) { throw "DLL build mode does not match installer variant $Variant." }
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'out/installer' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$stage = Join-Path $OutputDir "stage/$Variant"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
# Derived release assets must not appear as new source files after the default one-command build.
if (-not (Test-Path -LiteralPath (Join-Path $OutputDir '.gitignore'))) {
    Set-Content -LiteralPath (Join-Path $OutputDir '.gitignore') -Value '*' -Encoding ASCII
}
$sources = [ordered]@{
    'tw3k_coop.dll' = $DllPath
    'amd_ags_x64_proxy.dll' = 'runtime/proxy/amd_ags_x64.dll'
}
if ($Variant -eq 'Debug') {
    $sources['TW3K-Coop-Control.ps1'] = 'tools/installer/TW3K-Coop-Control.ps1'
    $sources['TW3K-Coop-Recovery.cmd'] = 'tools/installer/TW3K-Coop-Recovery.cmd'
}
$hashes = [ordered]@{}
foreach ($entry in $sources.GetEnumerator()) {
    $source = $entry.Value
    if (-not [IO.Path]::IsPathRooted($source)) { $source = Join-Path $repo $source }
    if (-not (Test-Path -LiteralPath $source) -or (Get-Item -LiteralPath $source).Length -eq 0) { throw "Missing/empty build input: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $entry.Key) -Force
    $hashes[$entry.Key] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
}
# ★ Label the bytes being packaged, even when -DllPath selects a release build from another tree.
$dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $stage 'tw3k_coop.dll')))
$stamp = [regex]::Matches($dllText, '[ -~]{8,}') | ForEach-Object { $_.Value } |
    Where-Object { ($Variant -eq 'Release' -and $_ -match '^1\.0\.0(?: [0-9a-f]{7,40}(?:\+dirty)?)?$') -or
        ($Variant -eq 'Debug' -and $_ -match '^\S+ [0-9a-f]{7,40}(?:\+dirty)?$') } | Select-Object -First 1
if (-not $stamp) { throw 'DLL embedded git stamp not found; cannot label unknown build as a release.' }
$numeric = ([regex]::Match($Version, '^\d+\.\d+\.\d+').Value) + '.0'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw ".NET Framework compiler missing: $compiler" }
$lines = @('using System.Collections.Generic;', 'internal static class PayloadStamp {', 'internal static readonly Dictionary<string,string> Hashes = new Dictionary<string,string> {')
foreach ($entry in $hashes.GetEnumerator()) { $lines += '{ @"' + $entry.Key + '", "' + $entry.Value + '" },' }
$lines += '};', ('internal const bool Debug = ' + ($Variant -eq 'Debug').ToString().ToLowerInvariant() + ';'), '}'
Set-Content -LiteralPath (Join-Path $stage 'PayloadStamp.cs') -Value $lines -Encoding UTF8
& $compiler /nologo /target:exe /platform:anycpu /optimize+ "/out:$stage\InstallerHelper.exe" (Join-Path $PSScriptRoot 'InstallerHelper.cs') (Join-Path $stage 'PayloadStamp.cs')
if ($LASTEXITCODE -ne 0) { throw 'Installer helper compilation failed.' }
if (-not (Test-Path -LiteralPath $ISCC)) { throw "Inno Setup 6 compiler missing: $ISCC" }
$compilerOutput = & $ISCC "/DVariant=$Variant" "/DVersion=$Version" "/DNumericVersion=$numeric" "/DDllStamp=$stamp" "/DWorkshopItemId=$WorkshopItemId" "/DStage=$stage" "/DOutput=$OutputDir" (Join-Path $PSScriptRoot 'TW3K-Coop.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
$compilerOutput | ForEach-Object { Write-Host $_ }
$compilerVersion = [regex]::Match(($compilerOutput -join "`n"), 'Compiler engine version: Inno Setup ([0-9.]+)').Groups[1].Value
$buildSources = [ordered]@{}
foreach ($name in @('Build-Installer.ps1', 'InstallerHelper.cs', 'TW3K-Coop.iss')) {
    $buildSources[$name] = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash
}
$suffix = ''; if ($Variant -eq 'Debug') { $suffix = '-debug' }
$baseName = "TW3K-Coop-Setup-$Version$suffix"
$exe = Join-Path $OutputDir "$baseName.exe"
$manifest = [ordered]@{ version=$Version; variant=$Variant; dllStamp=$stamp; workshopItemId=$WorkshopItemId; repositoryCommit=(& git -C $repo rev-parse HEAD); inputs=$hashes; buildSources=$buildSources; helperSha256=(Get-FileHash -LiteralPath (Join-Path $stage 'InstallerHelper.exe')).Hash; installer=[IO.Path]::GetFileName($exe); installerSha256=(Get-FileHash -LiteralPath $exe).Hash; compiler=$compilerVersion }
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDir "$baseName.hashes.json") -Encoding UTF8
Write-Host "ok built $exe"
Write-Host "DLL source: $stamp"
Write-Host "SHA256: $($manifest.installerSha256)"
