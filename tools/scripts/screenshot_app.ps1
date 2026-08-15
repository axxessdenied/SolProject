# Launches the game, waits, captures its window to a PNG, then kills it.
# Used for automated visual verification of renderer changes.
param(
    [string]$OutFile = "$PSScriptRoot\screenshot.png",
    [int]$WaitMs = 2500,
    [string]$Exe = ''
)
$ErrorActionPreference = 'Stop'
if ($Exe -eq '') {
    $repo = (Resolve-Path "$PSScriptRoot\..\..").Path
    $Exe = "$repo\build\dev\bin\sol.exe"
}

Get-Process -Name sol -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $WaitMs
$p.Refresh()
if ($p.MainWindowHandle -eq [IntPtr]::Zero) { $p.Kill(); throw 'no window' }

& "$PSScriptRoot\capture_window.ps1" -ProcessId $p.Id -OutFile $OutFile

$p.Kill()
$p.WaitForExit()
