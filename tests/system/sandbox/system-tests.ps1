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

# The icons (M14): the user's Documents folder and a Start menu shortcut of the user's own.
$documents = [Environment]::GetFolderPath('MyDocuments')
$startPrograms = [Environment]::GetFolderPath('Programs')
$notepadLink = Join-Path $startPrograms 'Rmw Notepad.lnk'
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($notepadLink)
$link.TargetPath = 'C:\Windows\System32\notepad.exe'
$link.Save()
$clsid = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID'
# The lock screen (M14): the file Windows says its picture came from, and Spotlight's switches.
$null = [Windows.System.UserProfile.LockScreen, Windows.System.UserProfile, ContentType = WindowsRuntime]
$delivery = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager'
# The ProgID .bat files go to, as Windows finds it (what the icons module writes under; the Sandbox
# has no program for .txt).
Add-Type -Namespace Rmw -Name Assoc -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shlwapi.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int AssocQueryStringW(int flags, int str, string assoc, string extra, System.Text.StringBuilder outText, ref int outSize);
'@
$progIdText = New-Object System.Text.StringBuilder 256
$progIdSize = 256
[void][Rmw.Assoc]::AssocQueryStringW(0, 20, '.bat', $null, $progIdText, [ref]$progIdSize) # ASSOCSTR_PROGID
$typeProgId = $progIdText.ToString()
# What M14 writes, as reg.exe prints it: before the apply and after the restore, the same (no residue).
$watched = @('HKCU\AppEvents\Schemes', 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID', "HKCU\Software\Classes\$typeProgId")
function Registry-Text {
    ($watched | ForEach-Object {
        $lines = & reg.exe query $_ /s 2>$null
        if ($LASTEXITCODE -eq 0) { "[$_]"; $lines } else { "[$_ absent]" }
    }) -join "`n"
}

function Read-State {
    $personalize = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue
    $dwm = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\DWM' -ErrorAction SilentlyContinue
    $advanced = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -ErrorAction SilentlyContinue
    $search = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Search' -ErrorAction SilentlyContinue
    $cursors = Get-ItemProperty 'HKCU:\Control Panel\Cursors' -ErrorAction SilentlyContinue
    $sounds = 'HKCU:\AppEvents\Schemes'
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
        SoundScheme  = (Get-ItemProperty $sounds -ErrorAction SilentlyContinue).'(default)'
        Notify       = (Get-ItemProperty "$sounds\Apps\.Default\Notification.Default\.Current" -ErrorAction SilentlyContinue).'(default)'
        Asterisk     = (Get-ItemProperty "$sounds\Apps\.Default\SystemAsterisk\.Current" -ErrorAction SilentlyContinue).'(default)'
        RecycleBin   = (Get-ItemProperty "$sounds\Apps\Explorer\EmptyRecycleBin\.Current" -ErrorAction SilentlyContinue).'(default)'
        ThisPcIcon   = (Get-ItemProperty "$clsid\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\DefaultIcon" -ErrorAction SilentlyContinue).'(default)'
        BinIcon      = (Get-ItemProperty "$clsid\{645FF040-5081-101B-9F08-00AA002F954E}\DefaultIcon" -ErrorAction SilentlyContinue).empty
        BinIconNow   = (Get-ItemProperty "$clsid\{645FF040-5081-101B-9F08-00AA002F954E}\DefaultIcon" -ErrorAction SilentlyContinue).'(default)'
        DocsIcon     = (Get-Content (Join-Path $documents 'desktop.ini') -ErrorAction SilentlyContinue | Where-Object { $_ -like 'IconResource=*' }) -join '|'
        NotepadIcon  = $shell.CreateShortcut($notepadLink).IconLocation
        TypeIcon     = (Get-ItemProperty "HKCU:\Software\Classes\$typeProgId\DefaultIcon" -ErrorAction SilentlyContinue).'(default)'
        LockImage    = "$([Windows.System.UserProfile.LockScreen]::OriginalImageFile.LocalPath)"
        Spotlight    = (Get-ItemProperty $delivery -ErrorAction SilentlyContinue).RotatingLockScreenEnabled
        LockTips     = (Get-ItemProperty $delivery -ErrorAction SilentlyContinue).RotatingLockScreenOverlayEnabled
        LockTips2    = (Get-ItemProperty $delivery -ErrorAction SilentlyContinue).'SubscribedContent-338387Enabled'
    }
}

# A 16x16 32-bit .ico of one color.
function New-Ico([string] $Path, [byte] $R, [byte] $G, [byte] $B) {
    $stream = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter $stream
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]1)
    $w.Write([byte]16); $w.Write([byte]16); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32](40 + 1024 + 64)); $w.Write([uint32]22)
    $w.Write([uint32]40); $w.Write([int32]16); $w.Write([int32]32); $w.Write([uint16]1); $w.Write([uint16]32)
    foreach ($i in 1..6) { $w.Write([uint32]0) }
    foreach ($i in 1..256) { $w.Write($B); $w.Write($G); $w.Write($R); $w.Write([byte]255) }
    $w.Write((New-Object byte[] 64))
    $w.Flush()
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    [IO.File]::WriteAllBytes($Path, $stream.ToArray())
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
New-Item -ItemType Directory -Force (Join-Path $rice 'assets\sounds') | Out-Null
$wav = Get-ChildItem 'C:\Windows\Media' -Filter '*.wav' -ErrorAction SilentlyContinue | Sort-Object Length | Select-Object -First 1
Copy-Item $wav.FullName (Join-Path $rice 'assets\sounds\pop.wav')
New-Ico (Join-Path $rice 'assets\icons\violet.ico') 0x9a 0x6b 0xf7
$image = Get-ChildItem 'C:\Windows\Web' -Recurse -Include *.jpg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($image) {
    Copy-Item $image.FullName (Join-Path $rice 'assets\wall.jpg')
    Copy-Item $image.FullName (Join-Path $rice 'assets\lock.jpg')
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

[sounds]
scheme = "none"

[sounds.events]
notification = "assets/sounds/pop.wav"

[icons]
this-pc = "assets/icons/violet.ico"
recycle-bin = "assets/icons/violet.ico"

[icons.folders]
documents = "assets/icons/violet.ico"

[icons.apps]
"notepad.exe" = "assets/icons/violet.ico"

[icons.types]
bat = "assets/icons/violet.ico"

[lockscreen]
image = "assets/lock.jpg"
tips = false

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
    # The sections below start from what this leaves: a restore that timed out (Windows can be slow
    # to report a theme in a fresh sandbox) gets a second chance.
    foreach ($try in 1..3) {
        $diag = & $rmw restore --original --data-dir $diagData 2>&1 | Out-String
        $restoreCode = $LASTEXITCODE
        if ($restoreCode -eq 0) { break }
        Start-Sleep -Seconds 3
    }
    $results.Add([pscustomobject]@{ name = 'diag:restore-without-mitigations'; ok = $true; detail = "exit=$restoreCode tries=$try $($diag.Trim())" })
}
& $rmw shutdown --data-dir $diagData *> $null
Remove-Item Env:\RMW_DISABLE_MITIGATIONS
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $diagData 'logs')) { Copy-Item (Join-Path $diagData 'logs') (Join-Path $Out 'logs-diag') -Recurse -Force }

# ---- 1. validate, apply, restore ----------------------------------------------------------------
$r = Invoke-Rmw @('validate', $rice)
Check 'validate' ($r.code -eq 0) $r.output

$before = Read-State
$registryBefore = Registry-Text
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
Check 'apply:sounds' ("$($after.Notify)" -like '*\store\*.wav' -and "$($after.Asterisk)" -eq '' -and "$($after.RecycleBin)" -eq '' -and $after.SoundScheme -eq '.Current') "notify=$($after.Notify) asterisk='$($after.Asterisk)' scheme=$($after.SoundScheme)"
Check 'apply:icons' ("$($after.ThisPcIcon)" -like '*\store\*.ico,0' -and "$($after.BinIcon)" -like '*\store\*.ico,0' -and "$($after.BinIconNow)" -like '*\store\*.ico,0' -and "$($after.DocsIcon)" -like 'IconResource=*\store\*.ico,0' -and "$($after.NotepadIcon)" -like '*\store\*.ico,0') "thisPc=$($after.ThisPcIcon) bin=$($after.BinIcon) now=$($after.BinIconNow) docs=$($after.DocsIcon) notepad=$($after.NotepadIcon)"
Check 'apply:file-type-icon' ("$($after.TypeIcon)" -like '*\store\*.ico,0' -and -not (Test-Path 'HKCU:\Software\Classes\Unknown\DefaultIcon')) "progId=$typeProgId icon=$($after.TypeIcon)"
Check 'apply:lockscreen' ("$($after.LockImage)" -like '*\store\*.jpg' -and "$($after.Spotlight)" -eq '0' -and "$($after.LockTips)" -eq '0' -and "$($after.LockTips2)" -eq '0') "image=$($after.LockImage) spotlight=$($after.Spotlight) tips=$($after.LockTips)/$($after.LockTips2) (before: $($before.LockImage) $($before.Spotlight) $($before.LockTips))"
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
$registryAfter = Registry-Text
$residue = @(Compare-Object ($registryBefore -split "`n") ($registryAfter -split "`n") | ForEach-Object { "$($_.SideIndicator) $($_.InputObject.Trim())" })
Check 'restore:no-registry-residue' ($residue.Count -eq 0) (($residue | Select-Object -First 12) -join ' | ')
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

# ==== M2: resident =================================================================================
Add-Type -Namespace Rmw -Name Input -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern bool SystemParametersInfoW(uint action, uint uiParam, string pvParam, uint winIni);
'@
$config = Join-Path $data 'config'
$settings = Join-Path $config 'settings.toml'
$hotkeySettings = @'
[[hotkey]]
keys = "F13"
action = "appearance.toggle"
'@

function Wait-Until([scriptblock] $Condition, [int] $Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return [bool](& $Condition)
}

function Stop-Core {
    Invoke-Rmw @('shutdown') | Out-Null
    for ($i = 0; $i -lt 100 -and (Get-Process rmw-core -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
}

function Get-Managed([string] $Module) {
    $r = Invoke-Rmw @('inspect', $Module)
    if ($r.code -ne 0) { return $null }
    return @(($r.output | ConvertFrom-Json).managed)
}

# ---- 4. a global hotkey runs its action; settings.toml is picked up while the core runs ----------
Remove-Item $settings -ErrorAction SilentlyContinue
$r = Invoke-Rmw @('status') # nothing to do: it stays in the tray all the same (the default since v1.1)
Check 'resident:tray-by-default' ($r.output -match 'Resident \(tray icon\)') $r.output
# Shown next to the clock, not behind the "^" (Windows 11 keeps that per icon).
$promoted = Wait-Until {
    @(Get-ChildItem 'HKCU:\Control Panel\NotifyIconSettings' -ErrorAction SilentlyContinue | Where-Object {
        $v = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        $v.ExecutablePath -like '*rmw-core.exe' -and $v.IsPromoted -eq 1 }).Count -gt 0
} 20
Check 'resident:tray-icon-promoted' $promoted ''
New-Item -ItemType Directory -Force $config | Out-Null
"[core]`nkeep_in_tray = false`n" | Out-File $settings -Encoding ascii
$quiet = Wait-Until { (Invoke-Rmw @('status')).output -match 'Not resident' } 10
Check 'resident:not-resident-when-asked' $quiet (Invoke-Rmw @('status')).output
$hotkeySettings | Out-File $settings -Encoding ascii
$picked = Wait-Until { $s = Invoke-Rmw @('status'); $s.output -match 'F13 -> appearance.toggle' -and $s.output -notmatch 'not active' } 10
$status = (Invoke-Rmw @('status')).output
Check 'hotkey:settings-picked-up' $picked $status
Check 'hotkey:resident' ($status -match 'Resident \(tray icon\)') $status
$historyBefore = @((Invoke-Rmw @('history')).output -split "`n" | Where-Object { $_ -match '^#' }).Count
$lightBefore = (Read-State).AppsLight
[Rmw.Input]::keybd_event(0x7C, 0, 0, [UIntPtr]::Zero) # VK_F13 down
[Rmw.Input]::keybd_event(0x7C, 0, 2, [UIntPtr]::Zero) # up
$flipped = Wait-Until { "$((Read-State).AppsLight)" -ne "$lightBefore" } 15
Check 'hotkey:toggled-appearance' $flipped "before=$lightBefore after=$((Read-State).AppsLight)"
Start-Sleep -Seconds 1
$historyAfter = @((Invoke-Rmw @('history')).output -split "`n" | Where-Object { $_ -match '^#' }).Count
Check 'hotkey:recorded-in-history' ($historyAfter -eq $historyBefore + 1) "before=$historyBefore after=$historyAfter"
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'hotkey:restore' ($r.code -eq 0) $r.output

# ---- 5. dark mode switches by itself on schedule -------------------------------------------------
while ((Get-Date).Second -gt 40) { Start-Sleep -Seconds 1 } # not right before a minute turns
$now = Get-Date
$lightAt = $now.AddMinutes(-2).ToString('HH:mm')
$darkAt = $now.AddMinutes(1).ToString('HH:mm')
$scheduled = Join-Path $Work 'scheduled'
New-Item -ItemType Directory -Force $scheduled | Out-Null
@"
schema = 1

[meta]
name = "Scheduled"

[appearance]
mode = "schedule"

[appearance.schedule]
light_at = "$lightAt"
dark_at = "$darkAt"
"@ | Out-File (Join-Path $scheduled 'rice.toml') -Encoding ascii
$r = Invoke-Rmw @('apply', $scheduled, '--wait')
Check 'schedule:apply' ($r.code -eq 0) $r.output
$state = Read-State
Check 'schedule:light-now' ($state.AppsLight -eq 1 -and $state.SystemLight -eq 1) "apps=$($state.AppsLight) system=$($state.SystemLight) light=$lightAt dark=$darkAt"
$status = (Invoke-Rmw @('status')).output
Check 'schedule:in-status' ($status -match "light at $lightAt, dark at $darkAt") $status
$historyBefore = @((Invoke-Rmw @('history')).output -split "`n" | Where-Object { $_ -match '^#' }).Count
$started = Get-Date
$dark = Wait-Until { $s = Read-State; $s.AppsLight -eq 0 -and $s.SystemLight -eq 0 } 100
Check 'schedule:dark-on-time' $dark ("switched {0:N0} s after the apply (dark at {1})" -f ((Get-Date) - $started).TotalSeconds, $darkAt)
$historyAfter = @((Invoke-Rmw @('history')).output -split "`n" | Where-Object { $_ -match '^#' }).Count
Check 'schedule:no-history-entry' ($historyAfter -eq $historyBefore) "before=$historyBefore after=$historyAfter"
$r = Invoke-Rmw @('restore', '--original', '--wait')
$status = (Invoke-Rmw @('status')).output
Check 'schedule:restore-ends-it' ($r.code -eq 0 -and $status -notmatch 'Appearance schedule') $status

# ---- 6. changing the wallpaper in Windows detaches that field, without a fight -------------------
$r = Invoke-Rmw @('apply', $rice, '--wait')
Check 'drift:apply' ($r.code -eq 0) $r.output
Start-Sleep -Seconds 6 # past the moment where changes are our own
$managed = Get-Managed 'wallpaper'
Check 'drift:wallpaper-managed-after-apply' (@($managed | Where-Object { $_ -like 'monitor:*' }).Count -gt 0) ($managed -join ', ')
$outside = Join-Path $Work 'outside.bmp'
[byte[]] $green = 0x42,0x4D,70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,2,0,0,0,2,0,0,0,1,0,24,0,0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,255,0,0,0,0,255,0,0,255,0,0,0
[IO.File]::WriteAllBytes($outside, $green)
$set = [Rmw.Input]::SystemParametersInfoW(0x14, 0, $outside, 3) # SPI_SETDESKWALLPAPER, update + broadcast
$detached = Wait-Until { $m = Get-Managed 'wallpaper'; $null -ne $m -and @($m | Where-Object { $_ -like 'monitor:*' }).Count -eq 0 } 20
Check 'drift:wallpaper-detached' $detached "set=$set managed=$((Get-Managed 'wallpaper') -join ', ')"
Start-Sleep -Seconds 3
$shownNow = (Invoke-Rmw @('inspect', 'wallpaper')).output
Check 'drift:user-wallpaper-stays' ($shownNow -notmatch '\\\\store\\\\') $shownNow
$cursorsManaged = Get-Managed 'cursors'
Check 'drift:other-modules-still-managed' (@($cursorsManaged).Count -gt 0) ($cursorsManaged -join ', ')
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'drift:restore' ($r.code -eq 0) $r.output

# ---- 7. start with Windows only when asked ------------------------------------------------------
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$r = Invoke-Rmw @('autostart', 'on')
$entry = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).RicingManagerWindows
Check 'autostart:on' ($r.code -eq 0 -and "$entry" -like '*rmw-core.exe*--startup') "$($r.output) entry=$entry"
$r = Invoke-Rmw @('autostart', 'status')
Check 'autostart:status' ($r.output -match 'starts with Windows') $r.output
$r = Invoke-Rmw @('autostart', 'off')
$entry = (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).RicingManagerWindows
Check 'autostart:off' ($r.code -eq 0 -and $null -eq $entry) "$($r.output) entry=$entry"

# ---- 8. --startup stays only when something needs it -------------------------------------------
Stop-Core
"[core]`nkeep_in_tray = false`n" | Out-File $settings -Encoding ascii
$p = Start-Process -FilePath $core -ArgumentList @('--data-dir', "`"$data`"", '--startup') -PassThru -WindowStyle Hidden
$exited = $p.WaitForExit(15000)
Check 'startup:exits-when-not-needed' ($exited -and $p.ExitCode -eq 0) "exited=$exited code=$($p.ExitCode)"
$hotkeySettings | Out-File $settings -Encoding ascii
$p = Start-Process -FilePath $core -ArgumentList @('--data-dir', "`"$data`"", '--startup') -PassThru -WindowStyle Hidden
$stayed = -not $p.WaitForExit(4000)
$status = (Invoke-Rmw @('status')).output
Check 'startup:stays-when-needed' ($stayed -and $status -match 'Resident') $status
Stop-Core

# ---- 9. three starts that crash open safe mode ---------------------------------------------------
foreach ($i in 1..3) {
    $p = Start-Process -FilePath $core -ArgumentList @('--data-dir', "`"$data`"", '--startup', '--test-exit-unclean') -PassThru -WindowStyle Hidden
    [void]$p.WaitForExit(20000)
}
$attempts = Get-Content (Join-Path $data 'startup-attempts') -ErrorAction SilentlyContinue
Check 'safe-mode:crashes-counted' ("$attempts" -eq '3') "startup-attempts=$attempts"
$p = Start-Process -FilePath $core -ArgumentList @('--data-dir', "`"$data`"", '--startup') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$status = (Invoke-Rmw @('status')).output
Check 'safe-mode:opens-after-3-crashes' ($status -match 'SAFE MODE \(the last starts crashed\)') $status
Check 'safe-mode:no-hotkeys' ($status -match 'not active') $status
Stop-Core
Check 'safe-mode:clean-exit-resets' (-not (Test-Path (Join-Path $data 'startup-attempts'))) ''
$status = (Invoke-Rmw @('status')).output
Check 'safe-mode:next-start-is-normal' ($status -notmatch 'SAFE MODE') $status

# ---- 10. Explorer restarts: what is ours is reapplied (diagnostic) -------------------------------
$r = Invoke-Rmw @('apply', $rice, '--wait')
Start-Sleep -Seconds 5
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
if (-not (Wait-Until { Get-Process explorer -ErrorAction SilentlyContinue } 20)) { Start-Process explorer.exe }
Start-Sleep -Seconds 10
$coreLog = Get-Content (Join-Path $data 'logs\core.log') -ErrorAction SilentlyContinue | Out-String
$results.Add([pscustomobject]@{ name = 'diag:explorer-restart-seen'; ok = $true; detail = "seen=$($coreLog -match 'Explorer restarted')" })
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'explorer:restore' ($r.code -eq 0) $r.output

Remove-Item $settings -ErrorAction SilentlyContinue
Stop-Core
Start-Sleep -Seconds 1
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }

# ---- report -----------------------------------------------------------------------------------------
$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'system.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
