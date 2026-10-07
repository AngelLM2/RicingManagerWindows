<#
    v1.8 break-it suite (docs/16, "a campanha de quebra"): what an impatient or unlucky person does, and what
    a hostile file or a damaged disk does. Run ONLY inside Windows Sandbox (scripts\sandbox.ps1 -Suite break):
    it applies rices, damages Ricing Manager's own files, kills its processes and changes the time zone.

    Nothing here may crash a process of ours (an exit code above 4, or a core that has to be started again
    because it died), leave Windows half-changed, or leave the person without a taskbar.
#>
param(
    [Parameter(Mandatory)] [string] $Bin,
    [Parameter(Mandatory)] [string] $Work,
    [Parameter(Mandatory)] [string] $Out
)

$ErrorActionPreference = 'Continue'
$results = New-Object System.Collections.Generic.List[object]
function Check([string] $Name, $Condition, [string] $Detail = '') {
    $results.Add([pscustomobject]@{ name = $Name; ok = [bool]$Condition; detail = $Detail })
}
function Note([string] $Name, [string] $Detail) {
    $results.Add([pscustomobject]@{ name = "info:$Name"; ok = $true; detail = $Detail })
}

$rmw = Join-Path $Bin 'rmw.exe'
$data = Join-Path $Work 'data'
$settings = Join-Path $data 'config\settings.toml'
$riceRoot = Join-Path $Bin 'extra-rices'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr window);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SystemParametersInfoW(uint action, uint param, ref RECT value, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr window, ref RECT rect);
'@

# A process of ours, with a deadline: what it said, how it ended. An NTSTATUS (an access violation, a stack
# overflow, a fail-fast) is negative as an Int32; ours are 0..4.
function Run([string] $Exe, [string[]] $Arguments, [int] $Seconds = 60) {
    $stdout = Join-Path $env:TEMP "break-$([guid]::NewGuid()).out"
    $stderr = "$stdout.err"
    $quoted = @($Arguments | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
    $p = Start-Process $Exe -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $done = $p.WaitForExit($Seconds * 1000)
    if (-not $done) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $text = ((Get-Content $stdout -Raw -ErrorAction SilentlyContinue) + (Get-Content $stderr -Raw -ErrorAction SilentlyContinue)).Trim()
    Remove-Item $stdout, $stderr -Force -ErrorAction SilentlyContinue
    [pscustomobject]@{ code = $(if ($done) { $p.ExitCode } else { -1 }); output = $text; timedOut = -not $done }
}
function Rmw([string[]] $Arguments, [int] $Seconds = 120) { Run $rmw ($Arguments + @('--data-dir', $data)) $Seconds }
function Crashed($Result) { $Result.timedOut -or $Result.code -lt 0 -or $Result.code -gt 4 }
function Wait-Until([scriptblock] $Condition, [double] $Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return [bool](& $Condition)
}
function Core { Get-Process rmw-core -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Native-Visible { [Rmw.Win]::IsWindowVisible([Rmw.Win]::FindWindowW('Shell_TrayWnd', [NullString]::Value)) }
function Work-Area {
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::SystemParametersInfoW(0x30, 0, [ref]$rect, 0) # SPI_GETWORKAREA
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Screen-Bounds { [System.Windows.Forms.Screen]::PrimaryScreen.Bounds }
function Write-Settings([string] $Text) {
    New-Item -ItemType Directory -Force (Split-Path $settings) | Out-Null
    [System.IO.File]::WriteAllText($settings, $Text)
}
function Write-Rice([string] $Folder, [string] $Body) {
    New-Item -ItemType Directory -Force $Folder | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $Folder 'rice.toml'), "schema = 1`n`n[meta]`nname = `"Break`"`n`n$Body")
}
# Lines a crash or a terminate leaves in our logs.
function Fatal-Lines([string] $DataDir = $data) {
    @(Get-ChildItem (Join-Path $DataDir 'logs') -Filter '*.log' -ErrorAction SilentlyContinue |
        ForEach-Object { Select-String -Path $_.FullName -Pattern 'fatal|terminate called|unhandled' -ErrorAction SilentlyContinue } |
        ForEach-Object { "$($_.Filename): $($_.Line)" })
}
function Step([string] $Name, [scriptblock] $Body) {
    try { & $Body }
    catch { Check "break:$Name-ran" $false "$($_.Exception.Message) at $($_.InvocationInfo.ScriptLineNumber)" }
}
$sakura = Join-Path $riceRoot 'Sakura Dawn'
$mocha = Join-Path $riceRoot 'Mocha Kawaii'
$tokyo = Join-Path $riceRoot 'Tokyo Midnight'
$diva = Join-Path $riceRoot 'Diva Stage'
Note 'rices' "$(@(Get-ChildItem $riceRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', ')"
Note 'screen' "$(Screen-Bounds) work=$(Work-Area)"

# ---- 1. the owner's PC: an apostrophe in the profile (C:\Users\user'), accents in the paths --------------
Step 'odd-paths' {
    $oddRoot = Join-Path $Work ('Ricing Ma' + [char]0x00F1 + "ager's")
    $oddBin = Join-Path $oddRoot 'bin'
    New-Item -ItemType Directory -Force $oddRoot | Out-Null
    Copy-Item $Bin $oddBin -Recurse -Force
    $oddData = Join-Path $Work ('Jos' + [char]0x00E9 + "'s data")
    $oddRmw = Join-Path $oddBin 'rmw.exe'
    $r = Run $oddRmw @('apply', $sakura, '--wait', '--data-dir', $oddData) 180
    Check 'break:odd-paths-apply' ($r.code -eq 0) $r.output
    $bar = Get-Process rmw-bar -ErrorAction SilentlyContinue | Select-Object -First 1
    Check 'break:odd-paths-bar-runs' ($null -ne $bar -and $bar.Path -like "$oddBin*") "bar: $(if ($bar) { $bar.Path })"
    $s = Run $oddRmw @('status', '--data-dir', $oddData)
    Check 'break:odd-paths-status' ($s.code -eq 0 -and $s.output -match 'Sakura') $s.output
    $r = Run $oddRmw @('restore', '--original', '--wait', '--data-dir', $oddData) 180
    Check 'break:odd-paths-restore' ($r.code -eq 0) $r.output
    [void](Run $oddRmw @('shutdown', '--wait', '--data-dir', $oddData))
    $fatal = Fatal-Lines $oddData
    Check 'break:odd-paths-no-fatal' ($fatal.Count -eq 0) ($fatal -join ' | ')
    Check 'break:odd-paths-taskbar-back' (Wait-Until { Native-Visible } 10) ''
}

# ---- 2. settings.toml damaged in every way: the core reads it, says what is wrong, keeps going ----------
Step 'settings-damaged' {
    $r = Rmw @('status')
    Check 'break:core-starts' ($r.code -eq 0) $r.output
    $nested = ('a = ' + ('[' * 5000) + (']' * 5000))
    $variants = [ordered]@{
        'empty'        = ''
        'garbage'      = -join ((1..2000) | ForEach-Object { [char](Get-Random -Minimum 1 -Maximum 0xD7FF) })
        'not-toml'     = "{ `"json`": true }"
        'nul-bytes'    = "schema = 1`n" + ([string][char]0 * 64)
        'deep'         = "schema = 1`n$nested"
        'huge'         = "schema = 1`n" + ('# ' + ('x' * 1022) + "`n") * 6000
        'wrong-types'  = "schema = `"one`"`n[core]`nkeep_in_tray = 7`n[tiling]`nenabled = `"yes`"`ngap = -40`nworkspaces = 99`n"
        'bad-hotkeys'  = "schema = 1`n[[hotkey]]`nkeys = `"Ctrl+Ctrl+Ctrl`"`naction = `"nothing.at.all`"`n[[hotkey]]`nkeys = `"`"`naction = `"rice.apply`"`npath = `"C:\\nowhere`"`n"
        'bad-rules'    = "schema = 1`n[[rule]]`nwhen = `"time`"`nat = `"25:99`"`naction = `"appearance.dark`"`n[[rule]]`nwhen = `"nonsense`"`n"
    }
    foreach ($name in $variants.Keys) {
        Write-Settings $variants[$name]
        $reload = Rmw @('settings', 'reload') 60
        $after = Rmw @('status') 60
        Check "break:settings-$name" (-not (Crashed $reload) -and $after.code -eq 0) "reload=$($reload.code): $(($reload.output -split "`n" | Select-Object -First 2) -join ' / ') status=$($after.code)"
    }
    # Read-only, then gone while the core runs.
    Write-Settings "schema = 1`n[core]`nkeep_in_tray = true`n"
    Set-ItemProperty $settings -Name IsReadOnly -Value $true
    $reload = Rmw @('settings', 'reload')
    Check 'break:settings-read-only' (-not (Crashed $reload)) "$($reload.code): $($reload.output)"
    Set-ItemProperty $settings -Name IsReadOnly -Value $false
    Remove-Item $settings -Force
    $reload = Rmw @('settings', 'reload')
    Check 'break:settings-gone' (-not (Crashed $reload) -and (Rmw @('status')).code -eq 0) "$($reload.code): $($reload.output)"
    $fatal = Fatal-Lines
    Check 'break:settings-no-fatal' ($fatal.Count -eq 0) ($fatal -join ' | ')
}

# ---- 3. apply after apply after apply, without waiting; a restore in the middle -----------------------------
Step 'rapid-fire' {
    $coreBefore = Core
    $runs = foreach ($rice in @($sakura, $mocha, $diva, $sakura)) {
        Start-Process $rmw -ArgumentList @('apply', "`"$rice`"", '--data-dir', "`"$data`"") -NoNewWindow -PassThru
        Start-Sleep -Milliseconds 150
    }
    $restoring = Start-Process $rmw -ArgumentList @('restore', '--original', '--data-dir', "`"$data`"") -NoNewWindow -PassThru
    $codes = @($runs + $restoring | ForEach-Object { [void]$_.WaitForExit(120000); $_.ExitCode })
    Check 'break:rapid-fire-no-crash' (@($codes | Where-Object { $_ -lt 0 -or $_ -gt 4 }).Count -eq 0) "exit codes: $($codes -join ', ') (3 = busy)"
    # Whatever got through, it settles: one state, the core the same, nothing fatal.
    Start-Sleep -Seconds 10
    $status = Rmw @('status')
    Check 'break:rapid-fire-core-survives' ($null -ne (Core) -and $null -ne $coreBefore -and (Core).Id -eq $coreBefore.Id) "core $($coreBefore.Id) -> $((Core).Id)"
    Check 'break:rapid-fire-status' ($status.code -eq 0) $status.output
    $r = Rmw @('restore', '--original', '--wait') 180
    Check 'break:rapid-fire-restores' ($r.code -eq 0) $r.output
    Check 'break:rapid-fire-taskbar-back' (Wait-Until { Native-Visible } 10) ''
}

# ---- 4. the applied rice's folder deleted; undo and restore still work (the store keeps copies) ------------
Step 'rice-deleted' {
    $copy = Join-Path $Work 'deleted-rice'
    Copy-Item $mocha $copy -Recurse -Force
    $r = Rmw @('apply', $copy, '--wait') 180
    Check 'break:deleted-rice-applied' ($r.code -eq 0) $r.output
    Remove-Item $copy -Recurse -Force
    Start-Sleep -Seconds 3 # the folder watch sees it go
    $undo = Rmw @('action', 'rice.undo') 180
    Check 'break:deleted-rice-undo' (-not (Crashed $undo)) "$($undo.code): $($undo.output)"
    $r = Rmw @('restore', '--original', '--wait') 180
    Check 'break:deleted-rice-restore' ($r.code -eq 0) $r.output
    Check 'break:deleted-rice-core-alive' ((Rmw @('status')).code -eq 0) ''
}

# ---- 5. the bar killed again and again: the supervisor gives up, Windows' taskbar is there, bar.on brings ours
Step 'bar-killed' {
    $r = Rmw @('apply', $sakura, '--wait') 180
    Check 'break:bar-rice' ($r.code -eq 0 -and (Wait-Until { $null -ne (Get-Process rmw-bar -ErrorAction SilentlyContinue) } 15)) $r.output
    foreach ($round in 1..6) {
        Get-Process rmw-bar -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Milliseconds 1500
    }
    Check 'break:bar-killed-taskbar-shows' (Wait-Until { Native-Visible } 10) "bar: $(@(Get-Process rmw-bar -ErrorAction SilentlyContinue).Count)"
    Check 'break:bar-killed-core-alive' ((Rmw @('status')).code -eq 0) ''
    Start-Sleep -Seconds 5
    $on = Rmw @('action', 'bar.on')
    Check 'break:bar-on-after-giving-up' (-not (Crashed $on) -and (Wait-Until { $null -ne (Get-Process rmw-bar -ErrorAction SilentlyContinue) } 15)) "$($on.code): $($on.output)"
    $r = Rmw @('restore', '--original', '--wait') 180
    Check 'break:bar-killed-restore' ($r.code -eq 0 -and (Wait-Until { Native-Visible } 10)) $r.output
}

# ---- 6. the command line, given nonsense -----------------------------------------------------------------------
Step 'cli' {
    $randomFile = Join-Path $Work 'random.rice'
    [System.IO.File]::WriteAllBytes($randomFile, [byte[]](1..4096 | ForEach-Object { Get-Random -Maximum 256 }))
    $emptyFile = Join-Path $Work 'empty.rice'
    [System.IO.File]::WriteAllBytes($emptyFile, [byte[]]@())
    $long = 'x' * 32000
    $weird = 'rice ' + [char]0x2728 + ' ' + [char]0x65E5 + [char]0x672C + " 'quoted' `"double`""
    $cases = @(
        @(), @('apply'), @('apply', ''), @('apply', 'C:\nowhere\at\all'), @('apply', $rmw), @('apply', $randomFile),
        @('apply', $long), @('apply', $weird), @('validate', $Work), @('restore'), @('restore', '--snapshot', '-1'),
        @('restore', '--snapshot', '99999999999999999999'), @('restore', '--snapshot', 'abc'), @('export', ''),
        @('export', 'C:\Windows\System32\nope'), @('pack', 'C:\nowhere'), @('import', $randomFile), @('import', $emptyFile),
        @('import', 'C:\nowhere.rice'), @('inspect', 'nonsense'), @('action', 'nonsense'), @('action', 'launch.app'),
        @('action', 'launch.app', '--app', $long), @('action', 'rice.apply', '--path', $randomFile), @('autostart', 'maybe'),
        @('plugin', 'install', $randomFile), @('plugin', 'install', 'C:\nowhere'), @('plugin', 'remove', '..\..\x'),
        @('plugin', 'approve', $weird), @('plugin', 'new', 'virus', (Join-Path $Work 'p')), @('settings'), @('settings', 'explode'),
        @('--version'), @('--help'), @('history', '--bogus'), @($long), @($weird), @('monitors'), @('apps')
    )
    $bad = @()
    foreach ($case in $cases) {
        $r = Rmw $case 90
        if (Crashed $r) { $bad += "[$(($case | ForEach-Object { if ($_.Length -gt 40) { $_.Substring(0, 40) + '...' } else { $_ } }) -join ' ')] -> $($r.code)" }
    }
    Check 'break:cli-never-crashes' ($bad.Count -eq 0) ($bad -join '; ')
    Check 'break:cli-core-alive' ((Rmw @('status')).code -eq 0) ''
    $fatal = Fatal-Lines
    Check 'break:cli-no-fatal' ($fatal.Count -eq 0) ($fatal -join ' | ')
}

# ---- 7. a wallpaper picture far larger than any screen; a picture that is not one ---------------------------
Step 'huge-picture' {
    $folder = Join-Path $Work 'huge-picture'
    New-Item -ItemType Directory -Force $folder | Out-Null
    $bitmap = New-Object System.Drawing.Bitmap 7000, 7000
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point 7000, 7000), [System.Drawing.Color]::DeepPink, [System.Drawing.Color]::Navy)
    $graphics.FillRectangle($brush, 0, 0, 7000, 7000)
    $bitmap.Save((Join-Path $folder 'huge.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose(); $brush.Dispose()
    Write-Rice $folder "[wallpaper]`nfile = `"huge.png`"`nfit = `"fill`"`n"
    $r = Rmw @('apply', $folder, '--wait') 240
    Check 'break:huge-picture' (-not (Crashed $r)) "$($r.code): $($r.output)"
    $fake = Join-Path $Work 'fake-picture'
    New-Item -ItemType Directory -Force $fake | Out-Null
    Copy-Item $rmw (Join-Path $fake 'picture.png') # an executable named like a picture
    Write-Rice $fake "[wallpaper]`nfile = `"picture.png`"`n"
    $r = Rmw @('apply', $fake, '--wait') 120
    Check 'break:fake-picture-refused' ($r.code -ne 0 -and -not (Crashed $r)) "$($r.code): $($r.output)"
    $r = Rmw @('restore', '--original', '--wait') 180
    Check 'break:huge-picture-restore' ($r.code -eq 0) $r.output
}

# ---- 8. bars of absurd sizes: margins and a thickness larger than the screen ---------------------------------
Step 'absurd-bars' {
    $folder = Join-Path $Work 'absurd-bar'
    $screen = Screen-Bounds
    foreach ($bar in @(
            "[[bar]]`nposition = `"top`"`nthickness = 512`nmargin = [1024, 1024, 1024, 1024]`nmode = `"reserve`"`nleft = [`"start`", `"windows`"]`nright = [`"tray`", `"clock`"]`n",
            "[[bar]]`nposition = `"left`"`nthickness = 4`nradius = 1000`nlength = `"auto`"`ncenter = [`"clock`"]`n",
            "[[bar]]`nposition = `"bottom`"`nthickness = 40`nlength = 0`nleft = [`"clock`"]`n",
            "[[bar]]`nposition = `"right`"`nthickness = 300`nmode = `"reserve`"`ncenter = [`"windows`", `"tray`", `"media`", `"clock`"]`n")) {
        Write-Rice $folder $bar
        $r = Rmw @('apply', $folder, '--wait') 120
        Start-Sleep -Seconds 3
        # (Not $work: PowerShell names ignore case, and $Work is this script's folder.)
        $area = Work-Area
        $sane = $area.Width -ge $screen.Width / 2 - 2 -and $area.Height -ge $screen.Height / 2 - 2
        Check "break:absurd-bar-$($results.Count)" (-not (Crashed $r) -and $sane -and ((Rmw @('status')).code -eq 0)) "apply=$($r.code) work=$area screen=${screen}: $(($bar -split "`n")[1..3] -join ' ')"
    }
    $r = Rmw @('restore', '--original', '--wait') 180
    Check 'break:absurd-bars-restore' ($r.code -eq 0 -and (Wait-Until { (Work-Area).Height -ge $screen.Height - 100 } 15)) "work=$(Work-Area)"
}

# ---- 9. a time zone change in the middle of a light/dark schedule: it follows at once ----------------------------
Step 'time-zone' {
    function Apps-Light { (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue).AppsUseLightTheme }
    $zoneBefore = (Get-TimeZone).Id
    Set-TimeZone -Id 'E. South America Standard Time' -ErrorAction SilentlyContinue # UTC-3, the owner's
    $now = Get-Date
    $folder = Join-Path $Work 'scheduled'
    Write-Rice $folder "[appearance]`nmode = `"schedule`"`n`n[appearance.schedule]`nlight_at = `"$($now.AddHours(-1).ToString('HH:mm'))`"`ndark_at = `"$($now.AddHours(3).ToString('HH:mm'))`"`n"
    $r = Rmw @('apply', $folder, '--wait') 120
    Check 'break:schedule-light-now' ($r.code -eq 0 -and (Apps-Light) -eq 1) "$($r.output) light=$(Apps-Light)"
    # Twelve hours later on the clock (Tokyo): past dark_at, before the next light_at.
    Set-TimeZone -Id 'Tokyo Standard Time'
    $dark = Wait-Until { (Apps-Light) -eq 0 } 90
    Check 'break:schedule-follows-a-new-time-zone' $dark "light=$(Apps-Light) now $(Get-Date -Format 'HH:mm') in $((Get-TimeZone).Id)"
    Set-TimeZone -Id $zoneBefore
    $r = Rmw @('restore', '--original', '--wait') 120
    Check 'break:schedule-restore' ($r.code -eq 0) $r.output
}

# ---- 10. state.db damaged while the core is off: it starts, says so, and the next apply works -------------------
Step 'state-damaged' {
    [void](Rmw @('shutdown', '--wait'))
    $db = Join-Path $data 'state.db'
    $kept = Join-Path $Work 'state.db.kept'
    Copy-Item $db $kept -Force -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllBytes($db, [byte[]](1..512 | ForEach-Object { Get-Random -Maximum 256 }))
    $r = Rmw @('status') 60
    Check 'break:damaged-db-core-starts' ($r.code -eq 0) $r.output
    $r = Rmw @('apply', $sakura, '--wait') 180
    Check 'break:damaged-db-apply' ($r.code -eq 0) $r.output
    $r = Rmw @('restore', '--original', '--wait') 180
    Check 'break:damaged-db-restore' ($r.code -eq 0) $r.output
    Note 'damaged-db-files' "$(@(Get-ChildItem $data -Filter 'state*' | ForEach-Object Name) -join ', ')"
    [void](Rmw @('shutdown', '--wait'))
    [System.IO.File]::WriteAllBytes($db, [byte[]]@())
    $r = Rmw @('status') 60
    Check 'break:empty-db-core-starts' ($r.code -eq 0) $r.output
}

# ---- the end: nothing of ours left running, Windows' taskbar there, the logs kept --------------------------------
[void](Rmw @('shutdown', '--wait'))
Check 'break:taskbar-at-the-end' (Wait-Until { Native-Visible } 10) ''
$fatal = Fatal-Lines
Check 'break:no-fatal-at-all' ($fatal.Count -eq 0) ($fatal -join ' | ')
if (Test-Path (Join-Path $data 'logs')) {
    Remove-Item (Join-Path $Out 'break-logs') -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'break-logs') -Recurse -Force
}
$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'break.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
