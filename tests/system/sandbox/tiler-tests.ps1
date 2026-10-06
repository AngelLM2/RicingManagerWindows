<#
    M13 system tests (tiling, workspaces, shortcuts). Run ONLY inside Windows Sandbox
    (scripts\sandbox.ps1 -Suite tiler): they move every window of the (sandboxed) desktop.

    Acceptance (docs/14 §M13): 4 windows tiled and tiled again by shortcut; switching workspace in
    < 100 ms; turning tiling off puts every window back; a crash of the tiler (or of the core) puts
    every window back. Screenshots go to the output folder for a visual check (the focus border).
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
$app = Join-Path $Bin 'rmw_tiler_test_app.exe'
$data = Join-Path $Work 'data'
$settings = Join-Path $data 'config\settings.toml'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetCursorPos(int x, int y);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetForegroundWindow();
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowTextW(System.IntPtr window, System.Text.StringBuilder text, int length);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[System.Runtime.InteropServices.DllImport("dwmapi.dll")]
public static extern int DwmGetWindowAttribute(System.IntPtr window, uint attribute, ref RECT value, uint size);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr window, ref RECT rect);
[System.Runtime.InteropServices.DllImport("dwmapi.dll", EntryPoint = "DwmGetWindowAttribute")]
public static extern int DwmGetInt(System.IntPtr window, uint attribute, ref int value, uint size);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SystemParametersInfoW(uint action, uint param, ref RECT value, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetDC(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int ReleaseDC(System.IntPtr window, System.IntPtr dc);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct POINT { public int X; public int Y; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr WindowFromPoint(POINT point);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetAncestor(System.IntPtr window, uint flags);
[System.Runtime.InteropServices.DllImport("gdi32.dll")]
public static extern bool BitBlt(System.IntPtr target, int x, int y, int width, int height, System.IntPtr source, int sourceX, int sourceY, uint operation);
'@

function Invoke-Rmw([string[]] $Arguments) {
    $output = & $rmw @Arguments --data-dir $data 2>&1 | Out-String
    return [pscustomobject]@{ code = $LASTEXITCODE; output = $output.Trim() }
}
function Wait-Until([scriptblock] $Condition, [double] $Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return [bool](& $Condition)
}
function Tiler-Process { Get-Process rmw-tiler -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Work-Area {
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::SystemParametersInfoW(0x30, 0, [ref]$rect, 0) # SPI_GETWORKAREA
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
# What a person sees of the window (DWM's frame, without the invisible resize borders).
function Frame([IntPtr] $Window) {
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::DwmGetWindowAttribute($Window, 9, [ref]$rect, 16) # DWMWA_EXTENDED_FRAME_BOUNDS
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Outer([IntPtr] $Window) {
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::GetWindowRect($Window, [ref]$rect)
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Find([string] $Title) { [Rmw.Win]::FindWindowW('RmwTilerTestWindow', $Title) }
function Visible([IntPtr] $Window) { [Rmw.Win]::IsWindowVisible($Window) }
function Title([IntPtr] $Window) {
    $text = New-Object System.Text.StringBuilder 256
    [void][Rmw.Win]::GetWindowTextW($Window, $text, 256)
    $text.ToString()
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
function Click-At([int] $X, [int] $Y) {
    [void][Rmw.Win]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 100
    [Rmw.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [Rmw.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 300
}
function Click-On([IntPtr] $Window) {
    $frame = Frame $Window
    [void][Rmw.Win]::SetCursorPos($frame.X + [int]($frame.Width / 2), $frame.Y + [int]($frame.Height / 2))
    Start-Sleep -Milliseconds 100
    [Rmw.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [Rmw.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 300
}
# Keys down in order, up in reverse (the arrows and Win are extended keys).
function Chord([byte[]] $Keys) {
    foreach ($key in $Keys) {
        $flags = if ($key -in 0x25, 0x26, 0x27, 0x28, 0x5B) { 1 } else { 0 }
        [Rmw.Win]::keybd_event($key, 0, $flags, [UIntPtr]::Zero)
    }
    for ($i = $Keys.Length - 1; $i -ge 0; $i--) {
        $flags = if ($Keys[$i] -in 0x25, 0x26, 0x27, 0x28, 0x5B) { 3 } else { 2 }
        [Rmw.Win]::keybd_event($Keys[$i], 0, $flags, [UIntPtr]::Zero)
    }
}
$Ctrl = 0xA2; $Alt = 0xA4; $Shift = 0xA0; $Win = 0x5B
$Left = 0x25; $Up = 0x26; $Right = 0x27; $Down = 0x28

function Overlap($a, $b) { $a.Left -lt $b.Right -and $b.Left -lt $a.Right -and $a.Top -lt $b.Bottom -and $b.Top -lt $a.Bottom }
# The windows tile the work area: none overlaps another, none goes out, together they cover it
# (but for the gaps). (Not $work: PowerShell names ignore case, and $Work is this script's folder.)
function Tiled([IntPtr[]] $Windows) {
    $room = Work-Area
    $frames = @($Windows | ForEach-Object { Frame $_ })
    $area = 0
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $f = $frames[$i]
        if ($f.Left -lt $room.Left - 1 -or $f.Top -lt $room.Top - 1 -or $f.Right -gt $room.Right + 1 -or $f.Bottom -gt $room.Bottom + 1) { return $false }
        for ($j = $i + 1; $j -lt $frames.Count; $j++) { if (Overlap $f $frames[$j]) { return $false } }
        $area += $f.Width * $f.Height
    }
    return $area -ge 0.85 * $room.Width * $room.Height
}
# The top row starts at the work area's top plus the gap (8 px at 100%): placed for the area as it is.
function Top-Row([IntPtr[]] $Windows) {
    $top = ($Windows | ForEach-Object { (Frame $_).Top } | Measure-Object -Minimum).Minimum
    $top -eq (Work-Area).Top + 8
}
function Describe([IntPtr[]] $Windows) { ($Windows | ForEach-Object { "$(Title $_)=$(Frame $_)" }) -join '; ' }
function Write-Settings([string] $Tiling) {
    New-Item -ItemType Directory -Force (Split-Path $settings) | Out-Null
    "schema = 1`n`n[core]`nkeep_in_tray = true`n`n$Tiling" | Out-File $settings -Encoding ascii
}

# ---- 0. four app windows, where Windows put them -------------------------------------------------
$names = 'T1', 'T2', 'T3', 'T4'
foreach ($name in $names) {
    Start-Process $app $name | Out-Null
    Start-Sleep -Milliseconds 400
}
[void](Wait-Until { @($names | Where-Object { (Find $_) -ne [IntPtr]::Zero }).Count -eq 4 } 10)
$windows = [IntPtr[]]@($names | ForEach-Object { Find $_ })
$before = @{}
foreach ($w in $windows) { $before[[int64]$w] = Outer $w }
Note 'work-area' "$(Work-Area)"
Note 'before' (Describe $windows)
Shot 'tiler-0-before'

# ---- 1. tiling on: the four tile the screen --------------------------------------------------------
Write-Settings "[tiling]`nenabled = true`n`n[tiling.keys]`ntoggle = `"Ctrl+Alt+T`"`n"
$r = Invoke-Rmw @('settings', 'reload')
Check 'tiler:settings' ($r.code -eq 0) $r.output
$running = Wait-Until { $null -ne (Tiler-Process) } 15
Check 'tiler:running' $running ((Invoke-Rmw @('status')).output)
$tiled = Wait-Until { Tiled $windows } 10
Check 'tiler:four-tiled' $tiled (Describe $windows)
$status = (Invoke-Rmw @('status')).output
Check 'tiler:status' ($status -match 'Tiling: running') $status
Click-On $windows[3]
Start-Sleep -Milliseconds 500
Shot 'tiler-1-tiled'

# ---- 2. shortcuts: focus, move, layout, full screen, float -----------------------------------------
$start = [Rmw.Win]::GetForegroundWindow()
Chord @($Ctrl, $Alt, $Left)
$moved = Wait-Until { [Rmw.Win]::GetForegroundWindow() -ne $start -and $windows -contains [Rmw.Win]::GetForegroundWindow() } 3
Check 'tiler:focus-left' $moved "from $(Title $start) to $(Title ([Rmw.Win]::GetForegroundWindow()))"

$focused = [Rmw.Win]::GetForegroundWindow()
$placeBefore = Frame $focused
Chord @($Ctrl, $Alt, $Shift, $Right)
$swapped = Wait-Until { "$(Frame $focused)" -ne "$placeBefore" -and (Tiled $windows) } 3
Check 'tiler:move-right' $swapped "$(Title $focused): $placeBefore -> $(Frame $focused)"

$layoutBefore = Describe $windows
Chord @($Ctrl, $Alt, 0x20) # Space: dwindle -> master-stack
$relaid = Wait-Until { (Describe $windows) -ne $layoutBefore -and (Tiled $windows) } 3
Check 'tiler:next-layout' $relaid (Describe $windows)
Shot 'tiler-2-master-stack'

$focused = [Rmw.Win]::GetForegroundWindow()
Chord @($Ctrl, $Alt, 0x4D) # M: the focused one takes the whole area
$area = Work-Area
$script:seen = ''
$full = Wait-Until { $f = Frame $focused; $script:seen = "$f"; $f.Width -ge $area.Width - 40 -and $f.Height -ge $area.Height - 40 } 3
Check 'tiler:fullscreen' $full "$(Title $focused) seen $script:seen, now $(Frame $focused); work=$area"
Chord @($Ctrl, $Alt, 0x4D)
Check 'tiler:fullscreen-back' (Wait-Until { Tiled $windows } 3) (Describe $windows)

$floater = [Rmw.Win]::GetForegroundWindow()
Chord @($Ctrl, $Alt, 0x46) # F: it floats, the other three share the screen
$others = [IntPtr[]]@($windows | Where-Object { $_ -ne $floater })
Check 'tiler:float' (Wait-Until { Tiled $others } 3) (Describe $others)
Chord @($Ctrl, $Alt, 0x46)
Check 'tiler:float-back' (Wait-Until { Tiled $windows } 3) (Describe $windows)

# ---- 3. workspaces: Ctrl+Win+2 hides the four, a new window there, Ctrl+Win+1 brings them back -----
$watch = [System.Diagnostics.Stopwatch]::StartNew()
Chord @($Ctrl, $Win, 0x32)
$hidden = Wait-Until { @($windows | Where-Object { Visible $_ }).Count -eq 0 } 3
$hideMs = $watch.ElapsedMilliseconds
Check 'tiler:workspace-2-hides' $hidden "$hideMs ms"
Start-Process $app 'T5' | Out-Null
[void](Wait-Until { (Find 'T5') -ne [IntPtr]::Zero } 5)
$t5 = Find 'T5'
Check 'tiler:workspace-2-new-window-fills' (Wait-Until { Tiled @($t5) } 5) "T5=$(Frame $t5)"
Shot 'tiler-3-workspace-2'
$watch.Restart()
Chord @($Ctrl, $Win, 0x31)
$back = Wait-Until { @($windows | Where-Object { Visible $_ }).Count -eq 4 -and -not (Visible $t5) } 3
$switchMs = $watch.ElapsedMilliseconds
Check 'tiler:workspace-1-back' $back "$switchMs ms"
Check 'tiler:switch-under-100ms' ($switchMs -lt 100 -and $hideMs -lt 100) "to 2: $hideMs ms, back to 1: $switchMs ms"
Check 'tiler:workspace-1-tiled' (Wait-Until { Tiled $windows } 3) (Describe $windows)
# No Start menu after Ctrl+Win+N (the mask key went out before Win's release).
Start-Sleep -Milliseconds 500
Shot 'tiler-4-workspace-1'

# Smooth switches (v1.7): the windows that come show in their slots, above the ones that go. Sampled
# every 10 ms: a coming window seen in two places (it moved after showing), or covered by a going one.
# The other app windows on the screen (not the ones the switch shows or hides): a switch during which one
# came or went is not counted (the layout changed for a good reason: the windows made room).
function Apps-Shown([IntPtr[]] $Except) {
    @(Get-Process | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and $Except -notcontains $_.MainWindowHandle -and [Rmw.Win]::IsWindowVisible($_.MainWindowHandle) } |
        ForEach-Object { "$($_.ProcessName):$($_.MainWindowHandle)" } | Sort-Object) -join ' '
}
function Sample-Switch([byte] $Key, [IntPtr[]] $Coming, [IntPtr[]] $Going) {
    $appsBefore = Apps-Shown ($Coming + $Going)
    Chord @($Ctrl, $Win, $Key)
    $seen = @{}
    $covered = 0
    $until = (Get-Date).AddMilliseconds(700)
    while ((Get-Date) -lt $until) {
        foreach ($w in $Coming) {
            if (-not (Visible $w)) { continue }
            $f = Frame $w
            if (-not $seen.ContainsKey("$w")) { $seen["$w"] = New-Object System.Collections.Generic.HashSet[string] }
            [void]$seen["$w"].Add("$($f.X),$($f.Y),$($f.Width),$($f.Height)")
            $point = New-Object Rmw.Win+POINT
            $point.X = $f.X + [int]($f.Width / 2); $point.Y = $f.Y + [int]($f.Height / 2)
            $top = [Rmw.Win]::GetAncestor([Rmw.Win]::WindowFromPoint($point), 2) # GA_ROOT
            if ($Going -contains $top) { $covered++ }
        }
        Start-Sleep -Milliseconds 10
    }
    $moved = @($seen.Keys | Where-Object { $seen[$_].Count -gt 1 }).Count
    $where = @($seen.Keys | Where-Object { $seen[$_].Count -gt 1 } | ForEach-Object { "$(Title ([IntPtr][long]$_)) at $(@($seen[$_]) -join ' then ')" }) -join ', '
    $appsAfter = Apps-Shown ($Coming + $Going)
    if ($moved -gt 0 -and $appsAfter -ne $appsBefore) {
        # Another window came or went meanwhile: the move was the tiler making room, not a jolt.
        $where += " (not counted: the windows changed from [$appsBefore] to [$appsAfter])"
        $moved = 0
    }
    [pscustomobject]@{ moved = $moved; covered = $covered; seen = $seen.Count; where = $where }
}
# The layout of workspace 2 changes while it is hidden (a window sent there): its windows get new slots.
Click-On $windows[1]
$travel = [Rmw.Win]::GetForegroundWindow()
Chord @($Ctrl, $Win, $Shift, 0x32)
[void](Wait-Until { -not (Visible $travel) } 3)
Start-Sleep -Milliseconds 500
$stay = [IntPtr[]]@($windows | Where-Object { $_ -ne $travel })
$jolts = @()
$s = Sample-Switch 0x32 @($t5, $travel) $stay
$jolts += "to 2 (new slots): moved=$($s.moved) covered=$($s.covered) of $($s.seen) $($s.where)"
$moved = $s.moved; $covered = $s.covered
foreach ($round in 1..2) {
    $s = Sample-Switch 0x31 $stay @($t5, $travel)
    $jolts += "to 1: moved=$($s.moved) covered=$($s.covered) of $($s.seen) $($s.where)"; $moved += $s.moved; $covered += $s.covered
    $s = Sample-Switch 0x32 @($t5, $travel) $stay
    $jolts += "to 2: moved=$($s.moved) covered=$($s.covered) of $($s.seen) $($s.where)"; $moved += $s.moved; $covered += $s.covered
}
Check 'tiler:switch-without-jolts' ($moved -eq 0) ($jolts -join '; ')
# Each app takes the show in its own time (all asynchronous: a hung app never stops the tiler): up to two
# samples (~20 ms) per switch.
Check 'tiler:switch-covers-first' ($covered -le 2 * 5) ($jolts -join '; ')
# Back as it was: the traveller to workspace 1 again, and workspace 1 shown.
Click-On $travel
Chord @($Ctrl, $Win, $Shift, 0x31)
[void](Wait-Until { -not (Visible $travel) } 3)
Chord @($Ctrl, $Win, 0x31)
[void](Wait-Until { @($windows | Where-Object { Visible $_ }).Count -eq 4 } 3)
[void](Wait-Until { Tiled $windows } 3)

# Send the focused window to workspace 3: it hides, the other three share the screen.
Click-On $windows[0]
$sent = [Rmw.Win]::GetForegroundWindow()
Chord @($Ctrl, $Win, $Shift, 0x33)
$rest = [IntPtr[]]@($windows | Where-Object { $_ -ne $sent })
Check 'tiler:send-to-3' (Wait-Until { -not (Visible $sent) -and (Tiled $rest) } 3) "$(Title $sent) visible=$(Visible $sent); $(Describe $rest)"

# A window with a dialog: the dialog goes and comes with it.
Start-Process $app 'T6', '--dialog' | Out-Null
[void](Wait-Until { (Find 'T6 dialog') -ne [IntPtr]::Zero } 5)
$t6 = Find 'T6'
$t6Dialog = Find 'T6 dialog'
Start-Sleep -Milliseconds 500
Chord @($Ctrl, $Win, 0x34)
Check 'tiler:dialog-hides-with-its-window' (Wait-Until { -not (Visible $t6) -and -not (Visible $t6Dialog) } 3) "window=$(Visible $t6) dialog=$(Visible $t6Dialog)"
Chord @($Ctrl, $Win, 0x31)
Check 'tiler:dialog-comes-back' (Wait-Until { (Visible $t6) -and (Visible $t6Dialog) } 3) "window=$(Visible $t6) dialog=$(Visible $t6Dialog)"

# ---- 3a. the settings window on a workspace: whole when it comes back (v1.6 on the owner's PC: white
# until a page changed)
function Frame-Shot([IntPtr] $Window) {
    $f = Frame $Window
    if ($f.Width -le 0 -or $f.Height -le 0) { return $null }
    $bitmap = New-Object System.Drawing.Bitmap $f.Width, $f.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($f.X, $f.Y, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    $bitmap
}
function Shot-Difference($A, $B) {
    if ($null -eq $A -or $null -eq $B -or $A.Width -ne $B.Width -or $A.Height -ne $B.Height) { return 255 }
    $total = 0.0; $count = 0
    for ($y = 4; $y -lt $A.Height - 4; $y += [math]::Max(1, [int]($A.Height / 40))) {
        for ($x = 4; $x -lt $A.Width - 4; $x += [math]::Max(1, [int]($A.Width / 40))) {
            $p = $A.GetPixel($x, $y); $q = $B.GetPixel($x, $y)
            $total += ([math]::Abs($p.R - $q.R) + [math]::Abs($p.G - $q.G) + [math]::Abs($p.B - $q.B)) / 3.0
            $count++
        }
    }
    $total / [math]::Max(1, $count)
}
$settingsApp = Start-Process (Join-Path $Bin 'rmw-settings.exe') -ArgumentList @('--data-dir', "`"$data`"") -PassThru
# The window itself (a small helper window can come first): the one that is wide.
[void](Wait-Until { $settingsApp.Refresh(); $settingsApp.MainWindowHandle -ne [IntPtr]::Zero -and (Frame $settingsApp.MainWindowHandle).Width -gt 300 } 20)
$settingsWindow = $settingsApp.MainWindowHandle
Start-Sleep -Seconds 5 # tiled with the others, connected, painted
$beforeTrips = Frame-Shot $settingsWindow
foreach ($trip in 1..3) {
    Chord @($Ctrl, $Win, 0x32)
    Start-Sleep -Milliseconds 700
    Chord @($Ctrl, $Win, 0x31)
    Start-Sleep -Milliseconds 1200
}
$afterTrips = Frame-Shot $settingsWindow
$tripDiff = Shot-Difference $beforeTrips $afterTrips
if ($tripDiff -ge 8) {
    if ($null -ne $beforeTrips) { $beforeTrips.Save((Join-Path $Out 'tiler-settings-before-trips.png')) }
    if ($null -ne $afterTrips) { $afterTrips.Save((Join-Path $Out 'tiler-settings-after-trips.png')) }
}
Check 'tiler:settings-whole-after-workspaces' ($tripDiff -lt 8) ('{0:N1} average difference; frame {1}' -f $tripDiff, (Frame $settingsWindow))
if (-not $settingsApp.HasExited) { [void]$settingsApp.CloseMainWindow() }
if (-not (Wait-Until { $settingsApp.HasExited } 10)) { Stop-Process -Id $settingsApp.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 1

# ---- 3b. the bar's workspaces widget: 1, 2 and 3 in use; a click on 2 goes there -------------------
$riceDir = Join-Path $Work 'rice-workspaces'
New-Item -ItemType Directory -Force $riceDir | Out-Null
@'
schema = 1

[meta]
name = "Workspaces"

[[bar]]
position = "top"
thickness = 32
background = { kind = "solid", tint = "#1e1e2e" }
foreground = "#cdd6f4"
left = ["workspaces"]
center = ["clock"]
'@ | Out-File (Join-Path $riceDir 'rice.toml') -Encoding ascii
$r = Invoke-Rmw @('apply', $riceDir, '--wait')
Check 'tiler:bar-applied' ($r.code -eq 0 -and (Wait-Until { (Work-Area).Top -eq 32 } 15)) "$($r.output) work=$(Work-Area)"
$onScreen = [IntPtr[]]@($rest + $t6)
Check 'tiler:bar-retiled' (Wait-Until { (Tiled $onScreen) -and (Top-Row $onScreen) } 5) (Describe $onScreen)
Start-Sleep -Seconds 1
Shot 'tiler-5-bar-widget'
# The widget shows 1 (shown), 2 (T5) and 3 (T1): 30 px each from x = 6, 2 px apart.
Click-At 53 16
Check 'tiler:bar-click-goes-to-2' (Wait-Until { (Visible $t5) -and -not (Visible $rest[0]) } 3) "T5=$(Visible $t5) $(Title $rest[0])=$(Visible $rest[0])"
Shot 'tiler-5b-bar-workspace-2'
Click-At 21 16
Check 'tiler:bar-click-back-to-1' (Wait-Until { -not (Visible $t5) -and (Visible $rest[0]) } 3) "T5=$(Visible $t5)"
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'tiler:bar-gone' ($r.code -eq 0 -and (Wait-Until { (Work-Area).Top -eq 0 } 15)) "$($r.output) work=$(Work-Area)"
Check 'tiler:bar-gone-retiled' (Wait-Until { (Tiled $onScreen) -and (Top-Row $onScreen) } 5) (Describe $onScreen)

# ---- 3c. Toggle (Ctrl+Alt+T here): off for now puts everything back; again, everything is tiled ------
Chord @($Ctrl, $Alt, 0x54)
$paused = Wait-Until { @($rest | Where-Object { "$(Outer $_)" -ne "$($before[[int64]$_])" }).Count -eq 0 -and (Visible $t5) -and (Visible $windows[0]) } 5
Check 'tiler:toggle-off-puts-back' $paused (($rest | ForEach-Object { "$(Title $_): $(Outer $_)" }) -join '; ')
$allSix = [IntPtr[]]@($windows + $t5 + $t6)
Chord @($Ctrl, $Alt, 0x54)
Check 'tiler:toggle-on-tiles-again' (Wait-Until { Tiled $allSix } 5) (Describe $allSix)

# Idle: no work while nothing happens.
$tiler = Tiler-Process
if ($tiler) {
    $cpuBefore = $tiler.TotalProcessorTime
    Start-Sleep -Seconds 10
    $tiler.Refresh()
    $cpu = ($tiler.TotalProcessorTime - $cpuBefore).TotalMilliseconds / 10000.0 * 100.0
    $privateMb = $tiler.PrivateMemorySize64 / 1MB
    Check 'tiler:memory-under-15mb' ($privateMb -le 15) ('{0:N1} MB private' -f $privateMb)
    Check 'tiler:cpu-idle' ($cpu -lt 0.5) ('{0:N2}% over 10 s' -f $cpu)
}

# ---- 4. off: every window back where it was, the hidden ones shown --------------------------------
Write-Settings "[tiling]`nenabled = false`n"
$r = Invoke-Rmw @('settings', 'reload')
$gone = Wait-Until { $null -eq (Tiler-Process) } 10
Check 'tiler:off-leaves' $gone $r.output
$all = [IntPtr[]]@($windows + $t5 + $t6)
Check 'tiler:off-all-visible' (Wait-Until { @($all | Where-Object { -not (Visible $_) }).Count -eq 0 } 5) (($all | ForEach-Object { "$(Title $_)=$(Visible $_)" }) -join ' ')
$putBack = Wait-Until { @($windows | Where-Object { "$(Outer $_)" -ne "$($before[[int64]$_])" }).Count -eq 0 } 5
Check 'tiler:off-back-in-place' $putBack (($windows | ForEach-Object { "$(Title $_): $($before[[int64]$_]) -> $(Outer $_)" }) -join '; ')
Check 'tiler:off-no-file-left' (-not (Test-Path (Join-Path $data 'tiler\windows.json'))) ''
Shot 'tiler-5-off'

# ---- 5. a crash of the tiler: the hidden windows come back at once, then it tiles again ------------
Write-Settings "[tiling]`nenabled = true`n"
[void](Invoke-Rmw @('settings', 'reload'))
[void](Wait-Until { $null -ne (Tiler-Process) } 15)
Start-Sleep -Seconds 1
Click-On $windows[1]
Chord @($Ctrl, $Win, 0x32) # the six on workspace 1 hide
[void](Wait-Until { -not (Visible $windows[1]) } 3)
Stop-Process -Name rmw-tiler -Force -ErrorAction SilentlyContinue
$watch.Restart()
$shown = Wait-Until { @($all | Where-Object { -not (Visible $_) }).Count -eq 0 } 5
Check 'tiler:crash-all-visible' $shown ("$($watch.ElapsedMilliseconds) ms; " + (($all | ForEach-Object { "$(Title $_)=$(Visible $_)" }) -join ' '))
Check 'tiler:crash-restarted' (Wait-Until { $null -ne (Tiler-Process) } 15) ((Invoke-Rmw @('status')).output)
$sixTiled = [IntPtr[]]@($windows + $t5 + $t6)
Check 'tiler:crash-tiled-again' (Wait-Until { Tiled $sixTiled } 10) (Describe $sixTiled)
Shot 'tiler-6-after-crash'

# ---- 6. the core goes: the tiler puts every window back and leaves ---------------------------------
Click-On $windows[0]
Chord @($Ctrl, $Win, 0x32)
Check 'tiler:core-test-hidden' (Wait-Until { -not (Visible $windows[0]) } 3) ''
Stop-Process -Name rmw-core -Force -ErrorAction SilentlyContinue
$left = Wait-Until { $null -eq (Tiler-Process) } 10
Check 'tiler:core-gone-tiler-leaves' $left ''
Check 'tiler:core-gone-all-visible' (Wait-Until { @($all | Where-Object { -not (Visible $_) }).Count -eq 0 } 5) (($all | ForEach-Object { "$(Title $_)=$(Visible $_)" }) -join ' ')

# ---- 7. advanced mode: the tiler runs as administrator, and admin apps' windows are tiled too ------
# It needs the UAC to say yes by itself here (a prompt would wait for a person).
$uac = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -ErrorAction SilentlyContinue
$scriptElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Note 'uac' "EnableLUA=$($uac.EnableLUA) ConsentPromptBehaviorAdmin=$($uac.ConsentPromptBehaviorAdmin) script elevated=$scriptElevated"
if ($uac.EnableLUA -eq 0 -or $uac.ConsentPromptBehaviorAdmin -eq 0 -or $scriptElevated) {
    Write-Settings "[tiling]`nenabled = true`nrun_as_admin = true`n"
    [void](Invoke-Rmw @('settings', 'reload')) # the core starts again
    $adminLog = Join-Path $data 'logs\tiler-admin.log'
    $admin = Wait-Until { (Test-Path $adminLog) -and ((Get-Content $adminLog -Raw) -match 'running as administrator') } 20
    Check 'tiler:admin-runs' $admin "tiler processes: $(@(Get-Process rmw-tiler -ErrorAction SilentlyContinue).Count)"
    Start-Process $app 'A1' -Verb RunAs | Out-Null
    [void](Wait-Until { (Find 'A1') -ne [IntPtr]::Zero } 10)
    $withAdmin = [IntPtr[]]@($all + (Find 'A1'))
    Check 'tiler:admin-window-tiled' (Wait-Until { Tiled $withAdmin } 10) (Describe $withAdmin)
    Shot 'tiler-7-admin'
    Write-Settings "[tiling]`nenabled = false`n"
    [void](Invoke-Rmw @('settings', 'reload'))
    Check 'tiler:admin-off-leaves' (Wait-Until { $null -eq (Tiler-Process) } 10) ''
    Check 'tiler:admin-off-all-visible' (Wait-Until { @($withAdmin | Where-Object { -not (Visible $_) }).Count -eq 0 } 5) ''
}

# ---- 8. advanced mode from a tiler that is not an administrator (a PC with the UAC on) -------------
# Here everything runs as administrator. A core started at medium integrity (as every app is on a PC
# with the UAC on) has a tiler that is not: it leaves an admin app's window alone and starts an
# elevated copy, which does the work while it keeps beating for the supervisor.
function Tilers { @(Get-CimInstance Win32_Process -Filter "Name = 'rmw-tiler.exe'" -ErrorAction SilentlyContinue) }
# (Not -Wait: Windows PowerShell waits for the whole tree, and the windows started stay open.)
function Medium([string] $Line) { $p = Start-Process $app -ArgumentList "--medium $Line" -PassThru; [void]$p.WaitForExit(10000) }
[void](Invoke-Rmw @('shutdown', '--wait'))
Get-Process rmw_tiler_test_app -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Start-Process $app 'H1' | Out-Null # an admin app's window (everything here is)
[void](Wait-Until { (Find 'H1') -ne [IntPtr]::Zero } 5)
$h1 = Find 'H1'
$h1Before = Outer $h1
Medium "$app M1"
Medium "$app M2"
[void](Wait-Until { (Find 'M1') -ne [IntPtr]::Zero -and (Find 'M2') -ne [IntPtr]::Zero } 5)
$medium = [IntPtr[]]@((Find 'M1'), (Find 'M2'))
$adminLog = Join-Path $data 'logs\tiler-admin.log'
# Tilers that ran as administrator so far (the log's [number] is a thread, not a process).
function Admin-Runs { @(Get-Content $adminLog -ErrorAction SilentlyContinue | Where-Object { $_ -match 'tiler: running as administrator' }).Count }
$adminRunsBefore = Admin-Runs
Write-Settings "[tiling]`nenabled = true`nrun_as_admin = true`n"
Medium "$rmw settings reload --data-dir $data"
$both = Wait-Until { @(Tilers).Count -eq 2 } 20
$copy = Tilers | Where-Object { $_.CommandLine -match '--elevated-by' } | Select-Object -First 1
Check 'tiler:proxy-starts-a-copy' ($both -and $null -ne $copy) ((Tilers | ForEach-Object { "$($_.ProcessId): $($_.CommandLine)" }) -join ' | ')
Start-Sleep -Seconds 2
# Did Windows give the copy the rights? (With the UAC off, "run as administrator" gives none.)
$copyIsAdmin = $null -ne $copy -and (Admin-Runs) -gt $adminRunsBefore
Note 'proxy-copy-is-admin' "$copyIsAdmin"
if ($copyIsAdmin) {
    $three = [IntPtr[]]@($medium + $h1)
    Check 'tiler:proxy-copy-tiles-admin-windows-too' (Wait-Until { Tiled $three } 10) (Describe $three)
}
else {
    # A tiler that is not an administrator: the medium windows share the screen, H1 stays where it was.
    Check 'tiler:proxy-copy-tiles-its-own' (Wait-Until { Tiled $medium } 10) (Describe $medium)
    Check 'tiler:admin-window-left-alone' ("$(Outer $h1)" -eq "$h1Before") "H1: $h1Before -> $(Outer $h1)"
}
Shot 'tiler-8-proxy'
$proxy = Tilers | Where-Object { $_.CommandLine -notmatch '--elevated-by' } | Select-Object -First 1
if ($proxy -and $copy) {
    # The supervised one is ended (as the core does to a hung worker): the copy puts everything back
    # and leaves.
    Click-On $medium[0]
    Chord @($Ctrl, $Win, 0x32)
    [void](Wait-Until { -not (Visible $medium[0]) } 3)
    Stop-Process -Id $proxy.ProcessId -Force -ErrorAction SilentlyContinue
    Check 'tiler:proxy-gone-copy-leaves' (Wait-Until { $null -eq (Get-Process -Id $copy.ProcessId -ErrorAction SilentlyContinue) } 10) ''
    Check 'tiler:proxy-gone-all-visible' (Wait-Until { @($medium | Where-Object { -not (Visible $_) }).Count -eq 0 } 5) ''
}
Get-ChildItem (Join-Path $data 'logs') -Filter 'tiler*.log' -ErrorAction SilentlyContinue | ForEach-Object {
    Note "log-$($_.BaseName)" ((Get-Content $_.FullName -Tail 8) -join ' | ')
}

# ---- 9. like a Mac: Windows' own virtual desktops, sliding ------------------------------------------
# (Another virtual desktop's windows are cloaked: visible to IsWindowVisible, but not shown.)
function Cloaked([IntPtr] $Window) { $value = 0; [void][Rmw.Win]::DwmGetInt($Window, 14, [ref]$value, 4); $value -ne 0 }
[void](Invoke-Rmw @('shutdown', '--wait'))
Get-Process rmw-tiler -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Write-Settings "[tiling]`nenabled = true`nmode = `"mac`"`n"
[void](Invoke-Rmw @('settings', 'reload'))
$macOne = [IntPtr[]]@(@((Find 'M1'), (Find 'M2'), (Find 'H1')) | Where-Object { $_ -ne [IntPtr]::Zero })
Check 'tiler:mac-tiles-desktop-1' (Wait-Until { Tiled $macOne } 15) (Describe $macOne)
Chord @($Ctrl, $Win, 0x44) # Windows' own Ctrl+Win+D: a new desktop, shown
Start-Sleep -Seconds 2
Start-Process $app 'D2' | Out-Null
[void](Wait-Until { (Find 'D2') -ne [IntPtr]::Zero } 5)
$d2 = Find 'D2'
Check 'tiler:mac-desktop-2-alone' (Wait-Until { (Tiled @($d2)) -and -not (Cloaked $d2) } 10) "D2=$(Frame $d2) cloaked=$(Cloaked $d2)"
Shot 'tiler-9-mac-desktop-2'
# Windows' slide between desktops takes its time: a switch sent while one still slides gets mixed up with
# it (seen: desktop 2 shown, then 1 again 12 ms later). A person never switches twice within 20 ms.
Start-Sleep -Milliseconds 800
$watch.Restart()
Chord @($Ctrl, $Win, 0x31) # ours: to desktop 1, with Windows' slide
$slid = Wait-Until { (Cloaked $d2) -and -not (Cloaked $macOne[0]) } 8
Check 'tiler:mac-go-to-1' $slid "$($watch.ElapsedMilliseconds) ms (with Windows' animation)"
Check 'tiler:mac-desktop-1-still-tiled' (Wait-Until { Tiled $macOne } 5) (Describe $macOne)
Start-Sleep -Milliseconds 800 # the slide ends
Chord @($Ctrl, $Win, 0x32) # to desktop 2
Check 'tiler:mac-go-to-2' (Wait-Until { -not (Cloaked $d2) -and (Cloaked $macOne[0]) } 8) ''
$desktops = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops' -ErrorAction SilentlyContinue
Note 'mac-registry' "ids=$(@($desktops.VirtualDesktopIDs).Count) bytes, current=$(@($desktops.CurrentVirtualDesktop).Count) bytes"
$adminLog = Join-Path $data 'logs\tiler-admin.log'
Note 'mac-log' ((Get-Content $adminLog -Tail 10 -ErrorAction SilentlyContinue) -join ' | ')

# ---- 10. a rice's window look (M14), tiling off: rmw-tiler paints other apps' title bars, and only that
Get-Process rmw_tiler_test_app -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Settings ''
[void](Invoke-Rmw @('settings', 'reload'))
Check 'look:tiler-off-first' (Wait-Until { $null -eq (Tiler-Process) } 15) ''
# The color DWM draws at a point of the screen.
function Pixel([int] $X, [int] $Y) {
    $bitmap = New-Object System.Drawing.Bitmap 1, 1
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $target = $graphics.GetHdc()
    $screenDc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    [void][Rmw.Win]::BitBlt($target, 0, 0, 1, 1, $screenDc, $X, $Y, 0x40CC0020)
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $screenDc)
    $graphics.ReleaseHdc($target)
    $color = $bitmap.GetPixel(0, 0)
    $graphics.Dispose(); $bitmap.Dispose()
    return $color
}
# The middle of the title bar, where only the caption's color is.
function Caption([IntPtr] $Window) {
    $frame = Frame $Window
    Pixel ($frame.X + [int]($frame.Width / 2)) ($frame.Y + 8)
}
function Magenta($Color) { $Color.R -ge 0xF0 -and $Color.G -le 0x10 -and $Color.B -ge 0xF0 }
Start-Process $app 'L1' | Out-Null
[void](Wait-Until { (Find 'L1') -ne [IntPtr]::Zero } 10)
$l1 = Find 'L1'
Click-On $l1
Start-Sleep -Milliseconds 500
$captionBefore = Caption $l1
$placeBefore = Outer $l1
$lookRice = Join-Path $Work 'look-rice'
New-Item -ItemType Directory -Force $lookRice | Out-Null
"schema = 1`n`n[meta]`nname = `"Window look`"`n`n[windows]`ncaption = `"#FF00FF`"`ncorners = `"square`"`n" |
    Out-File (Join-Path $lookRice 'rice.toml') -Encoding ascii
$r = Invoke-Rmw @('apply', $lookRice)
Check 'look:applied' ($r.code -eq 0) $r.output
Check 'look:tiler-runs-for-it' (Wait-Until { $null -ne (Tiler-Process) } 15) ((Invoke-Rmw @('status')).output)
$painted = Wait-Until { Magenta (Caption $l1) } 10
Check 'look:caption-painted' $painted "before=$captionBefore now=$(Caption $l1)"
Shot 'tiler-10-look'
Check 'look:no-tiling' ((Outer $l1) -eq $placeBefore) "before=$placeBefore now=$(Outer $l1)"
Start-Process $app 'L2' | Out-Null
[void](Wait-Until { (Find 'L2') -ne [IntPtr]::Zero } 10)
$l2 = Find 'L2'
Click-On $l2
Check 'look:new-window-painted' (Wait-Until { Magenta (Caption $l2) } 10) "now=$(Caption $l2)"
$r = Invoke-Rmw @('restore', '--original')
Check 'look:restored' ($r.code -eq 0) $r.output
Check 'look:tiler-leaves' (Wait-Until { $null -eq (Tiler-Process) } 15) ''
Click-On $l2
Check 'look:caption-back' (Wait-Until { -not (Magenta (Caption $l2)) } 10) "before=$captionBefore now=$(Caption $l2)"
Shot 'tiler-10-look-back'

Get-Process rmw_tiler_test_app -ErrorAction SilentlyContinue | Stop-Process -Force
Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $data 'logs')) {
    Remove-Item (Join-Path $Out 'tiler-logs') -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'tiler-logs') -Recurse -Force
}

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'tiler.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
