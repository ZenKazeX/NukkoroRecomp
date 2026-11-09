<#
.SYNOPSIS
    Removes an installed copy of Shadow the Hedgehog Recompiled.

.DESCRIPTION
    Removes only what the installer recorded in install-manifest.json: the game, the copied disc files, the shortcuts
    it made, and its own bookkeeping files. Anything else in the folder is left alone. A recorded file that has
    changed since it was installed is kept and reported.

    Saves and settings (save-data, snapshots, options.cfg, display.cfg, pad.cfg) are kept unless -RemoveSaves is given.

    The folder is removed only if nothing is left in it.

.EXAMPLE
    .\Uninstall.ps1
    .\Uninstall.ps1 -Yes -RemoveSaves
#>
param(
    [string]$InstallPath = $PSScriptRoot,
    [switch]$RemoveSaves,
    [switch]$Yes,
    [switch]$Launcher  # Uninstall.cmd removes itself and the folder after this script returns
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$GameExe = 'ShadowTheHedgehog.exe'
$ManifestName = 'install-manifest.json'
# The player's own data: never removed unless -RemoveSaves names it.
$PlayerFolders = @('save-data', 'snapshots')
$PlayerFiles = @('options.cfg', 'display.cfg', 'pad.cfg')

function Stop-Uninstall([string]$message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

$root = [System.IO.Path]::GetFullPath($InstallPath).TrimEnd('\')
$manifestPath = Join-Path $root $ManifestName
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Stop-Uninstall "$root has no $ManifestName, so it is not an install this program can safely remove. Nothing was changed."
}
try { $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json }
catch { Stop-Uninstall "$manifestPath cannot be read: $($_.Exception.Message). Nothing was changed." }
if ($manifest.schema -ne 1 -or $manifest.product -ne 'Shadow the Hedgehog Recompiled') {
    Stop-Uninstall "$manifestPath is not this game's install record. Nothing was changed."
}

$exePath = Join-Path $root $GameExe
$running = @(Get-CimInstance Win32_Process -Filter "Name='$GameExe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $exePath })
if ($running.Count -gt 0) { Stop-Uninstall 'The game is running from this folder. Close it and run the uninstaller again. Nothing was changed.' }

# A path from the manifest, checked to be strictly inside the install folder; $null otherwise.
function Resolve-Inside([string]$relative) {
    if (-not $relative -or [System.IO.Path]::IsPathRooted($relative)) { return $null }
    $full = [System.IO.Path]::GetFullPath((Join-Path $root $relative))
    if ($full.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) { return $full }
    return $null
}

function Test-PlayerData([string]$full) {
    $relative = $full.Substring($root.Length + 1)
    foreach ($name in $PlayerFiles) { if ($relative -ieq $name) { return $true } }
    foreach ($name in $PlayerFolders) {
        if ($relative -ieq $name -or $relative.StartsWith($name + '\', [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

if (-not $Yes) {
    Write-Host "This removes the game from $root."
    if ($RemoveSaves) { Write-Host 'Your saves and settings WILL be deleted.' -ForegroundColor Yellow }
    else { Write-Host 'Your saves and settings are kept.' }
    $answer = Read-Host 'Continue? [y/N]'
    if ($answer -notmatch '^[yY]') { Write-Host 'Cancelled. Nothing was changed.'; exit 2 }
}

$removed = 0
$kept = New-Object System.Collections.ArrayList
$failed = New-Object System.Collections.ArrayList
$folders = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

function Remove-One([string]$full) {
    try {
        Remove-Item -LiteralPath $full -Force
        $script:removed++
    } catch { [void]$script:failed.Add("$full ($($_.Exception.Message))") }
}

foreach ($entry in @($manifest.files)) {
    $full = Resolve-Inside ([string]$entry.path)
    if ($null -eq $full) { [void]$kept.Add("$($entry.path) (outside the install folder: refused)"); continue }
    if (Test-PlayerData $full) { [void]$kept.Add("$($entry.path) (player data)"); continue }

    $parent = Split-Path -Parent $full
    while ($parent.Length -gt $root.Length) { [void]$folders.Add($parent); $parent = Split-Path -Parent $parent }

    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
    $unchanged = (Get-Item -LiteralPath $full -Force).Length -eq [long]$entry.size
    if ($unchanged -and $entry.PSObject.Properties['sha256'] -and $entry.sha256) {
        $unchanged = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ieq $entry.sha256
    }
    if (-not $unchanged) { [void]$kept.Add("$($entry.path) (changed since it was installed)"); continue }
    Remove-One $full
}

# Shortcuts live outside the folder: only .lnk files the manifest names, and only if they still point into this install.
$shell = New-Object -ComObject WScript.Shell
foreach ($link in @($manifest.shortcuts)) {
    if (-not $link) { continue }
    $path = [string]$link
    if ($path -notmatch '\.lnk$' -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $target = [string]$shell.CreateShortcut($path).TargetPath
    if ($target.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) { Remove-One $path }
    else { [void]$kept.Add("$path (points somewhere else now)") }
}
foreach ($dir in @($manifest.shortcut_folders)) {
    if (-not $dir) { continue }
    $path = [string]$dir
    if ((Split-Path -Leaf $path) -ne 'Shadow the Hedgehog Recompiled') { continue }
    if ((Test-Path -LiteralPath $path -PathType Container) -and -not (Get-ChildItem -LiteralPath $path -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $path -Force
    }
}

if ($RemoveSaves) {
    foreach ($name in $PlayerFiles) {
        $path = Join-Path $root $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-One $path }
    }
    foreach ($name in $PlayerFolders) {
        $path = Join-Path $root $name
        if (Test-Path -LiteralPath $path -PathType Container) {
            try { Remove-Item -LiteralPath $path -Recurse -Force; $removed++ }
            catch { [void]$failed.Add("$path ($($_.Exception.Message))") }
        }
    }
}

# Folders the install's files were in, deepest first, only when empty.
foreach ($dir in @($folders | Sort-Object Length -Descending)) {
    if ((Test-Path -LiteralPath $dir -PathType Container) -and -not (Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)) {
        try { Remove-Item -LiteralPath $dir -Force } catch { [void]$failed.Add("$dir ($($_.Exception.Message))") }
    }
}

if ($failed.Count -gt 0) {
    Write-Host 'These could not be removed:' -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $_" }
    Write-Host "Close whatever is using them and run the uninstaller again. $ManifestName was kept so it can."
    exit 1
}

foreach ($name in @($ManifestName, 'install-info.json', 'install.log')) {
    $path = Join-Path $root $name
    if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
}

Write-Host "Removed $removed items."
foreach ($line in $kept) { Write-Host "Kept: $line" }
$left = @($PlayerFolders + $PlayerFiles | Where-Object { Test-Path -LiteralPath (Join-Path $root $_) })
if ($left.Count -gt 0) { Write-Host "Your saves and settings are still in ${root}: $($left -join ', ')" }

# This script last. PowerShell has already read it, so deleting it here is safe.
Remove-Item -LiteralPath (Join-Path $root 'Uninstall.ps1') -Force -ErrorAction SilentlyContinue
if (-not $Launcher) {
    Remove-Item -LiteralPath (Join-Path $root 'Uninstall.cmd') -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem -LiteralPath $root -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue }
}
exit 0
