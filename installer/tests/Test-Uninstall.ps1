<#
.SYNOPSIS
    Tests installer/Uninstall.ps1 and Uninstall.cmd on fake installs under .build/installer-test. Touches nothing else.
    Exit 0 only when every check passes.
#>
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$base = Join-Path $repo '.build\installer-test'
if (Test-Path -LiteralPath $base) { Remove-Item -LiteralPath $base -Recurse -Force }
$script:failures = 0

function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok   $what" } else { Write-Host "FAIL $what" -ForegroundColor Red; $script:failures++ }
}
function Put([string]$path, [string]$text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllText($path, $text)
}
function Entry([string]$root, [string]$rel) {
    @{ path = $rel; size = (Get-Item -LiteralPath (Join-Path $root $rel)).Length }
}
function New-FakeInstall([string]$name) {
    $root = Join-Path $base $name
    Put (Join-Path $root 'ShadowTheHedgehog.exe') 'game'
    Put (Join-Path $root 'files\a\b.dat') 'bbb'
    Put (Join-Path $root 'files\c.dat') 'cc'
    Put (Join-Path $root 'install-info.json') '{}'
    Put (Join-Path $root 'install.log') 'log'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\Uninstall.ps1') -Destination $root
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\Uninstall.cmd') -Destination $root
    Put (Join-Path $root 'save-data\card.gci') 'my save'
    Put (Join-Path $root 'options.cfg') 'language=1'
    Put (Join-Path $root 'files\mine.txt') 'a file the player added'
    Put (Join-Path $root 'notes.txt') 'unrelated'
    Put (Join-Path $root 'session-report.json') '{}'
    $root
}
function Write-Manifest([string]$root, $files, $shortcuts = @(), $folders = @()) {
    $m = [ordered]@{ schema = 1; product = 'Shadow the Hedgehog Recompiled'; files = @($files); shortcuts = @($shortcuts); shortcut_folders = @($folders) }
    [System.IO.File]::WriteAllText((Join-Path $root 'install-manifest.json'), ($m | ConvertTo-Json -Depth 4))
}
function Files([string]$root) { @('ShadowTheHedgehog.exe', 'files\a\b.dat', 'files\c.dat' | ForEach-Object { Entry $root $_ }) }
function New-Link([string]$path, [string]$target) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    $l = (New-Object -ComObject WScript.Shell).CreateShortcut($path); $l.TargetPath = $target; $l.Save()
}
function Run-Uninstall([string]$root, [string[]]$more = @()) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'Uninstall.ps1') -Yes @more | Out-Null
    $LASTEXITCODE
}

# 1. Default: saves, settings, unrelated files stay; the game, files and bookkeeping go; shortcuts inside go.
$r = New-FakeInstall 'default'
$menu = Join-Path $base 'menu\Shadow the Hedgehog Recompiled'
$lnk = Join-Path $menu 'Shadow the Hedgehog Recompiled.lnk'
New-Link $lnk (Join-Path $r 'ShadowTheHedgehog.exe')
$foreign = Join-Path $base 'desktop\other.lnk'
New-Link $foreign 'C:\Windows\notepad.exe'
Write-Manifest $r (Files $r) @($lnk, $foreign) @($menu)
$rc = Run-Uninstall $r
Check ($rc -eq 0) 'default: exit 0'
Check (-not (Test-Path (Join-Path $r 'ShadowTheHedgehog.exe'))) 'default: the game is gone'
Check (-not (Test-Path (Join-Path $r 'files\a'))) 'default: an emptied folder is gone'
Check (Test-Path (Join-Path $r 'files\mine.txt')) 'default: a file the installer did not put there stays'
Check (Test-Path (Join-Path $r 'notes.txt')) 'default: an unrelated file stays'
Check ((Get-Content -Raw (Join-Path $r 'save-data\card.gci')) -eq 'my save') 'default: the save stays, unchanged'
Check (Test-Path (Join-Path $r 'options.cfg')) 'default: settings stay'
Check (-not (Test-Path (Join-Path $r 'session-report.json'))) 'default: the report the game writes for itself is gone'
Check (-not (Test-Path (Join-Path $r 'install-manifest.json'))) 'default: the manifest is gone'
Check (-not (Test-Path (Join-Path $r 'Uninstall.ps1'))) 'default: the uninstaller removed itself'
Check (-not (Test-Path $lnk)) 'default: its shortcut is gone'
Check (-not (Test-Path $menu)) 'default: its emptied Start menu folder is gone'
Check (Test-Path $foreign) 'default: a shortcut to something else stays'

# 2. -RemoveSaves: the saves go, unrelated files still stay.
$r = New-FakeInstall 'saves'
Write-Manifest $r (Files $r)
$rc = Run-Uninstall $r @('-RemoveSaves')
Check ($rc -eq 0) 'remove saves: exit 0'
Check (-not (Test-Path (Join-Path $r 'save-data'))) 'remove saves: saves gone'
Check (-not (Test-Path (Join-Path $r 'options.cfg'))) 'remove saves: settings gone'
Check (Test-Path (Join-Path $r 'notes.txt')) 'remove saves: an unrelated file stays'

# 3. A changed file is kept.
$r = New-FakeInstall 'changed'
$list = Files $r
Put (Join-Path $r 'files\c.dat') 'changed by the player'
Write-Manifest $r $list
$rc = Run-Uninstall $r
Check ($rc -eq 0) 'changed: exit 0'
Check (Test-Path (Join-Path $r 'files\c.dat')) 'changed: the changed file stays'
Check (-not (Test-Path (Join-Path $r 'files\a\b.dat'))) 'changed: an unchanged file is gone'

# 4. A hostile or damaged manifest cannot reach outside, or take player data.
$r = New-FakeInstall 'escape'
$outside = Join-Path $base 'outside.txt'
Put $outside 'must survive'
$absolute = Join-Path $base 'absolute.txt'
Put $absolute 'must survive'
$bad = @(Files $r) + @(@{ path = '..\outside.txt'; size = 12 }, @{ path = $absolute; size = 12 }, @{ path = 'save-data\card.gci'; size = 7 })
Write-Manifest $r $bad
$rc = Run-Uninstall $r
Check ($rc -eq 0) 'escape: exit 0'
Check (Test-Path $outside) 'escape: a relative path out of the folder is refused'
Check (Test-Path $absolute) 'escape: an absolute path is refused'
Check (Test-Path (Join-Path $r 'save-data\card.gci')) 'escape: a listed save is still kept'

# 5. No manifest, or someone else's: nothing changes.
$r = New-FakeInstall 'nomanifest'
$rc = Run-Uninstall $r
Check ($rc -eq 1 -and (Test-Path (Join-Path $r 'ShadowTheHedgehog.exe'))) 'no manifest: refused, nothing removed'
$r = New-FakeInstall 'foreign'
[System.IO.File]::WriteAllText((Join-Path $r 'install-manifest.json'), '{"schema":1,"product":"Other","files":[{"path":"notes.txt","size":9}]}')
$rc = Run-Uninstall $r
Check ($rc -eq 1 -and (Test-Path (Join-Path $r 'notes.txt'))) 'another product''s manifest: refused'

# 6. Uninstall.cmd, on a folder with no saves: the whole folder goes, including the launcher itself.
$r = New-FakeInstall 'cmd'
Remove-Item -LiteralPath (Join-Path $r 'save-data') -Recurse -Force
Remove-Item -LiteralPath (Join-Path $r 'options.cfg') -Force
Remove-Item -LiteralPath (Join-Path $r 'notes.txt') -Force
Remove-Item -LiteralPath (Join-Path $r 'files\mine.txt') -Force
Write-Manifest $r (Files $r)
& cmd.exe /c "`"$(Join-Path $r 'Uninstall.cmd')`" -Yes" | Out-Null
Start-Sleep -Milliseconds 300
Check (-not (Test-Path -LiteralPath $r)) 'Uninstall.cmd: an install with nothing left removes its folder'

# 7. Uninstall.cmd keeps the folder when saves remain.
$r = New-FakeInstall 'cmd-saves'
Write-Manifest $r (Files $r)
& cmd.exe /c "`"$(Join-Path $r 'Uninstall.cmd')`" -Yes" | Out-Null
Start-Sleep -Milliseconds 300
Check (Test-Path (Join-Path $r 'save-data\card.gci')) 'Uninstall.cmd: saves stay and the folder with them'
Check (-not (Test-Path (Join-Path $r 'Uninstall.cmd'))) 'Uninstall.cmd: removed itself'

if ($script:failures) { Write-Host "$script:failures checks failed" -ForegroundColor Red; exit 1 }
Write-Host 'all uninstaller checks passed' -ForegroundColor Green
exit 0
