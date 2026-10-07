<#
    Runs INSIDE Windows Sandbox (started by scripts\sandbox.ps1).
    C:\rmw\build  = build output (read-only)
    C:\rmw\runner = this folder (read-only)
    C:\rmw\out    = results (read-write, visible on the host)
#>
param(
    [ValidateSet('unit', 'system', 'bar', 'plugins', 'tray', 'video', 'settings', 'packages', 'installer', 'tiler', 'break', 'spike')]
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
            Copy-Item 'C:\rmw\build\bin' $work -Recurse -Force
            & (Join-Path $work 'rmw_unit_tests.exe') "--gtest_output=xml:$out\unit.xml" *> $log
            $exitCode = $LASTEXITCODE
        }
        'system' {
            # Real registry, theme, cursors, wallpaper and taskbar - inside the Sandbox only.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            # Explorer must be up: the theme and wallpaper APIs talk to it.
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'system-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'bar' {
            # The custom taskbar on the real (sandboxed) desktop: hides the native taskbar.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'bar-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'plugins' {
            # Plugins on a real (sandboxed) desktop: a bar with plugin widgets, tweaks, hostile scripts.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'plugins-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out -Examples 'C:\rmw\plugins\examples' *> $log
            $exitCode = $LASTEXITCODE
        }
        'tray' {
            # The system tray in our bar: our tray window takes every app's tray icon messages.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'tray-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out -Examples 'C:\rmw\plugins\examples' *> $log
            $exitCode = $LASTEXITCODE
        }
        'video' {
            # Video wallpapers behind the desktop icons; Explorer is restarted.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'video-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'settings' {
            # The settings window, worked through UI Automation; it applies rices.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'settings-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'tiler' {
            # Tiling: every window of the (sandboxed) desktop is moved, hidden and put back.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'tiler-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'break' {
            # v1.8, the break-it campaign: damaged files, rapid fire, odd paths, absurd bars, a time zone change.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'break-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'packages' {
            # .rice and .rmwp packages, hostile ones too; it applies a rice.
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item 'C:\rmw\build\bin' (Join-Path $work 'bin') -Recurse -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'package-tests.ps1') -Bin (Join-Path $work 'bin') -Work $work -Out $out -Examples 'C:\rmw\plugins\examples' *> $log
            $exitCode = $LASTEXITCODE
        }
        'installer' {
            # The setup from scripts\package.ps1 (mapped at C:\rmw\package): install, apply, update, uninstall.
            $setup = Get-ChildItem 'C:\rmw\package' -Recurse -Filter 'RicingManager-*-setup.exe' |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if (-not $setup) { throw 'No installer under C:\rmw\package (run scripts\package.ps1 first).' }
            $work = 'C:\rmw\work'
            New-Item -ItemType Directory -Force $work | Out-Null
            Copy-Item $setup.FullName $work -Force
            for ($i = 0; $i -lt 60 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
            Start-Sleep -Seconds 5
            & (Join-Path $PSScriptRoot 'installer-tests.ps1') -Setup (Join-Path $work $setup.Name) -Out $out *> $log
            $exitCode = $LASTEXITCODE
        }
        'spike' {
            # Copy the spike's whole folder: some spikes ship DLLs next to the exe (Slint, VC runtime).
            $work = 'C:\rmw\work'
            $source = Join-Path 'C:\rmw\build' $Spike
            New-Item -ItemType Directory -Force $work | Out-Null
            Get-ChildItem (Split-Path $source) -File | Where-Object { $_.Extension -in '.exe', '.dll' } |
                Copy-Item -Destination $work -Force
            $exe = Join-Path $work (Split-Path $Spike -Leaf)
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
