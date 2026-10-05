<#
    M1 system tests. Run ONLY inside Windows Sandbox (run-in-sandbox.ps1 -Suite system): they
    apply a rice to the real (sandboxed) Windows and restore it.

    Acceptance of M1 (docs/04): apply a rice with wallpaper + dark mode + accent + cursors +
    native taskbar; `rmw restore --original` brings everything back; killing the core in the
    middle of an apply is undone on the next start; a second concurrent apply gets `busy`.
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

$rmw = Join-Path $Bin 'rmw.exe'
$core = Join-Path $Bin 'rmw-core.exe'
$data = Join-Path $Work 'data'
$rice = Join-Path $Work 'rice'

function Invoke-Rmw([string[]] $Arguments) {
    $output = & $rmw @Arguments --data-dir $data 2>&1 | Out-String
    return [pscustomobject]@{ code = $LASTEXITCODE; output = $output.Trim() }
}

function Start-Core([string[]] $Extra) {
    & $rmw shutdown --data-dir $data *> $null
    # The old core must be gone: it holds the single-instance mutex until it exits.
    for ($i = 0; $i -lt 100 -and (Get-Process rmw-core -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
    Start-Process -FilePath $core -ArgumentList (@('--data-dir', "`"$data`"") + $Extra) -WindowStyle Hidden | Out-Null
    for ($i = 0; $i -lt 50 -and -not (Get-Process rmw-core -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 500
}

function Read-State {
    $personalize = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue
    $dwm = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\DWM' -ErrorAction SilentlyContinue
    $advanced = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -ErrorAction SilentlyContinue
    $search = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Search' -ErrorAction SilentlyContinue
    $cursors = Get-ItemProperty 'HKCU:\Control Panel\Cursors' -ErrorAction SilentlyContinue
    [ordered]@{
        AppsLight    = $personalize.AppsUseLightTheme
        SystemLight  = $personalize.SystemUsesLightTheme
        Prevalence   = $personalize.ColorPrevalence
        Accent       = if ($null -ne $dwm.ColorizationColor) { '{0:X6}' -f ($dwm.ColorizationColor -band 0xFFFFFF) } else { $null }
        TaskView     = $advanced.ShowTaskViewButton
        Search       = $search.SearchboxTaskbarMode
        Arrow        = $cursors.Arrow
        Wait         = $cursors.Wait
        Scheme       = $cursors.'(default)'
        SchemeSource = $cursors.'Scheme Source'
    }
}

function Compare-State($Before, $After) {
    $diff = @()
    foreach ($key in $Before.Keys) {
        if ("$($Before[$key])" -ne "$($After[$key])") { $diff += "$key '$($Before[$key])' -> '$($After[$key])'" }
    }
    return $diff
}

# ---- a rice that touches every M1 module, built from files Windows ships ----------------------
New-Item -ItemType Directory -Force (Join-Path $rice 'assets\cursors') | Out-Null
Copy-Item 'C:\Windows\Cursors\aero_link.cur' (Join-Path $rice 'assets\cursors\pointer.cur')
Copy-Item 'C:\Windows\Cursors\aero_busy.ani' (Join-Path $rice 'assets\cursors\busy.ani')
$image = Get-ChildItem 'C:\Windows\Web' -Recurse -Include *.jpg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($image) {
    Copy-Item $image.FullName (Join-Path $rice 'assets\wall.jpg')
} else {
    # 2x2 red BMP
    [byte[]] $bmp = 0x42,0x4D,70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,2,0,0,0,2,0,0,0,1,0,24,0,0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,255,0,0,0,0,255,0,0,255,0,0
    [IO.File]::WriteAllBytes((Join-Path $rice 'assets\wall.jpg'), $bmp)
}
@'
schema = 1

[meta]
name = "Sandbox system test"

[appearance]
mode = "dark"
accent = "#7aa2f7"
accent_on_taskbar = true

[taskbar]
task_view = false
search = "icon"

[cursors]
source = "assets/cursors"
[cursors.map]
normal = "pointer.cur"
busy = "busy.ani"

[wallpaper]
background = "#1a1b26"

[[wallpaper.monitor]]
match = "all"
source = "assets/wall.jpg"
fit = "fill"
'@ | Out-File (Join-Path $rice 'rice.toml') -Encoding ascii

# ---- 0. diagnostics (never fail the run) ----------------------------------------------------------
Add-Type -Namespace Rmw -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern bool SystemParametersInfoW(uint action, uint uiParam, System.IntPtr pvParam, uint winIni);
'@
foreach ($flags in 0, 3) {
    $ok = [Rmw.Native]::SystemParametersInfoW(0x57, 0, [IntPtr]::Zero, $flags) # SPI_SETCURSORS
    $error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $results.Add([pscustomobject]@{ name = "diag:native-spi-setcursors-flags$flags"; ok = $true; detail = "returned=$ok lastError=$error" })
}
$diagData = Join-Path $Work 'diag-data'
$env:RMW_DISABLE_MITIGATIONS = '1'
$diag = & $rmw apply $rice --data-dir $diagData 2>&1 | Out-String
$diagCode = $LASTEXITCODE
$results.Add([pscustomobject]@{ name = 'diag:apply-without-mitigations'; ok = $true; detail = "exit=$diagCode $($diag.Trim())" })
if ($diagCode -eq 0) {
    $diag = & $rmw restore --original --data-dir $diagData 2>&1 | Out-String
    $results.Add([pscustomobject]@{ name = 'diag:restore-without-mitigations'; ok = $true; detail = "exit=$LASTEXITCODE $($diag.Trim())" })
}
& $rmw shutdown --data-dir $diagData *> $null
Remove-Item Env:\RMW_DISABLE_MITIGATIONS
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $diagData 'logs')) { Copy-Item (Join-Path $diagData 'logs') (Join-Path $Out 'logs-diag') -Recurse -Force }

# ---- 1. validate, apply, restore ----------------------------------------------------------------
$r = Invoke-Rmw @('validate', $rice)
Check 'validate' ($r.code -eq 0) $r.output

$before = Read-State
# Control Panel\Desktop\WallPaper points at TranscodedWallpaper after any theme change: the
# wallpaper is checked through what the module itself sees.
$wallpaperBefore = (Invoke-Rmw @('inspect', 'wallpaper')).output
$r = Invoke-Rmw @('apply', $rice)
Check 'apply' ($r.code -eq 0) $r.output
Start-Sleep -Seconds 2
$after = Read-State
Check 'apply:dark' ($after.AppsLight -eq 0 -and $after.SystemLight -eq 0) "apps=$($after.AppsLight) system=$($after.SystemLight)"
Check 'apply:accent' ($after.Accent -eq '7AA2F7') "accent=$($after.Accent)"
Check 'apply:accent-on-taskbar' ($after.Prevalence -eq 1) "prevalence=$($after.Prevalence)"
Check 'apply:taskbar' ($after.TaskView -eq 0 -and $after.Search -eq 1) "taskView=$($after.TaskView) search=$($after.Search)"
Check 'apply:cursors' ("$($after.Arrow)" -like '*\store\*.cur' -and "$($after.Wait)" -like '*\store\*.ani') "arrow=$($after.Arrow) wait=$($after.Wait)"
$wallpaperAfter = (Invoke-Rmw @('inspect', 'wallpaper')).output
$shown = @(($wallpaperAfter | ConvertFrom-Json).state.PSObject.Properties | Where-Object { $_.Name -like 'monitor:*' } | ForEach-Object { $_.Value.file })
Check 'apply:wallpaper' ($shown.Count -gt 0 -and @($shown | Where-Object { $_ -notlike '*\store\*' }).Count -eq 0) ($shown -join ', ')

$exported = Join-Path $Work 'exported'
$r = Invoke-Rmw @('export', $exported, '--name', 'Exported from the Sandbox')
Check 'export' ($r.code -eq 0 -and (Test-Path (Join-Path $exported 'rice.toml'))) $r.output
$r = Invoke-Rmw @('validate', $exported)
Check 'export:valid' ($r.code -eq 0) $r.output

$r = Invoke-Rmw @('history')
Check 'history' ($r.code -eq 0 -and $r.output -match 'Sandbox system test') $r.output

$r = Invoke-Rmw @('restore', '--original')
Check 'restore' ($r.code -eq 0) $r.output
Start-Sleep -Seconds 2
$restored = Read-State
$diff = Compare-State $before $restored
Check 'restore:everything-back' ($diff.Count -eq 0) ($diff -join '; ')
$wallpaperRestored = (Invoke-Rmw @('inspect', 'wallpaper')).output
Check 'restore:wallpaper-back' ($wallpaperRestored -eq $wallpaperBefore) "before=$wallpaperBefore after=$wallpaperRestored"

# ---- 2. a second operation while one runs gets busy ---------------------------------------------
Start-Core @('--test-delay-ms', '6000')
$first = Start-Process -FilePath $rmw -ArgumentList @('apply', "`"$rice`"", '--data-dir', "`"$data`"") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$second = Invoke-Rmw @('apply', $rice)
$first.WaitForExit(60000) | Out-Null
Check 'busy:second-refused' ($second.code -eq 3) "exit=$($second.code) $($second.output)"
Check 'busy:first-finished' ($first.ExitCode -eq 0) "exit=$($first.ExitCode)"
$r = Invoke-Rmw @('restore', '--original')
Check 'busy:restore' ($r.code -eq 0) $r.output

# ---- 3. kill the core in the middle of an apply; the next start undoes it ----------------------
$beforeCrash = Read-State
Start-Core @('--test-pause-after', 'appearance:30000')
$victim = Start-Process -FilePath $rmw -ArgumentList @('apply', "`"$rice`"", '--data-dir', "`"$data`"") -PassThru -WindowStyle Hidden
$dark = $false
for ($i = 0; $i -lt 150 -and -not $dark; $i++) {
    Start-Sleep -Milliseconds 100
    $dark = (Read-State).AppsLight -eq 0
}
Check 'crash:appearance-applied-before-kill' $dark ''
Stop-Process -Name rmw-core -Force -ErrorAction SilentlyContinue
$victim.WaitForExit(30000) | Out-Null
Start-Sleep -Seconds 1
$r = Invoke-Rmw @('status') # starts a fresh core, which undoes the interrupted apply first
Check 'crash:status-after-restart' ($r.code -eq 0 -and $r.output -notmatch 'WARNING') $r.output
Start-Sleep -Seconds 2
$afterCrash = Read-State
$diff = Compare-State $beforeCrash $afterCrash
Check 'crash:undone' ($diff.Count -eq 0) ($diff -join '; ')

Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }

# ---- report -----------------------------------------------------------------------------------------
$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'system.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
