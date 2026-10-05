<#
.SYNOPSIS
    Builds the libFuzzer targets (fuzz/, docs/06 §4) and runs each for a while, seeded from the
    repository's own files (rices, plugins, widgets) and Windows' own cursors.
.DESCRIPTION
    Corpora grow in build\fuzz\corpus\<target> (kept between runs); a crash, leak or timeout lands
    in build\fuzz\crashes\<target> and fails the run. Reproduce one with
    build\fuzz\bin\fuzz_<target>.exe <file>.
.EXAMPLE
    .\scripts\fuzz.ps1                                  # every target, 60 s each (the CI smoke)
    .\scripts\fuzz.ps1 -Target rice_toml -Seconds 3600  # one target, an hour
#>
[CmdletBinding()]
param(
    [string[]] $Target = @(),
    [int] $Seconds = 60,
    [switch] $NoBuild
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$root = $script:RepoRoot
$work = Join-Path $root 'build\fuzz'
$bin = Join-Path $work 'bin'

if (-not $NoBuild) {
    Push-Location $root
    try {
        cmake --preset fuzz | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Configure failed (preset fuzz).' }
        cmake --build --preset fuzz
        if ($LASTEXITCODE -ne 0) { throw 'Build failed (preset fuzz).' }
    }
    finally {
        Pop-Location
    }
}

$all = Get-ChildItem $bin -Filter 'fuzz_*.exe' | ForEach-Object { $_.BaseName.Substring(5) } | Sort-Object
if ($Target.Count -gt 0) { $all = $all | Where-Object { $_ -in $Target } }
if (-not $all) { throw 'No fuzz targets to run.' }

$utf8 = New-Object Text.UTF8Encoding $false
function Seed([string] $Name, [string] $File, [byte[]] $Bytes) {
    $dir = Join-Path $work "corpus\$Name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $dir $File), $Bytes)
}
function Seed-Text([string] $Name, [string] $File, [string] $Text, [int] $Prefix = -1) {
    [byte[]] $bytes = $utf8.GetBytes($Text)
    if ($Prefix -ge 0) { $bytes = [byte[]]@($Prefix) + $bytes }
    Seed $Name $File $bytes
}
function Seed-Files([string] $Name, [string] $Pattern, [int] $Prefix = -1) {
    $i = 0
    foreach ($file in Get-ChildItem $root -Recurse -File -Filter $Pattern -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch '\\build\\|\\\.git\\' }) {
        [byte[]] $bytes = [IO.File]::ReadAllBytes($file.FullName)
        if ($Prefix -ge 0) { $bytes = [byte[]]@($Prefix) + $bytes }
        Seed $Name ("repo-{0}-{1}" -f $i++, $file.Name) $bytes
    }
}

# ---- seeds: what the parsers really read --------------------------------------------------------
Seed-Files 'rice_toml' 'rice.toml'
Seed-Files 'plugin_toml' 'plugin.toml'
Seed-Files 'widget_slint' '*.slint'
Seed-Text 'settings_toml' 'seed.toml' @'
[core]
keep_in_tray = true

[[hotkey]]
keys = "Win+Alt+D"
action = "appearance.toggle"

[[rule]]
when = "time"
at = "19:00"
action = "rice.apply"
rice = 'C:\Rices\night'
'@
$cursors = Join-Path $env:WINDIR 'Cursors'
$i = 0
foreach ($file in Get-ChildItem $cursors -File -Include '*.cur', '*.ani' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 40) {
    $prefix = if ($file.Extension -eq '.ani') { 1 } else { 0 }
    Seed 'cursor_file' ("win-{0}-{1}" -f $i++, $file.Name) ([byte[]]@($prefix) + [IO.File]::ReadAllBytes($file.FullName))
}
Seed-Text 'cursor_inf' 'seed.inf' @'
[Version]
signature="$CHICAGO$"

[DefaultInstall]
AddReg = Scheme.Reg

[Scheme.Reg]
HKCU,"Control Panel\Cursors\Schemes","%SCHEME_NAME%",,"%10%\%CUR_DIR%\%pointer%,%10%\%CUR_DIR%\%help%,%10%\%CUR_DIR%\%work%"

[Strings]
CUR_DIR = "Cursors\Neon"
SCHEME_NAME = "Neon"
pointer = "normal.cur"
help = "help.cur"
work = "working.ani"
'@
Seed-Text 'tweaks' 'seed.txt' "Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced`nTaskbarAl`nsome text"
Seed-Text 'url' 'seed.txt' "https://api.open-meteo.com/v1/forecast?latitude=-23.5&longitude=-46.6`nAccept: application/json"
Seed-Text 'bridge' 'hello.json' '{"type":"hello","version":1,"plugins":[{"id":"io.github.x.y","instance":1}]}'
Seed-Text 'bridge' 'set.json' '{"type":"set","instance":3,"property":"title","value":"Song"}'
Seed-Text 'plans' 'video.json' '{"version":1,"entries":[{"monitor":"primary","file":"C:\\v\\a.mp4","fit":"fill"},{"monitor":"all","file":"","fit":"fill"}],"policy":{"pauseOnFullscreen":true,"pauseOnBattery":true,"pauseWhenCovered":true,"fpsLimit":30,"mute":true}}' 2
Seed-Text 'plans' 'host.json' '{"version":1,"plugins":[]}' 1
Seed-Text 'plans' 'bar.json' '{"version":1,"bars":[]}' 0
foreach ($message in '{"jsonrpc":"2.0","id":1,"method":"status.get","params":{}}',
                     '{"jsonrpc":"2.0","id":1,"result":{"ok":true}}',
                     '{"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"no"}}') {
    [byte[]] $body = $utf8.GetBytes($message)
    [byte[]] $length = [BitConverter]::GetBytes([uint32]$body.Length)
    Seed 'ipc' ("frame-{0}.bin" -f $message.Length) ([byte[]]@(7) + $length + $body)
}

# ---- run ------------------------------------------------------------------------------------------
$failed = @()
foreach ($name in $all) {
    $corpus = Join-Path $work "corpus\$name"
    $crashes = Join-Path $work "crashes\$name"
    New-Item -ItemType Directory -Force $corpus, $crashes | Out-Null
    $log = Join-Path $work "$name.log"
    $exe = Join-Path $bin "fuzz_$name.exe"
    Write-Host "fuzz $name ($Seconds s)..."
    # libFuzzer reports on stderr; PowerShell 5.1 would turn every line into an error.
    $arguments = "`"$corpus`" -max_total_time=$Seconds -max_len=65536 -timeout=10 -rss_limit_mb=2048 " +
        "`"-artifact_prefix=$crashes\\`" -print_final_stats=1"
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -Wait -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput "$log.out"
    $code = $process.ExitCode
    $runs = (Select-String -Path $log -Pattern 'stat::number_of_executed_units:\s*(\d+)' | Select-Object -Last 1)
    $count = if ($runs) { $runs.Matches[0].Groups[1].Value } else { '?' }
    $found = @(Get-ChildItem $crashes -File -ErrorAction SilentlyContinue)
    if ($code -ne 0 -or $found.Count -gt 0) {
        $failed += $name
        Write-Host "  FAIL: exit $code, $($found.Count) artifact(s) in $crashes (log: $log)"
    }
    else {
        Write-Host "  ok: $count runs, corpus $(@(Get-ChildItem $corpus -File).Count) inputs"
    }
}
if ($failed.Count -gt 0) { throw "Fuzzing found problems in: $($failed -join ', ')" }
Write-Host "fuzz: $($all.Count) target(s), nothing found."
