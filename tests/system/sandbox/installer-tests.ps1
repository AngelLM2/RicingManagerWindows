<#
    M8 system tests (the installer). Run ONLY inside Windows Sandbox (run-in-sandbox.ps1 -Suite
    installer): it installs, applies rices and uninstalls.

    Acceptance of v1.0 (docs/04 M8): installs and uninstalls without admin (everything per user:
    files under %LOCALAPPDATA%, the uninstall entry in HKCU); the official rices land in the
    library, validate and apply in under 3 s; installing over it stops and restarts the core;
    uninstalling offers to restore the original Windows and always gives Windows' taskbar back,
    removes the start-with-Windows entry, and keeps or removes the data as asked.
#>
param(
    [Parameter(Mandatory)] [string] $Setup,
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

Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowEx(System.IntPtr parent, System.IntPtr after, string className, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(System.IntPtr window, out uint pid);
'@
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$app = Join-Path $env:LOCALAPPDATA 'Programs\Ricing Manager'
$data = Join-Path $env:LOCALAPPDATA 'RicingManagerWindows'
$rmw = Join-Path $app 'rmw.exe'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B1E3C2A-8F4D-4E7B-9A51-2D7C0F3E9B84}_is1'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$official = 'Diva Stage', 'Tokyo Midnight', 'Sakura Dawn', 'Mocha Kawaii', 'Your Rice'

function Invoke-Rmw([string[]] $Arguments) {
    $output = $null | & $rmw @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ code = $LASTEXITCODE; output = $output.Trim() }
}
function Wait-Until([scriptblock] $Condition, [double] $Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return [bool](& $Condition)
}
function Ours { @(Get-Process rmw-core, rmw-bar, rmw-wallpaper, rmw-plugin-host, rmw-settings -ErrorAction SilentlyContinue) }
# Windows' own taskbar: Explorer's Shell_TrayWnd (the bar has a window of that class too, for the tray).
function Taskbar-Visible {
    $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object Id)
    $window = [IntPtr]::Zero
    while (($window = [Rmw.Win]::FindWindowEx([IntPtr]::Zero, $window, 'Shell_TrayWnd', $null)) -ne [IntPtr]::Zero) {
        $owner = [uint32]0
        [void][Rmw.Win]::GetWindowThreadProcessId($window, [ref]$owner)
        if ($explorer -contains $owner) { return [Rmw.Win]::IsWindowVisible($window) }
    }
    return $false
}
function Light { (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue).AppsUseLightTheme }
function Shot([string] $Name) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bitmap.Save((Join-Path $Out "installer-$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bitmap.Dispose()
}
function Install([string] $Log) {
    $started = Get-Date
    # Only setup itself: -Wait would also wait for the core an update starts again (it stays).
    $p = Start-Process $Setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS=rices', "/LOG=`"$(Join-Path $Out $Log)`"" -PassThru
    if (-not $p.WaitForExit(300000)) { return [pscustomobject]@{ code = -1; seconds = 300 } }
    return [pscustomobject]@{ code = $p.ExitCode; seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1) }
}
# The uninstaller copies itself to %TEMP% and runs from there: wait for the programs to go.
function Uninstall([string[]] $Answers, [string] $Log) {
    $uninstaller = Join-Path $app 'unins000.exe'
    if (-not (Test-Path $uninstaller)) { return $false }
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$(Join-Path $Out $Log)`"") + $Answers
    $p = Start-Process $uninstaller -ArgumentList $arguments -PassThru
    [void]$p.WaitForExit(60000)
    return Wait-Until { -not (Test-Path (Join-Path $app 'rmw-core.exe')) -and -not (Get-Process '_iu*' -ErrorAction SilentlyContinue) } 180
}
# What of ours still runs, and how it was started (for a failure's detail).
function Ours-Detail {
    (Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'rmw*' -or $_.Name -like '_iu*' } |
        ForEach-Object { "$($_.ProcessId) $($_.CommandLine) (parent $($_.ParentProcessId))" }) -join ' | '
}

$lightBefore = Light
Note 'setup' "$(Split-Path $Setup -Leaf), $([math]::Round((Get-Item $Setup).Length / 1MB, 1)) MB"

# ---- install ---------------------------------------------------------------------------------------------
$r = Install 'setup-1.log'
Check 'installer:installs' ($r.code -eq 0) "exit $($r.code) in $($r.seconds) s"
$programs = 'rmw-core.exe', 'rmw.exe', 'rmw-settings.exe', 'rmw-bar.exe', 'rmw-plugin-host.exe', 'rmw-wallpaper.exe', 'slint_cpp.dll', 'vcruntime140.dll'
$missing = @($programs | Where-Object { -not (Test-Path (Join-Path $app $_)) })
Check 'installer:programs-per-user' ($missing.Count -eq 0) ("in $app" + $(if ($missing) { '; missing ' + ($missing -join ', ') } else { '' }))
Check 'installer:uninstall-entry-in-hkcu' (Test-Path $uninstallKey) ''
$hklm = Test-Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B1E3C2A-8F4D-4E7B-9A51-2D7C0F3E9B84}_is1'
Check 'installer:nothing-machine-wide' (-not $hklm) ''
Check 'installer:start-menu' (Test-Path (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Ricing Manager.lnk')) ''
Check 'installer:examples-and-docs' ((Test-Path (Join-Path $app 'plugins\examples\clock\plugin.toml')) -and (Test-Path (Join-Path $app 'docs\user-guide.md'))) ''
$present = @($official | Where-Object { Test-Path (Join-Path $data "rices\$_\rice.toml") })
Check 'installer:official-rices-in-library' ($present.Count -eq $official.Count) ($present -join ', ')

# ---- the official rices --------------------------------------------------------------------------------------
foreach ($rice in $official) {
    $r = Invoke-Rmw @('validate', (Join-Path $data "rices\$rice"))
    Check "installer:valid-$($rice.ToLower().Replace(' ', '-'))" ($r.code -eq 0) $r.output
}
$times = @()
foreach ($rice in 'Sakura Dawn', 'Mocha Kawaii', 'Tokyo Midnight', 'Diva Stage') {
    $started = Get-Date
    $r = Invoke-Rmw @('apply', (Join-Path $data "rices\$rice"), '--wait')
    $ms = [int]((Get-Date) - $started).TotalMilliseconds
    $times += "$rice $ms ms"
    Check "installer:applies-$($rice.ToLower().Replace(' ', '-'))" ($r.code -eq 0) "$ms ms; $($r.output)"
    Start-Sleep -Seconds 3
    Shot ($rice.ToLower().Replace(' ', '-'))
}
Note 'apply-times' ($times -join '; ')
$slow = @($times | Where-Object { [int](($_ -split ' ')[-2]) -ge 3000 })
Check 'installer:apply-under-3s' ($slow.Count -eq 0) ($times -join '; ')
Check 'installer:bar-replaces-taskbar' ((Wait-Until { (Get-Process rmw-bar -ErrorAction SilentlyContinue) -and -not (Taskbar-Visible) } 15)) ''
Check 'installer:video-plays' ((Wait-Until { Get-Process rmw-wallpaper -ErrorAction SilentlyContinue } 15)) ''

# The window opens from where it was installed.
$settings = Start-Process (Join-Path $app 'rmw-settings.exe') -PassThru
$shown = Wait-Until { $settings.Refresh(); $settings.MainWindowHandle -ne [IntPtr]::Zero } 20
Check 'installer:settings-opens' $shown ''
if (-not $settings.HasExited) { [void]$settings.CloseMainWindow(); [void](Wait-Until { $settings.HasExited } 10) }

# ---- start with Windows, then install over it ---------------------------------------------------------------------
$r = Invoke-Rmw @('autostart', 'on')
$entry = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).RicingManagerWindows
Check 'installer:autostart-entry' ($r.code -eq 0 -and $entry -like "*$app\rmw-core.exe*--startup*") "$entry"
$coreBefore = @(Get-Process rmw-core -ErrorAction SilentlyContinue | ForEach-Object Id)
$r = Install 'setup-2.log'
Check 'installer:updates-in-place' ($r.code -eq 0) "exit $($r.code) in $($r.seconds) s"
$restarted = Wait-Until { @(Get-Process rmw-core -ErrorAction SilentlyContinue | Where-Object { $coreBefore -notcontains $_.Id }).Count -gt 0 } 30
Check 'installer:update-restarts-the-core' $restarted "before: $($coreBefore -join ',')"
Check 'installer:update-keeps-the-rice' ((Wait-Until { Get-Process rmw-bar -ErrorAction SilentlyContinue } 20)) ''

# ---- uninstall: restore, remove everything ---------------------------------------------------------------------------
$gone = Uninstall @('/RESTORE=1', '/KEEPDATA=0') 'uninstall-1.log'
Check 'installer:uninstalls' $gone (Ours-Detail)
Check 'installer:processes-gone' ((Wait-Until { (Ours).Count -eq 0 } 20)) ((Ours | ForEach-Object Name) -join ', ')
Check 'installer:taskbar-back' ((Wait-Until { Taskbar-Visible } 10)) ''
Check 'installer:original-look-back' ((Light) -eq $lightBefore) "AppsUseLightTheme $lightBefore -> $(Light)"
Check 'installer:autostart-removed' ($null -eq (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).RicingManagerWindows) ''
Check 'installer:entry-removed' (-not (Test-Path $uninstallKey)) ''
Check 'installer:data-removed-when-asked' (-not (Test-Path $data)) ''
Shot 'after-uninstall'

# ---- again, keeping the data ------------------------------------------------------------------------------------------
$r = Install 'setup-3.log'
Check 'installer:reinstalls' ($r.code -eq 0) "exit $($r.code)"
$r = Invoke-Rmw @('apply', (Join-Path $data 'rices\Sakura Dawn'), '--wait')
$gone = Uninstall @('/RESTORE=0') 'uninstall-2.log'
Check 'installer:uninstalls-again' $gone (Ours-Detail)
Check 'installer:data-kept-by-default' (Test-Path (Join-Path $data 'rices\Sakura Dawn\rice.toml')) ''
Check 'installer:taskbar-back-without-restore' ((Wait-Until { Taskbar-Visible } 10)) ''
Check 'installer:processes-gone-again' ((Wait-Until { (Ours).Count -eq 0 } 20)) ((Ours | ForEach-Object Name) -join ', ')

if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }
$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'installer.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
