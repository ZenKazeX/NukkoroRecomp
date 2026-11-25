<#
.SYNOPSIS
  One headless tape run of an executable under one coordinator reservation: the gate's argument set plus whatever
  the measurement adds. For diagnostic and timing runs that are not a gate (watch runs, audio dumps, A/B timings).

.DESCRIPTION
  Copies the tape's card / SRAM / cfg files into -Out, runs the executable there with the same base arguments as
  gate.ps1 (headless, no audio device, tape playback, session.json and state.json) plus -Extra and -Env, writes
  stdout.log / stderr.log, and prints one summary line (exit code, game ticks, game-thread CPU seconds and cycles,
  state md5). -Repeat N runs it N times into <Out>\r1 .. rN and prints the spread.

  -Exclusive asks the coordinator for an exclusive reservation (nothing else heavy may run): use it for every run
  whose time is the result. A run that has not exited within -RunTimeoutSeconds is stopped and reported as failed.

  Exit codes: 0 every run exited 0 without a failed stop; 1 a run failed; 2 cannot run; 3 reservation not granted.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\native\run_tape.ps1 -Exe <exe> -Out .build\audio\watch `
      -Tape westopolis -Extra '--watch-guest','ranges.txt','--watch-report','watch.csv','--stop-at-tick','2430000000'
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateSet('westopolis', 'final-haunt')][string]$Tape = 'westopolis',
    [string[]]$Extra = @(),
    [string[]]$Env = @(),
    [int]$BudgetMs = 0,                  # 0 = the tape's reference budget
    [int]$Repeat = 1,
    [switch]$Exclusive,
    [string]$Session = 'run-tape',
    [string]$Task,
    [int]$EstMinutes = 3,
    [int]$WaitMinutes = 60,
    [int]$RunTimeoutSeconds = 900,
    [switch]$InsideReservation,
    [string]$Workspace
)

$ErrorActionPreference = 'Stop'
# 'powershell -File' hands an array argument over as ONE comma-joined string: split it back (no argument of the
# runtime contains a comma).
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$Tree = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Workspace) { $Workspace = Split-Path $Tree -Parent }
$Coord = Join-Path $Workspace '.coord\coord.ps1'
function Fail-Setup([string]$message) { Write-Host "[run-tape] cannot run: $message"; exit 2 }

if (-not (Test-Path -LiteralPath $Exe)) { Fail-Setup "executable not found: $Exe" }
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$ref = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'gate_references.json') -Raw | ConvertFrom-Json
$tapeRef = if ($Tape -eq 'westopolis') { $ref.westopolis } else { $ref.final_haunt }
$tapeDir = Join-Path $Workspace ($tapeRef.tape_dir -replace '/', '\')
$tapeFile = Join-Path $tapeDir 'input.tape'
if (-not (Test-Path -LiteralPath $tapeFile)) { Fail-Setup "tape not found: $tapeFile" }
if ($BudgetMs -le 0) { $BudgetMs = [int]$tapeRef.budget_ms }
foreach ($pair in $Env) { if ($pair -notmatch '^[A-Za-z_][A-Za-z0-9_]*=') { Fail-Setup "-Env wants NAME=value, got '$pair'" } }
$outFull = [IO.Path]::GetFullPath($Out)
foreach ($protected in (Join-Path $Workspace 'game'), $tapeDir) {
    if ($outFull.StartsWith([IO.Path]::GetFullPath($protected), [StringComparison]::OrdinalIgnoreCase)) { Fail-Setup "-Out must not be inside $protected" }
}

$script:Reservation = $null
function Heartbeat { if ($script:Reservation) { try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord heartbeat -Id $script:Reservation | Out-Null } catch {} } }

function Run-Once([string]$directory) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Force $directory | Out-Null
    foreach ($file in 'card.raw', 'SRAM.pc.raw', 'display.cfg', 'pad.cfg') {
        if (Test-Path -LiteralPath (Join-Path $tapeDir $file)) { Copy-Item -LiteralPath (Join-Path $tapeDir $file) -Destination $directory }
        elseif (Test-Path -LiteralPath (Join-Path $tapeDir "initial\$file")) { Copy-Item -LiteralPath (Join-Path $tapeDir "initial\$file") -Destination $directory }
        else { throw "tape directory $tapeDir has no $file (nor initial\$file)" }
    }
    $arguments = @('--game-data', ((Join-Path $Workspace 'game') -replace '\\', '/'), '--boot-stage', $tapeRef.stage, '--headless', '--no-audio',
        '--memory-card', 'card.raw', '--display-cfg', 'display.cfg', '--pad-cfg', 'pad.cfg', '--state-file', 'unused.gekkostate',
        '--snapshot-key', 'none', '--no-xinput', '--budget-ms', "$BudgetMs", '--input-tape-play', ($tapeFile -replace '\\', '/'),
        '--native-output', '1280x720', '--width', '1280', '--height', '720', '--json-report', 'session.json',
        '--state-report', 'state.json') + $Extra
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exePath; $info.WorkingDirectory = $directory; $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    foreach ($pair in $Env) { $name, $value = $pair -split '=', 2; $info.EnvironmentVariables[$name] = [string]$value }
    $process = [System.Diagnostics.Process]::Start($info)
    $timedOut = $false
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        $deadline = (Get-Date).AddSeconds($RunTimeoutSeconds)
        while (-not $process.WaitForExit(15000)) {
            Heartbeat
            if ((Get-Date) -ge $deadline) { $timedOut = $true; $process.Kill(); break }   # this script's own child
        }
        $process.WaitForExit()
        [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $stdout.Result)
        [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $stderr.Result)
    } finally { if (-not $process.HasExited) { try { $process.Kill() } catch {} } }
    $row = [ordered]@{ dir = $directory; exit = $process.ExitCode; failed = $false; reason = ''; ticks = 0; cpu_s = 0.0; cycles = 0.0; wall_s = 0.0; state = '(missing)' }
    if ($timedOut) { $row.failed = $true; $row.reason = "no exit within $RunTimeoutSeconds s" }
    elseif ($process.ExitCode -ne 0) { $row.failed = $true; $row.reason = "exit code $($process.ExitCode)" }
    try {
        $s = Get-Content -LiteralPath (Join-Path $directory 'session.json') -Raw | ConvertFrom-Json
        $row.ticks = $s.guest.game_ticks; $row.cpu_s = [double]$s.host.game_thread_cpu_s; $row.cycles = [double]$s.host.game_thread_cycles
        $row.wall_s = [double]$s.host.wall_seconds
        if ($s.stop.failed) { $row.failed = $true; $row.reason = ($row.reason + " stop.failed (reason $($s.stop.reason))").Trim() }
    } catch { $row.failed = $true; $row.reason = ($row.reason + ' no readable session.json').Trim() }
    $statePath = Join-Path $directory 'state.json'
    if (Test-Path -LiteralPath $statePath) { $row.state = (Get-FileHash -LiteralPath $statePath -Algorithm MD5).Hash.ToLower().Substring(0, 12) }
    Heartbeat
    Write-Host ("[run-tape] {0}: {1} ticks {2} cpu {3:F3} s cycles {4:F2} G wall {5:F2} s state {6}" -f (Split-Path $directory -Leaf),
        $(if ($row.failed) { "FAILED ($($row.reason))" } else { 'ok' }), $row.ticks, $row.cpu_s, ($row.cycles / 1e9), $row.wall_s, $row.state)
    $row
}

Write-Host "[run-tape] exe $exePath (md5 $((Get-FileHash -LiteralPath $exePath -Algorithm MD5).Hash.ToLower().Substring(0, 8))); tape $Tape ($($tapeRef.stage)); budget $BudgetMs ms; extra '$($Extra -join ' ')'; env '$($Env -join ' ')'; repeat $Repeat$(if ($Exclusive) { '; exclusive' })"
if (-not $InsideReservation) {
    if (-not (Test-Path -LiteralPath $Coord)) { Fail-Setup "coordinator not found: $Coord (or pass -InsideReservation)" }
    if (-not $Task) { $Task = "tape run x$Repeat ($Tape) of $(Split-Path $exePath -Leaf) -> $outFull" }
    $coordArguments = @('acquire', '-Session', $Session, '-Task', $Task, '-Kind', $(if ($Exclusive) { 'benchmark' } else { 'game-run' }),
        '-Threads', '4', '-MemGB', '6', '-EstMinutes', "$EstMinutes", '-OwnerPid', "$PID", '-WaitMinutes', "$WaitMinutes")
    if ($Exclusive) { $coordArguments += '-Exclusive' }
    $answer = & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord @coordArguments
    $code = $LASTEXITCODE
    $answer | ForEach-Object { Write-Host $_ }
    $id = @($answer)[-1]
    if ($code -ne 0) {
        if ($id) { try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord cancel -Id $id | Out-Null } catch {} }
        Write-Host "[run-tape] reservation not granted within $WaitMinutes min; nothing was started"
        exit 3
    }
    $script:Reservation = $id
}
$exitCode = 1
try {
    $rows = @()
    if ($Repeat -le 1) { $rows += Run-Once $outFull }
    else { for ($i = 1; $i -le $Repeat; $i++) { $rows += Run-Once (Join-Path $outFull "r$i") } }
    if ($Repeat -gt 1) {
        $cpu = @($rows | ForEach-Object { $_.cpu_s } | Sort-Object); $cyc = @($rows | ForEach-Object { $_.cycles } | Sort-Object)
        $median = { param($v) if ($v.Count % 2) { $v[($v.Count - 1) / 2] } else { ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2 } }
        Write-Host ("[run-tape] cpu median {0:F3} s [{1:F3}..{2:F3}]; cycles median {3:F2} G [{4:F2}..{5:F2}]; states {6}" -f (& $median $cpu), $cpu[0], $cpu[-1],
            ((& $median $cyc) / 1e9), ($cyc[0] / 1e9), ($cyc[-1] / 1e9), ((@($rows | ForEach-Object { $_.state }) | Sort-Object -Unique) -join ','))
        [IO.File]::WriteAllText((Join-Path $outFull 'runs.json'), (@($rows) | ConvertTo-Json -Depth 3), (New-Object System.Text.UTF8Encoding($false)))
    }
    $exitCode = if (@($rows | Where-Object { $_.failed }).Count) { 1 } else { 0 }
} finally {
    if ($script:Reservation) {
        try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord release -Id $script:Reservation | ForEach-Object { Write-Host $_ } }
        catch { Write-Host "[run-tape] could NOT release reservation $($script:Reservation): $_ - release it by hand" }
    }
}
exit $exitCode
