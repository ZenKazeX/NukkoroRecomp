<#
.SYNOPSIS
    Installs Shadow the Hedgehog Recompiled (static recompilation, work in progress) from the console.

.DESCRIPTION
    The scripted counterpart of the setup window (Install.cmd opens the window). It validates the extracted PAL
    disc, builds (the recompiler reads sys/main.dol), copies the executable and the disc's files/ tree to the
    install folder, and creates shortcuts. It uses the same steps as the window (InstallCore.psm1).

.EXAMPLE
    .\installer\Install.ps1 -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "D:\Games\Shadow the Hedgehog Recompiled"
#>
param(
    [Parameter(Mandatory = $true)][string]$DiscPath,
    [Parameter(Mandatory = $true)][string]$InstallPath,
    [switch]$NoShortcuts,
    [ValidateRange(1, 64)][int]$Jobs = 2,
    [string]$DesktopFolder = [Environment]::GetFolderPath('Desktop'),
    [string]$StartMenuFolder = (Join-Path ([Environment]::GetFolderPath('Programs')) 'Shadow the Hedgehog Recompiled')
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'InstallCore.psm1') -Force

function Fail([string]$message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

$prereqs = @(Test-Prerequisites)
$missing = @($prereqs | Where-Object { -not $_.Ok })
foreach ($m in $missing) { Write-Host "Missing: $($m.Name) - $($m.Detail) ($($m.Url))" -ForegroundColor Red }
if ($missing) { Fail 'Install the missing tools and run the installer again.' }
$python = ($prereqs | Where-Object { $_.Name -eq 'Python 3' }).Value

$disc = Test-DiscFolder $DiscPath
if (-not $disc.Ok) { Fail $disc.Message }
Write-Host "Disc: $($disc.Root) - $($disc.Message)"

$target = Test-InstallFolder $InstallPath $disc.Root (Get-FolderSize (Join-Path $disc.Root 'files'))
if (-not $target.Ok) { Fail $target.Message }
Write-Host "Install folder: $($target.Path) - $($target.Message)"

$shortcuts = -not $NoShortcuts
$ok = Invoke-GameInstall $disc $target.Path $python $Jobs $shortcuts $shortcuts {
    param($e)
    switch ($e.type) {
        'stage' { if ($e.state -eq 'run') { Write-Host ""; Write-Host "== $($e.name)" -ForegroundColor Cyan } }
        'log' { Write-Host -NoNewline $e.text }
        'error' { Write-Host ""; Write-Host "ERROR: $($e.message) Log: $($e.log)" -ForegroundColor Red }
        'done' { Write-Host ""; Write-Host "Installed to $($e.target)" -ForegroundColor Green }
    }
} $DesktopFolder $StartMenuFolder
if (-not $ok) { exit 1 }
