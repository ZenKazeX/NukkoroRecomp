<#
.SYNOPSIS
    Removes an installed copy of Shadow the Hedgehog (the PC recompilation).

.DESCRIPTION
    Removes only what the installer recorded in install-manifest.json: the game, the copied disc files, the Japanese
    voices, the uninstaller, the shortcuts it made, its entry in Windows' Installed apps and its own bookkeeping files.
    Anything else in the folder is left alone. A recorded file that has changed since it was installed is kept and
    reported.

    The player's data is kept unless named: -RemoveData saves,settings,mods,snapshots (any of them), or -RemoveSaves
    for all four. saves: save-data. settings: options.cfg, display.cfg, pad.cfg, controls.cfg, appearance.cfg.
    mods: the mods folder. snapshots: the snapshots folder.

    The folder is removed only if nothing is left in it.

    -Plan changes nothing: it prints, as one line of JSON, what an uninstall would remove and keep (the uninstall
    window shows it). -Json prints the progress as lines of JSON instead of text (the uninstall window reads them):
    {type:'stage'|'progress'|'log'|'done'|'error', ...}.

.EXAMPLE
    .\Uninstall.ps1
    .\Uninstall.ps1 -Yes -RemoveSaves
    .\Uninstall.ps1 -Yes -RemoveData settings,mods
#>
param(
    [string]$InstallPath = $PSScriptRoot,
    [switch]$RemoveSaves,
    [string[]]$RemoveData = @(),
    [switch]$Yes,
    [switch]$Plan,
    [switch]$Json,
    [switch]$Launcher  # Uninstall.cmd removes itself and the folder after this script returns
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
if ($Plan -or $Json) { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false }

$GameExe = 'ShadowTheHedgehog.exe'
$ManifestName = 'install-manifest.json'
$Products = @('Shadow the Hedgehog', 'Shadow the Hedgehog Recompiled')  # installs from before the rename used the second
# The player's own data, never removed unless named. Each kind: its folders and its files in the install folder.
$DataKinds = [ordered]@{
    saves     = @{ name = 'Saves'; folders = @('save-data'); files = @() }
    settings  = @{ name = 'Settings'; folders = @(); files = @('options.cfg', 'display.cfg', 'pad.cfg', 'controls.cfg', 'appearance.cfg') }
    mods      = @{ name = 'Mods'; folders = @('mods'); files = @() }
    snapshots = @{ name = 'Snapshots'; folders = @('snapshots'); files = @() }
}
# What the game writes for itself and rewrites on every run: not the player's data, so it goes with the game.
$GeneratedFiles = @('session-report.json')
# The uninstaller's own files: removed last (Uninstall.exe runs from a copy in Temp, so it is not in use).
$UninstallerFiles = @('Uninstall.ps1', 'Uninstall.cmd', 'Uninstall.exe')
$AppsKeyRoot = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'

function Send-Event([hashtable]$event) { [Console]::Out.WriteLine(($event | ConvertTo-Json -Compress -Depth 5)) }
function Say([string]$text, [string]$color = '') {
    if ($Json) { Send-Event @{ type = 'log'; text = "$text`n" } }
    elseif ($color) { Write-Host $text -ForegroundColor $color }
    else { Write-Host $text }
}
function Stop-Uninstall([string]$message) {
    if ($Plan) { Send-Event @{ ok = $false; message = $message } }
    elseif ($Json) { Send-Event @{ type = 'error'; message = $message } }
    else { Write-Host "ERROR: $message" -ForegroundColor Red }
    exit 1
}

$wanted = @($RemoveData | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
if ($RemoveSaves) { $wanted = @($DataKinds.Keys) }
foreach ($kind in $wanted) { if (-not $DataKinds.Contains($kind)) { Stop-Uninstall "Unknown kind of data '$kind' (saves, settings, mods or snapshots). Nothing was changed." } }

$root = [System.IO.Path]::GetFullPath($InstallPath).TrimEnd('\')
$manifestPath = Join-Path $root $ManifestName
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Stop-Uninstall "$root has no $ManifestName, so it is not an install this program can safely remove. Nothing was changed."
}
try { $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json }
catch { Stop-Uninstall "$manifestPath cannot be read: $($_.Exception.Message). Nothing was changed." }
if ($manifest.schema -ne 1 -or $Products -notcontains $manifest.product) {
    Stop-Uninstall "$manifestPath is not this game's install record. Nothing was changed."
}

$exePath = Join-Path $root $GameExe
function Test-GameRunning {
    @(Get-CimInstance Win32_Process -Filter "Name='$GameExe'" | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $exePath }).Count -gt 0
}

# A path from the manifest, checked to be strictly inside the install folder; $null otherwise.
function Resolve-Inside([string]$relative) {
    if (-not $relative -or [System.IO.Path]::IsPathRooted($relative)) { return $null }
    $full = [System.IO.Path]::GetFullPath((Join-Path $root $relative))
    if ($full.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) { return $full }
    return $null
}
# The kind of player data a path inside the folder belongs to, or $null.
function Get-DataKind([string]$full) {
    $relative = $full.Substring($root.Length + 1)
    foreach ($kind in $DataKinds.Keys) {
        foreach ($name in $DataKinds[$kind].files) { if ($relative -ieq $name) { return $kind } }
        foreach ($name in $DataKinds[$kind].folders) {
            if ($relative -ieq $name -or $relative.StartsWith($name + '\', [System.StringComparison]::OrdinalIgnoreCase)) { return $kind }
        }
    }
    return $null
}
function Get-Size([string]$path) {
    if (Test-Path -LiteralPath $path -PathType Leaf) { return [long](Get-Item -LiteralPath $path -Force).Length }
    if (Test-Path -LiteralPath $path -PathType Container) {
        $sum = (Get-ChildItem -LiteralPath $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        if ($sum) { return [long]$sum }
    }
    return [long]0
}
# The Installed apps entry this install made (its key name is in the manifest), when it still names this folder.
function Get-AppsEntry {
    $name = if ($manifest.PSObject.Properties['apps_entry']) { [string]$manifest.apps_entry } else { '' }
    if (-not $name -or $name -notmatch '^[A-Za-z0-9._-]+$') { return $null }
    $key = Join-Path $AppsKeyRoot $name
    if (-not (Test-Path -LiteralPath $key)) { return $null }
    $location = [string](Get-ItemProperty -LiteralPath $key -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
    if ($location.TrimEnd('\') -ine $root) { return $null }
    return $key
}

# ---- the plan: what an uninstall removes and keeps, nothing changed -------------------------------------------------
if ($Plan) {
    $gameFiles = 0; $gameBytes = [long]0; $voiceFiles = 0; $voiceBytes = [long]0
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-Inside ([string]$entry.path)
        if ($null -eq $full -or $null -ne (Get-DataKind $full) -or -not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
        [void]$listed.Add($full)
        $bytes = [long](Get-Item -LiteralPath $full -Force).Length
        if (([string]$entry.path).StartsWith('voice_jp\', [System.StringComparison]::OrdinalIgnoreCase)) { $voiceFiles++; $voiceBytes += $bytes }
        else { $gameFiles++; $gameBytes += $bytes }
    }
    $data = @()
    foreach ($kind in $DataKinds.Keys) {
        $bytes = [long]0; $present = $false
        foreach ($name in @($DataKinds[$kind].folders) + @($DataKinds[$kind].files)) {
            $path = Join-Path $root $name
            if (Test-Path -LiteralPath $path) { $present = $true; $bytes += Get-Size $path }
        }
        $data += [ordered]@{ key = $kind; name = $DataKinds[$kind].name; present = $present; bytes = $bytes }
    }
    # Files nobody recorded (the player's own, or an older version's): always kept, so the folder stays with them.
    $otherFiles = 0; $otherBytes = [long]0
    $ignored = @($ManifestName, 'install-info.json', 'install.log') + $GeneratedFiles + $UninstallerFiles
    foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        if ($listed.Contains($file.FullName) -or $null -ne (Get-DataKind $file.FullName)) { continue }
        $relative = $file.FullName.Substring($root.Length + 1)
        if ($ignored -contains $relative) { continue }
        $otherFiles++; $otherBytes += $file.Length
    }
    $shortcuts = @(@($manifest.shortcuts) | Where-Object { $_ -and (Test-Path -LiteralPath ([string]$_) -PathType Leaf) } | ForEach-Object { [string]$_ })
    $installed = ''
    $info = Join-Path $root 'install-info.json'
    if (Test-Path -LiteralPath $info) { try { $installed = [string](Get-Content -LiteralPath $info -Raw -Encoding UTF8 | ConvertFrom-Json).installed } catch {} }
    Send-Event ([ordered]@{
        ok = $true; path = $root; installed = $installed; running = (Test-GameRunning)
        game = [ordered]@{ files = $gameFiles; bytes = $gameBytes }
        voices = [ordered]@{ files = $voiceFiles; bytes = $voiceBytes }
        shortcuts = @($shortcuts); apps_entry = ($null -ne (Get-AppsEntry))
        data = @($data); other = [ordered]@{ files = $otherFiles; bytes = $otherBytes }
    })
    exit 0
}

if (Test-GameRunning) { Stop-Uninstall 'The game is running from this folder. Close it and run the uninstaller again. Nothing was changed.' }

if (-not $Yes) {
    Write-Host "This removes the game from $root."
    if ($wanted.Count) { Write-Host "These WILL be deleted too: $(@($wanted | ForEach-Object { $DataKinds[$_].name }) -join ', ')." -ForegroundColor Yellow }
    else { Write-Host 'Your saves, settings and mods are kept.' }
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

# ---- the recorded files --------------------------------------------------------------------------------------------
if ($Json) { Send-Event @{ type = 'stage'; index = 0; name = 'Remove the game' } }
$entries = @($manifest.files)
$done = 0; $lastShown = -1
foreach ($entry in $entries) {
    $done++
    if ($Json) {
        $percent = [int][Math]::Floor(80 * $done / [Math]::Max(1, $entries.Count))
        if ($percent -ne $lastShown) { $lastShown = $percent; Send-Event @{ type = 'progress'; value = $percent / 100.0 } }
    }
    $full = Resolve-Inside ([string]$entry.path)
    if ($null -eq $full) { [void]$kept.Add("$($entry.path) (outside the install folder: refused)"); continue }
    if ($null -ne (Get-DataKind $full)) { [void]$kept.Add("$($entry.path) (player data)"); continue }

    $parent = Split-Path -Parent $full
    while ($parent.Length -gt $root.Length) { [void]$folders.Add($parent); $parent = Split-Path -Parent $parent }

    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
    $relative = $full.Substring($root.Length + 1)
    if ($UninstallerFiles -contains $relative) { continue }  # last, below
    $unchanged = (Get-Item -LiteralPath $full -Force).Length -eq [long]$entry.size
    if ($unchanged -and $entry.PSObject.Properties['sha256'] -and $entry.sha256) {
        $unchanged = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ieq $entry.sha256
    }
    if (-not $unchanged) { [void]$kept.Add("$($entry.path) (changed since it was installed)"); continue }
    Remove-One $full
}
if ($Json) { Send-Event @{ type = 'log'; text = "Removed $removed of the game's files.$(if ($kept.Count) { " Kept $($kept.Count): $($kept -join '; ')" })`n" } }

# ---- shortcuts and the Installed apps entry ------------------------------------------------------------------------
if ($Json) { Send-Event @{ type = 'stage'; index = 1; name = 'Remove the shortcuts' }; Send-Event @{ type = 'progress'; value = 0.82 } }
# Shortcuts live outside the folder: only .lnk files the manifest names, and only if they still point into this install.
$shell = New-Object -ComObject WScript.Shell
foreach ($link in @($manifest.shortcuts)) {
    if (-not $link) { continue }
    $path = [string]$link
    if ($path -notmatch '\.lnk$' -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $target = [string]$shell.CreateShortcut($path).TargetPath
    if ($target.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-One $path
        if ($Json) { Send-Event @{ type = 'log'; text = "Removed the shortcut $path`n" } }
    }
    else { [void]$kept.Add("$path (points somewhere else now)") }
}
foreach ($dir in @($manifest.shortcut_folders)) {
    if (-not $dir) { continue }
    $path = [string]$dir
    if ($Products -notcontains (Split-Path -Leaf $path)) { continue }
    if ((Test-Path -LiteralPath $path -PathType Container) -and -not (Get-ChildItem -LiteralPath $path -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $path -Force
    }
}
$appsKey = Get-AppsEntry
if ($appsKey) {
    try {
        Remove-Item -LiteralPath $appsKey -Recurse -Force
        Say 'Removed the game from Windows'' Installed apps.'
    } catch { [void]$failed.Add("$appsKey ($($_.Exception.Message))") }
}

# ---- the player's data, when named ---------------------------------------------------------------------------------
if ($wanted.Count) {
    if ($Json) { Send-Event @{ type = 'stage'; index = 2; name = 'Remove your data' }; Send-Event @{ type = 'progress'; value = 0.88 } }
    foreach ($kind in $wanted) {
        foreach ($name in $DataKinds[$kind].files) {
            $path = Join-Path $root $name
            if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-One $path }
        }
        foreach ($name in $DataKinds[$kind].folders) {
            $path = Join-Path $root $name
            if (Test-Path -LiteralPath $path -PathType Container) {
                try { Remove-Item -LiteralPath $path -Recurse -Force; $removed++ }
                catch { [void]$failed.Add("$path ($($_.Exception.Message))") }
            }
        }
        Say "Deleted your $($DataKinds[$kind].name.ToLowerInvariant())."
    }
}

# Folders the install's files were in, deepest first, only when empty.
if ($Json) { Send-Event @{ type = 'stage'; index = 3; name = 'Tidy up' }; Send-Event @{ type = 'progress'; value = 0.94 } }
foreach ($dir in @($folders | Sort-Object Length -Descending)) {
    if ((Test-Path -LiteralPath $dir -PathType Container) -and -not (Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)) {
        try { Remove-Item -LiteralPath $dir -Force } catch { [void]$failed.Add("$dir ($($_.Exception.Message))") }
    }
}

if ($failed.Count -gt 0) {
    $message = "These could not be removed: $($failed -join '; '). Close whatever is using them and run the uninstaller again. $ManifestName was kept so it can."
    if ($Json) { Send-Event @{ type = 'error'; message = $message } }
    else {
        Write-Host 'These could not be removed:' -ForegroundColor Red
        $failed | ForEach-Object { Write-Host "  $_" }
        Write-Host "Close whatever is using them and run the uninstaller again. $ManifestName was kept so it can."
    }
    exit 1
}

foreach ($name in $GeneratedFiles) {
    $path = Join-Path $root $name
    if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-One $path }
}
foreach ($name in @($ManifestName, 'install-info.json', 'install.log')) {
    $path = Join-Path $root $name
    if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
}
# The uninstall window's own files (its pages and art), when it put them there.
$pages = Join-Path $root 'uninstall'
if (Test-Path -LiteralPath (Join-Path $pages 'uninstall\index.html') -PathType Leaf) {
    Remove-Item -LiteralPath $pages -Recurse -Force -ErrorAction SilentlyContinue
}

$left = @(foreach ($kind in $DataKinds.Keys) {
    if (@(@($DataKinds[$kind].folders) + @($DataKinds[$kind].files) | Where-Object { Test-Path -LiteralPath (Join-Path $root $_) }).Count) { $DataKinds[$kind].name }
})
if (-not $Json) {
    Write-Host "Removed $removed items."
    foreach ($line in $kept) { Write-Host "Kept: $line" }
    if ($left.Count -gt 0) { Write-Host "Your data is still in ${root}: $($left -join ', ')" }
}

# The uninstaller last. PowerShell has already read this script, so deleting it here is safe; Uninstall.exe runs from
# a copy in Temp.
Remove-Item -LiteralPath (Join-Path $root 'Uninstall.ps1') -Force -ErrorAction SilentlyContinue
# Uninstall.exe may still be closing (it starts its copy in Temp and exits): a few tries. Its copy also removes it later.
$exe = Join-Path $root 'Uninstall.exe'
for ($try = 0; $try -lt 6 -and (Test-Path -LiteralPath $exe); $try++) {
    Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $exe) { Start-Sleep -Milliseconds 500 }
}
$folderRemoved = $false
if (-not $Launcher) {
    Remove-Item -LiteralPath (Join-Path $root 'Uninstall.cmd') -Force -ErrorAction SilentlyContinue
    if (-not (Get-ChildItem -LiteralPath $root -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
        $folderRemoved = -not (Test-Path -LiteralPath $root)
    }
}
if ($Json) {
    Send-Event @{ type = 'progress'; value = 1.0 }
    Send-Event ([ordered]@{ type = 'done'; removed = $removed; kept = @($kept); left = @($left); folder_removed = $folderRemoved; path = $root })
}
exit 0
