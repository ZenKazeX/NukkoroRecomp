# Developer build: configure once, rebuild after every edit, optionally run the result.
#
#   Build.cmd                          build (asks for the disc folder the first time, then remembers it)
#   Build.cmd -Run                     build, then start the game with --game-data <disc folder>
#   Build.cmd -Run --boot-stage stg0100 --fullscreen
#                                      anything the script does not know is passed to the game
#   Build.cmd -Disc D:\Dumps\Shadow    use (and remember) another disc folder
#   Build.cmd -Target ppcrecomp        build one target
#   Build.cmd -Reconfigure             run CMake's configure step again
#   Build.cmd -Clean                   delete .build\release, then build from scratch
#   Build.cmd -Jobs 4                  parallel compile jobs (default 2; the recompiled code is large and runs hot)
#
# Output: .build\release\bin\Release\ShadowTheHedgehog.exe. Settings, saves and the memory card of a -Run game
# live next to that exe, apart from any installed copy. Nothing is deployed or installed.

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Disc,
    [switch]$Run,
    [string]$Target,
    [switch]$Reconfigure,
    [switch]$Clean,
    [ValidateRange(1, 64)][int]$Jobs = 2,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot '..\installer\InstallCore.psm1') -Force
$root = Get-RepoRoot
Set-Location -LiteralPath $root  # the presets are found from the working directory
$buildDir = Join-Path $root '.build\release'
$discFile = Join-Path $root '.build\dev-disc.txt'
$exe = Get-BuiltExe

function Fail([string]$message) {
    Write-Host $message -ForegroundColor Red
    exit 1
}

function Invoke-Checked([string]$what, [string[]]$arguments) {
    Write-Host "> cmake $($arguments -join ' ')" -ForegroundColor DarkGray
    & cmake @arguments
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit $LASTEXITCODE)." }
}

if ($GameArgs -and -not $Run) { Fail "Unknown option(s): $($GameArgs -join ' '). Game options need -Run." }

# ---- tools ----
$missing = @(Test-Prerequisites | Where-Object { -not $_.Ok })
foreach ($item in $missing) { Write-Host "Missing: $($item.Name) - $($item.Detail) ($($item.Url))" -ForegroundColor Red }
if ($missing) { exit 1 }
$python = (Test-Prerequisites | Where-Object { $_.Name -eq 'Python 3' }).Value

# ---- disc folder: -Disc, else the remembered one, else ask ----
$saved = if (Test-Path -LiteralPath $discFile) { (Get-Content -LiteralPath $discFile -Raw).Trim() } else { '' }
if (-not $Disc) { $Disc = $saved }
if (-not $Disc) {
    Write-Host 'The build needs your extracted PAL disc (Dolphin: Properties > Filesystem > Extract Entire Disc).'
    $Disc = Read-Host 'Disc folder (holds sys and files)'
}
$check = Test-DiscFolder $Disc
if (-not $check.Ok) { Fail $check.Message }
$discRoot = $check.Root
New-Item -ItemType Directory -Force (Split-Path -Parent $discFile) | Out-Null
if ($discRoot -ne $saved) { Set-Content -LiteralPath $discFile -Value $discRoot -Encoding UTF8 }
Write-Host "Disc: $discRoot ($($check.Message))"

# ---- a running build output cannot be overwritten ----
$running = @(Get-Process -Name ShadowTheHedgehog* -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Path -eq $exe } catch { $false } })
if ($running) { Fail "The game built here is running (PID $($running[0].Id)). Close it, then build again." }

$started = Get-Date

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

# ---- configure when there is no build tree, the disc changed, or on request ----
# The preset's own SHADOW_RECOMP_DOL points inside the repository, so the disc's DOL is always passed explicitly.
$dol = (Join-Path $discRoot 'sys\main.dol') -replace '\\', '/'
$cache = Join-Path $buildDir 'CMakeCache.txt'
$cachedDol = if (Test-Path -LiteralPath $cache) {
    (Select-String -LiteralPath $cache -Pattern '^SHADOW_RECOMP_DOL:[A-Z]+=(.*)$' | Select-Object -First 1 |
        ForEach-Object { $_.Matches[0].Groups[1].Value })
}
if ($Reconfigure -or -not $cachedDol -or $cachedDol -ne $dol) {
    Invoke-Checked 'Configure' @('--preset', 'release', "-DSHADOW_RECOMP_DOL=$dol",
        "-DSHADOW_RECOMP_COMPILE_JOBS=$Jobs", "-DPython3_EXECUTABLE=$python")
}

# ---- build ----
$buildArgs = @('--build', '--preset', 'release', '--parallel', "$Jobs")
if ($Target) { $buildArgs += @('--target', $Target) }
$buildArgs += @('--', '/nodeReuse:false', '/verbosity:minimal')
Invoke-Checked 'Build' $buildArgs

$elapsed = (Get-Date) - $started
Write-Host ("Build succeeded in {0:mm\:ss}." -f $elapsed) -ForegroundColor Green
if (-not $Target) { Write-Host "  $exe" }

# ---- run ----
if ($Run) {
    if (-not (Test-Path -LiteralPath $exe)) { Fail "No game executable at $exe." }
    $arguments = @('--game-data', $discRoot) + @($GameArgs | Where-Object { $_ })
    Write-Host "> ShadowTheHedgehog.exe $($arguments -join ' ')" -ForegroundColor DarkGray
    Push-Location (Split-Path -Parent $exe)
    try { & $exe @arguments } finally { Pop-Location }
    exit $LASTEXITCODE
}
