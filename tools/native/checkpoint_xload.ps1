<#
.SYNOPSIS
  Checkpoint cross-load check for two executables: a savestate written by either must load in the other and continue
  to the same state as it does in the executable that wrote it.

.DESCRIPTION
  For a change that touches savestate.cpp or anything a checkpoint records. Six headless runs of one tape
  (default Final Haunt), every one in its own directory under -Out with a private copy of the tape's card / SRAM / cfg:

    save-a, save-b     each executable plays the tape, writes a checkpoint at -SaveSecond and stops at -StopSecond
    load-XY (4 runs)   executable Y restores the checkpoint written by X and continues to -StopSecond

  Checks (the gate fails on any of them):
    every run exits 0 and reports no failed stop
    straight runs       state(save-a) == state(save-b)        the two executables agree without any checkpoint
    cross-load of A's   state(load-ab) == state(load-aa)      B continues A's checkpoint exactly as A does
    cross-load of B's   state(load-ba) == state(load-bb)
  -NewerFormat A|B: that executable writes a checkpoint format the other one is not expected to read (an intended
  format step). The older executable restoring the newer one's checkpoint is then reported, not required; the newer
  one must still read the older one's.
  State comparisons use the guest part of the state report (game tick, VI frames, memory block digests).
  Reported, not required (a difference is printed as "note"):
    the two checkpoint files are byte-identical
    the whole state report of a restored run equals the straight run's (it does not: the report also carries
    render_frames, a count since the process started)
  Also required:
    restore is transparent   the guest part of the state report (game tick, VI frames, every memory region's block
                             digests) of a restored run equals the straight run's, in each executable

  One coordinator reservation covers the six runs (-InsideReservation to skip). Exit codes as gate.ps1:
  0 all required checks hold; 1 a check failed; 2 cannot run; 3 reservation not granted.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\native\checkpoint_xload.ps1 -ExeA .build\nw\base.exe `
      -ExeB .build\nw\n1.exe -Out .build\xload\n1 -Session native-win
#>
param(
    [Parameter(Mandatory = $true)][string]$ExeA,
    [Parameter(Mandatory = $true)][string]$ExeB,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Stage = 'stg0604',
    [string]$TapeDir = 'source\.build\recordings\final-haunt',   # relative to the workspace
    [int]$SaveSecond = 40,
    [int]$StopSecond = 110,
    [string[]]$Extra = @(),
    [string]$Session = 'xload',
    [int]$WaitMinutes = 60,
    [int]$RunTimeoutSeconds = 300,       # a run that has not exited by then is stopped and fails its check
    [ValidateSet('none', 'A', 'B')][string]$NewerFormat = 'none',   # that executable writes a checkpoint format the other is not expected to read
    [switch]$InsideReservation,
    [string]$Workspace
)

$ErrorActionPreference = 'Stop'
# 'powershell -File' hands an array argument over as ONE comma-joined string: split it back (no argument of the
# runtime contains a comma).
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$TicksPerSecond = 40500000   # gekko::kGameTicksPerSecond (session.json: game_ticks / game_seconds)
$Tree = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Workspace) { $Workspace = Split-Path $Tree -Parent }
$Coord = Join-Path $Workspace '.coord\coord.ps1'
function Fail-Setup([string]$message) { Write-Host "[xload] cannot run: $message"; exit 2 }

foreach ($exe in $ExeA, $ExeB) { if (-not (Test-Path -LiteralPath $exe)) { Fail-Setup "executable not found: $exe" } }
$exes = @{ a = (Resolve-Path -LiteralPath $ExeA).Path; b = (Resolve-Path -LiteralPath $ExeB).Path }
$tapeDirFull = Join-Path $Workspace $TapeDir
$tape = Join-Path $tapeDirFull 'input.tape'
if (-not (Test-Path -LiteralPath $tape)) { Fail-Setup "tape not found: $tape" }
if ($SaveSecond -le 0 -or $StopSecond -le $SaveSecond) { Fail-Setup '-StopSecond must be after -SaveSecond, both positive' }
$outFull = [IO.Path]::GetFullPath($Out)
foreach ($protected in (Join-Path $Workspace 'game'), $tapeDirFull) {
    if ($outFull.StartsWith([IO.Path]::GetFullPath($protected), [StringComparison]::OrdinalIgnoreCase)) { Fail-Setup "-Out must not be inside $protected" }
}
$saveTick = [int64]$SaveSecond * $TicksPerSecond
$stopTick = [int64]$StopSecond * $TicksPerSecond

function Md5([string]$path) { if (Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path -Algorithm MD5).Hash.ToLower() } else { '(missing)' } }
# md5 over the guest-visible part of a state report: game tick, VI frames and every memory region's block digests.
# The whole-file md5 also covers render_frames, a count of frames handed to the renderer since the PROCESS started,
# which a restored run cannot share with a straight one.
function Guest-Digest([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return '(missing)' }
    try {
        $report = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        $text = New-Object System.Text.StringBuilder
        [void]$text.Append("$($report.game_ticks)|$($report.vi_frames)|$($report.boundary_tick)|")
        foreach ($region in $report.memory.regions) { [void]$text.Append("$($region.name):$($region.base):$($region.committed):$($region.blocks -join ',')|") }
        $md5 = [System.Security.Cryptography.MD5]::Create()
        (([BitConverter]::ToString($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToString())))) -replace '-', '').ToLower()
    } catch { '(unreadable)' }
}
function Copy-Persist([string]$directory) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Force $directory | Out-Null
    foreach ($file in 'card.raw', 'SRAM.pc.raw', 'display.cfg', 'pad.cfg') {
        if (Test-Path -LiteralPath (Join-Path $tapeDirFull $file)) { Copy-Item -LiteralPath (Join-Path $tapeDirFull $file) -Destination $directory }
        elseif (Test-Path -LiteralPath (Join-Path $tapeDirFull "initial\$file")) { Copy-Item -LiteralPath (Join-Path $tapeDirFull "initial\$file") -Destination $directory }
        else { throw "tape directory $tapeDirFull has no $file (nor initial\$file)" }
    }
}
$script:Reservation = $null
function Heartbeat { if ($script:Reservation) { try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord heartbeat -Id $script:Reservation | Out-Null } catch {} } }

# Runs one executable in $directory; returns @{ failed; reason; state; ticks }.
function Run-Game([string]$exe, [string]$directory, [string[]]$more, [bool]$coldBoot = $true) {
    # --boot-stage is a cold-boot option: the runtime refuses it together with --load-state (and then does not exit).
    $boot = if ($coldBoot) { @('--boot-stage', $Stage) } else { @() }
    $arguments = @('--game-data', ((Join-Path $Workspace 'game') -replace '\\', '/')) + $boot + @('--headless', '--no-audio',
        '--memory-card', 'card.raw', '--display-cfg', 'display.cfg', '--pad-cfg', 'pad.cfg', '--snapshot-key', 'none', '--no-xinput',
        '--budget-ms', "$(($StopSecond + 60) * 1000)", '--stop-at-tick', "$stopTick", '--input-tape-play', ($tape -replace '\\', '/'),
        '--native-output', '1280x720', '--width', '1280', '--height', '720', '--json-report', 'session.json',
        '--state-report', 'state.json') + $more + $Extra
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe; $info.WorkingDirectory = $directory; $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        $deadline = (Get-Date).AddSeconds($RunTimeoutSeconds); $timedOut = $false
        while (-not $process.WaitForExit(15000)) {
            Heartbeat
            if ((Get-Date) -ge $deadline) { $timedOut = $true; $process.Kill(); break }   # this script's own child
        }
        $process.WaitForExit()
        [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $stdout.Result)
        [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $stderr.Result)
    } finally { if (-not $process.HasExited) { try { $process.Kill() } catch {} } }
    $result = @{ failed = $false; reason = ''; state = (Md5 (Join-Path $directory 'state.json')); guest = (Guest-Digest (Join-Path $directory 'state.json')); ticks = ''; render_frames = '' }
    if ($timedOut) { $result.failed = $true; $result.reason = "no exit within $RunTimeoutSeconds s (stopped; see stderr.log)" }
    elseif ($process.ExitCode -ne 0) { $result.failed = $true; $result.reason = "exit code $($process.ExitCode)" }
    try {
        $session = Get-Content -LiteralPath (Join-Path $directory 'session.json') -Raw | ConvertFrom-Json
        $result.ticks = "$($session.guest.game_ticks)"
        try { $result.render_frames = "$((Get-Content -LiteralPath (Join-Path $directory 'state.json') -Raw | ConvertFrom-Json).render_frames)" } catch {}
        if ($session.stop.failed) { $result.failed = $true; $result.reason = ($result.reason + " stop.failed (reason $($session.stop.reason))").Trim() }
    } catch { $result.failed = $true; $result.reason = ($result.reason + ' no readable session.json').Trim() }
    Heartbeat
    $result
}

$script:Checks = New-Object System.Collections.ArrayList
function Check([string]$name, [bool]$ok, [string]$detail, [bool]$required = $true) {
    [void]$script:Checks.Add([ordered]@{ check = $name; pass = $ok; required = $required; detail = $detail })
    $mark = if ($ok) { 'ok  ' } elseif ($required) { 'FAIL' } else { 'note' }
    Write-Host ("[xload] {0,-44} {1}  {2}" -f $name, $mark, $detail)
}

Write-Host "[xload] A $($exes.a)`n[xload] B $($exes.b)`n[xload] $Stage, tape $tape, checkpoint at $SaveSecond s (tick $saveTick), stop at $StopSecond s (tick $stopTick)"
if (-not $InsideReservation) {
    if (-not (Test-Path -LiteralPath $Coord)) { Fail-Setup "coordinator not found: $Coord (or pass -InsideReservation)" }
    $answer = & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord acquire -Session $Session -Kind gate -Threads 4 -MemGB 6 `
        -Task "checkpoint cross-load ($(Split-Path $exes.a -Leaf) / $(Split-Path $exes.b -Leaf)) -> $outFull" -EstMinutes 4 -OwnerPid $PID -WaitMinutes $WaitMinutes
    $code = $LASTEXITCODE
    $answer | ForEach-Object { Write-Host $_ }
    $id = @($answer)[-1]
    if ($code -ne 0) {
        if ($id) { try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord cancel -Id $id | Out-Null } catch {} }
        Write-Host "[xload] reservation not granted within $WaitMinutes min; nothing was started"
        exit 3
    }
    $script:Reservation = $id
}

$exitCode = 1
try {
    New-Item -ItemType Directory -Force $outFull | Out-Null
    $runs = @{}
    foreach ($x in 'a', 'b') {
        $directory = Join-Path $outFull "save-$x"; Copy-Persist $directory
        $runs["save-$x"] = Run-Game $exes[$x] $directory @('--state-file', 'unused.gekkostate', '--save-state-at-tick', "${saveTick}:checkpoint.gekkostate")
        $checkpoint = Join-Path $directory 'checkpoint.gekkostate'
        Check "save-$x run completed" (-not $runs["save-$x"].failed) $(if ($runs["save-$x"].failed) { $runs["save-$x"].reason } else { "state $($runs["save-$x"].state.Substring(0, 12)) at tick $($runs["save-$x"].ticks)" })
        Check "save-$x wrote its checkpoint" (Test-Path -LiteralPath $checkpoint) $(if (Test-Path -LiteralPath $checkpoint) { "$((Get-Item -LiteralPath $checkpoint).Length) bytes, md5 $((Md5 $checkpoint).Substring(0, 12))" } else { 'checkpoint.gekkostate missing' })
    }
    foreach ($x in 'a', 'b') {
        $checkpoint = Join-Path $outFull "save-$x\checkpoint.gekkostate"
        if (-not (Test-Path -LiteralPath $checkpoint)) { continue }
        foreach ($y in 'a', 'b') {
            $name = "load-$x$y"; $directory = Join-Path $outFull $name; Copy-Persist $directory
            Copy-Item -LiteralPath $checkpoint -Destination (Join-Path $directory 'checkpoint.gekkostate')
            $runs[$name] = Run-Game $exes[$y] $directory @('--state-file', 'checkpoint.gekkostate', '--load-state') $false
            # With -NewerFormat X the other executable may refuse X's checkpoint: that direction is reported, not required.
            $backward = ($NewerFormat -ne 'none' -and $x -eq $NewerFormat.ToLower() -and $y -ne $x)
            $detail = if ($runs[$name].failed) { $runs[$name].reason + $(if ($backward) { ' - expected: the older executable is not required to read the newer format' } else { '' }) } else { "state $($runs[$name].state.Substring(0, 12)) at tick $($runs[$name].ticks)" }
            Check "$name run completed ($($y.ToUpper()) restores $($x.ToUpper())'s)" (-not $runs[$name].failed) $detail (-not $backward)
        }
    }
    $state = { param($name) if ($runs.ContainsKey($name)) { $runs[$name].guest } else { '(not run)' } }   # guest part of the report
    $same = { param($one, $two) $s1 = & $state $one; $s2 = & $state $two; @(($s1 -eq $s2 -and $s1 -notlike '(*'), "$one $($s1.Substring(0, [Math]::Min(12, $s1.Length))) / $two $($s2.Substring(0, [Math]::Min(12, $s2.Length)))") }
    $r = & $same 'save-a' 'save-b'; Check 'straight runs agree' $r[0] $r[1]
    $r = & $same 'load-aa' 'load-ab'; Check "B continues A's checkpoint as A does" $r[0] $r[1] ($NewerFormat -ne 'A')
    $r = & $same 'load-bb' 'load-ba'; Check "A continues B's checkpoint as B does" $r[0] $r[1] ($NewerFormat -ne 'B')
    $md5a = Md5 (Join-Path $outFull 'save-a\checkpoint.gekkostate'); $md5b = Md5 (Join-Path $outFull 'save-b\checkpoint.gekkostate')
    Check 'checkpoint files byte-identical' ($md5a -eq $md5b -and $md5a -notlike '(*') "A $($md5a.Substring(0, 12)) / B $($md5b.Substring(0, 12))" $false
    foreach ($x in 'a', 'b') {
        $straight = $runs["save-$x"]; $restored = $runs["load-$x$x"]
        if ($straight -and $restored) {
            Check "restore is transparent in $($x.ToUpper()) (guest memory)" ($straight.guest -eq $restored.guest -and $straight.guest -notlike '(*') "straight $($straight.guest.Substring(0, 12)) / restored $($restored.guest.Substring(0, 12))"
            Check "whole state report equal in $($x.ToUpper())" ($straight.state -eq $restored.state) "straight $($straight.state.Substring(0, 12)) (render_frames $($straight.render_frames)) / restored $($restored.state.Substring(0, 12)) (render_frames $($restored.render_frames))" $false
        }
    }

    $failed = @($script:Checks | Where-Object { $_.required -and -not $_.pass })
    $summary = [ordered]@{ exe_a = $exes.a; exe_b = $exes.b; stage = $Stage; tape = $tape; save_tick = $saveTick; stop_tick = $stopTick
        extra = $Extra; finished = (Get-Date).ToUniversalTime().ToString('o'); pass = ($failed.Count -eq 0); checks = @($script:Checks) }
    [IO.File]::WriteAllText((Join-Path $outFull 'result.json'), ($summary | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    Get-ChildItem -LiteralPath $outFull -Recurse -Filter '*.gekkostate' | Remove-Item -Force -Confirm:$false   # large, reproducible
    if ($failed.Count -eq 0) { Write-Host "[xload] PASS ($outFull\result.json)"; $exitCode = 0 }
    else { Write-Host "[xload] FAIL: $($failed.Count) required checks failed ($outFull\result.json)"; $exitCode = 1 }
} finally {
    if ($script:Reservation) {
        try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord release -Id $script:Reservation | ForEach-Object { Write-Host $_ } }
        catch { Write-Host "[xload] could NOT release reservation $($script:Reservation): $_ - release it by hand" }
    }
}
exit $exitCode
