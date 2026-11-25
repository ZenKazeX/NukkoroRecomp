# One-command profile-guided game image with clang-cl + lld-link (the LLVM that ships in Visual Studio), Ninja.
#
#   powershell -File tools/perf/clang_pgo_build.ps1 -Tree <build tree> -Game <game dir> -Recordings <recordings dir>
#              -VisualStudio <VS install dir> -Options <CMakeCache.txt of a configured tree to copy SHADOW_* options from>
#              [-Python <python.exe>] [-Lto thin|full|none] [-Jobs 20] [-BudgetMs 150000] [-NoStrictAliasing]
#
#   1. <tree>-gen: instrumented build (-fprofile-generate), ThinLTO;
#   2. trains it on recorded gameplay that is neither the benchmark nor a gate tape (lethal-highway-sfx,
#      user-westopolis-720p-20261109-092727, as tools/perf/pgo_build.ps1 does) and merges the counts;
#   3. <tree>: optimized build (-fprofile-use), ThinLTO. Always a clean tree: objects compiled against an older profile
#      do not link with new ones.
#
# Compile options beyond the project's own: -ffp-contract=off (clang-cl contracts a*b+c under /fp:precise; the guest's
# separately rounded operations must stay separate) and -fstrict-aliasing (off by default in clang-cl; every guest
# memory access is volatile, so it only lets Cpu members stay in registers across guest stores; -NoStrictAliasing
# leaves it out). The profile decides layout, inlining and branch order only; gate every image like any other build.
#
# Measured 2026-11-24 (docs/agents/CPU_OPT_30_RESULT.md): about 2.5 minutes per build on 20 threads.
# KNOWN GAP: the three post-link checks (check_native_boundary.py, verify_native_math.py, make_empty_variadic_fixture.py)
# read MSVC's map and dumpbin output and fail on lld's; the script reports POSTLINK-CHECK-FAILED and keeps the exe.
param(
    [Parameter(Mandatory)][string]$Tree,
    [Parameter(Mandatory)][string]$Game,
    [Parameter(Mandatory)][string]$Recordings,
    [Parameter(Mandatory)][string]$VisualStudio,
    [Parameter(Mandatory)][string]$Options,
    [string]$Python = '',
    [string]$Lto = 'thin',
    [int]$Jobs = 20,
    [int]$BudgetMs = 150000,
    [switch]$NoStrictAliasing
)
$ErrorActionPreference = 'Stop'
$src = (Resolve-Path "$PSScriptRoot\..\..").Path
cmd /c "`"$VisualStudio\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
$llvm = "$VisualStudio\VC\Tools\Llvm\x64"
$bin = ($llvm -replace '\\', '/') + '/bin'
$env:PATH = "$llvm\bin;$env:PATH"
$opts = Get-Content $Options | Where-Object { $_ -match '^(SHADOW_[A-Z0-9_]+):[A-Z]+=(.*)$' -and $_ -notmatch '^SHADOW_(LTO|PGO|DUMPBIN|DXC|LTO_SHARDS|LTO_HOT_SHARDS|PGO_PGD)' } |
    ForEach-Object { $null = $_ -match '^(SHADOW_[A-Z0-9_]+):[A-Z]+=(.*)$'; "-D$($Matches[1])=$($Matches[2])" }
$common = '/DWIN32 /D_WINDOWS /clang:-ffp-contract=off'
if (-not $NoStrictAliasing) { $common += ' /clang:-fstrict-aliasing' }
if ($Lto -ne 'none') { $common += " /clang:-flto=$Lto" }

function Build-Image([string]$t, [string]$flags, [string]$link) {
    if (Test-Path $t) { Remove-Item -Recurse -Force $t }
    New-Item -ItemType Directory -Force "$t\generated" | Out-Null
    $layout = Join-Path (Split-Path $Options) 'generated\recomp_layout.csv'
    if (Test-Path $layout) { Copy-Item $layout "$t\generated\recomp_layout.csv" }   # same shard layout as the options tree
    $started = Get-Date
    $a = @('-S', $src, '-B', $t, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_C_COMPILER=$bin/clang-cl.exe", "-DCMAKE_CXX_COMPILER=$bin/clang-cl.exe",
        "-DCMAKE_LINKER=$bin/lld-link.exe", '-DCMAKE_LINKER_TYPE=LLD', "-DCMAKE_AR=$bin/llvm-lib.exe",
        "-DCMAKE_CXX_FLAGS=$common /EHsc $flags", "-DCMAKE_C_FLAGS=$common $flags", "-DCMAKE_EXE_LINKER_FLAGS=/machine:x64 $link",
        '-DSHADOW_LTO=OFF', '-DSHADOW_PGO=OFF') + $opts
    if ($Python) { $a += "-DPython3_EXECUTABLE=$Python" }
    & cmake @a
    if ($LASTEXITCODE -ne 0) { throw "clang_pgo_build: configure of $t failed" }
    & cmake --build $t --target ShadowTheHedgehog -- -j $Jobs -k 0
    $code = $LASTEXITCODE
    $exe = Get-Item "$t\bin\ShadowTheHedgehog.exe" -ErrorAction SilentlyContinue
    if (-not $exe -or $exe.LastWriteTime -lt $started) { throw "clang_pgo_build: $t linked no executable (exit $code)" }
    if ($code -ne 0) { 'POSTLINK-CHECK-FAILED (exe linked; a post-link step returned an error)' }
    $exe.FullName
}

# The profile runtime's path may hold spaces, which CMAKE_EXE_LINKER_FLAGS cannot quote through every layer: a copy.
New-Item -ItemType Directory -Force "$Tree-gen-rt" | Out-Null
Copy-Item "$llvm\lib\clang\*\lib\windows\clang_rt.profile*.lib" "$Tree-gen-rt\" -Force
$rt = ((Get-ChildItem "$Tree-gen-rt" -Filter 'clang_rt.profile*.lib' | Select-Object -First 1).FullName) -replace '\\', '/'
$raw = "$Tree-gen-raw"
if (Test-Path $raw) { Remove-Item -Recurse -Force $raw }
$gen = (Build-Image "$Tree-gen" "/clang:-fprofile-generate=$($raw -replace '\\','/')" $rt | Select-Object -Last 1)

function Train([string]$Name, [string]$Stage, [string]$TapeDir, [string]$CardDir) {
    $d = "$Tree-gen\train\$Name"
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item "$CardDir\display.cfg", "$CardDir\pad.cfg", "$CardDir\card.raw" $d
    if (Test-Path "$CardDir\SRAM.pc.raw") { Copy-Item "$CardDir\SRAM.pc.raw" $d }
    $arguments = @('--game-data', ($Game -replace '\\', '/'), '--boot-stage', $Stage, '--headless', '--no-audio', '--memory-card', 'card.raw',
        '--display-cfg', 'display.cfg', '--pad-cfg', 'pad.cfg', '--state-file', 'unused.gekkostate', '--snapshot-key', 'none', '--no-xinput',
        '--budget-ms', "$BudgetMs", '--input-tape-play', ("$TapeDir\input.tape" -replace '\\', '/'), '--native-output', '1280x720',
        '--width', '1280', '--height', '720', '--json-report', 'session.json')
    $p = Start-Process -FilePath $gen -ArgumentList $arguments -WorkingDirectory $d -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput "$d\stdout.log" -RedirectStandardError "$d\stderr.log"
    "$Name exit $($p.ExitCode)"
    if ($p.ExitCode -ne 0) { throw "clang_pgo_build: training run $Name failed" }
}
$lh = "$Recordings\lethal-highway-sfx"
Train 'lethal-highway' 'stg0202' $lh "$lh\initial"
$uw = "$Recordings\user-westopolis-720p-20261109-092727"
Train 'user-westopolis' 'stg0100' $uw "$lh\initial"

$prof = "$Tree-gen\merged.profdata"
& "$llvm\bin\llvm-profdata.exe" merge -o $prof (Get-ChildItem $raw -Filter '*.profraw' | ForEach-Object FullName)
if ($LASTEXITCODE -ne 0) { throw 'clang_pgo_build: llvm-profdata merge failed' }
$exe = (Build-Image $Tree "/clang:-fprofile-use=$($prof -replace '\\','/') -Wno-backend-plugin" '' | Select-Object -Last 1)
(Get-FileHash $exe -Algorithm MD5).Hash.ToLower()
