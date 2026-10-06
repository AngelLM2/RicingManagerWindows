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
[System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SystemParametersInfoW")]
public static extern bool SystemParameterBool(uint action, uint param, ref bool value, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SystemParametersInfoW")]
public static extern bool SystemParameterSet(uint action, uint param, System.IntPtr value, uint winIni);
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

# ---- 4b. M10: the five styles; a dock as long as its apps; the bar slides in ----------------------
function Centered($Rect) { [Math]::Abs(($Rect.Left + $Rect.Right) - ($screen.Left + $screen.Right)) -le 4 }
$styled = Join-Path $Work 'rice-styled'
$styleChecks = [ordered]@{
    'windows' = { (Work-Area).Bottom -eq $screen.Bottom - 48 -and (Bar-Rect).Width -eq $screen.Width }
    'status'  = { (Work-Area).Top -eq $screen.Top + 28 -and (Bar-Rect).Height -eq 28 }
    'side'    = { (Work-Area).Left -eq $screen.Left + 52 -and (Bar-Rect).Width -eq 52 }
    'island'  = { $b = Bar-Rect; (Work-Area).Top -eq $screen.Top -and $b.Top -eq $screen.Top + 8 -and $b.Width -lt $screen.Width -and (Centered $b) }
    'dock'    = { $b = Bar-Rect; (Work-Area).Bottom -eq $screen.Bottom -and $b.Bottom -eq $screen.Bottom - 10 -and $b.Width -lt $screen.Width / 2 -and (Centered $b) }
}
# The Sandbox turns Windows' animation effects off; on for this part (the bar slides when they are on).
[void][Rmw.Win]::SystemParameterSet(0x1043, 0, [IntPtr]1, 2) # SPI_SETCLIENTAREAANIMATION, send the change
$animations = $false
[void][Rmw.Win]::SystemParameterBool(0x1042, 0, [ref]$animations, 0) # SPI_GETCLIENTAREAANIMATION
Note 'animations' "Windows' animation effects: $animations"
foreach ($style in $styleChecks.Keys) {
    Write-Rice $styled "[[bar]]`nstyle = `"$style`"`n"
    # Applied in the background while the bar's place is watched: it slides in from its edge.
    $applying = Start-Process $rmw -ArgumentList @('apply', "`"$styled`"", '--wait', '--data-dir', "`"$data`"") -WindowStyle Hidden -PassThru
    $positions = New-Object System.Collections.Generic.HashSet[string]
    $sampleUntil = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $sampleUntil -and -not ($applying.HasExited -and $positions.Count -gt 3)) {
        $now = Bar-Rect
        if ($now.Width -gt 0) { [void]$positions.Add("$($now.Left),$($now.Top)") }
        Start-Sleep -Milliseconds 10
    }
    $applying.WaitForExit(30000) | Out-Null
    $r = [pscustomobject]@{ code = $applying.ExitCode; output = '' }
    $placed = Wait-Until $styleChecks[$style] 10
    Check "bar:style-$style" ($r.code -eq 0 -and $placed) "work=$(Work-Area) bar=$(Bar-Rect) apply=$($r.code): $($r.output)"
    if ($style -eq 'island') {
        Check 'bar:slides-in' ((-not $animations) -or $positions.Count -ge 3) "$($positions.Count) positions while it came (animations: $animations)"
    }
    Start-Sleep -Seconds 2
    Shot "bar-4b-style-$style"
}
# The dock: as long as its apps; another app, a longer dock.
$before = (Bar-Rect).Width
$console = Start-Process cmd.exe -PassThru
$grew = Wait-Until { (Bar-Rect).Width -ge $before + 30 -and (Centered (Bar-Rect)) } 10
Check 'bar:dock-grows-with-its-apps' $grew "before=$before now=$((Bar-Rect).Width)"
Shot 'bar-4b-dock-more-apps'
$bar = Bar-Process
if ($bar) {
    Start-Sleep -Seconds 2
    $bar.Refresh(); $cpuBefore = $bar.TotalProcessorTime
    Start-Sleep -Seconds 10
    $bar.Refresh()
    $cpu = ($bar.TotalProcessorTime - $cpuBefore).TotalMilliseconds / 10000.0 * 100.0
    $privateMb = $bar.PrivateMemorySize64 / 1MB
    Check 'bar:dock-memory-under-25mb' ($privateMb -le 25) ('{0:N1} MB private' -f $privateMb)
    Check 'bar:dock-cpu-idle' ($cpu -lt 1.0) ('{0:N2}% over 10 s' -f $cpu)
}
if ($console) { Stop-Process -Id $console.Id -Force -ErrorAction SilentlyContinue }

# ---- 4c. M11: Ricing Manager's Start ---------------------------------------------------------------
function Start-Visible { [Rmw.Win]::IsWindowVisible([Rmw.Win]::FindWindowW('RicingManagerWindows.Start', [NullString]::Value)) }
function Press-Start {
    $b = Bar-Rect
    [void][Rmw.Win]::SetCursorPos($b.Left + 26, $b.Top + [int]($b.Height / 2))
    Start-Sleep -Milliseconds 150
    $watch = [Diagnostics.Stopwatch]::StartNew()
    [Rmw.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [Rmw.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    while (-not (Start-Visible) -and $watch.ElapsedMilliseconds -lt 3000) { Start-Sleep -Milliseconds 2 }
    return $watch.ElapsedMilliseconds
}
$apps = @((Invoke-Rmw @('apps')).output -split "`r?`n" | Where-Object { $_ -match '\s->\s' })
$pickedApp = $null
foreach ($name in 'Windows PowerShell', 'Command Prompt', 'Notepad', 'Paint', 'Calculator') {
    $pickedApp = $apps | Where-Object { $_ -match ('^' + [regex]::Escape($name) + '\s+->') } | Select-Object -First 1
    if ($pickedApp) { break }
}
$appName = ($pickedApp -split '\s+->\s+')[0].Trim()
$appId = ($pickedApp -split '\s+->\s+')[1].Trim()
Note 'start-app' "$appName = $appId"
$startRice = Join-Path $Work 'rice-start'
Write-Rice $startRice ("[start]`nmenu = `"ours`"`npinned = [`"" + ($appId -replace '\\', '\\') + "`"]`n`n[[bar]]`nstyle = `"status`"`n")
$r = Invoke-Rmw @('apply', $startRice, '--wait')
$placed = Wait-Until { (Work-Area).Top -eq $screen.Top + 28 -and (Bar-Rect).Height -eq 28 } 20
Check 'start:apply' ($r.code -eq 0 -and $placed) "$($r.output) work=$(Work-Area) bar=$(Bar-Rect) native=$(Native-Visible)"
Start-Sleep -Seconds 2

$openMs = Press-Start
$opened = Start-Visible
Check 'start:opens-from-the-button' $opened ''
Check 'start:opens-under-150ms' ($opened -and $openMs -lt 150) ('{0} ms' -f $openMs)
Start-Sleep -Milliseconds 400
Shot 'bar-4c-start-open'
$before = @(Get-Process | ForEach-Object Id)
# The whole name: it is the first match (a few letters could match another app first).
[System.Windows.Forms.SendKeys]::SendWait(($appName -replace '([+^%~(){}\[\]])', '{$1}'))
Start-Sleep -Milliseconds 600
Shot 'bar-4c-start-search'
[System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
$new = $null
$launched = Wait-Until {
    $script:new = @(Get-Process | Where-Object { $before -notcontains $_.Id -and $_.Name -notmatch '^(rmw|conhost|svchost|RuntimeBroker|dllhost|backgroundTaskHost|smartscreen|SearchHost|StartMenuExperienceHost)' })
    # Windows starts processes of its own now and then (taskhostw): the app itself is waited for.
    @($script:new | Where-Object { $_.Name -match 'powershell|cmd|notepad|mspaint|calc' }).Count -gt 0
} 15
$opensWhat = @($script:new | Where-Object { $_.Name -match 'powershell|cmd|notepad|mspaint|calc' })
Check 'start:search-and-enter-opens-the-app' ($launched -and $opensWhat.Count -gt 0) "$appName -> $(($script:new | ForEach-Object Name) -join ', ')"
Check 'start:closes-after-it-opened-one' (-not (Start-Visible)) ''
$script:new | Stop-Process -Force -ErrorAction SilentlyContinue

$againMs = Press-Start
Note 'start-open-again' ('{0} ms' -f $againMs)
[System.Windows.Forms.SendKeys]::SendWait('{ESC}')
Check 'start:escape-closes' ((Wait-Until { -not (Start-Visible) } 3)) ''

# The Win key, when the user wants it (settings.toml [start] win_key).
$settingsFile = Join-Path $data 'config\settings.toml'
New-Item -ItemType Directory -Force (Split-Path $settingsFile) | Out-Null
"schema = 1`n`n[core]`nkeep_in_tray = true`n`n[start]`nwin_key = true`n" | Out-File $settingsFile -Encoding ascii
$heard = Wait-Until { (Get-Content (Join-Path $data 'bar\bars.json') -Raw -ErrorAction SilentlyContinue) -match '"winKey":\s*true' } 15
Start-Sleep -Seconds 3
[Rmw.Win]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero) # Win down
[Rmw.Win]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero) # Win up
$byKey = Wait-Until { Start-Visible } 3
Check 'start:win-key-opens-ours' ($heard -and $byKey) "plan heard: $heard"
Shot 'bar-4c-start-win-key'
[System.Windows.Forms.SendKeys]::SendWait('{ESC}')
[void](Wait-Until { -not (Start-Visible) } 3)
# Win+E: Windows' own shortcut, untouched.
[Rmw.Win]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
[Rmw.Win]::keybd_event(0x45, 0, 0, [UIntPtr]::Zero)
[Rmw.Win]::keybd_event(0x45, 0, 2, [UIntPtr]::Zero)
[Rmw.Win]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Seconds 1
Check 'start:win-shortcuts-stay-windows' (-not (Start-Visible)) ''
# Off again: the Win key is Windows' (its own Start opens; Esc closes it).
"schema = 1`n`n[core]`nkeep_in_tray = true`n" | Out-File $settingsFile -Encoding ascii
[void](Wait-Until { (Get-Content (Join-Path $data 'bar\bars.json') -Raw -ErrorAction SilentlyContinue) -match '"winKey":\s*false' } 15)
Start-Sleep -Seconds 3
[Rmw.Win]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
[Rmw.Win]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Seconds 1
Check 'start:win-key-is-windows-again' (-not (Start-Visible)) ''
[System.Windows.Forms.SendKeys]::SendWait('{ESC}')
Start-Sleep -Milliseconds 500

# Back to the two reserving bars the next steps expect.
$r = Invoke-Rmw @('apply', $acrylic, '--wait')
[void](Wait-Until { (Work-Area).Bottom -eq $screen.Bottom - 48 } 10)

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
# Its own folder: another suite's logs go to theirs.
if (Test-Path (Join-Path $data 'logs')) {
    Remove-Item (Join-Path $Out 'bar-logs') -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'bar-logs') -Recurse -Force
}

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'bar.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
