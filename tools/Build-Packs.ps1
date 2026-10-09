<#
.SYNOPSIS
    Rebuilds pack mods from their source trees under mods\, through the RPFM Server.

.DESCRIPTION
    The third gate nobody automated. Deploy-Packs.ps1 already reports built / installed / enabled
    and handles the last two; this handles the first, so "built" stops meaning "somebody ran the
    right sequence of calls by hand at some point".

    ★ You do NOT need an MCP client, or Claude, or the RPFM GUI to rebuild a pack. This script is
    plain PowerShell talking JSON-RPC to the RPFM Server over HTTP. Start RPFM (which starts the
    backend) and run it.

    ⚠ Do not reach for rpfm_cli.exe. It was REMOVED UPSTREAM in RPFM 5.0.0 — "consider migrating to
    the new RPFM Server" — and any copy still on disk is an orphan from an older install. The one
    here is v4.4.3, which cannot read a pack at all ("GameInfo has not been provided..."); that was
    fixed in 4.4.4 and the tool was deleted two weeks later in 5.0.0.

    Source layout, resolved in this order:

      mods\<name>\build\   a tree produced by that mod's own generator (coop_4player_ui)
      mods\<name>\src\     an explicit source root
      mods\<name>\         top-level in-pack folders directly: script\, db\, text\, ui\, ...

    Everything under the resolved root is added at its mirrored in-pack path. A .tsv under db\ or
    text\db\ is imported as a table rather than added verbatim, because the pack wants the binary.

    A mod is only built automatically when the mapping is unambiguous: one .pack, one source tree.
    Anything else is reported with the reason rather than guessed at — see "Not built automatically".

.EXAMPLE
    .\Build-Packs.ps1
    Status only. Which mods can be rebuilt, and whether their sources are newer than their pack.

.EXAMPLE
    .\Build-Packs.ps1 -Mods reveal_starting_map
    Rebuild that one mod and verify the result.

.EXAMPLE
    .\Build-Packs.ps1 -All -WhatIf
    Show every pack that would be rebuilt.
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    # Mod folder names under mods\. Accepts wildcards.
    [string[]]$Mods,

    # Rebuild every mod that can be built automatically.
    [switch]$All,

    # The game these packs are for. Only change this if the repo ever targets another title.
    [string]$Game = 'three_kingdoms',

    # RPFM Server address. Defaults to the one in .mcp.json.
    [string]$Uri
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'TwRpfm.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$modsRoot = Join-Path $repoRoot 'mods'

# Top-level folder names that are real in-pack roots. A mod folder containing one of these directly
# is its own source tree; anything else at the top level (design\, extracted\, archive\) is not.
$packRoots = @('script', 'db', 'text', 'ui', 'movies', 'variants', 'campaigns', 'audio', 'font')


function Resolve-SourceRoot {
    param([string]$ModDir)

    foreach ($candidate in @('build', 'src')) {
        $path = Join-Path $ModDir $candidate
        if (Test-Path -LiteralPath $path) { return $path }
    }
    foreach ($root in $packRoots) {
        if (Test-Path -LiteralPath (Join-Path $ModDir $root)) { return $ModDir }
    }
    return $null
}


function Get-ModPlan {
    <#
        Works out what, if anything, this mod's automatic build would be — and when it cannot,
        says why in a sentence rather than skipping quietly.
    #>
    param([System.IO.DirectoryInfo]$ModDir)

    $packs  = @(Get-ChildItem -LiteralPath $ModDir.FullName -Filter '*.pack' -File -ErrorAction SilentlyContinue)
    $source = Resolve-SourceRoot -ModDir $ModDir.FullName

    $plan = [pscustomobject]@{
        Mod        = $ModDir.Name
        SourceRoot = $source
        PackPath   = $null
        Files      = @()
        Buildable  = $false
        Reason     = ''
    }

    if (-not $source) {
        $plan.Reason = 'no source tree (no build\, src\ or in-pack folder)'
        return $plan
    }

    # ⚠ When the source root IS the mod folder, recursing all of it sweeps up the built .pack, the
    # README and any archive\ — and packing a pack into itself is a silent, plausible-looking
    # disaster. So in that case walk only the recognised in-pack roots, never the folder itself.
    if ($source -eq $ModDir.FullName) {
        $files = @()
        foreach ($root in $packRoots) {
            $rootPath = Join-Path $source $root
            if (Test-Path -LiteralPath $rootPath) {
                $files += @(Get-ChildItem -LiteralPath $rootPath -File -Recurse -ErrorAction SilentlyContinue)
            }
        }
    } else {
        $files = @(Get-ChildItem -LiteralPath $source -File -Recurse -ErrorAction SilentlyContinue)
    }

    # Never ship documentation or a previous build, wherever the root happens to be.
    $files = @($files | Where-Object { $_.Extension -ne '.md' -and $_.Extension -ne '.pack' })
    $plan.Files = $files

    if ($files.Count -eq 0) { $plan.Reason = 'source tree is empty'; return $plan }

    if ($packs.Count -gt 1) {
        $plan.Reason = "$($packs.Count) packs from one source tree — the split is per-file, see the mod's README"
        return $plan
    }

    if ($packs.Count -eq 1) { $plan.PackPath = $packs[0].FullName }
    else { $plan.PackPath = Join-Path $ModDir.FullName ($ModDir.Name + '.pack') }

    $plan.Buildable = $true
    return $plan
}


function Get-InPackPath {
    param([string]$SourceRoot, [System.IO.FileInfo]$File)

    $relative = $File.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
    return ($relative -replace '\\', '/')
}


function Get-TsvMeta {
    <#
    .SYNOPSIS
        Reads an RPFM TSV's own declaration of what table it is.

    .DESCRIPTION
        Line 2 of every RPFM TSV export is a metadata row naming the table, its definition version
        and its full in-pack path:

            #videos_tables;4;db/videos_tables/data__
            #incidents_tables;1;db/incidents_tables/dong_min_dies
            #Loc;1;text/db/dong_min_dies.loc

        That is the authoritative source for all three, so read it rather than deriving any of them
        from the file's position on disk. It also means a table can be created with the right
        version without consulting a schema.
    #>
    param([Parameter(Mandatory)][string]$Path)

    $lines = Get-Content -LiteralPath $Path -TotalCount 2 -ErrorAction Stop
    if ($lines.Count -lt 2) { return $null }

    $meta = ($lines[1] -split "`t")[0]
    if (-not $meta.StartsWith('#')) { return $null }

    $parts = $meta.TrimStart('#') -split ';'
    if ($parts.Count -lt 3) { return $null }

    return [pscustomobject]@{
        Table   = $parts[0]
        Version = [int]$parts[1]
        InPack  = $parts[2]
        IsLoc   = ($parts[0] -eq 'Loc')
    }
}


function Test-PackHeader {
    <#
        PFH5 + type 3 (Mod) is what the launcher lists. A pack that builds fine and is not type 3
        simply never appears, which looks exactly like the mod not working.
    #>
    param([string]$Path)

    try {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        if ($bytes.Length -lt 8) { return 'too short' }
        $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4)
        if ($magic -ne 'PFH5') { return "magic $magic" }
        if ($bytes[4] -ne 3)   { return "type $($bytes[4])" }
        return 'ok'
    } catch {
        return 'unreadable'
    }
}


# --------------------------------------------------------------------------------------------------
#  Select the mods
# --------------------------------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $modsRoot)) { throw "no mods folder at $modsRoot" }

# `pwsh -File` hands a comma list over as one string; an in-session call splits it. Same fix the
# other scripts here carry.
$Mods = @($Mods | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$allMods = @(Get-ChildItem -LiteralPath $modsRoot -Directory | Sort-Object Name)
if ($Mods.Count -gt 0) {
    $selected = @($allMods | Where-Object { $name = $_.Name; ($Mods | Where-Object { $name -like $_ }).Count -gt 0 })
    foreach ($pattern in $Mods) {
        if (@($allMods | Where-Object { $_.Name -like $pattern }).Count -eq 0) {
            Write-Warning "no mod folder matches '$pattern'"
        }
    }
} else {
    $selected = $allMods
}

$plans = @($selected | ForEach-Object { Get-ModPlan -ModDir $_ })

# --------------------------------------------------------------------------------------------------
#  Status only, unless asked to build
# --------------------------------------------------------------------------------------------------

$building = $All -or ($Mods.Count -gt 0)

if (-not $building) {
    Write-Host "`nmods that can be rebuilt from source:`n" -ForegroundColor Cyan

    $rows = foreach ($plan in $plans) {
        $stale = ''
        if ($plan.Buildable -and (Test-Path -LiteralPath $plan.PackPath)) {
            $packWritten = (Get-Item -LiteralPath $plan.PackPath).LastWriteTime
            $newest = ($plan.Files | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
            if ($newest -gt $packWritten) { $stale = 'SOURCES NEWER' }
        } elseif ($plan.Buildable) {
            $stale = 'NEVER BUILT'
        }

        [pscustomobject]@{
            Mod    = $plan.Mod
            Source = $(if ($plan.SourceRoot) { Split-Path -Leaf $plan.SourceRoot } else { '-' })
            Files  = $plan.Files.Count
            Build  = $(if ($plan.Buildable) { 'yes' } else { 'no' })
            State  = $stale
            Why    = $plan.Reason
        }
    }
    $rows | Format-Table Mod, Source, Files, Build, State, Why -AutoSize

    Write-Host "Rebuild one with:  .\Build-Packs.ps1 -Mods <name>     (or -All)" -ForegroundColor DarkGray
    Write-Host "No MCP client needed — this talks to the RPFM Server over plain HTTP.`n" -ForegroundColor DarkGray
    return
}

# --------------------------------------------------------------------------------------------------
#  Build
# --------------------------------------------------------------------------------------------------

$buildable = @($plans | Where-Object { $_.Buildable })
$skipped   = @($plans | Where-Object { -not $_.Buildable })

foreach ($plan in $skipped) {
    Write-Warning "$($plan.Mod): not built automatically — $($plan.Reason)"
}
if ($buildable.Count -eq 0) { Write-Host "nothing to build."; return }

$session = $null
if ($PSCmdlet.ShouldProcess('RPFM Server', 'connect')) {
    $connectArgs = @{}
    if ($Uri) { $connectArgs['Uri'] = $Uri }
    $session = Connect-TwRpfm @connectArgs

    # rebuild_dependencies stays $false on purpose: $true times out on this setup, and building a
    # pack from source files needs no dependency database.
    $null = Invoke-TwRpfmTool -Session $session -Name 'set_game_selected' `
        -Arguments @{ game_name = $Game; rebuild_dependencies = $false }
    Write-Host "RPFM Server: connected, game = $Game`n" -ForegroundColor Cyan
}

$results = New-Object System.Collections.Generic.List[object]

foreach ($plan in $buildable) {
    $target = $plan.PackPath
    if (-not $PSCmdlet.ShouldProcess($target, "rebuild from $($plan.SourceRoot)")) { continue }

    Write-Host ("building {0} ({1} file(s))" -f $plan.Mod, $plan.Files.Count) -ForegroundColor White

    try {
        $packKey = Get-TwRpfmPackKey (Invoke-TwRpfmTool -Session $session -Name 'new_pack')

        # A .tsv under db\ or text\db\ is a table source: the pack wants the imported binary, not
        # the text. Everything else goes in as-is.
        $verbatim = New-Object System.Collections.Generic.List[object]
        $tables   = New-Object System.Collections.Generic.List[object]

        foreach ($file in $plan.Files) {
            $inPack = Get-InPackPath -SourceRoot $plan.SourceRoot -File $file

            $meta = $null
            if ($file.Extension -eq '.tsv') { $meta = Get-TsvMeta -Path $file.FullName }

            if ($meta) {
                # The TSV names its own destination. Trust it, but say so if the source tree
                # disagrees — that means a table source is filed in the wrong folder, which would
                # otherwise produce a correct-looking pack containing the wrong path.
                $mirrored = $inPack -replace '\.tsv$', ''
                if ($mirrored -ne $meta.InPack) {
                    Write-Warning ("{0}: {1} declares '{2}' but sits at '{3}' — using the declaration." -f `
                        $plan.Mod, $file.Name, $meta.InPack, $mirrored)
                }
                $tables.Add([pscustomobject]@{ Disk = $file.FullName; Meta = $meta })
            } else {
                $verbatim.Add([pscustomobject]@{ Disk = $file.FullName; InPack = $inPack })
            }
        }

        if ($verbatim.Count -gt 0) {
            $sources      = @($verbatim | ForEach-Object { $_.Disk })
            $destinations = ConvertTo-Json @($verbatim | ForEach-Object { @{ File = $_.InPack } }) -Depth 4 -Compress

            # RPFM wants a JSON array. Getting one is version-dependent and both directions bite:
            # PowerShell 5.1 collapses a single-element array to a bare object, while 7 already
            # emits the array — so unconditionally wrapping produced [[{...}]] and a parse error.
            # -AsArray would settle it but is 7-only, and these scripts run under 5.1 too.
            # Test the string instead of trusting the host version.
            if (-not $destinations.StartsWith('[')) { $destinations = "[$destinations]" }

            $null = Invoke-TwRpfmTool -Session $session -Name 'add_packed_files' `
                -Arguments @{ pack_key = $packKey; source_paths = $sources; destination_paths = $destinations }
        }

        # ⚠ import_tsv imports INTO an existing table — it does not create one, and says so:
        # "File with the following path not found in the Pack". So each table is created first,
        # at the version its own TSV declares, and then filled.
        foreach ($table in $tables) {
            $meta     = $table.Meta
            $baseName = Split-Path -Leaf $meta.InPack

            if ($meta.IsLoc) {
                $newFile = ConvertTo-Json @{ Loc = $baseName } -Depth 3 -Compress
            } else {
                $newFile = '{"DB":["' + $baseName + '","' + $meta.Table + '",' + $meta.Version + ']}'
            }

            $null = Invoke-TwRpfmTool -Session $session -Name 'new_packed_file' `
                -Arguments @{ pack_key = $packKey; path = $meta.InPack; new_file = $newFile }

            $null = Invoke-TwRpfmTool -Session $session -Name 'import_tsv' `
                -Arguments @{ pack_key = $packKey; tsv_path = $table.Disk; table_path = $meta.InPack }
        }

        $null = Invoke-TwRpfmTool -Session $session -Name 'save_pack_as' `
            -Arguments @{ pack_key = $packKey; path = $target }

        $null = Invoke-TwRpfmTool -Session $session -Name 'close_pack' -Arguments @{ pack_key = $packKey }

        $header = Test-PackHeader -Path $target
        $size   = 0
        if (Test-Path -LiteralPath $target) { $size = (Get-Item -LiteralPath $target).Length }

        $results.Add([pscustomobject]@{
            Mod    = $plan.Mod
            Pack   = Split-Path -Leaf $target
            Files  = $verbatim.Count
            Tables = $tables.Count
            KB     = [int][math]::Round($size / 1KB)
            Header = $header
            Result = $(if ($size -gt 0 -and $header -eq 'ok') { 'ok' } else { 'CHECK' })
        })
    } catch {
        Write-Warning "$($plan.Mod): $($_.Exception.Message)"
        $results.Add([pscustomobject]@{
            Mod = $plan.Mod; Pack = Split-Path -Leaf $target; Files = 0; Tables = 0
            KB = 0; Header = '-'; Result = 'FAILED'
        })
    }
}

if ($results.Count -eq 0) { return }

Write-Host ''
$results | Format-Table Mod, Pack, Files, Tables, KB, Header, Result -AutoSize

$bad = @($results | Where-Object { $_.Result -ne 'ok' }).Count
if ($bad -gt 0) {
    Write-Warning "$bad pack(s) need looking at."
} else {
    Write-Host "all packs rebuilt and verified as PFH5 type 3 (Mod)." -ForegroundColor Green
    Write-Host "building is not installing — run Deploy-Packs.ps1 next.`n" -ForegroundColor DarkGray
}
