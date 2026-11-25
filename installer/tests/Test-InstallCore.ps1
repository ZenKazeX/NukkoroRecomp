# Checks of the installer's own logic that need no build: run  powershell -File installer\tests\Test-InstallCore.ps1
# Exit code 0 when every check passes. Works in a scratch folder under %TEMP% and removes it.
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'InstallCore.psm1') -Force
$backend = Join-Path (Split-Path -Parent $PSScriptRoot) 'Backend.ps1'
$script:passed = 0; $script:failed = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:passed++; Write-Host "ok    $name" } else { $script:failed++; Write-Host "FAIL  $name  $detail" -ForegroundColor Red }
}
$scratch = Join-Path $env:TEMP ("nukkoro-installcore-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
try {
    # --- a failed step names its cause ---------------------------------------------------------------
    $log = Join-Path $scratch 'a.log'
    Set-Content -LiteralPath $log -Value @('Install started', '==== Configure: cmake --preset release', 'CMake Error: Could not create named generator Visual Studio 18 2026', '', 'Generators') -Encoding UTF8
    $cause = Get-StepFailureCause $log 0
    Check 'cause: an older CMake (issue 7)' ($cause -match 'Could not create named generator' -and $cause -match 'CMake 4\.2') $cause
    Set-Content -LiteralPath $log -Value @('-- Selecting Windows SDK', 'CMake Error at CMakeLists.txt:12 (message):', '  The disc image is not the PAL one.', '', '-- Configuring incomplete') -Encoding UTF8
    $cause = Get-StepFailureCause $log 0
    Check 'cause: a CMake message with its text' ($cause -match 'CMakeLists\.txt:12' -and $cause -match 'not the PAL one') $cause
    Set-Content -LiteralPath $log -Value @('  recomp_017.cpp', 'H:\x\recomp_017.cpp(1,1): fatal error C1060: compiler is out of heap space [H:\x\a.vcxproj]') -Encoding UTF8
    $cause = Get-StepFailureCause $log 0
    Check 'cause: the compiler out of memory' ($cause -match 'C1060' -and $cause -match 'ran out of memory') $cause
    Set-Content -LiteralPath $log -Value @('  a.cpp', '  b.cpp', 'Build succeeded.') -Encoding UTF8
    Check 'cause: nothing to say about a clean log' ((Get-StepFailureCause $log 0) -eq '')
    $offset = (Get-Item -LiteralPath $log).Length
    Add-Content -LiteralPath $log -Value 'LINK : fatal error LNK1104: cannot open file x.lib' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $scratch 'b.log') -Value 'CMake Error: an earlier step' -Encoding UTF8
    Check 'cause: only what the step wrote' ((Get-StepFailureCause $log $offset) -match 'LNK1104')

    # --- the install folder ---------------------------------------------------------------------------
    $disc = Join-Path $scratch 'disc'; New-Item -ItemType Directory -Force -Path $disc | Out-Null
    $target = Join-Path $scratch 'Játék (új) & more\Shadow'
    $r = Test-InstallFolder $target $disc 1MB
    Check 'target: a new folder with spaces, accents, brackets and an ampersand' ($r.Ok -and -not $r.Existing) $r.Message
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    $exe = Join-Path $target 'ShadowTheHedgehog.exe'
    [System.IO.File]::WriteAllBytes($exe, [byte[]](1, 2, 3))
    $r = Test-InstallFolder $target $disc 1MB
    Check 'target: an existing install is an update' ($r.Ok -and $r.Existing) $r.Message
    $held = [System.IO.File]::Open($exe, 'Open', 'Read', 'Read')
    try { $r = Test-InstallFolder $target $disc 1MB } finally { $held.Dispose() }
    Check 'target: a running game is refused, by name' (-not $r.Ok -and $r.Message -match 'ShadowTheHedgehog\.exe is running') $r.Message
    $r = Test-InstallFolder $target $disc 1MB
    Check 'target: accepted again once it is closed' $r.Ok $r.Message
    Check 'target: the disc folder itself is refused' (-not (Test-InstallFolder (Join-Path $disc 'in') $disc 1MB).Ok)
    Check 'target: a drive root is refused' (-not (Test-InstallFolder 'C:\' $disc 1MB).Ok)

    # --- the backend, started the way the setup window starts it ---------------------------------------
    function Invoke-Backend { $out = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $backend @args 2>$null; [pscustomobject]@{ Exit = $LASTEXITCODE; Out = @($out) } }
    $missing = Join-Path $scratch 'no such disc'
    $b = Invoke-Backend install $missing $target 0 0 none none
    $parsed = $null; try { $parsed = $b.Out[-1] | ConvertFrom-Json } catch {}
    Check 'backend: install without a Japanese disc answers in JSON' ($b.Exit -eq 1 -and $parsed -and $parsed.type -eq 'error') "exit $($b.Exit): $($b.Out -join ' ')"
    $b = Invoke-Backend check-target $target $disc
    $parsed = $null; try { $parsed = $b.Out[-1] | ConvertFrom-Json } catch {}
    Check 'backend: check-target with an accented path' ($b.Exit -eq 0 -and $parsed -and $parsed.ok -and $parsed.path -eq $target) "exit $($b.Exit): $($b.Out -join ' ')"
    $b = Invoke-Backend check-disc $missing
    $parsed = $null; try { $parsed = $b.Out[-1] | ConvertFrom-Json } catch {}
    Check 'backend: check-disc of a missing folder' ($b.Exit -eq 0 -and $parsed -and -not $parsed.ok -and $parsed.message -match 'Folder not found') "exit $($b.Exit): $($b.Out -join ' ')"
    $b = Invoke-Backend prereqs
    $parsed = $null; try { $parsed = $b.Out[-1] | ConvertFrom-Json } catch {}
    Check 'backend: prereqs lists three tools' ($b.Exit -eq 0 -and @($parsed).Count -eq 3) "exit $($b.Exit): $($b.Out -join ' ')"
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$script:passed passed, $script:failed failed"
if ($script:failed) { exit 1 }
