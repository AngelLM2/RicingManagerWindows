<#
    M7 system tests (rmw-settings). Run ONLY inside Windows Sandbox (run-in-sandbox.ps1 -Suite
    settings): it applies rices and changes the theme.

    Acceptance of M7 (docs/04): what M1-M6 do can be done from the window; it opens in < 600 ms
    (warm); closing it frees all it held (the process ends); Narrator can work the main pages (here:
    UI Automation, what Narrator reads, finds and presses the controls); 100%-200% scale.
    Every page is captured (settings-<page>.png) for a look.
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
trap {
    $results.Add([pscustomobject]@{ name = 'script:error'; ok = $false; detail = ($_.Exception.ToString() + ' at ' + $_.InvocationInfo.PositionMessage) })
    continue
}

$rmw = Join-Path $Bin 'rmw.exe'
$settingsExe = Join-Path $Bin 'rmw-settings.exe'
$data = Join-Path $Work 'data'

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms, System.Drawing
Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr window, out RECT rect);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetWindowPos(System.IntPtr window, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool ShowWindow(System.IntPtr window, int command);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsIconic(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetForegroundWindow(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetForegroundWindow();
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetCursorPos(int x, int y);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, System.UIntPtr extra);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'@
$UIA = [System.Windows.Automation.AutomationElement]
$Scope = [System.Windows.Automation.TreeScope]

function Invoke-Rmw([string[]] $Arguments) {
    $output = $null | & $rmw @Arguments --data-dir $data 2>&1 | Out-String
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
function Settings-Processes { @(Get-Process rmw-settings -ErrorAction SilentlyContinue) }
function Start-Settings([hashtable] $Environment = @{}) {
    $info = New-Object System.Diagnostics.ProcessStartInfo $settingsExe
    $info.Arguments = "--data-dir `"$data`""
    $info.UseShellExecute = $false
    foreach ($key in $Environment.Keys) { $info.EnvironmentVariables[$key] = $Environment[$key] }
    $started = Get-Date
    $process = [System.Diagnostics.Process]::Start($info)
    $shown = Wait-Until { $process.Refresh(); $process.MainWindowHandle -ne [IntPtr]::Zero } 20
    $ms = ((Get-Date) - $started).TotalMilliseconds
    [pscustomobject]@{ process = $process; shown = $shown; ms = $ms }
}
function Window-Of($Process) {
    try {
        if ($Process.HasExited) { return $null }
        $Process.Refresh()
        $handle = [IntPtr]$Process.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero) { return $null }
        return $UIA::FromHandle($handle)
    }
    catch {
        $results.Add([pscustomobject]@{ name = 'script:window-of'; ok = $false; detail = $_.Exception.ToString() })
        return $null
    }
}
function Screen-Shot([string] $Name) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bitmap.Size)
    $bitmap.Save((Join-Path $Out "settings-$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
}
function Find-Named($Window, [string] $Name) {
    if ($null -eq $Window) { return $null }
    $condition = New-Object System.Windows.Automation.PropertyCondition($UIA::NameProperty, $Name)
    $Window.FindFirst($Scope::Descendants, $condition)
}
function Press($Window, [string] $Name) {
    $element = $null
    [void](Wait-Until { $script:found = Find-Named $Window $Name; $null -ne $script:found } 5)
    $element = $script:found
    if (-not $element) { return $false }
    try {
        $pattern = $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $pattern.Invoke()
        Start-Sleep -Milliseconds 400
        return $true
    }
    catch { return $false }
}
function Click-Element($Element) {
    if ($null -eq $Element) { return $false }
    $box = $Element.Current.BoundingRectangle
    if ($box.Width -le 0) { return $false }
    [void][Rmw.Win]::SetCursorPos([int]($box.X + $box.Width / 2), [int]($box.Y + $box.Height / 2))
    Start-Sleep -Milliseconds 150
    [Rmw.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [Rmw.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    return $true
}
# A ComboBox as a person uses it: open it, click the item.
function Pick($Window, [string] $Combo, [string] $Item) {
    if ($null -eq $Window) { return $false }
    $comboType = [System.Windows.Automation.ControlType]::ComboBox
    $named = New-Object System.Windows.Automation.PropertyCondition($UIA::NameProperty, $Combo)
    $isCombo = New-Object System.Windows.Automation.PropertyCondition($UIA::ControlTypeProperty, $comboType)
    $box = $Window.FindFirst($Scope::Descendants, (New-Object System.Windows.Automation.AndCondition($named, $isCombo)))
    if (-not (Click-Element $box)) { return $false }
    # The open list's item: named so, and not a combo box (a closed one may show the same text).
    $condition = New-Object System.Windows.Automation.PropertyCondition($UIA::NameProperty, $Item)
    $choice = @($Window.FindAll($Scope::Descendants, $condition) |
        Where-Object { $_.Current.ControlType -ne $comboType -and $_.Current.BoundingRectangle.Width -gt 0 }) |
        Select-Object -Last 1
    return (Click-Element $choice)
}
function Named-Count($Window) {
    if ($null -eq $Window) { return 0 }
    $all = $Window.FindAll($Scope::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    @($all | Where-Object { $_.Current.Name -ne '' }).Count
}
function Shot($Process, [string] $Name) {
    if ($Process.HasExited) { return }
    $Process.Refresh()
    $rect = New-Object Rmw.Win+RECT
    if (-not [Rmw.Win]::GetWindowRect($Process.MainWindowHandle, [ref] $rect)) { return }
    $width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { return }
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $bitmap.Save((Join-Path $Out "settings-$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
}
# The window's pixels as they are on the screen now.
function Window-Bitmap($Process) {
    $Process.Refresh()
    $rect = New-Object Rmw.Win+RECT
    if (-not [Rmw.Win]::GetWindowRect($Process.MainWindowHandle, [ref] $rect)) { return $null }
    $bitmap = New-Object System.Drawing.Bitmap ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    return $bitmap
}
# How different two shots are: the average of |a - b| over a grid of points (0..255).
function Bitmap-Difference($A, $B) {
    if ($null -eq $A -or $null -eq $B -or $A.Width -ne $B.Width -or $A.Height -ne $B.Height) { return 255 }
    $total = 0.0; $count = 0
    for ($y = 4; $y -lt $A.Height - 4; $y += [math]::Max(1, [int]($A.Height / 40))) {
        for ($x = 4; $x -lt $A.Width - 4; $x += [math]::Max(1, [int]($A.Width / 40))) {
            $p = $A.GetPixel($x, $y); $q = $B.GetPixel($x, $y)
            $total += ([math]::Abs($p.R - $q.R) + [math]::Abs($p.G - $q.G) + [math]::Abs($p.B - $q.B)) / 3.0
            $count++
        }
    }
    return $total / [math]::Max(1, $count)
}
function Light-Apps { (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue).AppsUseLightTheme }

for ($i = 0; $i -lt 30 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }

# ---- a rice to work with: light mode, a picture --------------------------------------------------------
$night = Join-Path $data 'rices\Night'
New-Item -ItemType Directory -Force (Join-Path $night 'assets') | Out-Null
$picture = New-Object System.Drawing.Bitmap 320, 180
$g = [System.Drawing.Graphics]::FromImage($picture); $g.Clear([System.Drawing.Color]::FromArgb(30, 40, 80)); $g.Dispose()
$picture.Save((Join-Path $night 'assets\night.png'), [System.Drawing.Imaging.ImageFormat]::Png); $picture.Dispose()
@'
schema = 1

[meta]
name = "Night"
author = "test"

[appearance]
mode = "light"

[[wallpaper.monitor]]
match = "all"
source = "assets/night.png"
'@ | Out-File (Join-Path $night 'rice.toml') -Encoding ascii
$r = Invoke-Rmw @('status') # starts the core for this data folder
Check 'settings:core' ($r.code -eq 0) ''

# ---- opening: cold, then warm (< 600 ms); closing ends the process ------------------------------
$cold = Start-Settings
Check 'settings:opens' $cold.shown ('{0:N0} ms (cold)' -f $cold.ms)
Start-Sleep -Seconds 2
Screen-Shot 'cold'
$cold.process.Refresh()
$responding = $cold.process.Responding
$title = $cold.process.MainWindowTitle
[void]$cold.process.CloseMainWindow()
$closed = Wait-Until { $cold.process.HasExited } 10
Check 'settings:closing-ends-it' $closed "responding=$responding title='$title'"
if (-not $closed) {
    Screen-Shot 'not-closed'
    Stop-Process -Id $cold.process.Id -Force -ErrorAction SilentlyContinue
    [void](Wait-Until { $cold.process.HasExited } 5)
}
$warm = Start-Settings
Check 'settings:opens-warm-under-600ms' ($warm.shown -and $warm.ms -lt 600) ('{0:N0} ms' -f $warm.ms)
$app = $warm.process
Start-Sleep -Seconds 2
$window = Window-Of $app
Check 'settings:uia-window' ($null -ne $window) ''

# ---- one instance --------------------------------------------------------------------------------------
$second = Start-Process $settingsExe "--data-dir `"$data`"" -PassThru
$secondGone = Wait-Until { $second.HasExited } 10
$front = [Rmw.Win]::GetForegroundWindow() -eq $app.MainWindowHandle
Check 'settings:one-instance' ($secondGone -and (Settings-Processes).Count -eq 1) "second exited=$secondGone, processes=$((Settings-Processes).Count), first in front=$front"

# ---- every page, by what Narrator reads ----------------------------------------------------------------
$pages = 'Home', 'Rices', 'Appearance', 'Wallpapers', 'Cursors', 'Taskbar', 'Plugins', 'Hotkeys and rules', 'Diagnostics', 'Settings'
$reached = 0
foreach ($page in $pages) {
    if (Press $window $page) {
        $reached++
        Start-Sleep -Milliseconds 800
        Shot $app ($page -replace ' ', '-').ToLower()
    }
}
Check 'settings:every-page-by-name' ($reached -eq $pages.Count) "$reached of $($pages.Count)"

[void](Press $window 'Rices')
Start-Sleep -Seconds 1
$named = Named-Count $window
Check 'settings:rices-readable' ((Find-Named $window 'Apply Night') -ne $null -and $named -ge 20) "$named named elements"
# M8: packages in and out (the packages suite works them through the core and the CLI).
Check 'settings:import-and-share' ((Find-Named $window 'Import a .rice…') -ne $null -and (Find-Named $window 'Share Night as a .rice file') -ne $null) ''
[void](Press $window 'Plugins')
Start-Sleep -Milliseconds 500
Check 'settings:install-from-file' ((Find-Named $window 'Install from a file…') -ne $null) ''
[void](Press $window 'Rices')
Start-Sleep -Milliseconds 500

# ---- edit a rice, apply it: dark mode -------------------------------------------------------------------
$light = Light-Apps
Check 'settings:edit-opens' (Press $window 'Edit Night') ''
Start-Sleep -Seconds 1
Shot $app 'edit-appearance'
[void][Rmw.Win]::SetForegroundWindow($app.MainWindowHandle)
$picked = Pick $window 'Light or dark' 'Dark'
$changed = Wait-Until { (Get-Content (Join-Path $night 'rice.toml') -Raw) -match 'mode = "dark"' } 5
Note 'picked' "$picked"
Check 'settings:edit-saves-the-rice' $changed ((Get-Content (Join-Path $night 'rice.toml') -Raw) -replace "`r?`n", ' | ')
Check 'settings:apply' (Press $window 'Apply') ''
$dark = Wait-Until { (Light-Apps) -eq 0 } 20
Check 'settings:applied-dark' $dark "AppsUseLightTheme before=$light after=$(Light-Apps)"
Start-Sleep -Seconds 1
Shot $app 'applied'

# ---- my current look as a rice -----------------------------------------------------------------------
[void](Press $window 'Rices')
Check 'settings:export' (Press $window 'Save my current look as a rice') ''
$exported = Wait-Until { Test-Path (Join-Path $data 'rices\My look\rice.toml') } 20
Check 'settings:exported' $exported ((Get-ChildItem (Join-Path $data 'rices') -Name) -join ', ')

# ---- a hotkey -------------------------------------------------------------------------------------------
[void](Press $window 'Hotkeys and rules')
Check 'settings:hotkey-add' (Press $window 'Add a hotkey') ''
$hotkey = Wait-Until { (Test-Path (Join-Path $data 'config\settings.toml')) -and ((Get-Content (Join-Path $data 'config\settings.toml') -Raw) -match '\[\[hotkey\]\]') } 10
Check 'settings:hotkey-saved' $hotkey ''
$status = Wait-Until { (Invoke-Rmw @('status')).output -match '1 hotkey' } 10
Check 'settings:hotkey-in-the-core' $status ''
Shot $app 'hotkeys-after'

# ---- launch.app: an installed app, by its id ------------------------------------------------------
$apps = @((Invoke-Rmw @('apps')).output -split "`r?`n" | Where-Object { $_ -match '\s->\s' })
$wanted = 'Notepad', 'Paint', 'Calculator', 'Windows PowerShell', 'Command Prompt', 'Terminal', 'Registry Editor',
    'Character Map', 'Windows Tools', 'Control Panel', 'Task Manager'
$picked = $null
foreach ($name in $wanted) {
    $picked = $apps | Where-Object { $_ -match ('^' + [regex]::Escape($name) + '\s+->') } | Select-Object -First 1
    if ($picked) { break }
}
Check 'settings:apps-listed' ($apps.Count -gt 3 -and $null -ne $picked) ("$($apps.Count) apps: " + (($apps | Select-Object -First 12) -join ' | '))
if ($picked -match '->\s+(\S.*)$') {
    $id = $Matches[1].Trim()
    $since = Get-Date
    $before = @(Get-Process | ForEach-Object Id)
    $r = Invoke-Rmw @('action', 'launch.app', '--app', $id)
    $new = $null
    $opened = Wait-Until {
        $script:new = @(Get-Process | Where-Object { $before -notcontains $_.Id -and $_.Name -notmatch '^(rmw|conhost|svchost|RuntimeBroker|dllhost|backgroundTaskHost|smartscreen)' })
        $script:new.Count -gt 0
    } 15
    Check 'settings:launch-app' ($r.code -eq 0 -and $opened) "$picked -> $($r.output) new: $(($script:new | ForEach-Object Name) -join ', ')"
    $script:new | Stop-Process -Force -ErrorAction SilentlyContinue
}
$r = Invoke-Rmw @('action', 'launch.app', '--app', 'C:\Windows\System32\cmd.exe')
Check 'settings:launch-app-only-installed' ($r.code -ne 0) $r.output

# ---- the budget -----------------------------------------------------------------------------------------
$privateMb = if ($app.HasExited) { -1 } else { $app.Refresh(); $app.PrivateMemorySize64 / 1MB }
Check 'settings:memory-under-120mb' ($privateMb -gt 0 -and $privateMb -le 120) ('{0:N1} MB private after every page' -f $privateMb)

# ---- closing frees everything ----------------------------------------------------------------------------
if (-not $app.HasExited) { [void]$app.CloseMainWindow() }
Check 'settings:closed' (Wait-Until { $app.HasExited } 10) ''
Check 'settings:nothing-left' ((Wait-Until { (Settings-Processes).Count -eq 0 } 5)) ((Settings-Processes | ForEach-Object { "pid $($_.Id) started $($_.StartTime.ToString('HH:mm:ss')) window '$($_.MainWindowTitle)'" }) -join '; ')

# ---- 200% ------------------------------------------------------------------------------------------------
$big = Start-Settings @{ SLINT_SCALE_FACTOR = '2' }
Check 'settings:opens-at-200%' $big.shown ''
Start-Sleep -Seconds 2
Shot $big.process 'scale-200'
if (-not $big.process.HasExited) { [void]$big.process.CloseMainWindow() }
[void](Wait-Until { $big.process.HasExited } 10)

# ---- in Portuguese (settings.toml [ui] language) ----------------------------------------------------
Add-Content (Join-Path $data 'config\settings.toml') "`n[ui]`nlanguage = `"pt-BR`"`n"
$pt = Start-Settings
Start-Sleep -Seconds 2
$ptWindow = Window-Of $pt.process
Check 'settings:portuguese' ($null -ne (Find-Named $ptWindow 'Início') -and $null -ne (Find-Named $ptWindow 'Atalhos e regras')) ''
Shot $pt.process 'pt-br-home'
[void](Press $ptWindow 'Barra de tarefas')
Start-Sleep -Seconds 1
Shot $pt.process 'pt-br-taskbar'
if (-not $pt.process.HasExited) { [void]$pt.process.CloseMainWindow() }
[void](Wait-Until { $pt.process.HasExited } 10)

# ---- off the screen and back, minimized and back: nothing goes missing ---------------------------------
# (The real-PC test saw the window turn transparent after it had been partly off the screen.)
$moving = Start-Settings
Start-Sleep -Seconds 2
$app2 = $moving.process
# The main window, not a helper the process made first.
[void](Wait-Until {
    $app2.Refresh()
    $probe = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::GetWindowRect($app2.MainWindowHandle, [ref] $probe)
    ($probe.Right - $probe.Left) -gt 300
} 10)
$handle = $app2.MainWindowHandle
$rect = New-Object Rmw.Win+RECT
[void][Rmw.Win]::GetWindowRect($handle, [ref] $rect)
$before = Window-Bitmap $app2
$away = [int](($rect.Right - $rect.Left) * 0.6)
foreach ($step in @(@(-$away, 0), @(0, 0), @(0, 500), @(0, 0))) {
    # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
    [void][Rmw.Win]::SetWindowPos($handle, [IntPtr]::Zero, $rect.Left + $step[0], $rect.Top + $step[1], 0, 0, 0x15)
    Start-Sleep -Milliseconds 700
}
$after = Window-Bitmap $app2
$moved = Bitmap-Difference $before $after
if ($moved -ge 6) { $after.Save((Join-Path $Out 'settings-after-off-screen.png')) }
Check 'settings:whole-after-off-screen' ($moved -lt 6) ('{0:N1} average difference' -f $moved)
[void][Rmw.Win]::ShowWindow($handle, 6)  # SW_MINIMIZE
Start-Sleep -Milliseconds 700
[void][Rmw.Win]::ShowWindow($handle, 9)  # SW_RESTORE
# Back where it was, at its size (Windows animates the restore).
[void](Wait-Until {
    $now = New-Object Rmw.Win+RECT
    [void][Rmw.Win]::GetWindowRect($handle, [ref] $now)
    $now.Left -eq $rect.Left -and $now.Top -eq $rect.Top -and ($now.Right - $now.Left) -eq ($rect.Right - $rect.Left)
} 5)
Start-Sleep -Milliseconds 600
$afterRestore = Window-Bitmap $app2
$restored = Bitmap-Difference $before $afterRestore
$now = New-Object Rmw.Win+RECT
[void][Rmw.Win]::GetWindowRect($handle, [ref] $now)
$where = "before {0},{1} {2}x{3}; after {4},{5} {6}x{7}; iconic={8}" -f $rect.Left, $rect.Top, ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top),
    $now.Left, $now.Top, ($now.Right - $now.Left), ($now.Bottom - $now.Top), [Rmw.Win]::IsIconic($handle)
if ($restored -ge 6) {
    if ($null -ne $afterRestore) { $afterRestore.Save((Join-Path $Out 'settings-after-minimize.png')) }
    Screen-Shot 'after-minimize-screen'
}
Check 'settings:whole-after-minimize' ($restored -lt 6) ('{0:N1} average difference; {1}' -f $restored, $where)
[void][Rmw.Win]::SetForegroundWindow($handle)

if (-not $app2.HasExited) { [void]$app2.CloseMainWindow() }
[void](Wait-Until { $app2.HasExited } 10)

$r = Invoke-Rmw @('restore', '--original', '--wait')
Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'settings.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
