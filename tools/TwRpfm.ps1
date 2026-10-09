<#
.SYNOPSIS
    A tiny RPFM Server client. Dot-sourced by Build-Packs.ps1; not run directly.

.DESCRIPTION
    Everything this repo does to a .pack goes through RPFM. There used to be three ways to drive it
    and now there is one:

      rpfm_ui.exe      the desktop app — fine by hand, no good in a script
      rpfm_cli.exe     REMOVED UPSTREAM in RPFM 5.0.0: "consider migrating to the new RPFM Server"
      rpfm_server.exe  the backend, speaking MCP over HTTP — what this file talks to

    ⚠ There may still be an rpfm_cli.exe next to your RPFM install. On this machine it is v4.4.3
    from 2025-06-18, orphaned by an upgrade to 5.0.5, and it cannot read a pack at all — every
    read fails with "GameInfo has not been provided to the pack-reading function". That was fixed
    in 4.4.4 and the tool was deleted in 5.0.0, so do not build anything on it.

    The server is the same process the MCP integration uses, but nothing here needs an MCP client:
    it is plain JSON-RPC over HTTP, so any script (or any person with curl) can drive it.

.EXAMPLE
    . (Join-Path $PSScriptRoot 'TwRpfm.ps1')
    $s = Connect-TwRpfm
    Invoke-TwRpfmTool -Session $s -Name 'set_game_selected' -Arguments @{ game_name = 'three_kingdoms'; rebuild_dependencies = $false }
#>

# The address the RPFM Server listens on. Matches .mcp.json in the repo root.
$script:TwRpfmDefaultUri = 'http://127.0.0.1:45127/mcp'


function Test-TwRpfmServer {
    <#
    .SYNOPSIS
        True if an RPFM Server is answering. Used for a clear message instead of a wall of red.
    #>
    param([string]$Uri = $script:TwRpfmDefaultUri)

    try {
        $null = Invoke-WebRequest -Uri $Uri -Method Post -TimeoutSec 5 `
            -ContentType 'application/json' `
            -Headers @{ 'Accept' = 'application/json, text/event-stream' } `
            -Body '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"probe","version":"1"}}}'
        return $true
    } catch {
        return $false
    }
}


function Connect-TwRpfm {
    <#
    .SYNOPSIS
        Performs the MCP handshake and returns a session object to pass to Invoke-TwRpfmTool.
    #>
    param([string]$Uri = $script:TwRpfmDefaultUri)

    if (-not (Test-TwRpfmServer -Uri $Uri)) {
        throw @"
No RPFM Server answering at $Uri.

Start it by launching RPFM (rpfm_ui.exe starts the backend), or run rpfm_server.exe directly.
The address is also in .mcp.json at the repo root — if you changed it there, pass -Uri.
"@
    }

    $init = @{
        jsonrpc = '2.0'; id = 1; method = 'initialize'
        params  = @{
            protocolVersion = '2025-06-18'
            capabilities    = @{}
            clientInfo      = @{ name = 'tw-build-packs'; version = '1.0' }
        }
    } | ConvertTo-Json -Depth 6

    $response = Invoke-WebRequest -Uri $Uri -Method Post -TimeoutSec 30 `
        -ContentType 'application/json' `
        -Headers @{ 'Accept' = 'application/json, text/event-stream' } `
        -Body $init

    # PowerShell 7 hands header values back as a string[], and interpolating that into a header
    # value yields "System.String[]" and a 404 "Session not found". Take the first element.
    $sessionId = @($response.Headers['Mcp-Session-Id'])[0]
    if (-not $sessionId) { throw "RPFM Server did not return an Mcp-Session-Id. Is it really RPFM 5.x?" }

    $headers = @{
        'Accept'         = 'application/json, text/event-stream'
        'Mcp-Session-Id' = $sessionId
    }

    # The protocol requires this before any tool call; the server rejects calls made without it.
    $null = Invoke-WebRequest -Uri $Uri -Method Post -TimeoutSec 30 `
        -ContentType 'application/json' -Headers $headers `
        -Body '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'

    return [pscustomobject]@{
        Uri     = $Uri
        Id      = $sessionId
        Headers = $headers
        NextId  = 10
    }
}


function Invoke-TwRpfmTool {
    <#
    .SYNOPSIS
        Calls one RPFM tool and returns its decoded result.

    .DESCRIPTION
        Results come back as Server-Sent Events carrying a JSON-RPC envelope, whose payload is
        itself a JSON string. This unwraps all three layers and returns the innermost object.

        ⚠ Do NOT name a parameter $Args here or in any caller: it collides with PowerShell's
        automatic $args variable and the arguments silently arrive empty. That cost an hour.
    #>
    param(
        [Parameter(Mandatory)] $Session,
        [Parameter(Mandatory)] [string]$Name,
        [hashtable]$Arguments = @{},
        [int]$TimeoutSec = 300
    )

    $Session.NextId++
    $body = @{
        jsonrpc = '2.0'; id = $Session.NextId; method = 'tools/call'
        params  = @{ name = $Name; arguments = $Arguments }
    } | ConvertTo-Json -Depth 12

    $raw = (Invoke-WebRequest -Uri $Session.Uri -Method Post -TimeoutSec $TimeoutSec `
        -ContentType 'application/json' -Headers $Session.Headers -Body $body).Content

    # SSE: keep only the data: lines that carry a JSON object, and join them.
    $payload = ($raw -split "`n" |
        Where-Object { $_ -like 'data: {*' } |
        ForEach-Object { $_.Substring(6).TrimEnd("`r") }) -join ''

    if (-not $payload) { throw "RPFM tool '$Name' returned no JSON-RPC payload." }

    $envelope = $payload | ConvertFrom-Json

    if ($envelope.error) {
        throw "RPFM tool '$Name' failed: $($envelope.error.message)"
    }
    if ($envelope.result.isError) {
        throw "RPFM tool '$Name' reported an error: $($envelope.result.content[0].text)"
    }

    $text = $envelope.result.content[0].text
    if (-not $text) { return $null }

    # Most tools answer with a JSON string; a few answer with plain text.
    try { return ($text | ConvertFrom-Json) } catch { return $text }
}


function Get-TwRpfmPackKey {
    <#
    .SYNOPSIS
        Pulls the pack key out of whatever shape a tool used to return it.

    .DESCRIPTION
        new_pack answers {"String":"new_pack.pack"}; other calls answer a bare string. Both are
        the key that every later call wants, so normalise here rather than at each call site.
    #>
    param([Parameter(Mandatory)] $Result)

    if ($Result -is [string]) { return $Result }
    if ($Result.PSObject.Properties.Name -contains 'String') { return $Result.String }
    throw "Could not find a pack key in the RPFM result: $($Result | ConvertTo-Json -Depth 4 -Compress)"
}
