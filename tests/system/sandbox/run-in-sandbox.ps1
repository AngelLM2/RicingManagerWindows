<#
    Runs INSIDE Windows Sandbox (started by scripts\sandbox.ps1).
    C:\rmw\build  = build output (read-only)
    C:\rmw\runner = this folder (read-only)
    C:\rmw\out    = results (read-write, visible on the host)
#>
param(
    [ValidateSet('unit', 'system', 'spike')]
    [string] $Suite = 'unit',
    [string] $Spike,
    [string] $SpikeArgs = '',
    [switch] $Shutdown
)

$ErrorActionPreference = 'Continue'
$out = 'C:\rmw\out'
$log = Join-Path $out "$Suite.log"
$exitCode = 1

try {
    switch ($Suite) {
        'unit' {
            # Copy out of the read-only mapping: tests create temp files next to nothing but
            # %TEMP%, yet a local copy avoids any surprise with mapped-folder semantics.
            $work = 'C:\rmw\work'
            Copy-Item 'C:\rmw\build\tests' $work -Recurse -Force
            & (Join-Path $work 'rmw_unit_tests.exe') "--gtest_output=xml:$out\unit.xml" *> $log
            $exitCode = $LASTEXITCODE
        }
        'system' {
            # System tests (real registry, cursors, wallpaper, theme) arrive with milestone M1.
            'No system tests yet.' | Out-File $log -Encoding utf8
            $exitCode = 0
        }
        'spike' {
            $work = 'C:\rmw\work'
            $exe = Join-Path $work (Split-Path $Spike -Leaf)
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item (Join-Path 'C:\rmw\build' $Spike) $exe -Force
            [string[]] $argList = if ($SpikeArgs) { $SpikeArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) } else { @() }
            & $exe @argList *> $log
            $exitCode = $LASTEXITCODE
        }
    }
}
catch {
    $_ | Out-String | Out-File $log -Append -Encoding utf8
    $exitCode = 1
}
finally {
    @{ suite = $Suite; exitCode = $exitCode; finishedAt = (Get-Date).ToString('o') } |
        ConvertTo-Json | Out-File (Join-Path $out 'done.json') -Encoding utf8
    if ($Shutdown) { shutdown.exe /s /t 5 }
}
