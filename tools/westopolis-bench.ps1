<#
.SYNOPSIS
  Runs the native build, samples host counters while it is up, and reduces every
  required metric into one row of the running performance log.

.DESCRIPTION
  This harness does not touch the game. It launches the shipping executable with
  the normal arguments, samples Windows performance counters from the outside,
  and parses the runtime's own stdout/stderr afterwards. It never injects input:
  boot to Westopolis and traverse the level yourself while it records.

  Stop the run by closing the window or pressing Escape. The harness then writes
  <label>.json, <label>.row.md and prints the row.

.EXAMPLE
  pwsh -File tools/westopolis-bench.ps1 -Label iter-04-loop-heads `
       -Notes "loop-head dispatch entries"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$Notes = '',
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\game\ShadowTheHedgehog.exe'),
    [string]$GameData = (Join-Path $PSScriptRoot '..\..\game'),
    [string]$Symbols = (Join-Path $PSScriptRoot '..\extract\functions-named.csv'),
    [Parameter(Mandatory = $true)][string]$OutDir,
    # Large enough not to end a manual traversal early. This is a diagnostic
    # limit, not a speed setting.
    [string]$Budget = '400000000000',
    [double]$SampleSeconds = 1.0,
    # Extra runtime flags, e.g. '--no-lazy-copies' for the eager baseline arm
    # of a lazy-copy A/B. Passed through verbatim.
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Get-Num([string]$text, [string]$pattern) {
    $m = [regex]::Match($text, $pattern)
    if (-not $m.Success) { return $null }
    return [double]::Parse($m.Groups[1].Value, $inv)
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$base = Join-Path $OutDir "$Label-$stamp"
$perfCsv = "$base.frames.csv"
$outLog = "$base.stdout.log"
$errLog = "$base.stderr.log"

$exePath = (Resolve-Path $Exe).Path
$argList = @(
    '--game-data', $GameData,
    '--dx12-raster',
    '--no-call-trace',
    '--budget', $Budget,
    '--perf-log', $perfCsv,
    '--json-report', "$base.session.json"
)
if (Test-Path $Symbols) { $argList += @('--symbols', (Resolve-Path $Symbols).Path) }
if ($ExtraArgs.Count -gt 0) { $argList += $ExtraArgs }

Write-Host "Launching $exePath"
Write-Host "  Boot to Westopolis and traverse the level. Close the window or press Escape to finish."
$proc = Start-Process -FilePath $exePath -ArgumentList $argList `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru

# ---- host counter sampling ------------------------------------------------
$samples = New-Object System.Collections.Generic.List[object]
$logical = [Environment]::ProcessorCount
$t0 = Get-Date
while (-not $proc.HasExited) {
    Start-Sleep -Seconds $SampleSeconds
    if ($proc.HasExited) { break }
    try { $p = Get-Process -Id $proc.Id -ErrorAction Stop } catch { break }

    # Process CPU% since the previous sample, normalised to one logical core.
    $cpuTotal = $p.TotalProcessorTime.TotalSeconds
    $elapsed = ((Get-Date) - $t0).TotalSeconds
    $wsMiB = [math]::Round($p.WorkingSet64 / 1MB, 2)
    $privMiB = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)

    $gpu = 0.0
    try {
        $eng = Get-Counter "\GPU Engine(pid_$($proc.Id)*engtype_3D)\Utilization Percentage" `
            -ErrorAction Stop
        $gpu = ($eng.CounterSamples | Measure-Object -Property CookedValue -Sum).Sum
    } catch { }
    $vram = 0.0
    try {
        $ded = Get-Counter "\GPU Process Memory(pid_$($proc.Id)*)\Dedicated Usage" -ErrorAction Stop
        $vram = [math]::Round((($ded.CounterSamples | Measure-Object -Property CookedValue -Sum).Sum) / 1MB, 2)
    } catch { }

    $samples.Add([pscustomobject]@{
        ElapsedSeconds = [math]::Round($elapsed, 3)
        CpuSeconds     = [math]::Round($cpuTotal, 3)
        GpuPercent     = [math]::Round($gpu, 2)
        WorkingSetMiB  = $wsMiB
        PrivateMiB     = $privMiB
        DedicatedVramMiB = $vram
    })
}
$proc.WaitForExit()
$wall = ((Get-Date) - $t0).TotalSeconds

# Average process CPU as a share of the whole machine and of one core.
$cpuSecondsUsed = 0.0
if ($samples.Count -gt 0) { $cpuSecondsUsed = $samples[$samples.Count - 1].CpuSeconds }
$cpuPercentMachine = if ($wall -gt 0) { 100.0 * $cpuSecondsUsed / ($wall * $logical) } else { 0 }
$cpuCoresBusy = if ($wall -gt 0) { $cpuSecondsUsed / $wall } else { 0 }
$gpuAvg = if ($samples.Count -gt 0) { ($samples | Measure-Object -Property GpuPercent -Average).Average } else { 0 }
$gpuPeak = if ($samples.Count -gt 0) { ($samples | Measure-Object -Property GpuPercent -Maximum).Maximum } else { 0 }
$ramPeak = if ($samples.Count -gt 0) { ($samples | Measure-Object -Property PrivateMiB -Maximum).Maximum } else { 0 }
$vramPeak = if ($samples.Count -gt 0) { ($samples | Measure-Object -Property DedicatedVramMiB -Maximum).Maximum } else { 0 }

# ---- frame intervals -------------------------------------------------------
$avgFps = 0.0; $minFps = 0.0; $low1 = 0.0; $cpuMs = 0.0; $guestMs = 0.0
$submitMs = 0.0; $presentMs = 0.0; $frames = 0
if (Test-Path $perfCsv) {
    $rows = Import-Csv $perfCsv
    # The first row has no preceding interval.
    $intervals = @($rows | Where-Object { [double]::Parse($_.frame_ms, $inv) -gt 0 } |
                   ForEach-Object { [double]::Parse($_.frame_ms, $inv) })
    $frames = $intervals.Count
    if ($frames -gt 0) {
        $sorted = $intervals | Sort-Object
        $sum = ($intervals | Measure-Object -Sum).Sum
        $avgFps = 1000.0 * $frames / $sum
        $minFps = 1000.0 / $sorted[-1]
        # 1% low: mean of the slowest 1% of frames, floor of one frame.
        $take = [math]::Max(1, [int][math]::Floor($frames * 0.01))
        $worst = $sorted[($frames - $take)..($frames - 1)]
        $low1 = 1000.0 / (($worst | Measure-Object -Average).Average)
        # execute/submit/retire/present columns are cumulative totals at the
        # time of each row, so per-frame averages are (last - first) / frames.
        # (Row-averaging a cumulative counter is meaningless, and the previous
        # host_active_ms / guest_ms columns were never written by --perf-log.)
        $firstRow = $rows[0]; $lastRow = $rows[$rows.Count - 1]
        $execDelta = [double]::Parse($lastRow.execute_ms, $inv) -
                     [double]::Parse($firstRow.execute_ms, $inv)
        $submitDelta = [double]::Parse($lastRow.submit_ms, $inv) -
                       [double]::Parse($firstRow.submit_ms, $inv)
        $retireDelta = [double]::Parse($lastRow.retire_ms, $inv) -
                       [double]::Parse($firstRow.retire_ms, $inv)
        $presentDelta = [double]::Parse($lastRow.present_ms, $inv) -
                        [double]::Parse($firstRow.present_ms, $inv)
        $guestMs = $execDelta / $frames
        $submitMs = $submitDelta / $frames
        $presentMs = $presentDelta / $frames
        $cpuMs = ($execDelta + $submitDelta + $retireDelta) / $frames
    }
}

# ---- runtime's own report --------------------------------------------------
$stdout = if (Test-Path $outLog) { Get-Content $outLog -Raw } else { '' }
$stderr = if (Test-Path $errLog) { Get-Content $errLog -Raw } else { '' }

$retired      = Get-Num $stdout 'Retired (\d+) instructions'
$execSeconds  = Get-Num $stdout 'XFB/s over ([\d.]+) s'
$xfbRate      = Get-Num $stdout 'execution presentation rate: ([\d.]+) XFB/s'
$nativePct    = Get-Num $stdout 'instructions \(([\d.]+)% of retired\)'
$interpInstr  = Get-Num $stdout 'Interpreter cost: (\d+) instructions'
$interpMs     = Get-Num $stdout 'Interpreter cost:.*? in ([\d.]+) ms'
$gpuExecS     = Get-Num $stdout 'GPU execution ([\d.]+) s'
$gpuWaitS     = Get-Num $stdout 'CPU fence wait ([\d.]+) s'
$recordS      = Get-Num $stdout 'command record ([\d.]+) s'
$readbackS    = Get-Num $stdout 'readback ([\d.]+) s'
$triangles    = Get-Num $stdout 'batches for (\d+) triangles'
$copies       = Get-Num $stdout 'primary: (\d+)/'
$maxGpuCopyMs = Get-Num $stdout 'maximum GPU copy ([\d.]+) ms'
# Lazy owed-copy ledger (docs/XFB_GPU_RESIDENT_PIPELINE.md): the A/B signal.
$lazyCopies    = Get-Num $stdout 'lazy copies: (\d+) GPU-resident'
$lazyDropped   = Get-Num $stdout '(\d+) superseded writes dropped'
$lazyMat       = Get-Num $stdout '(\d+) materialised on demand'
$lazyAvoidMiB  = Get-Num $stdout '([\d.]+) MiB readback avoided'
$ringFullWaits = Get-Num $stdout 'ring-full waits: (\d+)'
$xfbDrawGpuS   = Get-Num $stdout 'ms/copy: draws ([\d.]+) s'
$xfbCopyGpuS   = Get-Num $stdout 'ms/copy: draws [\d.]+ s, copy tail ([\d.]+) s'
$readyPollMs   = Get-Num $stdout 'ready-poll\s+\d+\s+([\d.]+) ms'
$guestDepMs    = Get-Num $stdout 'guest-memory-dependency\s+\d+\s+([\d.]+) ms'
$guestDepXfb   = Get-Num $stdout 'guest-memory-dependency[^\r\n]*XFB (\d+),'

$stallLines = @([regex]::Matches($stderr, '\[host-stall\][^\r\n]*'))
$stallCount = $stallLines.Count
$longestStall = 0.0
foreach ($m in $stallLines) {
    $v = Get-Num $m.Value '([\d.]+) ms'
    if ($v -ne $null -and $v -gt $longestStall) { $longestStall = $v }
}

$mips = if ($retired -and $execSeconds -and $execSeconds -gt 0) { $retired / $execSeconds / 1e6 } else { 0 }
$gpuMsPerFrame = if ($frames -gt 0 -and $gpuExecS) { 1000.0 * $gpuExecS / $frames } else { 0 }

$result = [ordered]@{
    label = $Label; timestamp = $stamp; notes = $Notes
    wallSeconds = [math]::Round($wall, 3)
    frames = $frames
    avgFps = [math]::Round($avgFps, 2); minFps = [math]::Round($minFps, 2)
    onePercentLowFps = [math]::Round($low1, 2)
    cpuFrameMs = [math]::Round($cpuMs, 3); gpuFrameMs = [math]::Round($gpuMsPerFrame, 3)
    guestMsPerFrame = [math]::Round($guestMs, 3)
    submitMsPerFrame = [math]::Round($submitMs, 3)
    presentMsPerFrame = [math]::Round($presentMs, 3)
    retiredInstructions = $retired; executionSeconds = $execSeconds
    mips = [math]::Round($mips, 1); xfbPerSecond = $xfbRate
    nativeSharePercent = $nativePct
    interpretedInstructions = $interpInstr; interpretedMs = $interpMs
    gpuExecutionSeconds = $gpuExecS; gpuFenceWaitSeconds = $gpuWaitS
    commandRecordSeconds = $recordS; readbackSeconds = $readbackS
    maxGpuCopyMs = $maxGpuCopyMs
    lazyCopies = $lazyCopies; lazySupersededDropped = $lazyDropped
    lazyMaterialisedOnDemand = $lazyMat; lazyReadbackMiBAvoided = $lazyAvoidMiB
    ringFullWaits = $ringFullWaits
    xfbDrawGpuSeconds = $xfbDrawGpuS; xfbCopyTailGpuSeconds = $xfbCopyGpuS
    readyPollMs = $readyPollMs; guestDependencyMs = $guestDepMs
    guestXfbDependencies = $guestDepXfb
    triangles = $triangles; xfbCopies = $copies
    hostStalls = $stallCount; longestStallMs = $longestStall
    cpuPercentMachine = [math]::Round($cpuPercentMachine, 2)
    cpuCoresBusy = [math]::Round($cpuCoresBusy, 2)
    gpuPercentAverage = [math]::Round($gpuAvg, 2); gpuPercentPeak = [math]::Round($gpuPeak, 2)
    ramPrivatePeakMiB = $ramPeak; vramDedicatedPeakMiB = $vramPeak
    logicalProcessors = $logical
}
$result | ConvertTo-Json -Depth 4 | Out-File "$base.json" -Encoding utf8
$samples | Export-Csv "$base.counters.csv" -NoTypeInformation -Encoding utf8

$row = '| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} |' -f `
    $Label, $Notes, $result.avgFps, $result.onePercentLowFps, $result.minFps,
    $result.cpuFrameMs, $result.gpuFrameMs, $result.mips, $result.nativeSharePercent,
    $result.interpretedMs, $result.hostStalls, $result.longestStallMs,
    $result.gpuPercentAverage, $result.ramPrivatePeakMiB, $result.vramDedicatedPeakMiB
$row | Out-File "$base.row.md" -Encoding utf8

Write-Host ''
Write-Host '| Iteration | Change | Avg FPS | 1% Low | Min FPS | CPU ms | GPU ms | MIPS | Native % | Interp ms | Stalls | Longest ms | GPU % | RAM MiB | VRAM MiB |'
Write-Host '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
Write-Host $row
Write-Host ''
Write-Host "Wrote $base.json / .row.md / .counters.csv / .frames.csv"
if ($frames -eq 0) {
    Write-Warning 'No presentation intervals were recorded. The run produced no XFB copies - did it reach a stage?'
}
