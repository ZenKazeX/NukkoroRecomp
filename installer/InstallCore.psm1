# Installer logic shared by the setup window (Backend.ps1) and the scripted mode (Install.ps1).
# Nothing here touches the user interface; long steps run as child processes that write to a log file,
# so the caller decides how to wait (a WinForms timer, or a console loop).

Set-StrictMode -Version 2.0

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:Shards = 48
$script:GameExe = 'ShadowTheHedgehog.exe'
$script:ConfigExe = 'ShadowPadConfig.exe'
$script:ModManagerExe = 'ShadowModManager.exe'
# Everything the build puts in the install folder, in one place: the programs the build writes beside the game
# (.build\release\bin\Release). The copy step, the install record and the uninstaller all follow this list.
$script:InstalledPrograms = @($script:GameExe, $script:ConfigExe, $script:ModManagerExe)
# Folders copied as they are into the install folder (source folder relative to the package root = destination
# relative to the install folder). mods-data: the mod manager's maps of the game, which the build puts beside it.
$script:InstalledFolders = [ordered]@{ '.build\release\bin\Release\mods-data' = 'mods-data' }

function Get-RepoRoot { $script:RepoRoot }

function Get-BuiltExe { Join-Path $script:RepoRoot ".build\release\bin\Release\$script:GameExe" }

function Get-DefaultInstallFolder { Join-Path $env:USERPROFILE 'Games\Shadow the Hedgehog Recompiled' }

function Format-Size([double]$bytes) {
    if ($bytes -ge 1GB) { return '{0:N1} GB' -f ($bytes / 1GB) }
    return '{0:N0} MB' -f ($bytes / 1MB)
}

# ---- prerequisites -----------------------------------------------------------

function Find-Python3 {
    foreach ($candidate in @(@('py', '-3'), @('python'), @('python3'))) {
        $command = Get-Command $candidate[0] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $command) { continue }
        # The real interpreter path (py.exe is only a launcher), for CMake's FindPython3.
        $arguments = @($candidate | Select-Object -Skip 1) + @('-c', 'import sys; print(sys.version_info[0], sys.executable)')
        try {
            $answer = & $command.Source @arguments 2>$null | Select-Object -First 1
            if ($answer -match '^3 (.+)$') { return $Matches[1].Trim() }
        } catch {}
    }
    # A Python that is installed but not on this process's PATH: one installed a moment ago (the PATH of a running
    # program does not change), or one installed without "Add python.exe to PATH". The registry names them.
    $installed = @()
    foreach ($hive in 'HKCU:\Software\Python\PythonCore', 'HKLM:\Software\Python\PythonCore') {
        foreach ($key in @(Get-ChildItem -Path $hive -ErrorAction SilentlyContinue)) {
            if ($key.PSChildName -notmatch '^3\.(\d+)') { continue }
            $minor = [int]$Matches[1]
            $path = (Get-ItemProperty -Path (Join-Path $key.PSPath 'InstallPath') -ErrorAction SilentlyContinue).'(default)'
            if ($path) { $installed += [pscustomobject]@{ Minor = $minor; Exe = Join-Path $path 'python.exe' } }
        }
    }
    foreach ($candidate in ($installed | Sort-Object Minor -Descending)) {
        if (-not (Test-Path -LiteralPath $candidate.Exe -PathType Leaf)) { continue }
        try {
            $answer = & $candidate.Exe -c 'import sys; print(sys.version_info[0], sys.executable)' 2>$null | Select-Object -First 1
            if ($answer -match '^3 (.+)$') { return $Matches[1].Trim() }
        } catch {}
    }
    return $null
}

# Any Visual Studio 2026 on this PC, with or without the C++ tools: its install folder, or $null.
function Find-AnyVisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    return (& $vswhere -version '[18.0,19.0)' -prerelease -products * -property installationPath 2>$null | Select-Object -First 1)
}

# ---- getting a missing tool ------------------------------------------------------
#
# The setup can fetch a missing tool for the player: it downloads the vendor's own installer from the vendor's own
# address, checks that the file is signed by that vendor, and opens it. The vendor's installer shows its own window
# and licence; nothing is installed silently.

$script:CppWorkload = 'Microsoft.VisualStudio.Workload.NativeDesktop'
$script:Tools = @{
    'vs'       = @{ Name = 'Visual Studio 2026 Community'; Url = 'https://aka.ms/vs/18/stable/vs_community.exe'; File = 'vs_community.exe'
                    Signer = 'Microsoft Corporation'; Arguments = "--add $script:CppWorkload --includeRecommended --wait" }
    'python'   = @{ Name = 'Python 3.13'; Url = 'https://www.python.org/ftp/python/3.13.9/python-3.13.9-amd64.exe'; File = 'python-3.13.9-amd64.exe'
                    Signer = 'Python Software Foundation'; Arguments = 'PrependPath=1 Include_launcher=1' }
    'webview2' = @{ Name = 'Microsoft Edge WebView2 Runtime'; Url = 'https://go.microsoft.com/fwlink/p/?LinkId=2124703'; File = 'MicrosoftEdgeWebview2Setup.exe'
                    Signer = 'Microsoft Corporation'; Arguments = '/install' }
}

# What "Install it for me" does for a tool, in words the setup window shows before it starts.
function Get-ToolPlan([string]$key) {
    if ($key -eq 'cmake') { $key = 'vs' }   # CMake comes with Visual Studio's C++ workload
    if (-not $script:Tools.ContainsKey($key)) { return $null }
    $tool = $script:Tools[$key]
    $plan = [pscustomobject]@{ Key = $key; Name = $tool.Name; Url = $tool.Url; File = $tool.File; Signer = $tool.Signer
                               Arguments = $tool.Arguments; Modify = $null; Text = '' }
    if ($key -eq 'vs') {
        $existing = Find-AnyVisualStudio
        $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        if ($existing -and (Test-Path -LiteralPath $installer -PathType Leaf)) {
            $plan.Modify = $installer
            $plan.Arguments = "modify --installPath `"$existing`" --add $script:CppWorkload --includeRecommended"
            $plan.Text = "Opens the Visual Studio Installer to add 'Desktop development with C++' (it brings CMake) to the Visual Studio 2026 in $existing. Confirm there with Modify."
            return $plan
        }
        $plan.Text = "Downloads Microsoft's Visual Studio 2026 Community installer (about 6 MB, from aka.ms) and opens it with 'Desktop development with C++' selected (it brings CMake). The installer then downloads several GB. Microsoft's licence terms apply; confirm there with Install."
    } elseif ($key -eq 'python' -and $env:NUKKORO_TEST_MISSING -and $env:NUKKORO_TEST_TOOL_URL) {
        # Test hook (the setup window's self-test clicks through a fetch): a stand-in served by the test, still signature-checked.
        $plan.Url = $env:NUKKORO_TEST_TOOL_URL; $plan.File = 'nukkoro-selftest-tool.exe'; $plan.Signer = 'Microsoft Corporation'; $plan.Arguments = '/?'
        $plan.Text = 'Self-test stand-in: downloads a harmless signed program from the test and runs it.'
    } elseif ($key -eq 'python') {
        $plan.Text = "Downloads Python 3.13 (about 27 MB, from python.org) and opens its installer with 'Add python.exe to PATH' selected. Confirm there with Install Now."
    } else {
        $plan.Text = "Downloads Microsoft's WebView2 Runtime installer (about 2 MB) and runs it."
    }
    return $plan
}

# Downloads (unless the tool's own installer is already on this PC), checks the signature, opens the installer and
# waits for it. Returns Ok / Message; whether the tool is there afterwards is the prerequisite check's job.
function Install-Tool([string]$key, $plan = $null, [switch]$DownloadOnly) {
    $r = [pscustomobject]@{ Ok = $false; Message = ''; ExitCode = $null; File = $null }
    if (-not $plan) { $plan = Get-ToolPlan $key }   # a test passes its own plan
    if (-not $plan) { $r.Message = "Unknown tool: $key"; return $r }
    $program = $plan.Modify
    if (-not $program) {
        $folder = Join-Path $env:TEMP 'NukkoroSetup'
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $program = Join-Path $folder $plan.File
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $previous = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
            try { Invoke-WebRequest -Uri $plan.Url -OutFile $program -UseBasicParsing } finally { $ProgressPreference = $previous }
        } catch { $r.Message = "The download of $($plan.Name) failed: $($_.Exception.Message) Check the internet connection, or get it from the website."; return $r }
        $signature = Get-AuthenticodeSignature -LiteralPath $program
        $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
        if ($signature.Status -ne 'Valid' -or $subject -notmatch ('(^|,\s*)O=' + [regex]::Escape($plan.Signer) + '(,|$)')) {
            Remove-Item -LiteralPath $program -Force -ErrorAction SilentlyContinue
            $r.Message = "The downloaded file is not signed by $($plan.Signer) ($($signature.Status)); it was deleted and not started."; return $r
        }
    }
    $r.File = $program
    if ($DownloadOnly) {   # a check of the address and the signature: nothing is started
        $r.Ok = $true; $r.Message = "$($plan.Name): $program is there and signed by $($plan.Signer)."; return $r
    }
    try {
        $process = Start-Process -FilePath $program -ArgumentList $plan.Arguments -PassThru
        $process.WaitForExit()
        $r.ExitCode = $process.ExitCode
    } catch { $r.Message = "Could not start the installer of $($plan.Name): $($_.Exception.Message)"; return $r }
    $r.Ok = $true
    $r.Message = "The installer of $($plan.Name) has closed (code $($r.ExitCode))."
    return $r
}

# Visual Studio 2026 with the C++ tools: its install folder, or $null.
function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    # A released Visual Studio first; then the preview ("Insiders") channel, which vswhere leaves out unless asked.
    foreach ($channel in @(@(), @('-prerelease'))) {
        $path = & $vswhere -version '[18.0,19.0)' @channel -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
        if ($path) { return $path }
    }
    return $null
}

# The generator the release preset names (CMakePresets.json). Only a CMake that knows it can configure the build:
# CMake 4.2 was the first. An older CMake fails at the first step with "Could not create named generator".
$script:Generator = 'Visual Studio 18 2026'

# The first line of `cmake --version` and whether this cmake.exe knows $script:Generator.
function Test-CMake([string]$path) {
    $version = ''; $knows = $false
    try {
        $version = [string](& $path --version 2>$null | Select-Object -First 1)
        $knows = [bool](& $path --help 2>$null | Select-String -SimpleMatch $script:Generator -Quiet)
    } catch {}
    return [pscustomobject]@{ Path = $path; Version = $version; Knows = $knows }
}

# Every cmake.exe the setup can use, in the order it tries them: the one Visual Studio installs with its C++ tools
# ("C++ CMake tools for Windows": it always knows its own Visual Studio and is on PATH only inside Visual Studio's
# command prompt), the ones on PATH, a default CMake install.
function Find-CMakeCandidates([string]$vsPath) {
    $candidates = @()
    if ($vsPath) { $candidates += Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
    $candidates += @(Get-Command cmake -CommandType Application -All -ErrorAction SilentlyContinue | ForEach-Object Source)
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) { if ($root) { $candidates += Join-Path $root 'CMake\bin\cmake.exe' } }
    $seen = @{}
    return @($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) -and -not $seen.ContainsKey($_.ToLowerInvariant()) } |
        ForEach-Object { $seen[$_.ToLowerInvariant()] = $true; $_ })
}

function Test-Prerequisites {
    $results = @()

    $vsPath = Find-VisualStudio
    $found = @(Find-CMakeCandidates $vsPath | ForEach-Object { Test-CMake $_ })
    $cmake = $found | Where-Object { $_.Knows } | Select-Object -First 1
    if ($cmake) {
        $cmakeDetail = "$($cmake.Version) ($($cmake.Path))"
        # The build steps start `cmake` by name: the chosen one goes first on this process's PATH (and its children's),
        # in front of an older CMake that may be there.
        $folder = Split-Path -Parent $cmake.Path
        if ((($env:PATH -split ';') | Select-Object -First 1) -ne $folder) { $env:PATH = "$folder;$env:PATH" }
    } elseif ($found.Count -gt 0) {
        $cmakeDetail = "$($found[0].Version) at $($found[0].Path) is too old: it does not know $script:Generator. Install CMake 4.2 or newer, " +
                       "or add 'C++ CMake tools for Windows' to Visual Studio 2026 in the Visual Studio Installer"
    } else {
        $cmakeDetail = "not found. Add 'C++ CMake tools for Windows' to Visual Studio 2026 in the Visual Studio Installer, or install CMake 4.2 or newer"
    }
    $results += [pscustomobject]@{ Name = 'CMake 4.2+'; Ok = [bool]$cmake; Detail = $cmakeDetail; Url = 'https://cmake.org/download/'; Value = $(if ($cmake) { $cmake.Path }); Key = 'cmake' }
    $python = Find-Python3
    $results += [pscustomobject]@{ Name = 'Python 3'; Ok = [bool]$python; Detail = $(if ($python) { $python } else { 'not found (PATH and the registry were searched)' }); Url = 'https://www.python.org/downloads/'; Value = $python; Key = 'python' }


    $results += [pscustomobject]@{ Name = 'Visual Studio 2026 C++ tools'; Ok = [bool]$vsPath; Detail = $(if ($vsPath) { $vsPath } else { "'Desktop development with C++' workload not found" }); Url = 'https://visualstudio.microsoft.com/downloads/'; Value = $vsPath; Key = 'vs' }

    # Test hook (the setup window's self-test): NUKKORO_TEST_MISSING=python,vs reports those tools as missing.
    if ($env:NUKKORO_TEST_MISSING) {
        foreach ($entry in $results) { if (($env:NUKKORO_TEST_MISSING -split ',') -contains $entry.Key) { $entry.Ok = $false; $entry.Detail = 'reported missing for a test (NUKKORO_TEST_MISSING)' } }
    }
    return $results
}

# ---- the disc folder ---------------------------------------------------------

# Accepts the extraction root, or a folder inside it the user may pick by mistake (sys, files, DATA).
function Resolve-DiscFolder([string]$path) {
    if (-not $path) { return $null }
    $path = $path.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { return $path }
    $full = (Resolve-Path -LiteralPath $path).ProviderPath.TrimEnd('\')
    foreach ($candidate in @($full, (Join-Path $full 'DATA'), (Split-Path -Parent $full))) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'sys\main.dol') -PathType Leaf)) { return $candidate }
    }
    return $full
}

function New-Result([bool]$ok, [string]$message) {
    [pscustomobject]@{ Ok = $ok; Message = $message; GameId = $null; Sha1 = $null; Root = $null }
}

function Test-DiscFolder([string]$path) {
    $root = Resolve-DiscFolder $path
    if (-not $root) { return New-Result $false 'Choose the folder of the extracted disc.' }
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return New-Result $false "Folder not found: $root" }

    $dol = Join-Path $root 'sys\main.dol'
    $boot = Join-Path $root 'sys\boot.bin'
    if (-not (Test-Path -LiteralPath $dol -PathType Leaf) -or -not (Test-Path -LiteralPath $boot -PathType Leaf)) {
        return New-Result $false "No sys\main.dol and sys\boot.bin here. Choose the folder Dolphin's 'Extract Entire Disc' wrote (it holds sys and files)."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $root 'files') -PathType Container)) {
        return New-Result $false 'The files folder is missing: extract the entire disc, not only the system data.'
    }

    $bootBytes = [System.IO.File]::ReadAllBytes($boot)
    if ($bootBytes.Length -lt 8) { return New-Result $false 'sys\boot.bin is too short.' }
    $gameId = [System.Text.Encoding]::ASCII.GetString($bootBytes, 0, 6)

    $catalog = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'extract\discs.json') -Raw | ConvertFrom-Json
    $sha1 = (Get-FileHash -LiteralPath $dol -Algorithm SHA1).Hash.ToLowerInvariant()
    $known = $catalog.discs | Where-Object { $_.main_dol.sha1 -eq $sha1 } | Select-Object -First 1

    $result = New-Result $false ''
    $result.GameId = $gameId; $result.Sha1 = $sha1; $result.Root = $root
    if ($null -eq $known) {
        $result.Message = "Unknown or modified main.dol (game ID '$gameId'). Use an unmodified dump of the PAL disc."
    } elseif ($known.game_id -ne 'GUPP8P') {
        $result.Message = "This is the $($known.region) release ($($known.game_id)). Only the PAL release (GUPP8P) is supported."
    } elseif ($gameId -ne 'GUPP8P') {
        $result.Message = "main.dol is the PAL one, but sys\boot.bin says '$gameId': the sys folder is mixed from different dumps."
    } elseif ($bootBytes[7] -ne 0) {
        $result.Message = "sys\boot.bin reports disc revision $($bootBytes[7]); only revision 0 is known."
    } else {
        $result.Ok = $true
        $result.Message = 'Shadow the Hedgehog, PAL (GUPP8P), verified.'
    }
    return $result
}

# ---- the Japanese disc (optional: the Japanese event and dialogue voices) --------
#
# The PAL disc carries the Japanese menu and subtitle text and the game keeps its voice-language switch, but the
# Japanese cutscene (event) and dialogue voices are only on the Japanese disc (GUPJ8P). The game serves them from
# <install>\voice_jp (runtime/src/gekko/voice_language.cpp): PRS_VOICE_J.afs and the Japanese twin of every PAL
# sng_*_E.adx / sng_*_e.adx, all of them or the game stays on English; the three sound\B*_J.mlt character banks are
# optional, all three or none. Setup copies exactly that set.

$script:JapaneseVoiceFolder = 'voice_jp'
$script:JapaneseBanks = @('B3_SHADOW_J.mlt', 'B4_SUBHERO_J.mlt', 'B5_SUBDARK_J.mlt')

# The files (relative to the Japanese disc's files folder) the game needs, worked out from the PAL disc as the game does.
function Get-JapaneseVoiceNames([string]$palRoot) {
    $names = @('PRS_VOICE_J.afs')
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $palRoot 'files') -File -Filter 'sng_*.adx' -Force)) {
        if ($file.Name -cmatch '^(sng_.+)_E\.adx$') { $names += "$($Matches[1])_J.adx" }
        elseif ($file.Name -cmatch '^(sng_.+)_e\.adx$') { $names += "$($Matches[1])_j.adx" }
    }
    return $names
}

# $path empty: no Japanese disc (Given = $false, Ok = $true). Otherwise Ok only for an unmodified GUPJ8P extraction,
# in another folder than the PAL one, holding every voice file the PAL disc calls for.
function Test-JapaneseDiscFolder([string]$path, [string]$palRoot) {
    $r = [pscustomobject]@{ Ok = $false; Given = $false; Message = ''; Root = $null; GameId = $null; Sha1 = $null
                            Files = @(); Bytes = 0.0; Banks = $false }
    if (-not $path -or -not $path.Trim().Trim('"')) {
        $r.Ok = $true
        $r.Message = 'No Japanese disc: the game speaks English (Japanese text still works).'
        return $r
    }
    $r.Given = $true
    $root = Resolve-DiscFolder $path
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { $r.Message = "Folder not found: $root."; return $r }
    $r.Root = $root
    if (-not $palRoot) { $r.Message = 'Choose the PAL disc first.'; return $r }
    $pal = [System.IO.Path]::GetFullPath($palRoot).TrimEnd('\')
    if ($root.TrimEnd('\') -ieq $pal) {
        $r.Message = 'This is the PAL disc folder. Extract the Japanese disc into a different folder and choose that.'; return $r
    }
    $dol = Join-Path $root 'sys\main.dol'
    $boot = Join-Path $root 'sys\boot.bin'
    if (-not (Test-Path -LiteralPath $dol -PathType Leaf) -or -not (Test-Path -LiteralPath $boot -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $root 'files') -PathType Container)) {
        $r.Message = "No sys and files folders here. Choose the folder Dolphin's 'Extract Entire Disc' wrote for the Japanese disc."
        return $r
    }
    $bootBytes = [System.IO.File]::ReadAllBytes($boot)
    if ($bootBytes.Length -lt 8) { $r.Message = 'sys\boot.bin is too short.'; return $r }
    $r.GameId = [System.Text.Encoding]::ASCII.GetString($bootBytes, 0, 6)
    $catalog = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'extract\discs.json') -Raw | ConvertFrom-Json
    $r.Sha1 = (Get-FileHash -LiteralPath $dol -Algorithm SHA1).Hash.ToLowerInvariant()
    $known = $catalog.discs | Where-Object { $_.main_dol.sha1 -eq $r.Sha1 } | Select-Object -First 1
    if ($null -eq $known) {
        $r.Message = "Unknown or modified main.dol (game ID '$($r.GameId)'). Use an unmodified dump of the Japanese disc."; return $r
    }
    if ($known.game_id -ne 'GUPJ8P') {
        $r.Message = "This is the $($known.region) release ($($known.game_id)). Japanese voices come from the Japanese release (GUPJ8P)."; return $r
    }
    if ($r.GameId -ne 'GUPJ8P') {
        $r.Message = "main.dol is the Japanese one, but sys\boot.bin says '$($r.GameId)': the sys folder is mixed from different dumps."; return $r
    }

    $files = Join-Path $root 'files'
    $wanted = @(Get-JapaneseVoiceNames $palRoot)
    $missing = @($wanted | Where-Object { -not (Test-Path -LiteralPath (Join-Path $files $_) -PathType Leaf) })
    if ($missing) {
        $shown = ($missing | Select-Object -First 3) -join ', '
        $r.Message = "The Japanese disc's files folder lacks $($missing.Count) voice file(s) ($shown). Extract the entire disc."
        return $r
    }
    $banks = @($script:JapaneseBanks | ForEach-Object { "sound\$_" })
    $r.Banks = @($banks | Where-Object { Test-Path -LiteralPath (Join-Path $files $_) -PathType Leaf }).Count -eq $banks.Count
    $r.Files = @($wanted) + $(if ($r.Banks) { $banks } else { @() })
    foreach ($relative in $r.Files) { $r.Bytes += (Get-Item -LiteralPath (Join-Path $files $relative) -Force).Length }
    $r.Ok = $true
    $r.Message = "Japan (GUPJ8P), verified: $($r.Files.Count) voice files ($(Format-Size $r.Bytes)) to copy."
    return $r
}

# Copies the Japanese voice files into <target>\voice_jp; returns their paths relative to <target>.
function Copy-JapaneseVoices($japanese, [string]$target) {
    $source = Join-Path $japanese.Root 'files'
    $copied = @()
    foreach ($relative in $japanese.Files) {
        $destination = Join-Path (Join-Path $target $script:JapaneseVoiceFolder) $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination -Force
        if ((Get-Item -LiteralPath $destination -Force).Length -ne (Get-Item -LiteralPath (Join-Path $source $relative) -Force).Length) {
            throw "The copy of $relative has the wrong size"
        }
        $copied += Join-Path $script:JapaneseVoiceFolder $relative
    }
    return $copied
}

# Sets the spoken language (options.cfg 'voice': 0 Japanese, 1 English) in <target>\options.cfg, keeping every other
# line of an existing file as it is (docs/PC_SETTINGS_FILES.md).
function Set-SpokenLanguage([string]$target, [int]$voice) {
    $path = Join-Path $target 'options.cfg'
    $line = "voice = $voice"
    $text = ''
    if (Test-Path -LiteralPath $path -PathType Leaf) { $text = [System.IO.File]::ReadAllText($path) }
    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = [System.Collections.Generic.List[string]]::new()
    if ($text) { foreach ($l in ($text -split "`r?`n")) { $lines.Add($l) } }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*voice\s*=') {
            if ($found) { $lines[$i] = '# ' + $lines[$i] } else { $lines[$i] = $line; $found = $true }
        }
    }
    if (-not $found) { $lines.Add($line) }
    [System.IO.File]::WriteAllText($path, ($lines -join $newline) + $newline, [System.Text.UTF8Encoding]::new($false))
}

function Get-FolderSize([string]$path) {
    $sum = (Get-ChildItem -LiteralPath $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    if ($null -eq $sum) { return 0 }
    return [double]$sum
}

# ---- the install folder ------------------------------------------------------

function Test-InstallFolder([string]$path, [string]$discRoot, [double]$assetBytes) {
    $r = [pscustomobject]@{ Ok = $false; Message = ''; Path = $null; Existing = $false; NeededBytes = 0.0; FreeBytes = 0.0 }
    if (-not $path) { $r.Message = 'Choose where to install the game.'; return $r }
    $path = $path.Trim().Trim('"')
    try { $full = [System.IO.Path]::GetFullPath($path).TrimEnd('\') } catch { $r.Message = "Not a valid folder path: $path"; return $r }
    $r.Path = $full
    if ($full.Length -le 3) { $r.Message = 'Choose a folder, not the root of a drive.'; return $r }

    if ($discRoot) {
        $disc = [System.IO.Path]::GetFullPath($discRoot).TrimEnd('\')
        if ($full -ieq $disc -or $full.StartsWith($disc + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            $r.Message = 'Install somewhere else than the disc folder; the installer copies from it.'; return $r
        }
    }
    $repo = $script:RepoRoot.TrimEnd('\')
    if ($full -ieq $repo) { $r.Message = 'Install somewhere else than the source folder.'; return $r }

    $r.Existing = Test-Path -LiteralPath (Join-Path $full $script:GameExe) -PathType Leaf
    # An update replaces these programs: one that is running cannot be replaced (the copy step would fail much later).
    foreach ($program in $script:InstalledPrograms) {
        $file = Join-Path $full $program
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        try { [System.IO.File]::Open($file, 'Open', 'ReadWrite', 'None').Dispose() }
        catch [System.IO.IOException] { $r.Message = "$program is running from this folder. Close it, then check again."; return $r }
        catch { $r.Message = "Cannot write to $program in this folder: $($_.Exception.Message)"; return $r }
    }
    $r.NeededBytes = $assetBytes + 150MB
    if ($r.Existing -and (Test-Path -LiteralPath (Join-Path $full 'files'))) { $r.NeededBytes = 150MB }

    $qualifier = [System.IO.Path]::GetPathRoot($full)
    try {
        $drive = New-Object System.IO.DriveInfo $qualifier
        if (-not $drive.IsReady) { $r.Message = "Drive $qualifier is not ready."; return $r }
        $r.FreeBytes = [double]$drive.AvailableFreeSpace
    } catch { $r.Message = "Cannot read drive $qualifier."; return $r }
    if ($r.FreeBytes -lt $r.NeededBytes) {
        $r.Message = "Not enough space: needs $(Format-Size $r.NeededBytes), $(Format-Size $r.FreeBytes) free."; return $r
    }

    $r.Ok = $true
    $r.Message = "Needs $(Format-Size $r.NeededBytes), $(Format-Size $r.FreeBytes) free."
    if ($r.Existing) { $r.Message += ' An existing install will be updated; saves and settings are kept.' }
    return $r
}

# ---- steps run as child processes ---------------------------------------------

# How many compilers to run at once: two threads stay free for the desktop, each compiler is budgeted 2 GB of free
# memory, and 12 is the ceiling (beyond it the compilers mostly add heat, measured on a 32-thread CPU).
function Get-InstallJobs {
    $byMemory = [int][Math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1KB / 2GB)
    return [Math]::Max(2, [Math]::Min(12, [Math]::Min([Environment]::ProcessorCount - 2, $byMemory)))
}

# The release package's prebuilt game-code-free parts (dev/Package-Release.ps1), or $null in a clone. CMake decides
# whether this PC's compiler can use them (docs/BUILDING.md "Prebuilt parts"); otherwise it builds everything.
function Get-PrebuiltFolder {
    $folder = Join-Path $script:RepoRoot 'prebuilt'
    if (Test-Path -LiteralPath $folder -PathType Container) { return $folder }
    return $null
}

# Each step: Name, File, Arguments, WorkingDirectory, MaxExit (highest exit code that means success).
function Get-BuildSteps([string]$discRoot, [string]$python, [int]$jobs = 2) {
    $dol = (Join-Path $discRoot 'sys\main.dol') -replace '\\', '/'
    $prebuilt = Get-PrebuiltFolder
    $prebuiltArgument = if ($prebuilt) { " `"-DSHADOW_PREBUILT_DIR=$($prebuilt -replace '\\', '/')`"" } else { '' }
    @(
        [pscustomobject]@{ Name = 'Configure'; File = 'cmake'; WorkingDirectory = $script:RepoRoot; MaxExit = 0
            Arguments = "--preset release `"-DSHADOW_RECOMP_DOL=$dol`" -DSHADOW_RECOMP_COMPILE_JOBS=$jobs `"-DPython3_EXECUTABLE=$python`"$prebuiltArgument" },
        [pscustomobject]@{ Name = 'Build'; File = 'cmake'; WorkingDirectory = $script:RepoRoot; MaxExit = 0
            Arguments = "--build --preset release --parallel $jobs -- /nodeReuse:false" }
    )
}

function Get-InstallSteps([string]$discRoot, [string]$target) {
    $binDir = Split-Path -Parent (Get-BuiltExe)
    @(
        # robocopy copies bytes as they are; exit codes below 8 are success.
        [pscustomobject]@{ Name = 'Copy game'; File = 'robocopy'; WorkingDirectory = $script:RepoRoot; MaxExit = 7
            Arguments = "`"$binDir`" `"$target`" $($script:InstalledPrograms -join ' ') /COPY:DAT /R:2 /W:1 /NP /NJH" },
        [pscustomobject]@{ Name = 'Copy game files'; File = 'robocopy'; WorkingDirectory = $script:RepoRoot; MaxExit = 7
            Arguments = "`"$(Join-Path $discRoot 'files')`" `"$(Join-Path $target 'files')`" /E /COPY:DAT /R:2 /W:1 /NP /NJH /NDL /NFL" }
    )
}

# Starts a step with its output appended to $log. Returns the Process.
function Start-Step($step, [string]$log) {
    Add-Content -LiteralPath $log -Value "`r`n==== $($step.Name): $($step.File) $($step.Arguments)" -Encoding UTF8
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $env:ComSpec
    $info.Arguments = "/d /s /c `"`"$($step.File)`" $($step.Arguments) >> `"$log`" 2>&1`""
    $info.WorkingDirectory = $step.WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    return [System.Diagnostics.Process]::Start($info)
}

function Test-StepSucceeded($step, [System.Diagnostics.Process]$process) {
    return $process.HasExited -and $process.ExitCode -le $step.MaxExit
}

function Stop-Step([System.Diagnostics.Process]$process) {
    if ($process -and -not $process.HasExited) {
        & taskkill.exe /T /F /PID $process.Id 2>$null | Out-Null
    }
}

# Reads what the log gained since $offset; returns @{ Text; Offset }.
function Read-LogTail([string]$log, [long]$offset) {
    if (-not (Test-Path -LiteralPath $log)) { return @{ Text = ''; Offset = $offset } }
    $stream = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        if ($stream.Length -le $offset) { return @{ Text = ''; Offset = $offset } }
        $stream.Seek($offset, 'Begin') | Out-Null
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::Default)
        $text = $reader.ReadToEnd()
        return @{ Text = $text; Offset = $stream.Length }
    } finally { $stream.Dispose() }
}

# What a failed step's log says went wrong, for the error message (the log has thousands of lines): the first error
# line the step wrote from $offset on, the line after a "CMake Error at" (its text), and a hint for the known causes.
function Get-StepFailureCause([string]$log, [long]$offset) {
    $text = (Read-LogTail $log $offset).Text
    if (-not $text) { return '' }
    $lines = @($text -split "`r?`n")
    $cause = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch 'CMake Error|\b(?:fatal )?error\s+[A-Z]+\d+\s*:|^ERROR[ :]|Traceback \(most recent call last\)') { continue }
        $cause = $lines[$i].Trim()
        if ($cause -match '^CMake Error at ' -or $cause -match '^CMake Error:?$') {
            $next = @($lines | Select-Object -Skip ($i + 1) | Where-Object { $_.Trim() } | Select-Object -First 1)
            if ($next) { $cause = "$cause $($next[0].Trim())" }
        }
        break
    }
    $hint = ''
    if ($text -match 'Could not create named generator') { $hint = 'This CMake does not know Visual Studio 2026: install CMake 4.2 or newer.' }
    elseif ($text -match 'could not find any instance of Visual Studio|No CMAKE_CXX_COMPILER could be found') { $hint = "Visual Studio 2026 needs the 'Desktop development with C++' workload (Visual Studio Installer, Modify)." }
    elseif ($text -match 'MSB8036|Windows SDK version .* was not found|No Windows SDK') { $hint = 'A Windows 10/11 SDK is missing: add one to Visual Studio 2026 in the Visual Studio Installer.' }
    elseif ($text -match '\bC1060\b|\bC1076\b|\bC3859\b|out of memory|MemoryError') { $hint = 'The compiler ran out of memory: close other programs and select Retry.' }
    elseif ($text -match 'There is not enough space on the disk|No space left|\bC1085\b|\bLNK1102\b|\bLNK1318\b') { $hint = "The drive of $($script:RepoRoot) is full: free some space and select Retry." }
    elseif ($text -match 'ModuleNotFoundError|No module named') { $hint = 'This Python lacks a standard module: install Python 3 from python.org.' }
    if ($cause.Length -gt 400) { $cause = $cause.Substring(0, 400) + '...' }
    return (@($cause, $hint) | Where-Object { $_ }) -join ' '
}

# Build progress counts every source file the compiler reports, so the bar moves from the first second. A recompiled
# shard costs about nine ordinary files (measured). A full build compiles OrdinaryFiles ordinary files besides the shards
# and their table; with the prebuilt parts only PrebuiltOrdinaryFiles (the empty stand-ins, the ISA gate, the launcher).
$script:OrdinaryBuildFiles = 217
$script:PrebuiltOrdinaryBuildFiles = 15
$script:ShardWeight = 9

function New-BuildProgress([bool]$prebuilt = $false) {
    @{ Units = 0; Partial = ''; Fraction = 0.0; Ordinary = $(if ($prebuilt) { $script:PrebuiltOrdinaryBuildFiles } else { $script:OrdinaryBuildFiles }) }
}

# Adds what the log gained to $progress. A line cut off at the end of $text is kept for the next call.
function Update-BuildProgress($progress, [string]$text) {
    $lines = ($progress.Partial + $text) -split "`r?`n"
    $progress.Partial = $lines[-1]
    $total = $progress.Ordinary + ($script:Shards + 1) * $script:ShardWeight
    foreach ($line in ($lines | Select-Object -SkipLast 1)) {
        if ($line -match '^\s*(?:\d+>)?\s*([\w.\-]+)\.(?:cpp|cxx|c)\s*$') {
            $progress.Units += $(if ($Matches[1] -match '^recomp_(?:\d{3}|table)$') { $script:ShardWeight } else { 1 })
            $progress.Fraction = [Math]::Max($progress.Fraction, [Math]::Min(0.99, $progress.Units / $total))
        } elseif ($line -match 'ShadowTheHedgehog\.vcxproj -> ') {
            $progress.Fraction = 1.0
        }
    }
}

# The prebuilt set the configured tree links, read from its CMake cache (empty: everything is built here).
function Get-PrebuiltUsed {
    $cache = Join-Path $script:RepoRoot '.build\release\CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { return '' }
    $line = Select-String -LiteralPath $cache -Pattern '^SHADOW_PREBUILT_USED:INTERNAL=(.*)$' | Select-Object -First 1
    if ($line) { return $line.Matches[0].Groups[1].Value }
    return ''
}

# ---- finishing touches --------------------------------------------------------

# The source revision this build came from. Git's stderr must not stop the install, so this function does not use Stop.
function Get-SourceRevision {
    $result = [ordered]@{ commit = 'unknown'; modified_files = $null }
    # A downloaded package carries its commit in this file; a clone keeps the file's unexpanded placeholder.
    $stamp = Join-Path $PSScriptRoot 'revision.txt'
    if (Test-Path -LiteralPath $stamp) {
        $packaged = (Get-Content -LiteralPath $stamp -Raw).Trim()
        if ($packaged -match '^[0-9a-f]{40}$') { $result.commit = $packaged; $result.modified_files = 0; return $result }
    }
    if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) { return $result }
    $ErrorActionPreference = 'Continue'
    $commit = git -C $script:RepoRoot rev-parse HEAD 2>$null | Select-Object -First 1
    if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') { return $result }
    $result.commit = $commit
    $result.modified_files = @(git -C $script:RepoRoot status --porcelain 2>$null).Count
    return $result
}

# The settings the build really used, read back from its CMake cache rather than assumed from the preset.
function Get-BuildConfiguration {
    $config = [ordered]@{ preset = 'release'; configuration = 'Release' }
    $cache = Join-Path $script:RepoRoot '.build\release\CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { return $config }
    $wanted = 'SHADOW_SHIPPING', 'SHADOW_RENDER_LIFTED', 'SHADOW_ENABLE_PLUME', 'SHADOW_ENABLE_TRACY', 'SHADOW_BUILD_TESTS',
              'SHADOW_BUILD_REFERENCE', 'SHADOW_RECOMP_SHARDS', 'SHADOW_RECOMP_COMPILE_JOBS', 'SHADOW_PREBUILT_USED',
              'CMAKE_GENERATOR', 'CMAKE_CXX_COMPILER'
    foreach ($line in [System.IO.File]::ReadAllLines($cache)) {
        if ($line -match '^([A-Z0-9_]+):[A-Z]+=(.*)$' -and $wanted -contains $Matches[1]) { $config[$Matches[1]] = $Matches[2] }
    }
    return $config
}

function Write-InstallInfo([string]$target, $disc, $japanese = $null) {
    $info = [ordered]@{
        installed = (Get-Date).ToString('s')
        disc_game_id = $disc.GameId
        disc_main_dol_sha1 = $disc.Sha1
        japanese_voices = $(if ($japanese -and $japanese.Given -and $japanese.Ok) {
            [ordered]@{ disc_game_id = $japanese.GameId; disc_main_dol_sha1 = $japanese.Sha1; files = $japanese.Files.Count; character_banks = $japanese.Banks } } else { $null })
        source = $script:RepoRoot
        source_revision = Get-SourceRevision
        build = Get-BuildConfiguration
        exe_sha256 = (Get-FileHash -LiteralPath (Join-Path $target $script:GameExe) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $info | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $target 'install-info.json') -Encoding UTF8
}

# Records exactly what setup put in the folder, which is all the uninstaller will ever remove. Only files setup copied
# are listed, so anything else in the folder (saves, settings, the player's own files) is never in the record. Entries
# from an earlier install that are still on disk are carried over, so an update cannot orphan them.
function Write-InstallManifest([string]$target, [string]$discRoot, [string[]]$shortcuts, [string[]]$shortcutFolders,
                               [string[]]$copiedFiles = @()) {
    $path = Join-Path $target 'install-manifest.json'
    $files = New-Object System.Collections.ArrayList
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $packaged = @($script:InstalledPrograms)
    foreach ($destination in $script:InstalledFolders.Values) {
        $root = (Join-Path $target $destination).TrimEnd('\')
        foreach ($file in (Get-ChildItem -LiteralPath $root -Recurse -File -Force)) {
            $packaged += $destination.TrimEnd('\') + $file.FullName.Substring($root.Length)
        }
    }
    foreach ($relative in $packaged) {
        $item = Get-Item -LiteralPath (Join-Path $target $relative)
        [void]$files.Add([ordered]@{ path = $relative; size = $item.Length; sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() })
        [void]$seen.Add($relative)
    }
    $source = (Join-Path $discRoot 'files').TrimEnd('\')
    foreach ($file in (Get-ChildItem -LiteralPath $source -Recurse -File -Force)) {
        $relative = 'files' + $file.FullName.Substring($source.Length)
        $copied = Join-Path $target $relative
        if (-not (Test-Path -LiteralPath $copied -PathType Leaf)) { throw "The copy is missing $relative" }
        $size = (Get-Item -LiteralPath $copied -Force).Length
        if ($size -ne $file.Length) { throw "The copy of $relative has the wrong size" }
        [void]$files.Add([ordered]@{ path = $relative; size = $size })
        [void]$seen.Add($relative)
    }
    # Files copied from another disc (the Japanese voices): recorded by size, like the PAL files.
    foreach ($relative in @($copiedFiles | Where-Object { $_ })) {
        [void]$files.Add([ordered]@{ path = $relative; size = (Get-Item -LiteralPath (Join-Path $target $relative) -Force).Length })
        [void]$seen.Add($relative)
    }
    $allShortcuts = @($shortcuts | Where-Object { $_ })
    $allFolders = @($shortcutFolders | Where-Object { $_ })
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $previous = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($entry in @($previous.files)) {
            if ($seen.Contains([string]$entry.path) -or -not (Test-Path -LiteralPath (Join-Path $target $entry.path) -PathType Leaf)) { continue }
            [void]$files.Add($entry)
        }
        $allShortcuts = @($allShortcuts + @($previous.shortcuts) | Where-Object { $_ } | Select-Object -Unique)
        $allFolders = @($allFolders + @($previous.shortcut_folders) | Where-Object { $_ } | Select-Object -Unique)
    }
    $manifest = [ordered]@{ schema = 1; product = 'Shadow the Hedgehog Recompiled'; files = @($files); shortcuts = @($allShortcuts); shortcut_folders = @($allFolders) }
    $manifest | ConvertTo-Json -Depth 4 -Compress | Set-Content -LiteralPath $path -Encoding UTF8
}

function New-GameShortcuts([string]$target, [bool]$desktop, [bool]$startMenu,
                           [string]$desktopDir = [Environment]::GetFolderPath('Desktop'),
                           [string]$startMenuDir = (Join-Path ([Environment]::GetFolderPath('Programs')) 'Shadow the Hedgehog Recompiled')) {
    $shell = New-Object -ComObject WScript.Shell
    $made = @()
    $places = @()
    if ($desktop) { $places += $desktopDir }
    if ($startMenu) { $places += $startMenuDir }
    foreach ($dir in $places) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $link = $shell.CreateShortcut((Join-Path $dir 'Shadow the Hedgehog Recompiled.lnk'))
        $link.TargetPath = Join-Path $target $script:GameExe
        $link.WorkingDirectory = $target
        $link.IconLocation = (Join-Path $target $script:GameExe) + ',0'
        $link.Description = 'Shadow the Hedgehog Recompiled (static recompilation, work in progress)'
        $link.Save()
        $made += $link.FullName
    }
    if ($startMenu) {
        New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null
        $link = $shell.CreateShortcut((Join-Path $startMenuDir 'Controller Buttons.lnk'))
        $link.TargetPath = Join-Path $target $script:ConfigExe
        $link.WorkingDirectory = $target
        $link.IconLocation = (Join-Path $target $script:ConfigExe) + ',0'
        $link.Description = 'Choose which controller buttons do what in the game.'
        $link.Save()
        $made += $link.FullName
        $link = $shell.CreateShortcut((Join-Path $startMenuDir 'Mod Manager.lnk'))
        $link.TargetPath = Join-Path $target $script:ModManagerExe
        $link.WorkingDirectory = $target
        $link.IconLocation = (Join-Path $target $script:ModManagerExe) + ',0'
        $link.Description = 'Add, remove and order mods for the game.'
        $link.Save()
        $made += $link.FullName
        $link = $shell.CreateShortcut((Join-Path $startMenuDir 'Uninstall Shadow the Hedgehog Recompiled.lnk'))
        $link.TargetPath = Join-Path $target 'Uninstall.cmd'
        $link.WorkingDirectory = $target
        $link.Description = 'Removes the game. Your saves and settings are kept.'
        $link.Save()
        $made += $link.FullName
    }
    return $made
}

# ---- the whole install ----------------------------------------------------------

$script:StageNames = @('Configure the build', 'Translate and compile the game', 'Copy the game', 'Copy the game files', 'Finish and create shortcuts')

# Runs every step and reports through $OnEvent with hashtables:
#   @{type='stage'; index; name; state='run'|'ok'|'fail'}, @{type='progress'; value=0..1},
#   @{type='log'; text}, @{type='done'; target; exe}, @{type='error'; message; log}.
# Overall progress: configure 0-3%, compile 3-85%, copy 85-99%, finish 100%.
function Invoke-GameInstall($disc, [string]$targetPath, [string]$python, [int]$jobs, [bool]$desktop, [bool]$startMenu,
                            [scriptblock]$OnEvent,
                            [string]$desktopDir = [Environment]::GetFolderPath('Desktop'),
                            [string]$startMenuDir = (Join-Path ([Environment]::GetFolderPath('Programs')) 'Shadow the Hedgehog Recompiled'),
                            $japanese = $null, [int]$voice = -1) {
    # $japanese: a Test-JapaneseDiscFolder result (or $null); $voice: options.cfg 'voice' to set (-1 leaves it alone).
    $log = Join-Path $script:RepoRoot '.build\install.log'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $log) | Out-Null
    Set-Content -LiteralPath $log -Value "Install started $(Get-Date -Format s)" -Encoding UTF8
    $fail = {
        param([int]$index, [string]$message)
        if ($index -ge 0) { & $OnEvent @{ type = 'stage'; index = $index; name = $script:StageNames[$index]; state = 'fail' } }
        & $OnEvent @{ type = 'error'; message = $message; log = $log }
    }
    try { New-Item -ItemType Directory -Force -Path $targetPath | Out-Null }
    catch { & $fail -1 "Cannot create $($targetPath): $($_.Exception.Message)"; return $false }

    # The build writes about 1.5 GB next to this setup (measured: a full build tree), often on another drive than the
    # install folder. Nothing to build again (the game is there from an earlier run of this package): no demand.
    $buildNeeds = 2.5GB
    if (-not (Test-Path -LiteralPath (Get-BuiltExe) -PathType Leaf)) {
        try {
            $buildDrive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($script:RepoRoot))
            if ($buildDrive.IsReady -and $buildDrive.AvailableFreeSpace -lt $buildNeeds) {
                & $fail -1 "Not enough space to build the game: drive $($buildDrive.Name) needs $(Format-Size $buildNeeds) free for the build files in $($script:RepoRoot), $(Format-Size $buildDrive.AvailableFreeSpace) free."
                return $false
            }
        } catch {}
    }

    $assetBytes = Get-FolderSize (Join-Path $disc.Root 'files')
    if ($jobs -lt 1) { $jobs = Get-InstallJobs }
    $steps = @(Get-BuildSteps $disc.Root $python $jobs) + @(Get-InstallSteps $disc.Root $targetPath)
    $buildProgress = New-BuildProgress
    for ($i = 0; $i -lt $steps.Count; $i++) {
        $step = $steps[$i]
        & $OnEvent @{ type = 'stage'; index = $i; name = $script:StageNames[$i]; state = 'run' }
        $offset = (Get-Item -LiteralPath $log).Length
        $stepStart = $offset
        $process = Start-Step $step $log
        $ticks = 0
        while ($true) {
            $exited = $process.WaitForExit(500)
            $tail = Read-LogTail $log $offset
            $offset = $tail.Offset
            if ($tail.Text) { & $OnEvent @{ type = 'log'; text = $tail.Text } }
            $ticks++
            switch ($i) {
                0 { & $OnEvent @{ type = 'progress'; value = 0.02 } }
                1 { Update-BuildProgress $buildProgress $tail.Text
                    & $OnEvent @{ type = 'progress'; value = 0.03 + 0.82 * $buildProgress.Fraction } }
                2 { & $OnEvent @{ type = 'progress'; value = 0.85 } }
                3 { # robocopy prints no running total; measure the copy every few seconds.
                    if (($ticks % 6 -eq 0 -or $exited) -and $assetBytes -gt 0) {
                        $copied = Get-FolderSize (Join-Path $targetPath 'files')
                        & $OnEvent @{ type = 'progress'; value = 0.86 + 0.13 * [Math]::Min(1, $copied / $assetBytes) }
                    } }
            }
            if ($exited) { break }
        }
        if (-not (Test-StepSucceeded $step $process)) {
            $cause = ''
            try { $cause = Get-StepFailureCause $log $stepStart } catch {}
            & $fail $i ("$($script:StageNames[$i]) failed (exit code $($process.ExitCode))." + $(if ($cause) { " $cause" } else { '' }))
            return $false
        }
        & $OnEvent @{ type = 'stage'; index = $i; name = $script:StageNames[$i]; state = 'ok' }
        if ($i -eq 0) { $buildProgress = New-BuildProgress ([bool](Get-PrebuiltUsed)) }
    }

    $last = $steps.Count
    & $OnEvent @{ type = 'stage'; index = $last; name = $script:StageNames[$last]; state = 'run' }
    try {
        foreach ($source in $script:InstalledFolders.Keys) {
            $destination = Join-Path $targetPath $script:InstalledFolders[$source]
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
            Copy-Item -Path (Join-Path (Join-Path $script:RepoRoot $source) '*') -Destination $destination -Recurse -Force
        }
        $japaneseFiles = @()
        if ($japanese -and $japanese.Given -and $japanese.Ok) {
            & $OnEvent @{ type = 'log'; text = "Copying the Japanese voices ($($japanese.Files.Count) files, $(Format-Size $japanese.Bytes)) to $($script:JapaneseVoiceFolder)`n" }
            $japaneseFiles = @(Copy-JapaneseVoices $japanese $targetPath)
        }
        if ($voice -ge 0) {
            Set-SpokenLanguage $targetPath $voice
            & $OnEvent @{ type = 'log'; text = "Spoken language: $(if ($voice -eq 0) { 'Japanese' } else { 'English' }) (options.cfg voice = $voice)`n" }
        }
        Write-InstallInfo $targetPath $disc $japanese
        foreach ($name in @('Uninstall.ps1', 'Uninstall.cmd')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $targetPath -Force }
        $shortcuts = @(New-GameShortcuts $targetPath $desktop $startMenu $desktopDir $startMenuDir)
        foreach ($made in $shortcuts) { & $OnEvent @{ type = 'log'; text = "Shortcut: $made`n" } }
        Write-InstallManifest $targetPath $disc.Root $shortcuts $(if ($startMenu) { @($startMenuDir) } else { @() }) $japaneseFiles
        Copy-Item -LiteralPath $log -Destination (Join-Path $targetPath 'install.log') -Force
    } catch {
        & $fail $last "Finishing failed: $($_.Exception.Message)"
        return $false
    }
    & $OnEvent @{ type = 'stage'; index = $last; name = $script:StageNames[$last]; state = 'ok' }
    & $OnEvent @{ type = 'progress'; value = 1.0 }
    & $OnEvent @{ type = 'done'; target = $targetPath; exe = (Join-Path $targetPath $script:GameExe) }
    return $true
}

Export-ModuleMember -Function Get-ToolPlan, Install-Tool, Find-AnyVisualStudio, Get-RepoRoot, Get-BuiltExe, Get-DefaultInstallFolder, Format-Size, Test-Prerequisites,
    Resolve-DiscFolder, Test-DiscFolder, Get-JapaneseVoiceNames, Test-JapaneseDiscFolder, Copy-JapaneseVoices,
    Set-SpokenLanguage, Get-FolderSize, Test-InstallFolder, Get-InstallJobs, Get-PrebuiltFolder,
    Get-BuildSteps, Get-InstallSteps, Start-Step, Test-StepSucceeded, Stop-Step, Read-LogTail, Get-StepFailureCause, New-BuildProgress,
    Update-BuildProgress, Get-PrebuiltUsed, Write-InstallInfo, Write-InstallManifest,
    Get-SourceRevision, Get-BuildConfiguration, New-GameShortcuts,
    Invoke-GameInstall
