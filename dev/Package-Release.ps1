<#
.SYNOPSIS
    Makes the release package (NukkoroRecomp-<version>.zip): the source at a commit, plus the prebuilt parts of the build
    that contain no game code.

.DESCRIPTION
    1. git archive of -Ref (the package every player gets; installer/revision.txt carries the commit).
    2. The package is unpacked into a staging folder and configured with the release preset and NO main.dol
       (-DSHADOW_RECOMP_DOL= -DSHADOW_PREBUILT_EXPORT=ON; CMake refuses the export with a main.dol), so no game input
       exists in that tree. The targets CMakeLists.txt lists in SHADOW_PREBUILT_LIBRARIES and SHADOW_PREBUILT_PROGRAMS
       are built and copied to prebuilt/<set>/ with prebuilt.json: the key a player's tree must match exactly to use
       them (compiler, Windows SDK, generator, flags, options, source hash), every file's SHA-256, and the toolchain.
    3. dev/scan_game_code.py checks every prebuilt file, and the mod manager's mods-data/ that the install copies,
       against the -ScanDisc disc (main.dol, sys/, all of files/):
       no shared byte run of 64 bytes or more, no defined translated function (fn_8xxxxxxx), no personal path.
       A failure stops the release.
    4. prebuilt/ is added to the zip.
    The setup exe is then made as before (docs/RELEASE_VALIDATION.md). -NoPrebuilt makes the plain source package.

.EXAMPLE
    powershell -File dev/Package-Release.ps1 -Version 0.0.4 -ScanDisc D:\Dumps\ShadowPAL
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Ref = 'HEAD',
    [string]$OutDir,
    [string]$ScanDisc,
    [ValidateRange(1, 64)][int]$Jobs = 8,
    # Where the package is unpacked and the prebuilt parts compiled: by default the folder the downloadable setup
    # unpacks this version into on a player's PC (%PUBLIC%\NukkoroRecomp\<version>). MSVC names anonymous namespaces
    # (?A0x<hash>, also in RTTI names) from each source file's full path, so compiling there gives the objects a
    # player's own build there would have. It must not exist; it is removed afterwards.
    [string]$StageDir,
    [switch]$NoPrebuilt
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo '.build\package' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath
$prefix = "NukkoroRecomp-$Version"
$zip = Join-Path $OutDir "$prefix.zip"

function Invoke-Native([string]$what, [string]$file, [string[]]$arguments) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $file @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

$commit = (git -C $repo rev-parse "$Ref^{commit}").Trim()
$base = Join-Path $OutDir "$prefix-source.zip"
Invoke-Native "Source package at $commit" 'git' @('-C', $repo, 'archive', '--format=zip', "--prefix=$prefix/", '-o', $base, $commit)
if ($NoPrebuilt) {
    Move-Item -LiteralPath $base -Destination $zip -Force
    Write-Host "Package: $zip (no prebuilt parts)"
    return
}
if (-not $ScanDisc) { throw '-ScanDisc <extracted PAL disc> is required: every prebuilt file is checked against it' }

# ---- build the prebuilt parts from the package itself, with no game input --------------------------------------------
if (-not $StageDir) { $StageDir = Join-Path $env:PUBLIC "NukkoroRecomp\$Version" }
if (Test-Path -LiteralPath $StageDir) { throw "$StageDir exists: remove it, or choose another -StageDir" }
$src = $StageDir
New-Item -ItemType Directory -Force -Path $src | Out-Null
$stage = Join-Path $OutDir "$prefix-stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Push-Location $src
try { Invoke-Native 'Unpack the source package' (Join-Path $env:SystemRoot 'System32\tar.exe') @('-xf', $base, '--strip-components=1') }
finally { Pop-Location }

$tree = Join-Path $src '.build\prebuilt-export'
Push-Location $src
try {
    Invoke-Native 'Configure (no main.dol)' 'cmake' @('--preset', 'release', '-B', $tree, '-DSHADOW_RECOMP_DOL=',
        '-DSHADOW_PREBUILT_EXPORT=ON')
} finally { Pop-Location }
$targets = [ordered]@{}
foreach ($line in [IO.File]::ReadAllLines((Join-Path $tree 'prebuilt-export\targets-Release.txt'))) {
    if ($line -match '^([^=]+)=(.+)$') { $targets[$Matches[1]] = $Matches[2] }
}
$buildArgs = @('--build', $tree, '--config', 'Release', '--parallel', "$Jobs", '--target') + @($targets.Keys) + @('--', '/nodeReuse:false')
Invoke-Native "Build $($targets.Count) prebuilt parts" 'cmake' $buildArgs

# ---- collect ---------------------------------------------------------------------------------------------------------
$key = @([IO.File]::ReadAllLines((Join-Path $tree 'prebuilt-export\key.txt')) | Where-Object { $_ })
$cache = @{}
foreach ($line in [IO.File]::ReadAllLines((Join-Path $tree 'CMakeCache.txt'))) {
    if ($line -match '^([A-Za-z0-9_]+):[A-Z]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
$compiler = ($key | Where-Object { $_ -like 'compiler=*' }) -replace '^compiler=', ''
$sdk = ($key | Where-Object { $_ -like 'windows_sdk=*' }) -replace '^windows_sdk=', ''
$setName = ('{0}-sdk{1}' -f ($compiler -replace '[^0-9A-Za-z.]+', '-'), $sdk).ToLowerInvariant()
$pkgRoot = Join-Path $stage 'package'
$setDir = Join-Path $pkgRoot "prebuilt\$setName"
New-Item -ItemType Directory -Force -Path $setDir | Out-Null
$files = @()
foreach ($target in $targets.Keys) {
    $built = $targets[$target]
    $name = $target + [IO.Path]::GetExtension($built)
    Copy-Item -LiteralPath $built -Destination (Join-Path $setDir $name)
    $item = Get-Item -LiteralPath (Join-Path $setDir $name)
    $files += [ordered]@{ target = $target; path = $name; size = $item.Length
                          sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$tools = Split-Path -Parent $cache['CMAKE_LINKER']
$manifest = [ordered]@{
    schema = 1
    about = 'Game-code-free parts of the Shadow the Hedgehog static recompilation (made with GekkoForge), compiled once by the release step from this package''s own sources with no game input. The player''s build uses them only when its key matches; see docs/BUILDING.md.'
    source_commit = $commit
    key = $key
    files = $files
    toolchain = [ordered]@{
        cl = (Get-Item -LiteralPath (Join-Path $tools 'cl.exe')).VersionInfo.FileVersion
        link = (Get-Item -LiteralPath $cache['CMAKE_LINKER']).VersionInfo.FileVersion
        lib = (Get-Item -LiteralPath (Join-Path $tools 'lib.exe')).VersionInfo.FileVersion
        msvc_tools = Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $tools)))
        windows_sdk = $sdk
        cmake = ((cmake --version | Select-Object -First 1) -replace '^cmake version ', '')
        generator = $cache['CMAKE_GENERATOR']
    }
}
[IO.File]::WriteAllText((Join-Path $setDir 'prebuilt.json'), ($manifest | ConvertTo-Json -Depth 5) + "`n", (New-Object Text.UTF8Encoding $false))

# ---- prove: no game code, no disc data -------------------------------------------------------------------------------
$scanReport = Join-Path $stage 'scan_game_code.json'
# The C runtime every program these tools link carries (its case tables match the game's own C library's): runs found
# there are reported as toolchain bytes, not counted against the parts.
$msvcLib = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $tools))) 'lib\x64'
$kits = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$references = @((Join-Path $kits "Lib\$sdk\ucrt\x64\libucrt.lib")) +
    @('libcmt.lib', 'libcpmt.lib', 'libvcruntime.lib' | ForEach-Object { Join-Path $msvcLib $_ })
$scanArgs = @((Join-Path $PSScriptRoot 'scan_game_code.py'), '--disc', $ScanDisc, '--dumpbin', (Join-Path $tools 'dumpbin.exe'),
    '--json', $scanReport)
foreach ($reference in $references) { $scanArgs += @('--toolchain-reference', $reference) }
# The mod manager's maps of the game (mods-data/, copied from test_data/mods by the build) are installed as they are:
# they are scanned with the prebuilt parts.
$modsData = Join-Path $tree 'bin\Release\mods-data'
if (-not (Test-Path -LiteralPath $modsData -PathType Container)) { throw "$modsData is missing: the mod manager's build did not copy it" }
Invoke-Native 'Scan the prebuilt parts and mods-data against the disc' 'python' ($scanArgs + @((Join-Path $pkgRoot 'prebuilt'), $modsData))

# ---- the package -----------------------------------------------------------------------------------------------------
Copy-Item -LiteralPath $base -Destination $zip -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, 'Update')
try {
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $pkgRoot 'prebuilt') -Recurse -File)) {
        $entry = "$prefix/" + ($file.FullName.Substring($pkgRoot.Length + 1) -replace '\\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, 'Optimal')
    }
} finally { $archive.Dispose() }
Remove-Item -LiteralPath $base -Force
Remove-Item -LiteralPath $src -Recurse -Force
Write-Host "Package: $zip ($([Math]::Round((Get-Item $zip).Length / 1MB, 1)) MB), prebuilt set $setName"
Write-Host "Scan report: $scanReport"
