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
    $target = Join-Path $scratch ("Caf" + [char]0xE9 + " (na" + [char]0xEF + "ve) & more\Shadow")
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

    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        $r = Test-InstallFolder (Join-Path $env:ProgramFiles 'Nukkoro Write Test\Shadow') $disc 1MB
        Check 'target: a folder Windows does not let this user write in is refused, in plain words' (-not $r.Ok -and $r.Message -match 'does not let you write') $r.Message
    }
    Check 'target: no probe file is left behind' (@(Get-ChildItem -LiteralPath $scratch -Recurse -Force -Filter '.nukkoro-write-test-*').Count -eq 0)
    Check 'tools: a Windows SDK is seen on this PC' (Test-WindowsSdk)
    $older = $null; $py = Find-Python3 ([ref]$older)
    Check 'tools: the Python found is 3.10 or newer' ($py -and ((& $py -c 'import sys; print(sys.version_info >= (3, 10))') -eq 'True')) "$py"

    # --- a Python under a folder with letters outside ASCII (C:\Users\<such a name>): its path must arrive whole ----
    $venv = Join-Path $scratch ("J" + [char]0xF6 + "rg " + [char]0x151 + "\venv")
    & $py -m venv --without-pip $venv 2>$null | Out-Null
    $vexe = Join-Path $venv 'Scripts\python.exe'
    if (Test-Path -LiteralPath $vexe) {
        $info = Get-PythonInfo $vexe
        Check 'tools: the path of a Python in a non-ASCII folder is read back exactly' ($info -and $info.Exe -ieq $vexe -and (Test-Path -LiteralPath $info.Exe)) "$($info.Exe)"
        $asWindow = & powershell.exe -NoProfile -Command "[Console]::OutputEncoding = [Text.UTF8Encoding]::new(`$false); Import-Module '$((Join-Path (Split-Path -Parent $PSScriptRoot) 'InstallCore.psm1'))' -Force; (Get-PythonInfo '$vexe').Exe -ieq '$vexe'"
        Check 'tools: the same when read as the setup window reads it (UTF-8)' ("$asWindow" -eq 'True') "$asWindow"
        $env:NUKKORO_PYTHON = $vexe
        try { Check 'tools: NUKKORO_PYTHON names the interpreter to use' ((Find-Python3) -ieq $vexe) } finally { $env:NUKKORO_PYTHON = $null }
    } else { Check 'tools: a test Python could be made in a non-ASCII folder' $false }
    Check 'tools: something that is not Python is not taken for one' ($null -eq (Get-PythonInfo (Join-Path $env:SystemRoot 'System32\whoami.exe')))
    Check 'tools: no Visual Studio installer is running, so nothing is waited for' ((Wait-VisualStudioInstaller) -le 1)
    $semi = Join-Path $scratch 'disc; copy'; New-Item -ItemType Directory -Force -Path $semi | Out-Null
    Check 'disc: a folder with a semicolon is refused, with the reason' ((Test-DiscFolder $semi).Message -match 'semicolon')
    $unc = Test-InstallFolder '\\nukkoro-test-server\share\Shadow' $disc 1MB
    Check 'target: a network folder that cannot be reached is refused in plain words, without an error' (-not $unc.Ok -and $unc.Message -match 'Cannot reach') $unc.Message
    $share = Test-InstallFolder ('\\localhost\' + $scratch.Substring(0,1) + '$' + $scratch.Substring(2) + '\net target') $disc 1MB
    Check 'target: a network folder that can be reached is accepted (free space not checked)' ($share.Ok -and $share.Message -match 'network folder') $share.Message
    # --- the temporary build files: removed only in a folder the setup exe unpacked ------------------------
    $pkg = Join-Path $scratch 'NukkoroRecomp\9.9.9'
    New-Item -ItemType Directory -Force -Path "$pkg\installer", "$pkg\.build\release\sub", "$pkg\.build\setup" | Out-Null
    Copy-Item (Join-Path (Split-Path -Parent $PSScriptRoot) 'InstallCore.psm1') "$pkg\installer\InstallCore.psm1"
    Set-Content "$pkg\.build\release\sub\big.bin" ('x' * 5000) -Encoding ASCII; Set-Content "$pkg\.build\install.log" 'log' -Encoding ASCII
    $old1 = Join-Path $scratch 'NukkoroRecomp\0.0.1'; $old2 = Join-Path $scratch 'NukkoroRecomp\0.0.2'; $other = Join-Path $scratch 'NukkoroRecomp\notes'
    New-Item -ItemType Directory -Force -Path $old1, $old2, $other | Out-Null
    Set-Content "$old1\.unpacked" 'unpacked' -Encoding ASCII; Set-Content "$old1\file.bin" 'x' -Encoding ASCII; Set-Content "$old2\mine.txt" 'x' -Encoding ASCII; Set-Content "$other\.unpacked" 'x' -Encoding ASCII
    $clean = { param($module, $keep) $env:NUKKORO_KEEP_BUILD = $keep; Import-Module $module -Force; Remove-BuildFiles }
    $run = { param($keep) $ps = [powershell]::Create().AddScript($clean).AddArgument("$pkg\installer\InstallCore.psm1").AddArgument($keep); try { [double]($ps.Invoke() | Select-Object -Last 1) } finally { $ps.Dispose() } }
    $freed = & $run ''
    Check 'build files: nothing is removed in a folder the setup exe did not unpack (no marker)' ($freed -eq 0 -and (Test-Path "$pkg\.build\release\sub\big.bin") -and (Test-Path $old1))
    Set-Content "$pkg\.unpacked" 'unpacked x' -Encoding ASCII
    $freed = & $run ''
    Check 'build files: nothing is removed without a packaged revision (a clone)' ($freed -eq 0 -and (Test-Path "$pkg\.build\release\sub\big.bin"))
    Set-Content "$pkg\installer\revision.txt" ('a' * 40) -Encoding ASCII
    $freed = & $run '1'
    Check 'build files: NUKKORO_KEEP_BUILD=1 keeps them' ($freed -eq 0 -and (Test-Path "$pkg\.build\release\sub\big.bin"))
    $freed = & $run ''
    Check 'build files: removed after an install from an unpacked package, the log stays' ($freed -gt 5000 -and -not (Test-Path "$pkg\.build\release") -and -not (Test-Path "$pkg\.build\setup") -and (Test-Path "$pkg\.build\install.log") -and (Test-Path "$pkg\installer\InstallCore.psm1"))
    Check 'build files: an earlier version the setup exe unpacked is removed' (-not (Test-Path $old1))
    Check 'build files: other folders beside it are left alone' ((Test-Path "$old2\mine.txt") -and (Test-Path "$other\.unpacked"))
    Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'InstallCore.psm1') -Force
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
    # --- fetching a missing tool: download, signature check, start, wait (a local server stands in for the vendor) ---
    $plans = @('vs', 'cmake', 'python', 'webview2' | ForEach-Object { Get-ToolPlan $_ })
    Check 'tools: every tool has a plan with an https address or an installer on this PC, and runs unattended' (@($plans | Where-Object { $_ -and $_.Text -and ($_.Modify -or $_.Url -like 'https://*') -and $_.Arguments -match '/passive|--passive|/silent' }).Count -eq 4)
    Check 'tools: an unknown tool has none' ($null -eq (Get-ToolPlan 'other'))
    $signed = Join-Path $env:SystemRoot 'System32\whoami.exe'        # signed by Microsoft, harmless to run
    $unsigned = Join-Path $scratch 'unsigned.exe'; [System.IO.File]::WriteAllBytes($unsigned, [System.IO.File]::ReadAllBytes($signed)[0..4095])
    $port = 0
    $listener = $null
    foreach ($candidate in 47811..47830) { try { $listener = New-Object System.Net.HttpListener; $listener.Prefixes.Add("http://127.0.0.1:$candidate/"); $listener.Start(); $port = $candidate; break } catch { $listener = $null } }
    if ($listener) {
        $server = [powershell]::Create().AddScript({ param($listener, $signed, $unsigned)
            while ($listener.IsListening) {
                try { $context = $listener.GetContext() } catch { break }
                $file = if ($context.Request.Url.AbsolutePath -eq '/signed.exe') { $signed } elseif ($context.Request.Url.AbsolutePath -eq '/unsigned.exe') { $unsigned } else { $null }
                if ($file) { $bytes = [System.IO.File]::ReadAllBytes($file); $context.Response.OutputStream.Write($bytes, 0, $bytes.Length) } else { $context.Response.StatusCode = 404 }
                $context.Response.Close()
            } }).AddArgument($listener).AddArgument($signed).AddArgument($unsigned)
        $pending = $server.BeginInvoke()
        try {
            $plan = [pscustomobject]@{ Key = 't'; Name = 'Test tool'; Url = "http://127.0.0.1:$port/signed.exe"; File = 'nukkoro-test-signed.exe'; Signer = 'Microsoft Corporation'; Arguments = '/?'; OkCodes = @(0, 1); Modify = $null; Text = 't'; Marker = $null }
            $r = Install-Tool 't' $plan
            Check 'tools: a file signed by the named vendor is run and waited for' ($r.Ok -and $null -ne $r.ExitCode) $r.Message
            $plan.OkCodes = @(99)
            $r = Install-Tool 't' $plan
            Check 'tools: an installer that ends with a failure code is reported' (-not $r.Ok -and $r.Message -match 'ended with code') $r.Message
            $plan.OkCodes = @(0, 1)
            # the whole automatic step, with the stand-in: Python "missing", fetched, checked again
            $done = Join-Path $env:TEMP 'NukkoroSetup\selftest-tool.done'; Remove-Item $done -Force -ErrorAction SilentlyContinue
            $env:NUKKORO_TEST_MISSING = 'python'; $env:NUKKORO_TEST_TOOL_URL = "http://127.0.0.1:$port/signed.exe"
            try {
                $before = @(Test-Prerequisites | Where-Object { -not $_.Ok } | ForEach-Object Key)
                $lines = New-Object System.Collections.ArrayList
                $m = Install-MissingTools { param($e) [void]$lines.Add($e.text) }
                Check 'tools: the automatic step installs what is missing and checks again' ($before -contains 'python' -and $m.Ok -and @($m.Prerequisites | Where-Object { -not $_.Ok }).Count -eq 0 -and $lines.Count -ge 2) "$($m.Message) | $($lines -join ' ')"
                Remove-Item $done -Force -ErrorAction SilentlyContinue
                $env:NUKKORO_TEST_TOOL_URL = "http://127.0.0.1:$port/unsigned.exe"
                $m = Install-MissingTools
                Check 'tools: the automatic step stops on a file it must not run' (-not $m.Ok -and $m.Message -match 'not signed') $m.Message
            } finally { $env:NUKKORO_TEST_MISSING = $null; $env:NUKKORO_TEST_TOOL_URL = $null; Remove-Item $done -Force -ErrorAction SilentlyContinue }
            $plan.Signer = 'Python Software Foundation'
            $r = Install-Tool 't' $plan
            Check 'tools: a file signed by somebody else is refused and deleted' (-not $r.Ok -and $r.Message -match 'not signed by Python Software Foundation' -and -not (Test-Path (Join-Path $env:TEMP 'NukkoroSetup\nukkoro-test-signed.exe'))) $r.Message
            $plan.Signer = 'Microsoft Corporation'; $plan.Url = "http://127.0.0.1:$port/unsigned.exe"; $plan.File = 'nukkoro-test-unsigned.exe'
            $r = Install-Tool 't' $plan
            Check 'tools: an unsigned file is refused and deleted' (-not $r.Ok -and $r.Message -match 'not signed' -and -not (Test-Path (Join-Path $env:TEMP 'NukkoroSetup\nukkoro-test-unsigned.exe'))) $r.Message
            $plan.Url = "http://127.0.0.1:$port/missing.exe"
            $r = Install-Tool 't' $plan
            Check 'tools: a failed download is reported' (-not $r.Ok -and $r.Message -match 'download of Test tool failed') $r.Message
        } finally { $listener.Stop(); $server.Dispose(); Remove-Item (Join-Path $env:TEMP 'NukkoroSetup\nukkoro-test-*.exe') -Force -ErrorAction SilentlyContinue }
    } else { Check 'tools: a local test server could be started' $false }
    $b = Invoke-Backend get-tool other
    $parsed = $null; try { $parsed = $b.Out[-1] | ConvertFrom-Json } catch {}
    Check 'backend: get-tool refuses an unknown tool' ($parsed -and -not $parsed.ok -and $parsed.message -match 'Unknown tool') "exit $($b.Exit): $($b.Out -join ' ')"
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$script:passed passed, $script:failed failed"
if ($script:failed) { exit 1 }
