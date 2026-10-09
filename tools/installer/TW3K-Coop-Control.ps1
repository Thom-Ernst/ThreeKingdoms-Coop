<#
.SYNOPSIS
    Public, local-only mod status and hidden-decision recovery client.
.DESCRIPTION
    Players double-click TW3K-Coop-Recovery.cmd; no PowerShell knowledge is required. The client
    talks only to the mod's local named pipe. There is no rig inventory, remoting or machine name.
    Recovery asks every player to inspect their own decision; only the owner may answer, enforced
    by both the displayed MINE reply and the DLL's answer command. No global unblock is offered.
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File TW3K-Coop-Control.ps1 -Command ping
#>
#Requires -Version 5.1
[CmdletBinding()]
param([ValidateSet('ping','dilemma')][string]$Command='ping', [switch]$Recover)
$ErrorActionPreference='Stop'
function Send-Local([string]$Text) {
    $pipe=New-Object IO.Pipes.NamedPipeClientStream('.', 'tw3k_coop', 'InOut')
    try {
        $pipe.Connect(3000)
        $writer=New-Object IO.StreamWriter($pipe); $writer.AutoFlush=$true
        $writer.WriteLine($Text)
        $reader=New-Object IO.StreamReader($pipe)
        $task=$reader.ReadToEndAsync()
        if (-not $task.Wait(40000)) { throw 'The mod did not answer within 40 seconds.' }
        return $task.Result
    } finally { $pipe.Dispose() }
}
try {
    if (-not $Recover) { Send-Local $Command; return }
    $reply=Send-Local 'dilemma'; Write-Host $reply
    if ($reply -notmatch 'MINE, this machine may answer it') {
        Write-Host 'This machine has no decision it may answer. The player whose reply says MINE must run recovery.'
    } else {
        $option=Read-Host 'Type an option number listed above (Enter to cancel)'
        if ($option -match '^\d{1,2}$') { Write-Host (Send-Local ('answer ' + $option + ' --yes')) }
    }
} catch { Write-Host ('Recovery stopped: ' + $_.Exception.Message) }
if ($Recover) { Read-Host 'Press Enter to close' | Out-Null }
