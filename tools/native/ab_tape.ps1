<#
.SYNOPSIS
  Interleaved A/B timing of two executables on one tape, under ONE exclusive coordinator reservation.

.DESCRIPTION
  -Rounds rounds; A runs first in odd rounds and B first in even rounds (ABBA), so drift over the session hits both
  arms alike. Every run is run_tape.ps1's run (the gate's argument set plus -Extra / -Env, the same for both arms
  unless -ExtraB / -EnvB are given). Runs land in <Out>\r<round>-A and <Out>\r<round>-B; ab_summary.py reads them.

  The reservation is exclusive: nothing else heavy may run, and no other session is granted while it is held.
  Exit codes: 0 all runs completed; 1 a run failed; 2 cannot run; 3 reservation not granted.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\native\ab_tape.ps1 -ExeA old.exe -ExeB new.exe `
      -Tape westopolis -Rounds 5 -Out .build\ab\step -Session native-win
  python tools/native/ab_summary.py .build\ab\step
#>
param(
    [Parameter(Mandatory = $true)][string]$ExeA,
    [Parameter(Mandatory = $true)][string]$ExeB,
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateSet('westopolis', 'final-haunt')][string]$Tape = 'westopolis',
    [int]$Rounds = 5,
    [string[]]$Extra = @(),
    [string[]]$ExtraB,
    [string[]]$Env = @(),
    [string[]]$EnvB,
    [string]$Session = 'ab-tape',
    [int]$WaitMinutes = 60,
    [int]$RunTimeoutSeconds = 900,
    [string]$Workspace
)

$ErrorActionPreference = 'Stop'
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
if ($null -eq $ExtraB) { $ExtraB = $Extra } else { $ExtraB = @($ExtraB | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' }) }
if ($null -eq $EnvB) { $EnvB = $Env }
$Tree = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Workspace) { $Workspace = Split-Path $Tree -Parent }
$Coord = Join-Path $Workspace '.coord\coord.ps1'
$RunTape = Join-Path $PSScriptRoot 'run_tape.ps1'
foreach ($exe in $ExeA, $ExeB) { if (-not (Test-Path -LiteralPath $exe)) { Write-Host "[ab-tape] cannot run: executable not found: $exe"; exit 2 } }
if (-not (Test-Path -LiteralPath $Coord)) { Write-Host "[ab-tape] cannot run: coordinator not found: $Coord"; exit 2 }
$outFull = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force $outFull | Out-Null

$task = "A/B timing $Tape x$Rounds rounds: $(Split-Path $ExeA -Leaf) vs $(Split-Path $ExeB -Leaf) (EXCLUSIVE)"
$answer = & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord acquire -Session $Session -Task $task -Kind benchmark -Exclusive `
    -Threads 4 -MemGB 6 -EstMinutes ([Math]::Max(2, $Rounds)) -OwnerPid $PID -WaitMinutes $WaitMinutes
$code = $LASTEXITCODE
$answer | ForEach-Object { Write-Host $_ }
$id = @($answer)[-1]
if ($code -ne 0) {
    if ($id) { try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord cancel -Id $id | Out-Null } catch {} }
    Write-Host "[ab-tape] reservation not granted within $WaitMinutes min; nothing was started"
    exit 3
}
$failed = 0
try {
    $arms = @{ A = @{ exe = $ExeA; extra = $Extra; env = $Env }; B = @{ exe = $ExeB; extra = $ExtraB; env = $EnvB } }
    for ($round = 1; $round -le $Rounds; $round++) {
        $order = if ($round % 2) { 'A', 'B' } else { 'B', 'A' }
        foreach ($arm in $order) {
            $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $RunTape, '-Exe', $arms[$arm].exe, '-Out', (Join-Path $outFull "r$round-$arm"),
                '-Tape', $Tape, '-InsideReservation', '-RunTimeoutSeconds', "$RunTimeoutSeconds")
            if ($arms[$arm].extra.Count) { $arguments += @('-Extra', ($arms[$arm].extra -join ',')) }
            if ($arms[$arm].env.Count) { $arguments += @('-Env', ($arms[$arm].env -join ',')) }
            & powershell @arguments | Where-Object { $_ -match '^\[run-tape\] r' } | ForEach-Object { Write-Host $_ }
            if ($LASTEXITCODE -ne 0) { $failed++ }
            try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord heartbeat -Id $id | Out-Null } catch {}
        }
    }
    $meta = [ordered]@{ exe_a = (Resolve-Path -LiteralPath $ExeA).Path; exe_b = (Resolve-Path -LiteralPath $ExeB).Path; tape = $Tape; rounds = $Rounds
        extra_a = $Extra; extra_b = $ExtraB; env_a = $Env; env_b = $EnvB; finished = (Get-Date).ToUniversalTime().ToString('o'); failed_runs = $failed }
    [IO.File]::WriteAllText((Join-Path $outFull 'ab.json'), ($meta | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
} finally {
    try { & powershell -NoProfile -ExecutionPolicy Bypass -File $Coord release -Id $id | ForEach-Object { Write-Host $_ } }
    catch { Write-Host "[ab-tape] could NOT release reservation ${id}: $_ - release it by hand" }
}
if ($failed) { Write-Host "[ab-tape] $failed runs failed"; exit 1 }
exit 0
