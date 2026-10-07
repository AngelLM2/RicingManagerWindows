<#
    M5 system tests (the system tray in our bar). Run ONLY inside Windows Sandbox
    (run-in-sandbox.ps1 -Suite tray): our tray window takes every app's Shell_NotifyIcon calls.

    Acceptance of M5 (docs/04): the apps' icons appear in the bar and answer clicks (left, right,
    double; NOTIFYICON_VERSION 0, 3 and 4); icons added before the bar started are found (also from
    message-only windows); hiding, changing and deleting follow; Explorer restarting -> the tray
    comes back; the bar going away leaves every icon in Windows' tray; plugins see the tray and
    click it. The test apps are rmw_tray_test_app.exe (tests/support/TrayTestApp.cpp).
#>
param(
    [Parameter(Mandatory)] [string] $Bin,
    [Parameter(Mandatory)] [string] $Work,
    [Parameter(Mandatory)] [string] $Out,
    [Parameter(Mandatory)] [string] $Examples
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
$app = Join-Path $Bin 'rmw_tray_test_app.exe'
$data = Join-Path $Work 'data'
$barLog = Join-Path $data 'logs\bar.log'
$pluginLog = Join-Path $data 'logs\plugins.log'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowExW(System.IntPtr parent, System.IntPtr after, string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr window, uint message, System.IntPtr wParam, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(System.IntPtr window, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetCursorPos(int x, int y);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr window, ref RECT rect);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetDC(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int ReleaseDC(System.IntPtr window, System.IntPtr dc);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SystemParametersInfoW(uint action, uint param, ref RECT value, uint winIni);
[System.Runtime.InteropServices.DllImport("gdi32.dll")]
public static extern bool BitBlt(System.IntPtr target, int x, int y, int width, int height, System.IntPtr source, int sourceX, int sourceY, uint operation);
'@

function Invoke-Rmw([string[]] $Arguments) {
    $output = $null | & $rmw @Arguments --data-dir $data 2>&1 | Out-String
    return [pscustomobject]@{ code = $LASTEXITCODE; output = $output.Trim() }
}
function Wait-Until([scriptblock] $Condition, [double] $Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return [bool](& $Condition)
}
function Bar-Process { Get-Process rmw-bar -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Pid-Of([IntPtr] $Window) { $p = [uint32]0; [void][Rmw.Win]::GetWindowThreadProcessId($Window, [ref]$p); $p }
# Explorer's taskbar: the first "Shell_TrayWnd" that is not our tray window (which comes first while
# the bar shows a tray).
function Native-Tray {
    $bar = Bar-Process
    $window = [IntPtr]::Zero
    while ($true) {
        $window = [Rmw.Win]::FindWindowExW([IntPtr]::Zero, $window, 'Shell_TrayWnd', [NullString]::Value)
        if ($window -eq [IntPtr]::Zero) { return $window }
        if (-not $bar -or (Pid-Of $window) -ne $bar.Id) { return $window }
    }
}
function Native-Visible { [Rmw.Win]::IsWindowVisible((Native-Tray)) }
function First-Tray-Is-Ours {
    $bar = Bar-Process
    $first = [Rmw.Win]::FindWindowW('Shell_TrayWnd', [NullString]::Value)
    $bar -and $first -ne [IntPtr]::Zero -and (Pid-Of $first) -eq $bar.Id
}
function Work-Area {
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::SystemParametersInfoW(0x30, 0, [ref]$rect, 0) # SPI_GETWORKAREA
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Bar-Rect {
    $window = [Rmw.Win]::FindWindowW('RicingManagerWindows.Bar', [NullString]::Value)
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::GetWindowRect($window, [ref]$rect)
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Shot([string] $Name) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $target = $graphics.GetHdc()
    $screenDc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    [void][Rmw.Win]::BitBlt($target, 0, 0, $bounds.Width, $bounds.Height, $screenDc, $bounds.X, $bounds.Y, 0x40CC0020)
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $screenDc)
    $graphics.ReleaseHdc($target)
    $bitmap.Save((Join-Path $Out "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
}
function Mouse([int] $X, [int] $Y, [uint32[]] $Flags, [int] $PauseMs = 700) {
    [void][Rmw.Win]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 200
    foreach ($flag in $Flags) { [Rmw.Win]::mouse_event($flag, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 30 }
    Start-Sleep -Milliseconds $PauseMs
}
function Left-Click([int] $X, [int] $Y) { Mouse $X $Y @(0x0002, 0x0004) }
function Right-Click([int] $X, [int] $Y) { Mouse $X $Y @(0x0008, 0x0010) }
function Double-Click([int] $X, [int] $Y) { Mouse $X $Y @(0x0002, 0x0004, 0x0002, 0x0004) }
function Away { [void][Rmw.Win]::SetCursorPos(400, 300); Start-Sleep -Milliseconds 300 }
function Log-Text([string] $File) { if (Test-Path $File) { Get-Content $File -Raw -ErrorAction SilentlyContinue } else { '' } }
function Log-Has([string] $File, [string] $Text) { (Log-Text $File) -match [regex]::Escape($Text) }
function Count-Of([string] $File, [string] $Text) {
    if (-not (Test-Path $File)) { return 0 }
    @(Get-Content $File -ErrorAction SilentlyContinue | Where-Object { $_ -match [regex]::Escape($Text) }).Count
}

# ---- the test apps ---------------------------------------------------------------------------------------
$apps = @{}
function Start-TrayApp([int] $N, [int] $Version, [string] $Kind) {
    $log = Join-Path $Work "tray-app-$N.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $process = Start-Process $app -ArgumentList "$N $Version $Kind `"$log`"" -PassThru
    $apps[$N] = [pscustomobject]@{ n = $N; version = $Version; kind = $Kind; log = $log; process = $process }
    [void](Wait-Until { Log-Has $log 'add ' } 5)
    return $apps[$N]
}
function App-Window($App) {
    $class = "RmwTrayTest$($App.n)"
    if ($App.kind -eq 'message') { return [Rmw.Win]::FindWindowExW([IntPtr]::new(-3), [IntPtr]::Zero, $class, [NullString]::Value) }
    return [Rmw.Win]::FindWindowW($class, [NullString]::Value)
}
function Tell($App, [int] $Offset) { [void][Rmw.Win]::PostMessageW((App-Window $App), 0x8000 + $Offset, [IntPtr]::Zero, [IntPtr]::Zero) }
function App-Log($App) { Log-Text $App.log }
# Our number for an app's icon: "tray: icon <number> added by rmw_tray_test_app.exe (pid <pid>)".
function Icon-Number($App) {
    $match = [regex]::Matches((Log-Text $barLog), "tray: icon (\d+) added by rmw_tray_test_app\.exe \(pid $($App.process.Id)\)")
    if ($match.Count -eq 0) { return $null }
    [int]$match[$match.Count - 1].Groups[1].Value
}
# The icons the widget shows, in order (the last "shows icons" line).
function Shown {
    $match = [regex]::Matches((Log-Text $barLog), 'tray: w\d+ shows icons \[([\d, ]*)\], (\d+) in the overflow')
    if ($match.Count -eq 0) { return @() }
    $list = $match[$match.Count - 1].Groups[1].Value
    if (-not $list) { return @() }
    @($list -split ',\s*' | ForEach-Object { [int]$_ })
}
function Overflow {
    $match = [regex]::Matches((Log-Text $barLog), 'tray: w\d+ shows icons \[[\d, ]*\], (\d+) in the overflow')
    if ($match.Count -eq 0) { return -1 }
    [int]$match[$match.Count - 1].Groups[1].Value
}
# Where an icon is: the tray is alone in the start zone (6 px in, 28 px a button), maybe after the
# chevron (22 px). The bar's height tells the scale (thickness 44).
function Icon-Point($App, [bool] $Chevron = $false) {
    $number = Icon-Number $App
    $index = [array]::IndexOf((Shown), $number)
    if ($index -lt 0) { return $null }
    $bar = Bar-Rect
    $scale = $bar.Height / 44.0
    $x = $bar.Left + [int](($(if ($Chevron) { 22 } else { 0 }) + 6 + 28 * $index + 14) * $scale)
    $y = $bar.Top + [int]($bar.Height / 2)
    return [System.Drawing.Point]::new($x, $y)
}
function Last-Rect($App) {
    $match = [regex]::Matches((App-Log $App), 'rect ok (-?\d+) (-?\d+) (-?\d+) (-?\d+)')
    if ($match.Count -eq 0) { return $null }
    $g = $match[$match.Count - 1].Groups
    [System.Drawing.Rectangle]::FromLTRB([int]$g[1].Value, [int]$g[2].Value, [int]$g[3].Value, [int]$g[4].Value)
}

function Write-Rice([string] $Folder, [string] $Tray, [string] $Extra = '') {
    New-Item -ItemType Directory -Force $Folder | Out-Null
    @"
schema = 1

[meta]
name = "Tray test"

[palette]
night = "#1a1b26"
ink = "#c0caf5"

$Extra

[[bar]]
position = "bottom"
mode = "reserve"
thickness = 44
background = { kind = "solid", tint = "palette.night" }
foreground = "palette.ink"
hide_native_taskbar = true
left = [$Tray]
center = ["clock"]
"@ | Out-File (Join-Path $Folder 'rice.toml') -Encoding ascii
}

for ($i = 0; $i -lt 30 -and -not (Native-Tray); $i++) { Start-Sleep 1 }
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

# ---- 1. icons added before the bar: a version-4 app (window) and a version-0 one (message-only) -----------
$a = Start-TrayApp 1 4 'top'
$b = Start-TrayApp 2 0 'message'
Check 'tray:apps-in-windows-tray-before' (Wait-Until { (App-Log $a) -match 'rect ok' -and (App-Log $b) -match 'rect ok' } 5) "$(App-Log $a) | $(App-Log $b)"

$rice = Join-Path $Work 'rice-tray'
Write-Rice $rice '{ id = "tray", show = "all" }'
$r = Invoke-Rmw @('apply', $rice, '--wait')
Check 'tray:apply' ($r.code -eq 0) $r.output
Check 'tray:bar-running' (Wait-Until { $null -ne (Bar-Process) -and -not (Native-Visible) } 20) ''
Check 'tray:tray-window-up' (Wait-Until { Log-Has $barLog 'tray: the tray window is up' } 10) ''
Check 'tray:our-window-comes-first' (Wait-Until { First-Tray-Is-Ours } 5) ''
$area = Work-Area # not $work: PowerShell names ignore case, and $Work is the folder
Check 'tray:app-bar-still-reserves' ($area.Bottom -gt 0 -and $area.Bottom -le $screen.Bottom - 40) "work area bottom $($area.Bottom), screen $($screen.Bottom)"

$found = Wait-Until { $null -ne (Icon-Number $a) -and $null -ne (Icon-Number $b) } 10
Check 'tray:icons-from-before-the-bar' $found "a=$(Icon-Number $a) b=$(Icon-Number $b)"
Check 'tray:message-only-window-invited' ((App-Log $b) -match 'taskbarcreated') (App-Log $b)

# ---- 2. an icon added while the bar is up (version 3) ----------------------------------------------------
$c = Start-TrayApp 3 3 'top'
Check 'tray:icon-added-after' (Wait-Until { $null -ne (Icon-Number $c) } 5) "c=$(Icon-Number $c)"
Check 'tray:all-shown' (Wait-Until { @(Shown).Count -ge 3 } 5) "shown: $(Shown)"
Check 'tray:still-in-windows-tray' (Wait-Until { Tell $c 16; (App-Log $c) -match 'rect ok' } 5) (App-Log $c)
Start-Sleep -Seconds 1
Shot 'tray-1-icons'

# ---- 3. clicks, as each version expects them ------------------------------------------------------------
$point = Icon-Point $a
if ($point) {
    Left-Click $point.X $point.Y
    $aLog = App-Log $a
    Check 'tray:v4-left-click' ($aLog -match 'event lbuttondown' -and $aLog -match 'event lbuttonup' -and $aLog -match 'event select') $aLog
    $anchor = [regex]::Match($aLog, 'event select id=1 at (-?\d+),(-?\d+)')
    Check 'tray:v4-anchor-is-the-click' ($anchor.Success -and [math]::Abs([int]$anchor.Groups[1].Value - $point.X) -le 2 -and [math]::Abs([int]$anchor.Groups[2].Value - $point.Y) -le 2) "click $point, $($anchor.Value)"
    Right-Click $point.X $point.Y
    $aLog = App-Log $a
    Check 'tray:v4-right-click' ($aLog -match 'event rbuttonup' -and $aLog -match 'event contextmenu') $aLog
    $rect = Last-Rect $a
    Check 'tray:icon-rect-is-on-our-bar' ($rect -and $rect.Contains($point)) "rect $rect, click $point"
} else { Check 'tray:v4-left-click' $false "no position for a (shown: $(Shown))" }
$point = Icon-Point $b
if ($point) {
    Left-Click $point.X $point.Y
    $bLog = App-Log $b
    Check 'tray:v0-left-click' ($bLog -match 'event lbuttondown id=2' -and $bLog -match 'event lbuttonup id=2' -and $bLog -notmatch 'event select') $bLog
} else { Check 'tray:v0-left-click' $false "no position for b (shown: $(Shown))" }
$point = Icon-Point $c
if ($point) {
    Double-Click $point.X $point.Y
    $cLog = App-Log $c
    Check 'tray:v3-double-click' ($cLog -match 'event dblclk' -and ([regex]::Matches($cLog, 'event lbuttondown')).Count -eq 1) $cLog
    # A tip on hovering (the standard one: version 3).
    Away
    [void][Rmw.Win]::SetCursorPos($point.X, $point.Y)
    $bar = Bar-Process
    $tip = Wait-Until {
        $window = [IntPtr]::Zero
        while (($window = [Rmw.Win]::FindWindowExW([IntPtr]::Zero, $window, 'tooltips_class32', [NullString]::Value)) -ne [IntPtr]::Zero) {
            if ((Pid-Of $window) -eq $bar.Id -and [Rmw.Win]::IsWindowVisible($window)) { return $true }
        }
        $false
    } 3
    Check 'tray:tooltip-on-hover' $tip ''
    Shot 'tray-2-tooltip'
    Away
} else { Check 'tray:v3-double-click' $false "no position for c (shown: $(Shown))" }

# ---- 4. hide, show, change, delete, and an app that dies --------------------------------------------------
$bNumber = Icon-Number $b
Tell $b 11
Check 'tray:hidden-icon-leaves' (Wait-Until { -not ((Shown) -contains $bNumber) } 5) "shown: $(Shown)"
Tell $b 12
Check 'tray:shown-icon-comes-back' (Wait-Until { (Shown) -contains $bNumber } 5) "shown: $(Shown)"
$cNumber = Icon-Number $c
Tell $c 13
Check 'tray:deleted-icon-leaves' (Wait-Until { Log-Has $barLog "tray: icon $cNumber removed" } 5) ''
$aNumber = Icon-Number $a
Stop-Process -Id $a.process.Id -Force
Check 'tray:dead-app-icon-leaves' (Wait-Until { Log-Has $barLog "tray: icon $aNumber went away with its app" } 10) ''

# ---- 5. Explorer restarts: the bar comes back with the tray -------------------------------------------------
$before = Count-Of $b.log 'taskbarcreated'
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe | Out-Null }
[void](Wait-Until { Native-Tray -ne [IntPtr]::Zero } 30)
Check 'tray:explorer-restart-asks-again' (Wait-Until { (Count-Of $b.log 'taskbarcreated') -gt $before } 20) "before $before, now $(Count-Of $b.log 'taskbarcreated')"
Check 'tray:icons-after-explorer-restart' (Wait-Until { (Shown) -contains (Icon-Number $b) } 15) "shown: $(Shown)"
Check 'tray:our-window-first-after-restart' (Wait-Until { First-Tray-Is-Ours } 10) ''
Start-Sleep -Seconds 6 # what M3 waits: Explorer settles, the bar takes its place again
$area = Work-Area
Check 'tray:bar-reserves-after-explorer-restart' ($area.Bottom -eq $screen.Bottom - (Bar-Rect).Height) "work $area, bar $(Bar-Rect)"
Check 'tray:bar-never-hung' (-not (Log-Has (Join-Path $data 'logs\core.log') 'worker bar stopped responding')) ''
Start-Sleep -Seconds 2
Shot 'tray-3-after-explorer'

# ---- 6. the bar goes away: icons stay in Windows' tray; it comes back and finds them ----------------------
$barBefore = Bar-Process
Stop-Process -Id $barBefore.Id -Force
Start-Sleep -Milliseconds 500
$rectsBefore = Count-Of $b.log 'rect ok'
Tell $b 16
Check 'tray:icons-stay-in-windows-tray' (Wait-Until { (Count-Of $b.log 'rect ok') -gt $rectsBefore } 5) (App-Log $b)
$back = Wait-Until { $bar = Bar-Process; $bar -and $bar.Id -ne $barBefore.Id } 30
Check 'tray:bar-back' $back ''
Check 'tray:icons-found-again' (Wait-Until { $null -ne (Icon-Number $b) -and (Shown) -contains (Icon-Number $b) } 15) "shown: $(Shown)"

# ---- 7. what Windows shows (promoted) and the overflow ----------------------------------------------------
$d = Start-TrayApp 4 4 'top'
[void](Wait-Until { $null -ne (Icon-Number $d) } 5)
$settings = 'HKCU:\Control Panel\NotifyIconSettings'
$promoted = 0
foreach ($key in (Get-ChildItem $settings -ErrorAction SilentlyContinue)) {
    $values = Get-ItemProperty $key.PSPath -ErrorAction SilentlyContinue
    if ("$($values.ExecutablePath)" -like '*rmw_tray_test_app.exe') {
        $value = if ($values.UID -eq 2) { 1 } else { 0 }
        Set-ItemProperty $key.PSPath -Name IsPromoted -Value $value -Type DWord
        $promoted += $value
    }
}
Note 'promoted-entries' "$promoted test icon(s) promoted"
Write-Rice $rice '"tray"'
$r = Invoke-Rmw @('apply', $rice, '--wait')
$dNumber = Icon-Number $d
$promotedShown = Wait-Until { $s = Shown; ($s -contains (Icon-Number $b)) -and -not ($s -contains $dNumber) -and (Overflow) -ge 1 } 15
Check 'tray:only-promoted-shown' $promotedShown "shown: $(Shown), overflow $(Overflow)"
$bar = Bar-Rect
$scale = $bar.Height / 44.0
Left-Click ($bar.Left + [int]((6 + 11) * $scale)) ($bar.Top + [int]($bar.Height / 2)) # the chevron
Check 'tray:chevron-shows-the-rest' (Wait-Until { (Shown) -contains $dNumber } 5) "shown: $(Shown)"
Shot 'tray-4-overflow-open'
# v1.8 (the owner: "as barras precisam ser mais fluidas quando vão expandir"): closing and opening the
# overflow slides the icons; the row of pixels across the tray passes through several states.
function Tray-Row {
    $width = [int](300 * $scale)
    $bitmap = New-Object System.Drawing.Bitmap $width, 1
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $target = $graphics.GetHdc()
    $screenDc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    # Past the chevron (its own hover fades in and out): only the icons.
    [void][Rmw.Win]::BitBlt($target, 0, 0, $width, 1, $screenDc, $bar.Left + [int](30 * $scale), $bar.Top + [int]($bar.Height / 2), 0x40CC0020)
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $screenDc)
    $graphics.ReleaseHdc($target)
    $row = (0..($width - 1) | Where-Object { $_ % 3 -eq 0 } | ForEach-Object { $bitmap.GetPixel($_, 0).ToArgb() }) -join ','
    $graphics.Dispose(); $bitmap.Dispose()
    $row
}
function Frames-After-Chevron {
    $states = New-Object System.Collections.Generic.List[string]
    Mouse ($bar.Left + [int]((6 + 11) * $scale)) ($bar.Top + [int]($bar.Height / 2)) @(0x0002, 0x0004) 0
    $until = (Get-Date).AddMilliseconds(700)
    while ((Get-Date) -lt $until) {
        $row = Tray-Row
        if ($states.Count -eq 0 -or $states[$states.Count - 1] -ne $row) { $states.Add($row) }
    }
    $states.Count
}
Start-Sleep -Milliseconds 500
$closing = Frames-After-Chevron
Start-Sleep -Milliseconds 500
$opening = Frames-After-Chevron
Check 'v18:tray-slides-open-and-closed' ($closing -ge 3 -and $opening -ge 3) "states while closing: $closing, opening: $opening"
Check 'v18:tray-open-again' (Wait-Until { (Shown) -contains $dNumber } 5) "shown: $(Shown)"

# ---- 8. a plugin sees the tray and clicks it -------------------------------------------------------------
$clicker = Join-Path $Work 'plugin-tray-clicker'
New-Item -ItemType Directory -Force $clicker | Out-Null
[System.IO.File]::WriteAllText((Join-Path $clicker 'plugin.toml'), @'
schema = 1
[plugin]
id = "io.github.test.tray-clicker"
name = "Tray clicker"
version = "1.0.0"
api = "^1.0"
kind = ["rule"]
[permissions]
providers = ["tray"]
actions = ["tray.click"]
[entry]
script = "main.luau"
'@, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path $clicker 'main.luau'), @'
local done = false
rmw.on("tray", function(icons)
    rmw.log("tray: " .. #icons .. " icons")
    for _, icon in icons do
        if icon.tip == "rmw tray 2" and not done then
            done = true
            rmw.action("tray.click", { id = icon.id, button = "right" })
        end
    end
end)
'@, [System.Text.UTF8Encoding]::new($false))
$r = Invoke-Rmw @('plugin', 'install', $clicker, '--yes')
Check 'tray:plugin-installed' ($r.code -eq 0) $r.output
$r = Invoke-Rmw @('plugin', 'enable', 'io.github.test.tray-clicker')
$menus = Count-Of $b.log 'event rbuttonup' # b is version 0: no WM_CONTEXTMENU
$r = Invoke-Rmw @('plugin', 'install', (Join-Path $Examples 'mini-tray'), '--yes')
Check 'tray:mini-tray-installed' ($r.code -eq 0) $r.output
Check 'tray:plugin-sees-the-tray' (Wait-Until { Log-Has $pluginLog 'io.github.test.tray-clicker' -and (Log-Text $pluginLog) -match 'tray: \d+ icons' } 20) ''
Check 'tray:plugin-clicks-the-tray' (Wait-Until { (Count-Of $b.log 'event rbuttonup') -gt $menus } 10) (App-Log $b)
Write-Rice $rice '"io.github.angellm2.mini-tray"' '[plugins]
requires = ["io.github.angellm2.mini-tray"]'
$r = Invoke-Rmw @('apply', $rice, '--wait')
Check 'tray:mini-tray-gets-values' (Wait-Until { (Log-Text $barLog) -match 'first values for \S+ \(io\.github\.angellm2\.mini-tray\)' } 20) ''
Start-Sleep -Seconds 2
Shot 'tray-5-mini-tray'

$bar = Bar-Process
if ($bar) { Note 'bar-memory' ('{0:N1} MB private' -f ($bar.PrivateMemorySize64 / 1MB)) }

# ---- 9. restore from our own icon's menu (v1.6 on the owner's PC: "funções da bandeja não funcionam" —
# the restore it ran failed). The bar's forwarding of clicks is checked above with the test apps; here the
# core gets what the bar sends for a right click on its icon (version 4: WM_CONTEXTMENU), shows its menu,
# and "Restore original Windows" is clicked in it. Then no tray window of ours; the icons in Windows' tray.
$coreWindow = [Rmw.Win]::FindWindowW('RicingManagerWindows.CoreWindow', [NullString]::Value)
$menuUsed = $false
if ($coreWindow -ne [IntPtr]::Zero) {
    [void][Rmw.Win]::PostMessageW($coreWindow, 0x8001, [IntPtr]::Zero, [IntPtr]0x7B) # WM_APP+1, WM_CONTEXTMENU
    Start-Sleep -Milliseconds 1500
    Shot 'tray-9-core-menu'
    # Windows keeps a hidden menu window around: the visible one, its HMENU (MN_GETHMENU), the item by its text,
    # and a click in the middle of it (GetMenuItemRect).
    Add-Type -Namespace RmwMenu -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowExW(System.IntPtr parent, System.IntPtr after, string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr SendMessageW(System.IntPtr window, uint message, System.IntPtr wParam, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int GetMenuItemCount(System.IntPtr menu);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetMenuStringW(System.IntPtr menu, uint item, System.Text.StringBuilder text, int max, uint flags);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetMenuItemRect(System.IntPtr window, System.IntPtr menu, uint item, out RECT rect);
'@
    $script:itemRect = $null
    $opened = Wait-Until {
        $menuWindow = [IntPtr]::Zero
        while (($menuWindow = [RmwMenu.Win]::FindWindowExW([IntPtr]::Zero, $menuWindow, '#32768', [NullString]::Value)) -ne [IntPtr]::Zero) {
            if (-not [Rmw.Win]::IsWindowVisible($menuWindow)) { continue }
            $hmenu = [RmwMenu.Win]::SendMessageW($menuWindow, 0x01E1, [IntPtr]::Zero, [IntPtr]::Zero) # MN_GETHMENU
            for ($i = 0; $i -lt [RmwMenu.Win]::GetMenuItemCount($hmenu); $i++) {
                $text = New-Object System.Text.StringBuilder 256
                [void][RmwMenu.Win]::GetMenuStringW($hmenu, [uint32]$i, $text, 256, 0x400) # MF_BYPOSITION
                if ($text.ToString() -eq 'Restore original Windows') {
                    $rect = New-Object RmwMenu.Win+RECT
                    if ([RmwMenu.Win]::GetMenuItemRect([IntPtr]::Zero, $hmenu, [uint32]$i, [ref]$rect)) { $script:itemRect = $rect }
                }
            }
            if ($null -ne $script:itemRect) { break }
        }
        $null -ne $script:itemRect
    } 5
    if ($null -ne $script:itemRect) {
        $r = $script:itemRect
        Left-Click ([int](($r.Left + $r.Right) / 2)) ([int](($r.Top + $r.Bottom) / 2))
        $menuUsed = $true
    }
    Note 'tray-menu' "core window $coreWindow; menu shown with the item: $opened"
}
Check 'tray:menu-restore' ($menuUsed -and (Wait-Until { Log-Has (Join-Path $data 'logs\core.log') 'restore.original: done' } 60)) "core window $coreWindow; menu used: $menuUsed"
if (-not $menuUsed) {
    $r = Invoke-Rmw @('restore', '--original', '--wait')
    Check 'tray:restore' ($r.code -eq 0) $r.output
}
Check 'tray:bar-gone' (Wait-Until { $null -eq (Bar-Process) -and (Native-Visible) } 15) ''
$first = [Rmw.Win]::FindWindowW('Shell_TrayWnd', [NullString]::Value)
Check 'tray:explorer-tray-first-again' ($first -eq (Native-Tray)) ''
$rectsBefore = Count-Of $b.log 'rect ok'
Tell $b 16
Check 'tray:icons-in-windows-tray-after' (Wait-Until { (Count-Of $b.log 'rect ok') -gt $rectsBefore } 5) (App-Log $b)
Shot 'tray-6-restored'

foreach ($each in $apps.Values) { Tell $each 15 }
Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }
foreach ($each in $apps.Values) { if (Test-Path $each.log) { Copy-Item $each.log $Out -Force } }

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'tray.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
