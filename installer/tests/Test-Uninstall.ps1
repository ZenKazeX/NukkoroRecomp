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
function Write-Manifest([string]$root, $files, $shortcuts = @(), $folders = @(), [string]$product = 'Shadow the Hedgehog') {
    $m = [ordered]@{ schema = 1; product = $product; files = @($files); shortcuts = @($shortcuts); shortcut_folders = @($folders) }
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
$menu = Join-Path $base 'menu\Shadow the Hedgehog'
$lnk = Join-Path $menu 'Shadow the Hedgehog.lnk'
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

# 1b. An install from before the rename (product and Start menu folder 'Shadow the Hedgehog Recompiled') still uninstalls.
$r = New-FakeInstall 'old-name'
$oldMenu = Join-Path $base 'menu-old\Shadow the Hedgehog Recompiled'
$oldLnk = Join-Path $oldMenu 'Shadow the Hedgehog Recompiled.lnk'
New-Link $oldLnk (Join-Path $r 'ShadowTheHedgehog.exe')
Write-Manifest $r (Files $r) @($oldLnk) @($oldMenu) 'Shadow the Hedgehog Recompiled'
$rc = Run-Uninstall $r
Check ($rc -eq 0) 'old name: exit 0'
Check (-not (Test-Path (Join-Path $r 'ShadowTheHedgehog.exe'))) 'old name: the game is gone'
Check (-not (Test-Path $oldMenu)) 'old name: its Start menu folder is gone'

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

# 8. -Plan: changes nothing, and says what goes and what stays.
$r = New-FakeInstall 'plan'
Put (Join-Path $r 'voice_jp\a.adx') 'voice'
Put (Join-Path $r 'mods\cream\mod.json') '{}'
Write-Manifest $r (@(Files $r) + @(Entry $r 'voice_jp\a.adx'))
$plan = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'Uninstall.ps1') -Plan | Select-Object -Last 1 | ConvertFrom-Json
Check ($plan.ok -and $plan.game.files -eq 3 -and $plan.voices.files -eq 1) 'plan: the game''s files and the voices counted'
$kinds = @{}; foreach ($d in $plan.data) { $kinds[$d.key] = $d }
Check ($kinds.saves.present -and $kinds.settings.present -and $kinds.mods.present -and -not $kinds.snapshots.present) 'plan: which data is there'
Check ($kinds.saves.bytes -eq 7) 'plan: the size of the saves'
Check ($plan.other.files -eq 2) 'plan: the player''s own files counted (they stay)'
Check (Test-Path (Join-Path $r 'ShadowTheHedgehog.exe')) 'plan: nothing removed'

# 9. -RemoveData settings,mods -Json: only those go; progress is JSON lines ending with done.
$lines = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'Uninstall.ps1') -Yes -Json -RemoveData 'settings,mods')
$events = @($lines | ForEach-Object { $_ | ConvertFrom-Json })
Check ($LASTEXITCODE -eq 0) 'remove data: exit 0'
Check (@($events | Where-Object { $_.type -eq 'progress' }).Count -gt 2) 'json: progress events'
$done = @($events | Where-Object { $_.type -eq 'done' })
Check ($done.Count -eq 1 -and @($done[0].left) -contains 'Saves' -and @($done[0].left) -notcontains 'Settings') 'json: done says what data is left'
Check (Test-Path (Join-Path $r 'save-data\card.gci')) 'remove data: the saves stay'
Check (-not (Test-Path (Join-Path $r 'options.cfg')) -and -not (Test-Path (Join-Path $r 'mods'))) 'remove data: settings and mods gone'
Check (-not (Test-Path (Join-Path $r 'voice_jp'))) 'remove data: the Japanese voices gone with the game'
$r = New-FakeInstall 'unknown-kind'
Write-Manifest $r (Files $r)
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'Uninstall.ps1') -Yes -RemoveData 'everything' | Out-Null
Check ($LASTEXITCODE -eq 1 -and (Test-Path (Join-Path $r 'ShadowTheHedgehog.exe'))) 'remove data: an unknown kind is refused, nothing removed'

# 10. The install side: Copy-Uninstaller puts the window in, Register-InstalledApp lists the game for this user, and the
# uninstall takes both away (the entry only when it still names this folder).
Import-Module (Join-Path $PSScriptRoot '..\InstallCore.psm1') -Force
$r = New-FakeInstall 'window'
$copied = @(Copy-Uninstaller $r)
$setupProgram = Find-SetupProgram
if ($setupProgram) {
    Check (Test-Path (Join-Path $r 'Uninstall.exe')) 'install: Uninstall.exe copied in'
    Check (Test-Path (Join-Path $r 'uninstall\uninstall\index.html')) 'install: the uninstall pages copied in'
    Check (Test-Path (Join-Path $r 'uninstall\web\setup.css')) 'install: the setup''s styles copied in'
} else { Write-Host 'skip no NukkoroSetup.exe built: the window''s files are not copied' }
$entry = Register-InstalledApp $r
$key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$entry"
try {
    Check ($entry -match '^NukkoroShadowTheHedgehog_[0-9a-f]{8}$' -and (Test-Path -LiteralPath $key)) 'install: listed in Installed apps'
    $props = Get-ItemProperty -LiteralPath $key
    Check ($props.InstallLocation -eq $r -and $props.DisplayName -eq 'Shadow the Hedgehog') 'install: the entry names the game and its folder'
    Check ($props.UninstallString -like "*$(if ($setupProgram) { 'Uninstall.exe' } else { 'Uninstall.cmd' })*") 'install: Uninstall opens the right uninstaller'
    $list = @(Files $r) + @($copied | ForEach-Object { Entry $r $_ })
    $m = [ordered]@{ schema = 1; product = 'Shadow the Hedgehog'; files = @($list); shortcuts = @(); shortcut_folders = @(); apps_entry = $entry }
    [System.IO.File]::WriteAllText((Join-Path $r 'install-manifest.json'), ($m | ConvertTo-Json -Depth 4))
    $plan = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'Uninstall.ps1') -Plan | Select-Object -Last 1 | ConvertFrom-Json
    Check ($plan.apps_entry -eq $true) 'plan: sees its Installed apps entry'
    $rc = Run-Uninstall $r
    Check ($rc -eq 0) 'window install: exit 0'
    Check (-not (Test-Path -LiteralPath $key)) 'uninstall: the Installed apps entry is gone'
    Check (-not (Test-Path (Join-Path $r 'uninstall')) -and -not (Test-Path (Join-Path $r 'Uninstall.exe'))) 'uninstall: the window''s files are gone'
    Check (Test-Path (Join-Path $r 'save-data\card.gci')) 'uninstall: the saves stay'
} finally {
    if ($entry -match '^NukkoroShadowTheHedgehog_[0-9a-f]{8}$' -and (Test-Path -LiteralPath $key)) { Remove-Item -LiteralPath $key -Recurse -Force }
}

# 11. An entry another folder owns is never removed.
$r = New-FakeInstall 'foreign-entry'
$other = Join-Path $base 'elsewhere'
New-Item -ItemType Directory -Force -Path $other | Out-Null
$entry = Register-InstalledApp $other
$key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$entry"
Check ($entry -match '^NukkoroShadowTheHedgehog_[0-9a-f]{8}$') 'install: an empty folder is listed too'
try {
    $m = [ordered]@{ schema = 1; product = 'Shadow the Hedgehog'; files = @(Files $r); shortcuts = @(); shortcut_folders = @(); apps_entry = $entry }
    [System.IO.File]::WriteAllText((Join-Path $r 'install-manifest.json'), ($m | ConvertTo-Json -Depth 4))
    $rc = Run-Uninstall $r
    Check ($rc -eq 0 -and (Test-Path -LiteralPath $key)) 'uninstall: another folder''s Installed apps entry stays'
} finally {
    if ($entry -match '^NukkoroShadowTheHedgehog_[0-9a-f]{8}$' -and (Test-Path -LiteralPath $key)) { Remove-Item -LiteralPath $key -Recurse -Force }
}
if ($script:failures) { Write-Host "$script:failures checks failed" -ForegroundColor Red; exit 1 }
Write-Host 'all uninstaller checks passed' -ForegroundColor Green
exit 0
