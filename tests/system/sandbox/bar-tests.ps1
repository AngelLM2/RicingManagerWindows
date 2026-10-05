<#
    M3 system tests (the custom taskbar). Run ONLY inside Windows Sandbox (run-in-sandbox.ps1
    -Suite bar): they hide the real (sandboxed) taskbar and reserve screen space.

    Acceptance of M3 (docs/04): a bar of any size/position as the rice says; the native taskbar
    hidden, and restored in < 3 s if rmw-bar is killed; the emergency hotkey works; Explorer
    restarts; <= 25 MB and ~0% CPU idle. Screenshots go to the output folder for a visual check.
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
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsIconic(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowTextW(System.IntPtr window, System.Text.StringBuilder text, int length);
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
    $output = & $rmw @Arguments --data-dir $data 2>&1 | Out-String
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
# [NullString]::Value: PowerShell turns $null into "" for string parameters, and FindWindow would
# then look for windows titled "".
function Native-Visible { [Rmw.Win]::IsWindowVisible([Rmw.Win]::FindWindowW('Shell_TrayWnd', [NullString]::Value)) }
function Bar-Process { Get-Process rmw-bar -ErrorAction SilentlyContinue | Select-Object -First 1 }
# Asked every time: .NET's Screen.WorkingArea is cached until a message loop sees the change.
function Work-Area {
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::SystemParametersInfoW(0x30, 0, [ref]$rect, 0) # SPI_GETWORKAREA
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Screen-Bounds { [System.Windows.Forms.Screen]::PrimaryScreen.Bounds }
function Shot([string] $Name) {
    $bounds = Screen-Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    # SRCCOPY | CAPTUREBLT: layered windows (our bar) are only in the capture with CAPTUREBLT,
    # which .NET's CopyFromScreen refuses as an enum value.
    $target = $graphics.GetHdc()
    $screenDc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    [void][Rmw.Win]::BitBlt($target, 0, 0, $bounds.Width, $bounds.Height, $screenDc, $bounds.X, $bounds.Y, 0x40CC0020)
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $screenDc)
    $graphics.ReleaseHdc($target)
    $bitmap.Save((Join-Path $Out "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
}
function Click([int] $X, [int] $Y) {
    [void][Rmw.Win]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 150
    [Rmw.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) # left down
    [Rmw.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) # left up
    Start-Sleep -Milliseconds 700
}
function Title([IntPtr] $Window) {
    $text = New-Object System.Text.StringBuilder 256
    [void][Rmw.Win]::GetWindowTextW($Window, $text, 256)
    $text.ToString()
}
function Bar-Rect {
    $window = [Rmw.Win]::FindWindowW('RicingManagerWindows.Bar', [NullString]::Value)
    $rect = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::GetWindowRect($window, [ref]$rect)
    [System.Drawing.Rectangle]::FromLTRB($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}
function Write-Rice([string] $Folder, [string] $Bar) {
    New-Item -ItemType Directory -Force $Folder | Out-Null
    @"
schema = 1

[meta]
name = "Bar test"

[palette]
night = "#1a1b26"
ink = "#c0caf5"

$Bar
"@ | Out-File (Join-Path $Folder 'rice.toml') -Encoding ascii
}

# Something to look at behind the bar, and buttons for its window list: two Explorer windows.
Start-Process explorer.exe 'C:\Windows' | Out-Null
Start-Sleep -Seconds 2
Start-Process explorer.exe 'C:\Users' | Out-Null
Start-Sleep -Seconds 2

$screen = Screen-Bounds
$workBefore = Work-Area
Note 'screen' "$($screen.Width)x$($screen.Height) work=$($workBefore)"
Shot 'bar-0-before'

# ---- 1. a solid bottom bar replaces the native taskbar --------------------------------------------
$solid = Join-Path $Work 'rice-solid'
Write-Rice $solid @'
[[bar]]
position = "bottom"
thickness = 44
background = { kind = "solid", tint = "palette.night" }
foreground = "palette.ink"
hide_native_taskbar = true
left = ["start", "desktops", "windows"]
center = ["clock"]
right = ["media", "cpu", "memory", "network", "volume", "battery", "notifications", { id = "clock", format = "%d/%m/%Y" }]
'@
$r = Invoke-Rmw @('apply', $solid, '--wait')
Check 'bar:apply' ($r.code -eq 0) $r.output
$running = Wait-Until { $null -ne (Bar-Process) } 15
Check 'bar:running' $running ''
$hidden = Wait-Until { -not (Native-Visible) } 15
Check 'bar:native-hidden' $hidden ''
$reserved = Wait-Until { (Work-Area).Bottom -eq $screen.Bottom - 44 } 10
Check 'bar:space-reserved' $reserved "work=$(Work-Area)"
Start-Sleep -Seconds 3
Shot 'bar-1-solid'

$bar = Bar-Process
if ($bar) {
    $cpuBefore = $bar.TotalProcessorTime
    Start-Sleep -Seconds 10
    $bar.Refresh()
    $cpu = ($bar.TotalProcessorTime - $cpuBefore).TotalMilliseconds / 10000.0 * 100.0
    $privateMb = $bar.PrivateMemorySize64 / 1MB
    Check 'bar:memory-under-25mb' ($privateMb -le 25) ('{0:N1} MB private' -f $privateMb)
    Check 'bar:cpu-idle' ($cpu -lt 1.0) ('{0:N2}% over 10 s (a clock ticks every second)' -f $cpu)
}
$status = (Invoke-Rmw @('status')).output
Check 'bar:status' ($status -match 'Bar: running') $status

# The window list: its first button (after start and the desktop number) is the oldest window.
# Click: it comes to the front; again: it minimizes; again: it is back.
$buttonX = 6 + 40 + 4 + 30 + 4 + 90
$buttonY = $screen.Bottom - 22
# Each click is asked of Windows, which animates: wait for each state (up to 2 s).
Click $buttonX $buttonY
Start-Sleep -Milliseconds 500
$first = [Rmw.Win]::GetForegroundWindow()
$firstTitle = Title $first
Click $buttonX $buttonY
$minimized = Wait-Until { [Rmw.Win]::IsIconic($first) } 2
Click $buttonX $buttonY
$restored = Wait-Until { -not [Rmw.Win]::IsIconic($first) -and [Rmw.Win]::GetForegroundWindow() -eq $first } 2
Check 'bar:window-button-toggles' ($minimized -and $restored) "window='$firstTitle' minimized=$minimized restored=$restored"
Shot 'bar-1b-window-list'

# ---- 2. killing rmw-bar brings the native taskbar back in < 3 s, then the bar comes back -------
$bar = Bar-Process
$killedAt = Get-Date
Stop-Process -Id $bar.Id -Force
$back = Wait-Until { Native-Visible } 3
$elapsed = ((Get-Date) - $killedAt).TotalMilliseconds
Check 'bar:killed-native-back-under-3s' $back ('{0:N0} ms' -f $elapsed)
$restarted = Wait-Until { $null -ne (Bar-Process) -and -not (Native-Visible) } 15
Check 'bar:restarted-after-kill' $restarted ''

# ---- 3. the emergency hotkey always brings the native taskbar back -------------------------------
Start-Sleep -Seconds 2
foreach ($key in 0x11, 0x12, 0x5B) { [Rmw.Win]::keybd_event($key, 0, 0, [UIntPtr]::Zero) } # Ctrl Alt Win
[Rmw.Win]::keybd_event(0x54, 0, 0, [UIntPtr]::Zero); [Rmw.Win]::keybd_event(0x54, 0, 2, [UIntPtr]::Zero) # T
foreach ($key in 0x5B, 0x12, 0x11) { [Rmw.Win]::keybd_event($key, 0, 2, [UIntPtr]::Zero) }
$emergency = Wait-Until { (Native-Visible) -and $null -eq (Bar-Process) } 5
Check 'bar:emergency-hotkey' $emergency ''
Start-Sleep -Seconds 1
$status = (Invoke-Rmw @('status')).output
Check 'bar:emergency-status-off' ($status -match 'Bar: off') $status
$r = Invoke-Rmw @('action', 'bar.on', '--wait')
$on = Wait-Until { $null -ne (Bar-Process) -and -not (Native-Visible) } 15
Check 'bar:on-again' ($r.code -eq 0 -and $on) $r.output

# ---- 3b. saving the applied rice updates its bar, without an apply -------------------------------
$text = Get-Content (Join-Path $solid 'rice.toml') -Raw
$text.Replace('thickness = 44', 'thickness = 56') | Out-File (Join-Path $solid 'rice.toml') -Encoding ascii
$followed = Wait-Until { (Work-Area).Bottom -eq $screen.Bottom - 56 } 10
Check 'bar:follows-rice-edits' $followed "work=$(Work-Area)"
Set-Content (Join-Path $solid 'rice.toml') 'this is not toml [' -Encoding ascii
Start-Sleep -Seconds 3
Check 'bar:broken-edit-keeps-bar' ((Work-Area).Bottom -eq $screen.Bottom - 56 -and $null -ne (Bar-Process)) "work=$(Work-Area)"

# ---- 3c. an auto-hidden bar takes no space and comes back with the mouse ------------------------
$autohide = Join-Path $Work 'rice-autohide'
Write-Rice $autohide @'
[[bar]]
position = "bottom"
thickness = 40
autohide = true
hide_native_taskbar = true
left = ["start", "windows"]
right = ["clock"]
'@
$r = Invoke-Rmw @('apply', $autohide, '--wait')
Check 'bar:autohide-apply' ($r.code -eq 0) $r.output
$free = Wait-Until { (Work-Area).Bottom -eq $screen.Bottom } 10
[void][Rmw.Win]::SetCursorPos([int]($screen.Width / 2), [int]($screen.Height / 2))
$tucked = Wait-Until { (Bar-Rect).Top -ge $screen.Bottom - 2 } 8
Check 'bar:autohide-reserves-nothing-and-tucks' ($free -and $tucked) "work=$(Work-Area) bar=$(Bar-Rect)"
[void][Rmw.Win]::SetCursorPos([int]($screen.Width / 2), $screen.Bottom - 1)
$shown = Wait-Until { (Bar-Rect).Top -eq $screen.Bottom - 40 } 5
Shot 'bar-1c-autohide-shown'
[void][Rmw.Win]::SetCursorPos([int]($screen.Width / 2), [int]($screen.Height / 2))
$away = Wait-Until { (Bar-Rect).Top -ge $screen.Bottom - 2 } 5
Check 'bar:autohide-follows-the-mouse' ($shown -and $away) "shown=$shown tucked-again=$away"

# ---- 4. looks: floating, rounded, translucent; acrylic; a vertical bar ---------------------------
$float = Join-Path $Work 'rice-float'
Write-Rice $float @'
[[bar]]
position = "top"
mode = "float"
thickness = 40
length = "60%"
margin = [10, 0]
radius = 14
background = { kind = "transparent", tint = "palette.night", opacity = 0.75 }
foreground = "palette.ink"
hide_native_taskbar = true
left = ["start"]
center = ["clock"]
'@
$r = Invoke-Rmw @('apply', $float, '--wait')
Check 'bar:float-apply' ($r.code -eq 0) $r.output
$released = Wait-Until { (Work-Area).Bottom -eq $screen.Bottom -and (Work-Area).Top -eq $screen.Top } 10
Check 'bar:float-reserves-nothing' $released "work=$(Work-Area)"
Start-Sleep -Seconds 3
Shot 'bar-2-float-rounded'

$acrylic = Join-Path $Work 'rice-acrylic'
Write-Rice $acrylic @'
[[bar]]
position = "bottom"
thickness = 48
background = { kind = "acrylic", tint = "palette.night", opacity = 0.35 }
foreground = "palette.ink"
hide_native_taskbar = true
left = ["start"]
right = ["clock"]

[[bar]]
position = "left"
thickness = 52
background = "solid"
left = ["start"]
right = [{ id = "clock", format = "%H" }]
'@
$r = Invoke-Rmw @('apply', $acrylic, '--wait')
Check 'bar:two-bars-apply' ($r.code -eq 0) $r.output
$both = Wait-Until { (Work-Area).Bottom -eq $screen.Bottom - 48 -and (Work-Area).Left -eq 52 } 10
Check 'bar:two-bars-reserve' $both "work=$(Work-Area)"
Start-Sleep -Seconds 3
Shot 'bar-3-acrylic-and-left'

# ---- 5. Explorer restarts: the bars and the hidden taskbar survive -------------------------------
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
if (-not (Wait-Until { Get-Process explorer -ErrorAction SilentlyContinue } 20)) { Start-Process explorer.exe }
Start-Sleep -Seconds 8
$survived = Wait-Until { $null -ne (Bar-Process) -and -not (Native-Visible) } 15
Check 'bar:explorer-restart' $survived "native=$(Native-Visible) work=$(Work-Area)"
$reservedAgain = Wait-Until { (Work-Area).Bottom -eq $screen.Bottom - 48 } 10
Check 'bar:explorer-restart-reserves' $reservedAgain "work=$(Work-Area)"
Shot 'bar-4-after-explorer-restart'

# ---- 6. restore: no bar, the native taskbar and its settings as before ---------------------------
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'bar:restore' ($r.code -eq 0) $r.output
$gone = Wait-Until { $null -eq (Bar-Process) -and (Native-Visible) } 10
Check 'bar:restore-bar-gone-native-back' $gone ''
$workAfter = Wait-Until { "$(Work-Area)" -eq "$workBefore" } 10
Check 'bar:restore-work-area' $workAfter "before=$workBefore after=$(Work-Area)"
Shot 'bar-5-restored'

Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
foreach ($folder in 'logs') {
    if (Test-Path (Join-Path $data $folder)) { Copy-Item (Join-Path $data $folder) (Join-Path $Out $folder) -Recurse -Force }
}

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'bar.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
