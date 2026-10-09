@echo off
setlocal
rem Windows PowerShell ships with Windows; players never type a PowerShell command.
set "TW3KRECOVERYROOT=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0TW3K-Coop-Control.ps1" -Recover
