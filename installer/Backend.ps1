# The setup window's backend (called by NukkoroSetup.exe). Every command prints JSON on stdout:
#   prereqs                         -> [ {name, ok, detail, url} ... ]
#   defaults                        -> {target}
#   check-disc <path>               -> {ok, message, root, gameId, sha1, assetBytes}
#   check-target <path> <discRoot>  -> {ok, message, path, existing, neededBytes, freeBytes}
#   check-jpdisc <path> <discRoot>  -> {ok, given, message, root, gameId, files, bytes, banks} (the optional Japanese disc)
#   install <disc> <target> <desktop 0|1> <startMenu 0|1> <japaneseDisc or -> <voice - | 0 Japanese | 1 English>
#           [jobs, 0 = chosen from this PC]
#                                   -> one JSON object per line (see Invoke-GameInstall), last is 'done' or 'error'
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Command,
    [Parameter(Position = 1)][string]$A,
    [Parameter(Position = 2)][string]$B,
    [Parameter(Position = 3)][string]$C,
    [Parameter(Position = 4)][string]$D,
    [Parameter(Position = 5)][string]$E,
    [Parameter(Position = 6)][string]$F,
    [Parameter(Position = 7)][int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Import-Module (Join-Path $PSScriptRoot 'InstallCore.psm1') -Force

function Out-Json($value) { [Console]::Out.WriteLine(($value | ConvertTo-Json -Compress -Depth 5)); [Console]::Out.Flush() }

try {
    switch ($Command) {
        'prereqs' {
            $list = @(Test-Prerequisites | ForEach-Object { [ordered]@{ name = $_.Name; ok = [bool]$_.Ok; detail = [string]$_.Detail; url = $_.Url } })
            [Console]::Out.WriteLine((ConvertTo-Json -InputObject $list -Compress))
        }
        'defaults' { Out-Json ([ordered]@{ target = Get-DefaultInstallFolder }) }
        'check-disc' {
            # PowerShell names are case-insensitive: no local may be called $a..$d (the parameters).
            $result = Test-DiscFolder $A
            $bytes = 0.0
            if ($result.Ok) { $bytes = Get-FolderSize (Join-Path $result.Root 'files') }
            Out-Json ([ordered]@{ ok = [bool]$result.Ok; message = $result.Message; root = $result.Root; gameId = $result.GameId; sha1 = $result.Sha1; assetBytes = $bytes })
        }
        'check-target' {
            $assetBytes = 0.0
            if ($B -and (Test-Path -LiteralPath (Join-Path $B 'files'))) { $assetBytes = Get-FolderSize (Join-Path $B 'files') }
            $result = Test-InstallFolder $A $B $assetBytes
            Out-Json ([ordered]@{ ok = [bool]$result.Ok; message = $result.Message; path = $result.Path; existing = [bool]$result.Existing; neededBytes = $result.NeededBytes; freeBytes = $result.FreeBytes })
        }
        'check-jpdisc' {
            $result = Test-JapaneseDiscFolder $A $B
            Out-Json ([ordered]@{ ok = [bool]$result.Ok; given = [bool]$result.Given; message = $result.Message; root = $result.Root
                                  gameId = $result.GameId; files = @($result.Files).Count; bytes = $result.Bytes; banks = [bool]$result.Banks })
        }
        'install' {
            $missing = @(Test-Prerequisites | Where-Object { -not $_.Ok })
            if ($missing) { Out-Json @{ type = 'error'; message = "Missing build tools: $(($missing | ForEach-Object Name) -join ', ')." }; exit 1 }
            $python = (Test-Prerequisites | Where-Object { $_.Name -eq 'Python 3' }).Value
            $disc = Test-DiscFolder $A
            if (-not $disc.Ok) { Out-Json @{ type = 'error'; message = $disc.Message }; exit 1 }
            # '-' = none (an empty argument does not survive every command line).
            if ($E -eq '-') { $E = '' }
            $japanese = Test-JapaneseDiscFolder $E $disc.Root
            if (-not $japanese.Ok) { Out-Json @{ type = 'error'; message = $japanese.Message }; exit 1 }
            $voice = -1
            if ($F -eq '0' -or $F -eq '1') { $voice = [int]$F }
            if ($voice -eq 0 -and -not $japanese.Given) { Out-Json @{ type = 'error'; message = 'Japanese voices need the Japanese disc folder.' }; exit 1 }
            $target = Test-InstallFolder $B $disc.Root ((Get-FolderSize (Join-Path $disc.Root 'files')) + $japanese.Bytes)
            if (-not $target.Ok) { Out-Json @{ type = 'error'; message = $target.Message }; exit 1 }
            $ok = Invoke-GameInstall $disc $target.Path $python $Jobs ($C -eq '1') ($D -eq '1') { param($e) Out-Json $e } -japanese $japanese -voice $voice
            if (-not $ok) { exit 1 }
        }
        default { Out-Json @{ type = 'error'; message = "Unknown command: $Command" }; exit 2 }
    }
} catch {
    Out-Json @{ type = 'error'; ok = $false; message = $_.Exception.Message }
    exit 1
}
