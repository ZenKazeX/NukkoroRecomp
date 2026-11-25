# One-command profile-guided build of the game image: the PowerShell form of pgo_build.sh + pgo_train.sh (cpu/opt4,
# for a machine without bash). Same steps, same training set, same options.
#
#   powershell -File tools/perf/pgo_build.ps1 -Tree <build tree> -Game <game dir> -Recordings <recordings dir>
#              [-BudgetMs 150000] [-Jobs 6] [-CMakeArgs @('-G','...','-DNAME=value',...)]
#
#   1. configure <tree> with SHADOW_LTO=ON SHADOW_LTO_SHARDS=OFF SHADOW_PGO=GEN and build ShadowTheHedgehog (the
#      runtime and native owners become /GL objects; the generated shards stay ordinary objects);
#   2. train the instrumented image on recorded gameplay that is neither the benchmark nor a gate tape
#      (lethal-highway-sfx, user-westopolis-720p-20261109-092727) and merge the counts into
#      <tree>/pgo/ShadowTheHedgehog.pgd;
#   3. reconfigure with SHADOW_PGO=USE and relink.
#
# cmake must be on PATH. The tree must be configurable for the game (generator, DOL): pass those -D arguments.
# The profile decides layout, inlining and branch order only; gate every image like any other build.
param(
    [Parameter(Mandatory)][string]$Tree,
    [Parameter(Mandatory)][string]$Game,
    [Parameter(Mandatory)][string]$Recordings,
    [int]$BudgetMs = 150000,
    [int]$Jobs = 6,
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = 'Stop'
$src = (Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force "$Tree\pgo" | Out-Null
$Tree = (Resolve-Path $Tree).Path
$pgd = "$Tree\pgo\ShadowTheHedgehog.pgd"
Remove-Item $pgd -ErrorAction SilentlyContinue

function Invoke-Checked([string[]]$Arguments, [string]$What) {
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "pgo_build: $What failed (exit $LASTEXITCODE)" }
}
$build = @('--build', $Tree, '--config', 'Release', '--target', 'ShadowTheHedgehog', '--', "/m:$Jobs", '/v:m')

Invoke-Checked (@('-S', $src, '-B', $Tree, '-DSHADOW_LTO=ON', '-DSHADOW_LTO_SHARDS=OFF', '-DSHADOW_PGO=GEN') + $CMakeArgs) 'configure (GEN)'
Invoke-Checked $build 'build (GEN)'

# The instrumented runtime (pgort140.dll) and pgomgr live next to the toolset's link.exe.
$linker = (& cmake -LA -N $Tree | Where-Object { $_ -match '^CMAKE_LINKER:FILEPATH=' }) -replace '^CMAKE_LINKER:FILEPATH=', ''
$linkDir = Split-Path ($linker -replace '/', '\')
$env:PATH = "$linkDir;$env:PATH"
$exe = "$Tree\bin\Release\ShadowTheHedgehog.exe"
$work = "$Tree\pgo\train"

function Train([string]$Name, [string]$Stage, [string]$TapeDir, [string]$FilesDir, [string]$CardDir) {
    $d = "$work\$Name"
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item "$FilesDir\display.cfg", "$FilesDir\pad.cfg" $d
    Copy-Item "$CardDir\card.raw" $d
    if (Test-Path "$CardDir\SRAM.pc.raw") { Copy-Item "$CardDir\SRAM.pc.raw" $d }
    $arguments = @('--game-data', ($Game -replace '\\', '/'), '--boot-stage', $Stage, '--headless', '--no-audio', '--memory-card', 'card.raw',
        '--display-cfg', 'display.cfg', '--pad-cfg', 'pad.cfg', '--state-file', 'unused.gekkostate', '--snapshot-key', 'none', '--no-xinput',
        '--budget-ms', "$BudgetMs", '--input-tape-play', ("$TapeDir\input.tape" -replace '\\', '/'), '--native-output', '1280x720',
        '--width', '1280', '--height', '720', '--json-report', 'session.json')
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $d -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput "$d\stdout.log" -RedirectStandardError "$d\stderr.log"
    "$Name exit $($p.ExitCode)"
    if ($p.ExitCode -ne 0) { throw "pgo_build: training run $Name failed" }
}
$lh = "$Recordings\lethal-highway-sfx"
Train 'lethal-highway' 'stg0202' $lh "$lh\initial" "$lh\initial"
$uw = "$Recordings\user-westopolis-720p-20261109-092727"
Train 'user-westopolis' 'stg0100' $uw "$uw\initial" "$lh\initial"

$counts = @(Get-ChildItem (Split-Path $exe) -Filter 'ShadowTheHedgehog!*.pgc')
if ($counts.Count -eq 0) { throw 'pgo_build: the instrumented runs wrote no counts (.pgc)' }
foreach ($f in $counts) {
    & pgomgr /merge $f.FullName $pgd | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "pgo_build: pgomgr /merge failed for $($f.Name)" }
    Remove-Item $f.FullName
}
& pgomgr /summary $pgd | Select-Object -First 20

Invoke-Checked @('-S', $src, '-B', $Tree, '-DSHADOW_PGO=USE') 'configure (USE)'
Invoke-Checked $build 'build (USE)'
(Get-FileHash $exe -Algorithm MD5).Hash.ToLower()
