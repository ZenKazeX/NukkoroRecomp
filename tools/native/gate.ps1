<#
.SYNOPSIS
  Tape gate as one command: run Westopolis and Final Haunt headless with one executable and compare the state, PCM,
  behaviour and screenshot hashes with the references. Exit 0 only when every check matches.

.DESCRIPTION
  Port of the Gate / Gate2 functions the cpu/opt sessions kept untracked under wt-cpu-opt4\.build\co (perf.ps1,
  gate2.ps1), with the references in gate_references.json beside this file and the machine reservation built in.

  Runs (each in its own directory under -Out, with a private copy of the tape's card / SRAM / cfg files):
    state   Westopolis                                  -> state.json md5
    beh     Westopolis + behaviour trace + audio dump   -> audio.pcm md5, behaviour.jsonl md5, behaviour_diff --exact
    shots   Westopolis + 10 native screenshots          -> combined md5 of the PNGs
    fh      Final Haunt + audio dump                    -> state.json md5, audio.pcm md5
  A run that does not exit 0, or whose session.json reports a failed stop, fails the gate whatever its hashes are.

  The whole gate holds ONE reservation of the shared coordinator (<workspace>\.coord\coord.ps1, kind "gate"): it is
  requested before the first run, waits up to -WaitMinutes, is heart-beaten while the game runs and is released on
  every exit path. -InsideReservation skips this for a caller that already holds a reservation covering the gate.

  Exit codes: 0 all checks equal the references; 1 at least one check differs or a run failed; 2 the gate could not
  run (missing executable, tape, reference, Python); 3 the reservation was not granted within -WaitMinutes (nothing
  was started).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\native\gate.ps1 -Exe .build\native\Release\ShadowTheHedgehog.exe `
      -Out .build\gate\n1 -Session native-win -WaitMinutes 120
.EXAMPLE
  ... gate.ps1 -Exe a.exe -Out .build\gate\pass -Extra '--chara-colli-pass' -Env 'GEKKO_OP3_AMPDB=0'
.EXAMPLE
  ... gate.ps1 -Exe a.exe -Out .build\gate\x -Plan        # print what would run and what is checked; start nothing
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$Extra = @(),              # extra command-line arguments for every run
    [string[]]$Env = @(),                # NAME=value pairs set for every run
    [ValidateSet('all', 'westopolis', 'final-haunt')][string]$Only = 'all',
    [string]$Session = 'gate',           # name shown in the coordinator
    [int]$WaitMinutes = 60,
    [int]$RunTimeoutSeconds = 900,      # a run that has not exited by then is stopped and fails the gate (raise it for --verify-* runs)
    [switch]$InsideReservation,
    [switch]$Plan,
    [string]$Workspace,                  # default: the directory that holds the worktrees (three levels above this file)
    [string]$References,                 # default: gate_references.json beside this file
    [string]$Python                      # default: python on PATH, else the Blender copy this machine uses
)

$ErrorActionPreference = 'Stop'
# 'powershell -File' hands an array argument over as ONE comma-joined string: split it back (no argument of the
# runtime contains a comma).
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$Tree = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Workspace) { $Workspace = Split-Path $Tree -Parent }
if (-not $References) { $References = Join-Path $PSScriptRoot 'gate_references.json' }
$Coord = Join-Path $Workspace '.coord\coord.ps1'

function Fail-Setup([string]$message) { Write-Host "[gate] cannot run: $message"; exit 2 }

function Md5-Prefix([string]$path, [int]$length) {
    if (-not (Test-Path -LiteralPath $path)) { return '(missing)' }
    (Get-FileHash -LiteralPath $path -Algorithm MD5).Hash.ToLower().Substring(0, $length)
}

# The tape's own card / SRAM / cfg files are copied into the run directory; the originals are only read.
function Copy-Persist([string]$source, [string]$directory) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Force $directory | Out-Null
    foreach ($file in 'card.raw', 'SRAM.pc.raw', 'display.cfg', 'pad.cfg') {
        if (Test-Path -LiteralPath (Join-Path $source $file)) { Copy-Item -LiteralPath (Join-Path $source $file) -Destination $directory }
        elseif (Test-Path -LiteralPath (Join-Path $source "initial\$file")) { Copy-Item -LiteralPath (Join-Path $source "initial\$file") -Destination $directory }
        else { throw "tape directory $source has no $file (nor initial\$file)" }
    }
}

function Game-Arguments([string]$stage, [string]$tape, [int]$budget, [string[]]$more) {
    @('--game-data', ((Join-Path $Workspace 'game') -replace '\\', '/'), '--boot-stage', $stage, '--headless', '--no-audio',
      '--memory-card', 'card.raw', '--display-cfg', 'display.cfg', '--pad-cfg', 'pad.cfg', '--state-file', 'unused.gekkostate',
      '--snapshot-key', 'none', '--no-xinput', '--budget-ms', "$budget", '--input-tape-play', ($tape -replace '\\', '/'),
      '--native-output', '1280x720', '--width', '1280', '--height', '720', '--json-report', 'session.json',
      '--state-report', 'state.json') + $more + $Extra
}

$script:Reservation = $null
function Heartbeat {
    if ($script:Reservation) {
        try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord heartbeat -Id $script:Reservation | Out-Null } catch {}
    }
}

# Returns @{ exit; failed; reason } for one run; stdout / stderr go to files in the run directory.
function Run-Game([string]$directory, [string[]]$arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $script:ExePath; $info.WorkingDirectory = $directory; $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    foreach ($pair in $Env) {
        $name, $value = $pair -split '=', 2
        $info.EnvironmentVariables[$name] = [string]$value
    }
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        $deadline = (Get-Date).AddSeconds($RunTimeoutSeconds); $timedOut = $false
        while (-not $process.WaitForExit(15000)) {
            Heartbeat
            if ((Get-Date) -ge $deadline) { $timedOut = $true; $process.Kill(); break }   # this gate's own child
        }
        $process.WaitForExit()
        [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'), $stdout.Result)
        [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'), $stderr.Result)
    } finally {
        if (-not $process.HasExited) { try { $process.Kill() } catch {} }   # only this gate's own child
    }
    $result = @{ exit = $process.ExitCode; failed = $false; reason = '' }
    if ($timedOut) { $result.failed = $true; $result.reason = "no exit within $RunTimeoutSeconds s (stopped; see stderr.log)" }
    elseif ($process.ExitCode -ne 0) { $result.failed = $true; $result.reason = "exit code $($process.ExitCode)" }
    $session = Join-Path $directory 'session.json'
    if (-not (Test-Path -LiteralPath $session)) { $result.failed = $true; $result.reason = ($result.reason + ' no session.json').Trim() }
    else {
        try {
            $stop = (Get-Content -LiteralPath $session -Raw | ConvertFrom-Json).stop
            if ($stop.failed) { $result.failed = $true; $result.reason = ($result.reason + " session.json stop.failed (reason $($stop.reason))").Trim() }
        } catch { $result.failed = $true; $result.reason = ($result.reason + ' unreadable session.json').Trim() }
    }
    Heartbeat
    $result
}

# The state report's md5 covers its "savestate_version" field, so each checkpoint version has its own reference.
# Returns @(version, expected); a version with no reference gives an expected value that cannot match.
function Expected-State($tapeReference, [string]$statePath) {
    $version = '?'
    try { $version = "$((Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json).savestate_version)" } catch {}
    $table = $tapeReference.state_md5_12_by_savestate_version
    if ($table -and ($table.PSObject.Properties.Name -contains $version)) { return @($version, [string]$table.$version) }
    if (-not $table) { return @($version, [string]$tapeReference.state_md5_12) }   # an older references file
    return @($version, "(no reference for checkpoint version $version)")
}

$script:Checks = New-Object System.Collections.ArrayList
function Check([string]$name, [string]$expected, [string]$actual) {
    $ok = ($expected -eq $actual)
    [void]$script:Checks.Add([ordered]@{ check = $name; expected = $expected; actual = $actual; pass = $ok })
    Write-Host ("[gate] {0,-34} {1}  {2}" -f $name, $(if ($ok) { 'ok  ' } else { 'FAIL' }), $(if ($ok) { $actual } else { "$actual (expected $expected)" }))
}
function Check-Run([string]$name, $run) { Check "$name run completed" 'yes' $(if ($run.failed) { "no: $($run.reason)" } else { 'yes' }) }

# ---- inputs -------------------------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Exe)) { Fail-Setup "executable not found: $Exe" }
$script:ExePath = (Resolve-Path -LiteralPath $Exe).Path
if (-not (Test-Path -LiteralPath $References)) { Fail-Setup "references not found: $References" }
$ref = Get-Content -LiteralPath $References -Raw | ConvertFrom-Json
$west = $ref.westopolis; $fh = $ref.final_haunt
$westDir = Join-Path $Workspace ($west.tape_dir -replace '/', '\'); $fhDir = Join-Path $Workspace ($fh.tape_dir -replace '/', '\')
$doWest = $Only -ne 'final-haunt'; $doFh = $Only -ne 'westopolis'
if ($doWest -and -not (Test-Path -LiteralPath (Join-Path $westDir 'input.tape'))) { Fail-Setup "Westopolis tape not found in $westDir" }
if ($doFh -and -not (Test-Path -LiteralPath (Join-Path $fhDir 'input.tape'))) { Fail-Setup "Final Haunt tape not found in $fhDir" }
if (-not (Test-Path -LiteralPath (Join-Path $Workspace 'game'))) { Fail-Setup "game data not found: $(Join-Path $Workspace 'game')" }
$layout = Join-Path $Tree 'tools\behaviour_layout.txt'
$behaviourDiff = Join-Path $Tree 'tools\behaviour_diff.py'
$behaviourBase = Join-Path $Workspace ($west.behaviour_base -replace '/', '\')
if ($doWest) {
    foreach ($needed in $layout, $behaviourDiff, $behaviourBase) { if (-not (Test-Path -LiteralPath $needed)) { Fail-Setup "not found: $needed" } }
    if (-not $Python) {
        $found = Get-Command python -ErrorAction SilentlyContinue
        if ($found -and $found.Source -notlike '*\WindowsApps\*') { $Python = $found.Source }
        else { $Python = 'G:\Program Files\Blender Foundation\Blender 5.2\5.2\python\bin\python.exe' }
    }
    if (-not (Test-Path -LiteralPath $Python)) { Fail-Setup "Python not found: $Python (pass -Python)" }
}
foreach ($pair in $Env) { if ($pair -notmatch '^[A-Za-z_][A-Za-z0-9_]*=') { Fail-Setup "-Env wants NAME=value, got '$pair'" } }
$outFull = [IO.Path]::GetFullPath($Out)
foreach ($protected in (Join-Path $Workspace 'game'), $westDir, $fhDir) {
    if ($outFull.StartsWith([IO.Path]::GetFullPath($protected), [StringComparison]::OrdinalIgnoreCase)) { Fail-Setup "-Out must not be inside $protected" }
}

$runs = @()
if ($doWest) { $runs += 'state', 'beh', 'shots' }
if ($doFh) { $runs += 'fh' }
Write-Host "[gate] exe $script:ExePath (md5 $(Md5-Prefix $script:ExePath 8)); runs: $($runs -join ', '); extra '$($Extra -join ' ')'; env '$($Env -join ' ')'"
if ($Plan) {
    Write-Host "[gate] plan only, nothing started. Output $outFull; references $References (base $($ref.base_commit))"
    if ($doWest) {
        Write-Host "[gate]   Westopolis $($west.stage), tape $westDir, budget $($west.budget_ms) ms: state $($west.state_md5_12), pcm $($west.pcm_md5_16), behaviour $($west.behaviour_md5_12) + --exact against $behaviourBase, $($west.shots_count) shots $($west.shots_md5_12)"
        Write-Host ("[gate]   arguments: " + ((Game-Arguments $west.stage (Join-Path $westDir 'input.tape') $west.budget_ms @()) -join ' '))
    }
    if ($doFh) { Write-Host "[gate]   Final Haunt $($fh.stage), tape $fhDir, budget $($fh.budget_ms) ms: state $($fh.state_md5_12), pcm $($fh.pcm_md5_16)" }
    Write-Host "[gate]   reservation: $(if ($InsideReservation) { 'none (inside the caller''s)' } else { "$Coord, kind gate, 4 threads, 6 GB, wait up to $WaitMinutes min" })"
    exit 0
}

# ---- reservation --------------------------------------------------------------------------------------------------
if (-not $InsideReservation) {
    if (-not (Test-Path -LiteralPath $Coord)) { Fail-Setup "coordinator not found: $Coord (or pass -InsideReservation)" }
    $task = "tape gate ($($runs -join '+')) of $(Split-Path $script:ExePath -Leaf) -> $outFull"
    $answer = & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord acquire -Session $Session -Task $task -Kind gate `
        -Threads 4 -MemGB 6 -EstMinutes 6 -OwnerPid $PID -WaitMinutes $WaitMinutes
    $code = $LASTEXITCODE
    $answer | ForEach-Object { Write-Host $_ }
    $id = @($answer)[-1]
    if ($code -ne 0) {
        if ($id) { try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord cancel -Id $id | Out-Null } catch {} }
        Write-Host "[gate] reservation not granted within $WaitMinutes min; nothing was started"
        exit 3
    }
    $script:Reservation = $id
}

$exitCode = 1
try {
    New-Item -ItemType Directory -Force $outFull | Out-Null
    if ($doWest) {
        $tape = Join-Path $westDir 'input.tape'
        $d = Join-Path $outFull 'state'; Copy-Persist $westDir $d
        $run = Run-Game $d (Game-Arguments $west.stage $tape $west.budget_ms @())
        Check-Run 'westopolis state' $run
        $want = Expected-State $west (Join-Path $d 'state.json')
        Check "westopolis state.json md5 (v$($want[0]))" $want[1] (Md5-Prefix (Join-Path $d 'state.json') 12)

        $d = Join-Path $outFull 'beh'; Copy-Persist $westDir $d
        $run = Run-Game $d (Game-Arguments $west.stage $tape $west.budget_ms @('--behaviour-trace', 'behaviour.jsonl',
            '--behaviour-layout', ($layout -replace '\\', '/'), '--behaviour-every', '10', '--audio-dump', 'audio.pcm'))
        Check-Run 'westopolis behaviour' $run
        Check 'westopolis audio.pcm md5' $west.pcm_md5_16 (Md5-Prefix (Join-Path $d 'audio.pcm') 16)
        Check 'westopolis behaviour.jsonl md5' $west.behaviour_md5_12 (Md5-Prefix (Join-Path $d 'behaviour.jsonl') 12)
        $verdict = '(behaviour.jsonl missing)'
        if (Test-Path -LiteralPath (Join-Path $d 'behaviour.jsonl')) {
            $ErrorActionPreference = 'Continue'
            $text = (& $Python -I -B $behaviourDiff --exact $behaviourBase (Join-Path $d 'behaviour.jsonl') 2>&1 | Out-String)
            $diffExit = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            [IO.File]::WriteAllText((Join-Path $d 'behaviour_diff.txt'), $text)
            $line = (($text -split "`n") | Where-Object { $_ -match 'VERDICT' } | Select-Object -First 1)
            $verdict = if ($line) { $line.Trim() } else { '(no VERDICT line)' }
            Check 'behaviour_diff --exact exit code' '0' "$diffExit"
        }
        Write-Host "[gate] behaviour_diff: $verdict"
        Remove-Item -LiteralPath (Join-Path $d 'audio.pcm') -ErrorAction SilentlyContinue -Confirm:$false

        $d = Join-Path $outFull 'shots'; Copy-Persist $westDir $d
        New-Item -ItemType Directory -Force (Join-Path $d 'png') | Out-Null
        $run = Run-Game $d (Game-Arguments $west.stage $tape $west.budget_ms @('--native-screenshot', ((Join-Path $d 'png\f.png') -replace '\\', '/'),
            '--native-screenshot-frame', '1000', '--native-screenshot-stride', '1500', '--native-screenshot-count', "$($west.shots_count)"))
        Check-Run 'westopolis screenshots' $run
        $files = @(Get-ChildItem -LiteralPath (Join-Path $d 'png') -Filter *.png | Sort-Object Name)
        $md5 = [System.Security.Cryptography.MD5]::Create()
        foreach ($file in $files) { $bytes = [IO.File]::ReadAllBytes($file.FullName); [void]$md5.TransformBlock($bytes, 0, $bytes.Length, $null, 0) }
        [void]$md5.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        Check 'westopolis screenshot count' "$($west.shots_count)" "$($files.Count)"
        Check 'westopolis screenshots md5' $west.shots_md5_12 ((([BitConverter]::ToString($md5.Hash)) -replace '-', '').ToLower().Substring(0, 12))
    }
    if ($doFh) {
        $d = Join-Path $outFull 'fh'; Copy-Persist $fhDir $d
        $run = Run-Game $d (Game-Arguments $fh.stage (Join-Path $fhDir 'input.tape') $fh.budget_ms @('--audio-dump', 'audio.pcm'))
        Check-Run 'final haunt' $run
        $want = Expected-State $fh (Join-Path $d 'state.json')
        Check "final haunt state.json md5 (v$($want[0]))" $want[1] (Md5-Prefix (Join-Path $d 'state.json') 12)
        Check 'final haunt audio.pcm md5' $fh.pcm_md5_16 (Md5-Prefix (Join-Path $d 'audio.pcm') 16)
        Remove-Item -LiteralPath (Join-Path $d 'audio.pcm') -ErrorAction SilentlyContinue -Confirm:$false
    }
    $failed = @($script:Checks | Where-Object { -not $_.pass })
    $summary = [ordered]@{
        exe = $script:ExePath; exe_md5_8 = (Md5-Prefix $script:ExePath 8); extra = $Extra; env = $Env; only = $Only
        references = $References; reference_base_commit = $ref.base_commit; finished = (Get-Date).ToUniversalTime().ToString('o')
        pass = ($failed.Count -eq 0); checks = @($script:Checks)
    }
    [IO.File]::WriteAllText((Join-Path $outFull 'result.json'), ($summary | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    if ($failed.Count -eq 0) { Write-Host "[gate] PASS: $($script:Checks.Count) checks equal the references ($outFull\result.json)"; $exitCode = 0 }
    else { Write-Host "[gate] FAIL: $($failed.Count) of $($script:Checks.Count) checks differ ($outFull\result.json)"; $exitCode = 1 }
} finally {
    if ($script:Reservation) {
        try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord release -Id $script:Reservation | ForEach-Object { Write-Host $_ } }
        catch { Write-Host "[gate] could NOT release reservation $($script:Reservation): $_ - release it by hand" }
    }
}
exit $exitCode
