# Installer logic shared by the wizard (InstallWizard.ps1) and the scripted mode (Install.ps1).
# Nothing here touches the user interface; long steps run as child processes that write to a log file,
# so the caller decides how to wait (a WinForms timer, or a console loop).

Set-StrictMode -Version 2.0

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:Shards = 48
$script:GameExe = 'ShadowTheHedgehog.exe'

function Get-RepoRoot { $script:RepoRoot }

function Get-BuiltExe { Join-Path $script:RepoRoot ".build\release\bin\Release\$script:GameExe" }

function Get-DefaultInstallFolder { Join-Path $env:USERPROFILE 'Games\Shadow the Hedgehog Recompiled' }

function Format-Size([double]$bytes) {
    if ($bytes -ge 1GB) { return '{0:N1} GB' -f ($bytes / 1GB) }
    return '{0:N0} MB' -f ($bytes / 1MB)
}

# ---- prerequisites -----------------------------------------------------------

function Find-Python3 {
    foreach ($candidate in @(@('py', '-3'), @('python'), @('python3'))) {
        $command = Get-Command $candidate[0] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $command) { continue }
        # The real interpreter path (py.exe is only a launcher), for CMake's FindPython3.
        $arguments = @($candidate | Select-Object -Skip 1) + @('-c', 'import sys; print(sys.version_info[0], sys.executable)')
        try {
            $answer = & $command.Source @arguments 2>$null | Select-Object -First 1
            if ($answer -match '^3 (.+)$') { return $Matches[1].Trim() }
        } catch {}
    }
    return $null
}

function Test-Prerequisites {
    $results = @()

    $cmake = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $cmakeOk = $false; $cmakeDetail = 'not found on PATH'
    if ($cmake) {
        $line = (& $cmake.Source --version 2>$null | Select-Object -First 1)
        if ($line -match '(\d+)\.(\d+)') {
            $cmakeOk = ([int]$Matches[1] -gt 3) -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -ge 25)
            $cmakeDetail = if ($cmakeOk) { $line } else { "$line (3.25 or newer needed)" }
        }
    }
    $results += [pscustomobject]@{ Name = 'CMake 3.25+'; Ok = $cmakeOk; Detail = $cmakeDetail; Url = 'https://cmake.org/download/'; Value = $(if ($cmake) { $cmake.Source }) }

    $python = Find-Python3
    $results += [pscustomobject]@{ Name = 'Python 3'; Ok = [bool]$python; Detail = $(if ($python) { $python } else { 'not found on PATH' }); Url = 'https://www.python.org/downloads/'; Value = $python }

    $vsPath = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsPath = & $vswhere -version '[18.0,19.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
    }
    $results += [pscustomobject]@{ Name = 'Visual Studio 2026 C++ tools'; Ok = [bool]$vsPath; Detail = $(if ($vsPath) { $vsPath } else { "'Desktop development with C++' workload not found" }); Url = 'https://visualstudio.microsoft.com/downloads/'; Value = $vsPath }

    return $results
}

# ---- the disc folder ---------------------------------------------------------

# Accepts the extraction root, or a folder inside it the user may pick by mistake (sys, files, DATA).
function Resolve-DiscFolder([string]$path) {
    if (-not $path) { return $null }
    $path = $path.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { return $path }
    $full = (Resolve-Path -LiteralPath $path).ProviderPath.TrimEnd('\')
    foreach ($candidate in @($full, (Join-Path $full 'DATA'), (Split-Path -Parent $full))) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'sys\main.dol') -PathType Leaf)) { return $candidate }
    }
    return $full
}

function New-Result([bool]$ok, [string]$message) {
    [pscustomobject]@{ Ok = $ok; Message = $message; GameId = $null; Sha1 = $null; Root = $null }
}

function Test-DiscFolder([string]$path) {
    $root = Resolve-DiscFolder $path
    if (-not $root) { return New-Result $false 'Choose the folder of the extracted disc.' }
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return New-Result $false "Folder not found: $root" }

    $dol = Join-Path $root 'sys\main.dol'
    $boot = Join-Path $root 'sys\boot.bin'
    if (-not (Test-Path -LiteralPath $dol -PathType Leaf) -or -not (Test-Path -LiteralPath $boot -PathType Leaf)) {
        return New-Result $false "No sys\main.dol and sys\boot.bin here. Choose the folder Dolphin's 'Extract Entire Disc' wrote (it holds sys and files)."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'files') -PathType Container)) {
        return New-Result $false 'The files folder is missing: extract the entire disc, not only the system data.'
    }

    $bootBytes = [System.IO.File]::ReadAllBytes($boot)
    if ($bootBytes.Length -lt 8) { return New-Result $false 'sys\boot.bin is too short.' }
    $gameId = [System.Text.Encoding]::ASCII.GetString($bootBytes, 0, 6)

    $catalog = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'extract\discs.json') -Raw | ConvertFrom-Json
    $sha1 = (Get-FileHash -LiteralPath $dol -Algorithm SHA1).Hash.ToLowerInvariant()
    $known = $catalog.discs | Where-Object { $_.main_dol.sha1 -eq $sha1 } | Select-Object -First 1

    $result = New-Result $false ''
    $result.GameId = $gameId; $result.Sha1 = $sha1; $result.Root = $root
    if ($null -eq $known) {
        $result.Message = "Unknown or modified main.dol (game ID '$gameId'). Use an unmodified dump of the PAL disc."
    } elseif ($known.game_id -ne 'GUPP8P') {
        $result.Message = "This is the $($known.region) release ($($known.game_id)). Only the PAL release (GUPP8P) is supported."
    } elseif ($gameId -ne 'GUPP8P') {
        $result.Message = "main.dol is the PAL one, but sys\boot.bin says '$gameId': the sys folder is mixed from different dumps."
    } elseif ($bootBytes[7] -ne 0) {
        $result.Message = "sys\boot.bin reports disc revision $($bootBytes[7]); only revision 0 is known."
    } else {
        $result.Ok = $true
        $result.Message = 'Shadow the Hedgehog, PAL (GUPP8P), verified.'
    }
    return $result
}

function Get-FolderSize([string]$path) {
    $sum = (Get-ChildItem -LiteralPath $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if ($null -eq $sum) { return 0 }
    return [double]$sum
}

# ---- the install folder ------------------------------------------------------

function Test-InstallFolder([string]$path, [string]$discRoot, [double]$assetBytes) {
    $r = [pscustomobject]@{ Ok = $false; Message = ''; Path = $null; Existing = $false; NeededBytes = 0.0; FreeBytes = 0.0 }
    if (-not $path) { $r.Message = 'Choose where to install the game.'; return $r }
    $path = $path.Trim().Trim('"')
    try { $full = [System.IO.Path]::GetFullPath($path).TrimEnd('\') } catch { $r.Message = "Not a valid folder path: $path"; return $r }
    $r.Path = $full
    if ($full.Length -le 3) { $r.Message = 'Choose a folder, not the root of a drive.'; return $r }

    if ($discRoot) {
        $disc = [System.IO.Path]::GetFullPath($discRoot).TrimEnd('\')
        if ($full -ieq $disc -or $full.StartsWith($disc + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            $r.Message = 'Install somewhere else than the disc folder; the installer copies from it.'; return $r
        }
    }
    $repo = $script:RepoRoot.TrimEnd('\')
    if ($full -ieq $repo) { $r.Message = 'Install somewhere else than the source folder.'; return $r }

    $r.Existing = Test-Path -LiteralPath (Join-Path $full $script:GameExe) -PathType Leaf
    $r.NeededBytes = $assetBytes + 150MB
    if ($r.Existing -and (Test-Path -LiteralPath (Join-Path $full 'files'))) { $r.NeededBytes = 150MB }

    $qualifier = [System.IO.Path]::GetPathRoot($full)
    try {
        $drive = New-Object System.IO.DriveInfo $qualifier
        if (-not $drive.IsReady) { $r.Message = "Drive $qualifier is not ready."; return $r }
        $r.FreeBytes = [double]$drive.AvailableFreeSpace
    } catch { $r.Message = "Cannot read drive $qualifier."; return $r }
    if ($r.FreeBytes -lt $r.NeededBytes) {
        $r.Message = "Not enough space: needs $(Format-Size $r.NeededBytes), $(Format-Size $r.FreeBytes) free."; return $r
    }

    $r.Ok = $true
    $r.Message = "Needs $(Format-Size $r.NeededBytes), $(Format-Size $r.FreeBytes) free."
    if ($r.Existing) { $r.Message += ' An existing install will be updated; saves and settings are kept.' }
    return $r
}

# ---- steps run as child processes ---------------------------------------------

# Each step: Name, File, Arguments, WorkingDirectory, MaxExit (highest exit code that means success).
function Get-BuildSteps([string]$discRoot, [string]$python, [int]$jobs = 2) {
    $dol = (Join-Path $discRoot 'sys\main.dol') -replace '\\', '/'
    @(
        [pscustomobject]@{ Name = 'Configure'; File = 'cmake'; WorkingDirectory = $script:RepoRoot; MaxExit = 0
            Arguments = "--preset release `"-DSHADOW_RECOMP_DOL=$dol`" -DSHADOW_RECOMP_COMPILE_JOBS=$jobs `"-DPython3_EXECUTABLE=$python`"" },
        [pscustomobject]@{ Name = 'Build'; File = 'cmake'; WorkingDirectory = $script:RepoRoot; MaxExit = 0
            Arguments = "--build --preset release --parallel $jobs -- /nodeReuse:false" }
    )
}

function Get-InstallSteps([string]$discRoot, [string]$target) {
    $binDir = Split-Path -Parent (Get-BuiltExe)
    @(
        # robocopy copies bytes as they are; exit codes below 8 are success.
        [pscustomobject]@{ Name = 'Copy game'; File = 'robocopy'; WorkingDirectory = $script:RepoRoot; MaxExit = 7
            Arguments = "`"$binDir`" `"$target`" $script:GameExe /COPY:DAT /R:2 /W:1 /NP /NJH" },
        [pscustomobject]@{ Name = 'Copy game files'; File = 'robocopy'; WorkingDirectory = $script:RepoRoot; MaxExit = 7
            Arguments = "`"$(Join-Path $discRoot 'files')`" `"$(Join-Path $target 'files')`" /E /COPY:DAT /R:2 /W:1 /NP /NJH /NDL /NFL" }
    )
}

# Starts a step with its output appended to $log. Returns the Process.
function Start-Step($step, [string]$log) {
    Add-Content -LiteralPath $log -Value "`r`n==== $($step.Name): $($step.File) $($step.Arguments)" -Encoding UTF8
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    $info.Arguments = "/d /s /c `"`"$($step.File)`" $($step.Arguments) >> `"$log`" 2>&1`""
    $info.WorkingDirectory = $step.WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    return [System.Diagnostics.Process]::Start($info)
}

function Test-StepSucceeded($step, [System.Diagnostics.Process]$process) {
    return $process.HasExited -and $process.ExitCode -le $step.MaxExit
}

function Stop-Step([System.Diagnostics.Process]$process) {
    if ($process -and -not $process.HasExited) {
        & taskkill.exe /T /F /PID $process.Id 2>$null | Out-Null
    }
}

# Reads what the log gained since $offset; returns @{ Text; Offset }.
function Read-LogTail([string]$log, [long]$offset) {
    if (-not (Test-Path -LiteralPath $log)) { return @{ Text = ''; Offset = $offset } }
    $stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        if ($stream.Length -le $offset) { return @{ Text = ''; Offset = $offset } }
        $stream.Seek($offset, 'Begin') | Out-Null
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::Default)
        $text = $reader.ReadToEnd()
        return @{ Text = $text; Offset = $stream.Length }
    } finally { $stream.Dispose() }
}

# Build progress 0..1 from the highest recompiled shard the compiler has reported.
function Get-BuildFraction([string]$text, [double]$previous) {
    $best = $previous
    foreach ($m in [regex]::Matches($text, 'recomp_(\d{3})\.cpp')) {
        $f = ([int]$m.Groups[1].Value + 1) / $script:Shards * 0.9
        if ($f -gt $best) { $best = $f }
    }
    if ($text -match 'ShadowTheHedgehog\.vcxproj -> ') { $best = 1.0 }
    return [Math]::Min($best, 1.0)
}

# ---- finishing touches --------------------------------------------------------

function Write-InstallInfo([string]$target, $disc) {
    $info = [ordered]@{
        installed = (Get-Date).ToString('s')
        disc_game_id = $disc.GameId
        disc_main_dol_sha1 = $disc.Sha1
        source = $script:RepoRoot
        exe_sha256 = (Get-FileHash -LiteralPath (Join-Path $target $script:GameExe) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $target 'install-info.json') -Encoding UTF8
}

function New-GameShortcuts([string]$target, [bool]$desktop, [bool]$startMenu,
                           [string]$desktopDir = [Environment]::GetFolderPath('Desktop'),
                           [string]$startMenuDir = (Join-Path ([Environment]::GetFolderPath('Programs')) 'Shadow the Hedgehog Recompiled')) {
    $shell = New-Object -ComObject WScript.Shell
    $made = @()
    $places = @()
    if ($desktop) { $places += $desktopDir }
    if ($startMenu) { $places += $startMenuDir }
    foreach ($dir in $places) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $link = $shell.CreateShortcut((Join-Path $dir 'Shadow the Hedgehog Recompiled.lnk'))
        $link.TargetPath = Join-Path $target $script:GameExe
        $link.WorkingDirectory = $target
        $link.IconLocation = (Join-Path $target $script:GameExe) + ',0'
        $link.Description = 'Shadow the Hedgehog Recompiled (static recompilation, work in progress)'
        $link.Save()
        $made += $link.FullName
    }
    return $made
}

# ---- the whole install ----------------------------------------------------------

$script:StageNames = @('Configure the build', 'Translate and compile the game', 'Copy the game', 'Copy the game files', 'Finish and create shortcuts')

# Runs every step and reports through $OnEvent with hashtables:
#   @{type='stage'; index; name; state='run'|'ok'|'fail'}, @{type='progress'; value=0..1},
#   @{type='log'; text}, @{type='done'; target; exe}, @{type='error'; message; log}.
# Overall progress: configure 0-3%, compile 3-85%, copy 85-99%, finish 100%.
function Invoke-GameInstall($disc, [string]$targetPath, [string]$python, [int]$jobs, [bool]$desktop, [bool]$startMenu,
                            [scriptblock]$OnEvent) {
    $log = Join-Path $script:RepoRoot '.build\install.log'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $log) | Out-Null
    Set-Content -LiteralPath $log -Value "Install started $(Get-Date -Format s)" -Encoding UTF8
    $fail = {
        param([int]$index, [string]$message)
        if ($index -ge 0) { & $OnEvent @{ type = 'stage'; index = $index; name = $script:StageNames[$index]; state = 'fail' } }
        & $OnEvent @{ type = 'error'; message = $message; log = $log }
    }
    try { New-Item -ItemType Directory -Force -Path $targetPath | Out-Null }
    catch { & $fail -1 "Cannot create $($targetPath): $($_.Exception.Message)"; return $false }

    $assetBytes = Get-FolderSize (Join-Path $disc.Root 'files')
    $steps = @(Get-BuildSteps $disc.Root $python $jobs) + @(Get-InstallSteps $disc.Root $targetPath)
    $buildFraction = 0.0
    for ($i = 0; $i -lt $steps.Count; $i++) {
        $step = $steps[$i]
        & $OnEvent @{ type = 'stage'; index = $i; name = $script:StageNames[$i]; state = 'run' }
        $offset = (Get-Item -LiteralPath $log).Length
        $process = Start-Step $step $log
        $ticks = 0
        while ($true) {
            $exited = $process.WaitForExit(500)
            $tail = Read-LogTail $log $offset
            $offset = $tail.Offset
            if ($tail.Text) { & $OnEvent @{ type = 'log'; text = $tail.Text } }
            $ticks++
            switch ($i) {
                0 { & $OnEvent @{ type = 'progress'; value = 0.02 } }
                1 { $buildFraction = Get-BuildFraction $tail.Text $buildFraction
                    & $OnEvent @{ type = 'progress'; value = 0.03 + 0.82 * $buildFraction } }
                2 { & $OnEvent @{ type = 'progress'; value = 0.85 } }
                3 { # robocopy prints no running total; measure the copy every few seconds.
                    if (($ticks % 6 -eq 0 -or $exited) -and $assetBytes -gt 0) {
                        $copied = Get-FolderSize (Join-Path $targetPath 'files')
                        & $OnEvent @{ type = 'progress'; value = 0.86 + 0.13 * [Math]::Min(1, $copied / $assetBytes) }
                    } }
            }
            if ($exited) { break }
        }
        if (-not (Test-StepSucceeded $step $process)) {
            & $fail $i "$($script:StageNames[$i]) failed (exit code $($process.ExitCode))."
            return $false
        }
        & $OnEvent @{ type = 'stage'; index = $i; name = $script:StageNames[$i]; state = 'ok' }
    }

    $last = $steps.Count
    & $OnEvent @{ type = 'stage'; index = $last; name = $script:StageNames[$last]; state = 'run' }
    try {
        Write-InstallInfo $targetPath $disc
        foreach ($made in (New-GameShortcuts $targetPath $desktop $startMenu)) { & $OnEvent @{ type = 'log'; text = "Shortcut: $made`n" } }
        Copy-Item -LiteralPath $log -Destination (Join-Path $targetPath 'install.log') -Force
    } catch {
        & $fail $last "Finishing failed: $($_.Exception.Message)"
        return $false
    }
    & $OnEvent @{ type = 'stage'; index = $last; name = $script:StageNames[$last]; state = 'ok' }
    & $OnEvent @{ type = 'progress'; value = 1.0 }
    & $OnEvent @{ type = 'done'; target = $targetPath; exe = (Join-Path $targetPath $script:GameExe) }
    return $true
}

Export-ModuleMember -Function Get-RepoRoot, Get-BuiltExe, Get-DefaultInstallFolder, Format-Size, Test-Prerequisites,
    Resolve-DiscFolder, Test-DiscFolder, Get-FolderSize, Test-InstallFolder, Get-BuildSteps, Get-InstallSteps,
    Start-Step, Test-StepSucceeded, Stop-Step, Read-LogTail, Get-BuildFraction, Write-InstallInfo, New-GameShortcuts,
    Invoke-GameInstall
