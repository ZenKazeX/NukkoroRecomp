<#
.SYNOPSIS
    Installs Shadow the Hedgehog Recompiled (static recompilation, work in progress) from the console.

.DESCRIPTION
    The scripted counterpart of the setup window (Install.cmd opens the window). It validates the extracted PAL
    disc, builds (the recompiler reads sys/main.dol), copies the executable and the disc's files/ tree to the
    install folder, and creates shortcuts. It uses the same steps as the window (InstallCore.psm1).

    Japanese voices: the PAL disc has no Japanese event (cutscene) or dialogue voices. To hear them in Japanese, extract
    the Japanese disc (GUPJ8P) into a different folder and pass it as -JapaneseDiscPath; setup copies its voice files
    to voice_jp in the install folder. -SpokenLanguage sets the language the game speaks (options.cfg 'voice'); it
    defaults to Japanese when a Japanese disc is given and is left as it is otherwise. Running setup again over an
    existing install with -JapaneseDiscPath adds the voices later; saves and settings are kept.

.EXAMPLE
    .\installer\Install.ps1 -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "D:\Games\Shadow the Hedgehog Recompiled"

.EXAMPLE
    .\installer\Install.ps1 -DiscPath "D:\Dumps\Shadow PAL" -JapaneseDiscPath "D:\Dumps\Shadow JP" -InstallPath "D:\Games\Shadow the Hedgehog Recompiled"
#>
param(
    [Parameter(Mandatory = $true)][string]$DiscPath,
    [Parameter(Mandatory = $true)][string]$InstallPath,
    [string]$JapaneseDiscPath = '',
    [ValidateSet('', 'Japanese', 'English')][string]$SpokenLanguage = '',
    [switch]$NoShortcuts,
    [ValidateRange(0, 64)][int]$Jobs = 0,  # 0 = chosen from this PC's processors and free memory
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

$japanese = Test-JapaneseDiscFolder $JapaneseDiscPath $disc.Root
if (-not $japanese.Ok) { Fail $japanese.Message }
Write-Host "Japanese disc: $($japanese.Message)"
$voice = switch ($SpokenLanguage) { 'Japanese' { 0 } 'English' { 1 } default { if ($japanese.Given) { 0 } else { -1 } } }
if ($voice -eq 0 -and -not $japanese.Given) { Fail 'Japanese voices need -JapaneseDiscPath (the extracted Japanese disc).' }

$target = Test-InstallFolder $InstallPath $disc.Root ((Get-FolderSize (Join-Path $disc.Root 'files')) + $japanese.Bytes)
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
} $DesktopFolder $StartMenuFolder -japanese $japanese -voice $voice
if (-not $ok) { exit 1 }
