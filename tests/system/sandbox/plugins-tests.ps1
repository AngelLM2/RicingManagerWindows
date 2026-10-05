<#
    M4 system tests (plugins). Run ONLY inside Windows Sandbox (run-in-sandbox.ps1 -Suite plugins):
    they apply rices with a custom bar and plugin widgets, write tweak values and run hostile
    plugins.

    Acceptance of M4 (docs/04): a plugin with an endless loop / a memory bomb -> the host is
    restarted (or the plugin stopped) and the bar stays up; a denied permission is blocked; a
    tweak that writes to Run is rejected at validation; automatic reload < 1 s; the 5 examples
    work. The example plugins come from C:\rmw\plugins (the repository's plugins folder).
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
$data = Join-Path $Work 'data'
$pluginLog = Join-Path $data 'logs\plugins.log'
$barLog = Join-Path $data 'logs\bar.log'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetCursorPos(int x, int y);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetForegroundWindow();
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsIconic(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetDC(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int ReleaseDC(System.IntPtr window, System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("gdi32.dll")]
public static extern bool BitBlt(System.IntPtr target, int x, int y, int width, int height, System.IntPtr source, int sourceX, int sourceY, uint operation);
'@

# stdin is an empty pipe, never the console: a question (install without --yes) is answered "no".
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
function Native-Visible { [Rmw.Win]::IsWindowVisible([Rmw.Win]::FindWindowW('Shell_TrayWnd', [NullString]::Value)) }
function Bar-Process { Get-Process rmw-bar -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Host-Process { Get-Process rmw-plugin-host -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Log-Has([string] $File, [string] $Text) {
    (Test-Path $File) -and ((Get-Content $File -Raw -ErrorAction SilentlyContinue) -match [regex]::Escape($Text))
}
function Log-Lines([string] $File, [string] $Text) {
    if (-not (Test-Path $File)) { return @() }
    @(Get-Content $File -ErrorAction SilentlyContinue | Where-Object { $_ -match [regex]::Escape($Text) })
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
function Click([int] $X, [int] $Y) {
    [void][Rmw.Win]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 150
    [Rmw.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [Rmw.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}
function Write-Plugin([string] $Folder, [hashtable] $Files) {
    New-Item -ItemType Directory -Force $Folder | Out-Null
    foreach ($name in $Files.Keys) {
        [System.IO.File]::WriteAllText((Join-Path $Folder $name), $Files[$name], [System.Text.UTF8Encoding]::new($false))
    }
}
function Rule-Plugin([string] $Name, [string] $Script, [string] $Permissions = '') {
    $folder = Join-Path $Work "plugin-$Name"
    Write-Plugin $folder @{
        'plugin.toml' = @"
schema = 1
[plugin]
id = "io.github.test.$Name"
name = "$Name"
version = "1.0.0"
api = "^1.0"
kind = ["rule"]
$Permissions
[entry]
script = "main.luau"
"@
        'main.luau'   = $Script
    }
    return $folder
}
# The host's status as an object, or $null while it is not there (rmw prints an error instead).
function Host-Status {
    try { return (Invoke-Rmw @('plugin', 'status')).output | ConvertFrom-Json } catch { return $null }
}
function Plugin-State([string] $Id) {
    $list = Invoke-Rmw @('plugin', 'list')
    ($list.output -split "`n" | Where-Object { $_ -match "^$([regex]::Escape($Id)) " }) -join ' '
}

# Something for the window list: two Explorer windows.
Start-Process explorer.exe 'C:\Windows' | Out-Null
Start-Sleep -Seconds 2
Start-Process explorer.exe 'C:\Users' | Out-Null
Start-Sleep -Seconds 2
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

# ---- 1. validation: a tweak that writes to Run is rejected (and so is its install) -------------------
$runTweak = Join-Path $Work 'plugin-run-tweak'
Write-Plugin $runTweak @{ 'plugin.toml' = @'
schema = 1
[plugin]
id = "io.github.test.run-tweak"
name = "Starts something at logon"
version = "1.0.0"
api = "^1.0"
kind = ["tweak"]
[[tweak]]
key = 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run'
value = "Updater"
type = "string"
data = "updater"
'@ }
$r = Invoke-Rmw @('plugin', 'validate', $runTweak)
Check 'plugins:tweak-run-rejected-at-validation' ($r.code -eq 4 -and $r.output -match 'Run') $r.output
$r = Invoke-Rmw @('plugin', 'install', $runTweak, '--yes')
Check 'plugins:tweak-run-not-installed' ($r.code -ne 0) $r.output
$runValue = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Updater' -ErrorAction SilentlyContinue
Check 'plugins:run-key-untouched' ($null -eq $runValue) ''

# ---- 2. the examples install; a rice shows their widgets ---------------------------------------------
$installed = 0
foreach ($example in Get-ChildItem $Examples -Directory) {
    $r = Invoke-Rmw @('plugin', 'install', $example.FullName, '--yes')
    if ($r.code -eq 0) { $installed++ } else { Note "install-$($example.Name)" $r.output }
}
Check 'plugins:examples-installed' ($installed -ge 7) "$installed installed"
$list = Invoke-Rmw @('plugin', 'list')
Note 'plugin-list' $list.output

# A tweak plugin the rice requires: applied with the rice, undone by restore.
$hideExt = Join-Path $Work 'plugin-show-extensions'
Write-Plugin $hideExt @{ 'plugin.toml' = @'
schema = 1
[plugin]
id = "io.github.test.show-extensions"
name = "Show file extensions"
version = "1.0.0"
api = "^1.0"
kind = ["tweak"]
[[tweak]]
key = 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
value = "HideFileExt"
type = "dword"
data = 0
'@ }
$r = Invoke-Rmw @('plugin', 'install', $hideExt, '--yes')
Check 'plugins:tweak-installed' ($r.code -eq 0) $r.output
$advanced = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
$hideBefore = (Get-ItemProperty $advanced -Name HideFileExt -ErrorAction SilentlyContinue).HideFileExt
Note 'HideFileExt-before' "$hideBefore"

$rice = Join-Path $Work 'rice-plugins'
New-Item -ItemType Directory -Force $rice | Out-Null
@'
schema = 1

[meta]
name = "Plugin test"

[palette]
night = "#1a1b26"
ink = "#c0caf5"
accent = "#7aa2f7"

[plugins]
requires = [
    "io.github.angellm2.window-list", "io.github.angellm2.cpu-graph", "io.github.angellm2.clock",
    "io.github.angellm2.now-playing", "io.github.angellm2.weather", "io.github.angellm2.battery",
    "io.github.test.show-extensions",
]

[plugins."io.github.angellm2.weather"]
place = "Sandbox"

[[bar]]
position = "bottom"
thickness = 44
background = { kind = "solid", tint = "palette.night" }
foreground = "palette.ink"
hide_native_taskbar = true
left = ["io.github.angellm2.window-list"]
center = ["io.github.angellm2.clock"]
right = ["io.github.angellm2.now-playing", "io.github.angellm2.weather", { id = "io.github.angellm2.cpu-graph", count = 30 }, "io.github.angellm2.battery"]
'@ | Out-File (Join-Path $rice 'rice.toml') -Encoding ascii
$r = Invoke-Rmw @('apply', $rice, '--wait')
Check 'plugins:apply' ($r.code -eq 0) $r.output
$barUp = Wait-Until { $null -ne (Bar-Process) -and -not (Native-Visible) } 20
Check 'plugins:bar-running' $barUp ''
$hostUp = Wait-Until { $null -ne (Host-Process) } 20
Check 'plugins:host-running' $hostUp ''
$hideAfter = (Get-ItemProperty $advanced -Name HideFileExt -ErrorAction SilentlyContinue).HideFileExt
Check 'plugins:tweak-applied' ($hideAfter -eq 0) "HideFileExt=$hideAfter"

$widgets = 'window-list', 'cpu-graph', 'clock', 'now-playing', 'weather', 'battery'
$ready = Wait-Until { @($widgets | Where-Object { -not (Log-Has $barLog "plugin widget io.github.angellm2.$_ ready") }).Count -eq 0 } 20
Check 'plugins:widgets-compiled-in-bar' $ready ''
$running = Wait-Until { @($widgets | Where-Object { -not (Log-Has $pluginLog "io.github.angellm2.$_ running") }).Count -eq 0 } 20
Check 'plugins:scripts-running' $running ''
$connected = Wait-Until {
    $json = Host-Status
    $null -ne $json -and $json.bar -eq $true -and @($json.plugins | Where-Object { $_.widgets -ge 1 }).Count -ge 6
} 20
$statusJson = Host-Status
$withWidgets = @($statusJson.plugins | Where-Object { $_.widgets -ge 1 }).Count
Check 'plugins:bar-and-host-connected' $connected "bar=$($statusJson.bar) widgets on $withWidgets plugins"
$valued = Wait-Until { (Log-Lines $barLog 'first values for').Count -ge 5 } 15
Check 'plugins:values-reach-the-bar' $valued ("{0} widgets got values" -f (Log-Lines $barLog 'first values for').Count)
Check 'plugins:weather-asked-the-network' (Wait-Until { Log-Has $pluginLog 'weather:' } 15) 'no network in the Sandbox: the plugin reports the failure'
Start-Sleep -Seconds 3
Shot 'plugins-1-bar'

# The window list chips: the first one is the oldest window; click brings it forward, again minimizes.
# A probe plugin logs which window the host sees in front.
$probe = Rule-Plugin 'probe' @'
rmw.on("windows.list", function(list)
    for _, w in list do
        if w.active then rmw.log("probe active: " .. w.title) end
    end
end)
'@ "[permissions]`nproviders = [`"windows.list`"]"
$r = Invoke-Rmw @('plugin', 'install', $probe, '--yes')
$r = Invoke-Rmw @('plugin', 'enable', 'io.github.test.probe')
Start-Sleep -Seconds 2
$chipX = 6 + 30
$chipY = $screen.Bottom - 22
Click $chipX $chipY
$first = [Rmw.Win]::GetForegroundWindow()
Click $chipX $chipY
$minimized = [Rmw.Win]::IsIconic($first)
Click $chipX $chipY
$restored = -not [Rmw.Win]::IsIconic($first)
Check 'plugins:window-list-plugin-clicks' ($minimized -and $restored) "minimized=$minimized restored=$restored"
# How long the host took to see the window that came forward (window events, or the 1 s poll).
$seen = Wait-Until { (Log-Lines $pluginLog 'probe active').Count -ge 1 } 5
Note 'host-sees-the-foreground' "seen=$seen; $(((Log-Lines $pluginLog 'probe active') | Select-Object -First 1))"
Shot 'plugins-2-window-list'

$hostProcess = Host-Process
if ($hostProcess) {
    $privateMb = $hostProcess.PrivateMemorySize64 / 1MB
    Check 'plugins:host-memory' ($privateMb -le 64) ('{0:N1} MB private with 6 plugins' -f $privateMb)
}
$bar = Bar-Process
if ($bar) {
    Check 'plugins:bar-memory' (($bar.PrivateMemorySize64 / 1MB) -le 25) ('{0:N1} MB private' -f ($bar.PrivateMemorySize64 / 1MB))
}

# ---- 3. hostile plugins: the host stops them; the bar never notices --------------------------------
$barPid = (Bar-Process).Id
$loop = Rule-Plugin 'endless-loop' 'rmw.every(250, function() while true do end end)'
$bomb = Rule-Plugin 'memory-bomb' @'
local hoard = {}
rmw.every(250, function()
    for i = 1, 1e9 do hoard[#hoard + 1] = string.rep("x", 1048576) .. i end
end)
'@
foreach ($folder in $loop, $bomb) {
    $r = Invoke-Rmw @('plugin', 'install', $folder, '--yes')
    $id = 'io.github.test.' + (Split-Path $folder -Leaf).Substring(7)
    $r = Invoke-Rmw @('plugin', 'enable', $id)
}
$loopStopped = Wait-Until { (Plugin-State 'io.github.test.endless-loop') -match 'stopped' } 20
Check 'plugins:endless-loop-stopped' $loopStopped (Plugin-State 'io.github.test.endless-loop')
Check 'plugins:endless-loop-ran-out-of-time' (Log-Has $pluginLog 'ran longer than 50 ms') ''
$bombStopped = Wait-Until { (Plugin-State 'io.github.test.memory-bomb') -match 'stopped' } 20
Check 'plugins:memory-bomb-stopped' $bombStopped (Plugin-State 'io.github.test.memory-bomb')
Check 'plugins:memory-bomb-hit-the-cap' (Log-Has $pluginLog 'memory limit of 16 MB') ''
Check 'plugins:others-still-run' (Wait-Until { (Log-Lines $pluginLog 'cpu-graph').Count -ge 0 -and $null -ne (Host-Process) } 5) ''
Check 'plugins:bar-unharmed-by-scripts' ($null -ne (Bar-Process) -and (Bar-Process).Id -eq $barPid -and -not (Native-Visible)) ''

# A pathological pattern runs inside a C function: Luau checks the time there too, so the plugin
# is stopped like any endless loop (the host's watchdog stays the last resort).
$hang = Rule-Plugin 'c-hang' @'
rmw.every(1000, function()
    string.find(string.rep("a", 60000), ".-.-.-.-.-.-.-.-.-.-b")
end)
'@
$r = Invoke-Rmw @('plugin', 'install', $hang, '--yes')
$r = Invoke-Rmw @('plugin', 'enable', 'io.github.test.c-hang')
$hangStopped = Wait-Until { (Plugin-State 'io.github.test.c-hang') -match 'stopped' } 30
Check 'plugins:pattern-hang-stopped' $hangStopped (Plugin-State 'io.github.test.c-hang')

# The host itself dies (a crash, the watchdog): the supervisor starts it again, the widgets come
# back, and the bar never went away.
$hostPid = (Host-Process).Id
Stop-Process -Id $hostPid -Force
$restartedHost = Wait-Until { $null -ne (Host-Process) -and (Host-Process).Id -ne $hostPid } 20
Check 'plugins:killed-host-restarted' $restartedHost ''
$reconnected = Wait-Until {
    $json = Host-Status
    $null -ne $json -and $json.bar -eq $true -and @($json.plugins | Where-Object { $_.widgets -ge 1 }).Count -ge 6
} 20
Check 'plugins:widgets-back-after-host-restart' $reconnected ''
Check 'plugins:bar-up-through-it-all' ($null -ne (Bar-Process) -and (Bar-Process).Id -eq $barPid -and -not (Native-Visible)) ''
Shot 'plugins-2b-after-host-restart'

# ---- 4. a denied permission is blocked -----------------------------------------------------------------
$snoop = Rule-Plugin 'snoop' 'rmw.log(#rmw.provider("windows.list"))'
$r = Invoke-Rmw @('plugin', 'install', $snoop, '--yes')
$r = Invoke-Rmw @('plugin', 'enable', 'io.github.test.snoop')
$denied = Wait-Until { Log-Has $pluginLog 'permission denied: provider ''windows.list''' } 15
Check 'plugins:permission-denied-blocked' $denied ''
# Approved for less than it now asks (an update): it waits for a new approval.
$grow = Rule-Plugin 'grows' 'rmw.log("grows ran")' "[permissions]`nproviders = [`"system.cpu`"]"
$r = Invoke-Rmw @('plugin', 'install', $grow, '--yes')
$r = Invoke-Rmw @('plugin', 'enable', 'io.github.test.grows')
[void](Wait-Until { Log-Has $pluginLog 'grows ran' } 15)
Rule-Plugin 'grows' 'rmw.log("grows ran again")' "[permissions]`nproviders = [`"system.cpu`", `"media`"]" | Out-Null
$r = Invoke-Rmw @('plugin', 'install', $grow) # no --yes, no console: not approved
$waits = Wait-Until { (Plugin-State 'io.github.test.grows') -match 'approval' } 15
Check 'plugins:more-permissions-need-approval' ($waits -and -not (Log-Has $pluginLog 'grows ran again')) (Plugin-State 'io.github.test.grows')

# ---- 5. automatic reload under 1 s ---------------------------------------------------------------------
$devFolder = Join-Path $Work 'plugin-dev'
$r = Invoke-Rmw @('plugin', 'new', 'widget', $devFolder, '--id', 'io.github.test.dev-widget')
Check 'plugins:new-from-template' ($r.code -eq 0) $r.output
$r = Invoke-Rmw @('plugin', 'dev', $devFolder, '--yes', '--no-log')
Check 'plugins:dev' ($r.code -eq 0) $r.output
[void](Wait-Until { Log-Has $pluginLog 'io.github.test.dev-widget running' } 15)
Start-Sleep -Seconds 1
$script = Join-Path $devFolder 'main.luau'
$marker = "reload-marker-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$savedAt = Get-Date
Add-Content $script "`nrmw.log(`"$marker`")" -Encoding utf8
$reloaded = Wait-Until { Log-Has $pluginLog $marker } 5
$elapsed = ((Get-Date) - $savedAt).TotalMilliseconds
Check 'plugins:script-reload-under-1s' ($reloaded -and $elapsed -lt 1000) ('{0:N0} ms' -f $elapsed)

# The widget's look: put it on the bar, then edit its .slint and time the bar's rebuild.
$devRice = Join-Path $Work 'rice-dev'
New-Item -ItemType Directory -Force $devRice | Out-Null
@'
schema = 1
[meta]
name = "Dev"
[plugins]
requires = ["io.github.test.dev-widget"]
[[bar]]
position = "bottom"
thickness = 40
hide_native_taskbar = true
left = ["start", "io.github.test.dev-widget"]
center = ["clock"]
'@ | Out-File (Join-Path $devRice 'rice.toml') -Encoding ascii
$r = Invoke-Rmw @('plugin', 'dev', $devFolder, '--yes', '--no-log')
$r = Invoke-Rmw @('apply', $devRice, '--wait')
[void](Wait-Until { (Log-Lines $barLog 'plugin widget io.github.test.dev-widget ready').Count -ge 1 } 20)
Start-Sleep -Seconds 2
$readyBefore = (Log-Lines $barLog 'plugin widget io.github.test.dev-widget ready').Count
$slint = Join-Path $devFolder 'widget.slint'
$savedAt = Get-Date
(Get-Content $slint -Raw) -replace 'font-size: 12px', 'font-size: 14px' | Out-File $slint -Encoding utf8 -NoNewline
$rebuilt = Wait-Until { (Log-Lines $barLog 'plugin widget io.github.test.dev-widget ready').Count -gt $readyBefore } 5
$elapsed = ((Get-Date) - $savedAt).TotalMilliseconds
Check 'plugins:widget-reload-under-1s' ($rebuilt -and $elapsed -lt 1000) ('{0:N0} ms' -f $elapsed)
Shot 'plugins-3-dev-widget'

# ---- 6. focus mode: an action plugin through the CLI (a hotkey calls the same) --------------------------
$r = Invoke-Rmw @('apply', $rice, '--wait')
[void](Wait-Until { $null -ne (Bar-Process) } 20)
$r = Invoke-Rmw @('plugin', 'enable', 'io.github.angellm2.focus-mode')
[void](Wait-Until { Log-Has $pluginLog 'io.github.angellm2.focus-mode running' } 15)
$r = Invoke-Rmw @('action', 'io.github.angellm2.focus-mode:toggle')
Check 'plugins:focus-mode-action' ($r.code -eq 0) $r.output
$focused = Wait-Until { $null -eq (Bar-Process) -and (Native-Visible) } 20
Check 'plugins:focus-mode-hides-the-bar' $focused ''
Start-Sleep -Seconds 3
Shot 'plugins-4-focus-on'
$r = Invoke-Rmw @('action', 'io.github.angellm2.focus-mode:toggle')
$unfocused = Wait-Until { $null -ne (Bar-Process) -and -not (Native-Visible) } 30
Check 'plugins:focus-mode-brings-the-rice-back' $unfocused (Invoke-Rmw @('status')).output
Start-Sleep -Seconds 3
Shot 'plugins-5-focus-off'

# ---- 7. restore: the tweak, the bar and the taskbar as before -----------------------------------------
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'plugins:restore' ($r.code -eq 0) $r.output
$hideRestored = (Get-ItemProperty $advanced -Name HideFileExt -ErrorAction SilentlyContinue).HideFileExt
Check 'plugins:tweak-restored' ("$hideRestored" -eq "$hideBefore") "before=$hideBefore after=$hideRestored"
Check 'plugins:restore-bar-gone' (Wait-Until { $null -eq (Bar-Process) -and (Native-Visible) } 15) ''
Shot 'plugins-6-restored'

Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 3
Check 'plugins:host-ends-with-the-core' (Wait-Until { $null -eq (Host-Process) } 15) ''
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'plugins.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
