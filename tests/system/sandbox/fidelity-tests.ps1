<#
    v1.9 fidelity suite: a Windows the person had already made their own, and nothing of it lost.

    The owner's fourth test: "restore goes back to Windows' default settings; that must be the last
    resort, or we break a system someone had already customized", "applying often bugs out", "the app
    breaks the PC". Every other suite starts from a Sandbox at Windows' defaults, where falling back to
    the defaults cannot be told from restoring. Here the Sandbox is first made someone's own (their theme
    file, their picture, dark mode and their accent, no sounds, black pointers at 64 px, a screen saver,
    desktop icons, a taskbar on the left), and every path must leave all of it as it was.

    Run ONLY inside Windows Sandbox (scripts\sandbox.ps1 -Suite fidelity).
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
function Step([string] $Name, [scriptblock] $Body) {
    try { & $Body }
    catch { Check "fidelity:$Name-ran" $false "$($_.Exception.Message) at $($_.InvocationInfo.ScriptLineNumber)" }
}

$rmw = Join-Path $Bin 'rmw.exe'
$data = Join-Path $Work 'data'
$riceRoot = Join-Path $Bin 'extra-rices'

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Rmw -Name Spi -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool SystemParametersInfoW(uint action, uint uiParam, string pvParam, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, EntryPoint = "SystemParametersInfoW")]
public static extern bool GetString(uint action, uint uiParam, System.Text.StringBuilder pvParam, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SystemParametersInfoW")]
public static extern bool GetUInt(uint action, uint uiParam, ref uint pvParam, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SystemParametersInfoW")]
public static extern bool SetUInt(uint action, uint uiParam, System.IntPtr pvParam, uint winIni);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr SendMessageTimeoutW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, string lParam, uint flags, uint timeout, out System.IntPtr result);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string cls, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int GetSystemMetrics(int index);
'@

# ---- running the app -----------------------------------------------------------------------------------------
function Run([string[]] $Arguments, [int] $Seconds = 180) {
    $stdout = Join-Path $env:TEMP "fidelity-$([guid]::NewGuid()).out"
    $quoted = @(($Arguments + @('--data-dir', $data)) | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
    $p = Start-Process $rmw -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError "$stdout.err"
    $null = $p.Handle
    $done = $p.WaitForExit($Seconds * 1000)
    if (-not $done) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $text = ((Get-Content $stdout -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) + (Get-Content "$stdout.err" -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)).Trim()
    Remove-Item $stdout, "$stdout.err" -Force -ErrorAction SilentlyContinue
    [pscustomobject]@{ code = $(if ($done) { $p.ExitCode } else { -1 }); output = $text }
}
function Apply([string] $Rice) { Run @('apply', $Rice, '--wait') }
function Restore-Original { Run @('restore', '--original', '--wait') }
function Stop-Core {
    [void](Run @('shutdown', '--wait') 60)
    for ($i = 0; $i -lt 50 -and (Get-Process rmw-core -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 200 }
}
function Native-Taskbar { [Rmw.Spi]::IsWindowVisible([Rmw.Spi]::FindWindowW('Shell_TrayWnd', [NullString]::Value)) }
function Broadcast([string] $Area) {
    $result = [IntPtr]::Zero
    [void][Rmw.Spi]::SendMessageTimeoutW([IntPtr]0xffff, 0x1A, [IntPtr]::Zero, $Area, 2, 3000, [ref]$result)
}

# ---- what the person sees, and what Windows keeps of it --------------------------------------------------------
function Wallpaper-Now { $b = New-Object System.Text.StringBuilder 520; [void][Rmw.Spi]::GetString(0x73, 520, $b, 0); $b.ToString() } # SPI_GETDESKWALLPAPER
function Cursor-Size-Now { $v = [uint32]0; [void][Rmw.Spi]::GetUInt(0x2028, 0, [ref]$v, 0); $v } # SPI_GETCURSORBASESIZE
function Value([string] $Key, [string] $Name) { (Get-ItemProperty "Registry::$Key" -ErrorAction SilentlyContinue).$Name }
$personalize = 'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$schemes = 'HKEY_CURRENT_USER\AppEvents\Schemes'
function Look-Now {
    [ordered]@{
        Wallpaper    = Wallpaper-Now
        Style        = Value 'HKEY_CURRENT_USER\Control Panel\Desktop' 'WallpaperStyle'
        AppsLight    = Value $personalize 'AppsUseLightTheme'
        SystemLight  = Value $personalize 'SystemUsesLightTheme'
        Transparency = Value $personalize 'EnableTransparency'
        Prevalence   = Value $personalize 'ColorPrevalence'
        Accent       = '{0:X6}' -f ((Value 'HKEY_CURRENT_USER\Software\Microsoft\Windows\DWM' 'ColorizationColor') -band 0xFFFFFF)
        Theme        = Value 'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes' 'CurrentTheme'
        SoundScheme  = Value $schemes '(default)'
        Asterisk     = Value "$schemes\Apps\.Default\SystemAsterisk\.Current" '(default)'
        Notify       = Value "$schemes\Apps\.Default\Notification.Default\.Current" '(default)'
        Arrow        = Value 'HKEY_CURRENT_USER\Control Panel\Cursors' 'Arrow'
        PointerSet   = Value 'HKEY_CURRENT_USER\Control Panel\Cursors' '(default)'
        PointerSize  = Cursor-Size-Now
        ScreenSaver  = Value 'HKEY_CURRENT_USER\Control Panel\Desktop' 'SCRNSAVE.EXE'
        SaverWait    = Value 'HKEY_CURRENT_USER\Control Panel\Desktop' 'ScreenSaveTimeOut'
        ThisPcIcon   = Value 'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel' '{20D04FE0-3AEA-1069-A2D8-08002B30309D}'
        TaskbarLeft  = Value 'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' 'TaskbarAl'
    }
}
function Differences($Before, $After, [string[]] $Only = @()) {
    @($Before.Keys | Where-Object { $Only.Count -eq 0 -or $Only -contains $_ } |
        Where-Object { "$($Before[$_])" -ne "$($After[$_])" } |
        ForEach-Object { "$_ '$($Before[$_])' -> '$($After[$_])'" })
}
# Everything Windows keeps for the look, as reg.exe prints it (values Windows changes on its own left out).
$residueKeys = @(
    'HKCU\Control Panel\Cursors', 'HKCU\Control Panel\Desktop', 'HKCU\Control Panel\Colors',
    'HKCU\Software\Microsoft\Accessibility', 'HKCU\AppEvents\Schemes',
    'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes',
    'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent', 'HKCU\Software\Microsoft\Windows\DWM',
    'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced',
    'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons',
    'HKCU\Software\Microsoft\Windows\CurrentVersion\Search')
$volatile = 'InstalledWin32AppsRevision|TranscodedImageCache|TranscodedImageCount|LastUpdated|TaskbarStateLastRun|Pre-High Contrast Scheme|ImageColor|LastWriteTime|ColorizationGlassAttribute|\\History\\Colors|ColorHistory|ThemeChangesDesktopIcons|ThemeChangesMousePointers'
function Registry-Snapshot {
    foreach ($key in $residueKeys) {
        $lines = & reg.exe query $key /s 2>$null
        if ($LASTEXITCODE -eq 0) {
            $section = $key
            foreach ($line in $lines) {
                if ($line -match '^HKEY_') { $section = $line.Trim(); continue }
                if ($line.Trim() -and $line -notmatch $volatile) { "$section :: $($line.Trim())" }
            }
        } else { "[$key absent]" }
    }
}
function Residue($Before, $After) { @(Compare-Object @($Before) @($After) | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) }
function Short($Lines) { (($Lines | Select-Object -First 12) -join ' | ') + $(if (@($Lines).Count -gt 12) { " (+$(@($Lines).Count - 12) more)" }) }

# ---- the person's own Windows ----------------------------------------------------------------------------------
$pictures = [Environment]::GetFolderPath('MyPictures')
New-Item -ItemType Directory -Force $pictures | Out-Null
function New-Picture([string] $Path, [System.Drawing.Color] $From, [System.Drawing.Color] $To) {
    $bitmap = New-Object System.Drawing.Bitmap 1600, 900
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point 1600, 900), $From, $To)
    $graphics.FillRectangle($brush, 0, 0, 1600, 900)
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose(); $brush.Dispose()
}
$mine = Join-Path $pictures 'Minha foto.png'
New-Picture $mine ([System.Drawing.Color]::DarkOrange) ([System.Drawing.Color]::Indigo)

# Their pointers: Windows Black (from Windows' own list), at 64 px.
$pointerNames = 'Arrow', 'Help', 'AppStarting', 'Wait', 'Crosshair', 'IBeam', 'NWPen', 'No', 'SizeNS', 'SizeWE', 'SizeNWSE', 'SizeNESW', 'SizeAll', 'UpArrow', 'Hand', 'Pin', 'Person'
$black = "$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Control Panel\Cursors\Schemes' -ErrorAction SilentlyContinue).'Windows Black')"
if (-not $black) { $black = "$((Get-ItemProperty 'HKCU:\Control Panel\Cursors\Schemes' -ErrorAction SilentlyContinue).'Windows Black')" }
$blackFiles = @($black -split ',')
Note 'pointer-scheme' "Windows Black: $($blackFiles.Count) files"

# Their theme file, as Settings writes one when you save a theme.
$themes = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Themes'
New-Item -ItemType Directory -Force $themes | Out-Null
$theirTheme = Join-Path $themes 'Meu tema.theme'
$cursorLines = for ($i = 0; $i -lt [Math]::Min($pointerNames.Count, $blackFiles.Count); $i++) { "$($pointerNames[$i])=$($blackFiles[$i])" }
@"
[Theme]
DisplayName=Meu tema

[Control Panel\Desktop]
Wallpaper=$mine
TileWallpaper=0
WallpaperStyle=10
Pattern=

[VisualStyles]
Path=%SystemRoot%\resources\themes\Aero\Aero.msstyles
ColorStyle=NormalColor
Size=NormalSize
AutoColorization=0
ColorizationColor=0XC4D2691E
SystemMode=Dark
AppMode=Dark
VisualStyleVersion=10

[MasterThemeSelector]
MTSM=RJSPBS

[Sounds]
SchemeName=

[Control Panel\Cursors]
DefaultValue=Windows Black
$($cursorLines -join "`r`n")

[boot]
SCRNSAVE.EXE=C:\Windows\system32\Bubbles.scr
"@ | Out-File $theirTheme -Encoding unicode

Step 'person' {
    # Double-clicking a .theme applies it (and opens Settings, closed after).
    Start-Process $theirTheme
    $named = $false
    for ($i = 0; $i -lt 60 -and -not $named; $i++) {
        Start-Sleep -Milliseconds 500
        $named = "$(Value 'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes' 'CurrentTheme')" -like '*Meu tema.theme'
    }
    Start-Sleep -Seconds 3
    Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
    Note 'theme-applied-by-windows' "named=$named"
    if (-not $named) {
        Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes' 'CurrentTheme' $theirTheme
    }

    # What a theme file does not carry, set the way Settings does.
    Set-ItemProperty 'HKCU:\Control Panel\Desktop' 'WallpaperStyle' '10'
    Set-ItemProperty 'HKCU:\Control Panel\Desktop' 'TileWallpaper' '0'
    [void][Rmw.Spi]::SystemParametersInfoW(0x14, 0, $mine, 3) # SPI_SETDESKWALLPAPER, saved and broadcast
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' 'AppsUseLightTheme' 0 -Type DWord
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' 'SystemUsesLightTheme' 0 -Type DWord
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' 'EnableTransparency' 0 -Type DWord
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' 'ColorPrevalence' 1 -Type DWord
    Broadcast 'ImmersiveColorSet'
    # No sounds: the scheme and every event's current sound.
    Set-ItemProperty 'HKCU:\AppEvents\Schemes' '(default)' '.None'
    Get-ChildItem 'HKCU:\AppEvents\Schemes\Apps' -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -eq '.Current' } |
        ForEach-Object { Set-ItemProperty $_.PSPath '(default)' '' }
    # Pointers at 64 px (Settings > Accessibility > Mouse pointer, size 3).
    Set-ItemProperty 'HKCU:\Control Panel\Cursors' 'CursorBaseSize' 64 -Type DWord
    New-Item -Force 'HKCU:\Software\Microsoft\Accessibility' | Out-Null
    Set-ItemProperty 'HKCU:\Software\Microsoft\Accessibility' 'CursorSize' 3 -Type DWord
    [void][Rmw.Spi]::SetUInt(0x57, 0, [IntPtr]::Zero, 3) # SPI_SETCURSORS
    [void][Rmw.Spi]::SetUInt(0x2029, 0, [IntPtr]64, 3) # SPI_SETCURSORBASESIZE
    # A screen saver, desktop icons, a taskbar on the left without search and task view.
    Set-ItemProperty 'HKCU:\Control Panel\Desktop' 'SCRNSAVE.EXE' 'C:\Windows\system32\Bubbles.scr'
    Set-ItemProperty 'HKCU:\Control Panel\Desktop' 'ScreenSaveActive' '1'
    Set-ItemProperty 'HKCU:\Control Panel\Desktop' 'ScreenSaveTimeOut' '600'
    $icons = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel'
    New-Item -Force $icons | Out-Null
    Set-ItemProperty $icons '{20D04FE0-3AEA-1069-A2D8-08002B30309D}' 0 -Type DWord
    Set-ItemProperty $icons '{59031a47-3f72-44a7-89c5-5595fe6b30ee}' 0 -Type DWord
    $advanced = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
    Set-ItemProperty $advanced 'TaskbarAl' 0 -Type DWord
    Set-ItemProperty $advanced 'ShowTaskViewButton' 0 -Type DWord
    New-Item -Force 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Search' | Out-Null
    Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Search' 'SearchboxTaskbarMode' 0 -Type DWord
    Broadcast 'TraySettings'
    Start-Sleep -Seconds 3
}
$person = Look-Now
$personRegistry = @(Registry-Snapshot)
Note 'person' (($person.Keys | ForEach-Object { "$_=$($person[$_])" }) -join '; ')

function Same-As-Person([string] $Name, [string] $Detail = '') {
    Start-Sleep -Seconds 2
    $look = Differences $person (Look-Now)
    $registry = Residue $personRegistry @(Registry-Snapshot)
    Check "fidelity:$Name" ($look.Count -eq 0 -and $registry.Count -eq 0) "$Detail look: $(Short $look); registry: $(Short $registry)"
}
# What a rice that sets only the look must never touch.
$notTheLook = 'Wallpaper', 'Style', 'SoundScheme', 'Asterisk', 'Notify', 'Arrow', 'PointerSet', 'PointerSize', 'ScreenSaver', 'SaverWait', 'ThisPcIcon', 'TaskbarLeft'

# A rice of colors only (light, a green accent): the theme layer and nothing else.
$colors = Join-Path $Work 'colors-only'
New-Item -ItemType Directory -Force $colors | Out-Null
[IO.File]::WriteAllText((Join-Path $colors 'rice.toml'), "schema = 1`n`n[meta]`nname = `"Colors only`"`n`n[appearance]`nmode = `"light`"`naccent = `"#2e9d5b`"`n")

function Colors-Only([string] $Name) {
    $r = Apply $colors
    $after = Look-Now
    $touched = Differences $person $after $notTheLook
    Check "fidelity:$Name-applies" ($r.code -eq 0 -and $after.AppsLight -eq 1 -and $after.Accent -eq '2E9D5B') "$($r.code): $($r.output); light=$($after.AppsLight) accent=$($after.Accent)"
    Check "fidelity:$Name-touches-only-the-look" ($touched.Count -eq 0) (Short $touched)
    $r = Restore-Original
    Same-As-Person "$Name-restores-all" "$($r.code): $(($r.output -split "`n")[0]);"
}

# ---- 1. a rice of colors only: nothing but the look changes, and all of it comes back --------------------------------
Step 'colors' { Colors-Only 'colors-only' }

# ---- 2. the same through Windows' legacy theme manager (the path a theme that does not show takes) ---------------------
Step 'legacy' {
    Stop-Core
    $env:RMW_TEST_THEME_LEGACY = '1'
    try { Colors-Only 'legacy' } finally { Stop-Core; Remove-Item Env:\RMW_TEST_THEME_LEGACY -ErrorAction SilentlyContinue }
}

# ---- 3. every official rice, one after the other, then the original ------------------------------------------------------
Step 'official' {
    $applied = @()
    foreach ($name in 'Sakura Dawn', 'Mocha Kawaii', 'Tokyo Midnight', 'Diva Stage', 'Your Rice') {
        $folder = Join-Path $riceRoot $name
        if (-not (Test-Path $folder)) { continue }
        $r = Apply $folder
        $applied += "$name=$($r.code)"
        Start-Sleep -Seconds 2
    }
    Note 'official-applied' ($applied -join ', ')
    Check 'fidelity:official-apply' (@($applied | Where-Object { $_ -notmatch '=0$' }).Count -eq 0) ($applied -join ', ')
    $r = Restore-Original
    Same-As-Person 'official-then-original' "$($r.code): $(($r.output -split "`n")[0]);"
    Check 'fidelity:official-taskbar-back' (Native-Taskbar) ''
}

# ---- 4. their theme file gone (deleted, moved, a cleanup): never Windows' defaults --------------------------------------
Step 'theme-gone' {
    $aside = "$theirTheme.aside"
    Move-Item $theirTheme $aside -Force
    try {
        Colors-Only 'theme-gone'
        Stop-Core
        $env:RMW_TEST_THEME_LEGACY = '1'
        try { Colors-Only 'theme-gone-legacy' } finally { Stop-Core; Remove-Item Env:\RMW_TEST_THEME_LEGACY -ErrorAction SilentlyContinue }
    }
    finally {
        Move-Item $aside $theirTheme -Force
    }
}

# ---- 5. closing the app with a rice on, opening it again: nothing changes by itself ------------------------------------------
Step 'close-open' {
    $r = Apply (Join-Path $riceRoot 'Sakura Dawn')
    Start-Sleep -Seconds 3
    $withRice = Look-Now
    Stop-Core
    Check 'fidelity:closed-taskbar-back' (Native-Taskbar) ''
    $afterClose = Look-Now
    Check 'fidelity:closed-rice-stays' ((Differences $withRice $afterClose).Count -eq 0) (Short (Differences $withRice $afterClose))
    [void](Run @('status'))
    Start-Sleep -Seconds 10
    $afterOpen = Look-Now
    Check 'fidelity:opened-nothing-changes' ((Differences $withRice $afterOpen).Count -eq 0) (Short (Differences $withRice $afterOpen))
    $r = Restore-Original
    Same-As-Person 'close-open-restores-all' "$($r.code): $(($r.output -split "`n")[0]);"
}

# ---- 6. the person changes something while a rice is on: the app leaves it alone ----------------------------------------------
Step 'person-changes' {
    $r = Apply (Join-Path $riceRoot 'Mocha Kawaii')
    Start-Sleep -Seconds 3
    $other = Join-Path $pictures 'Outra foto.png'
    New-Picture $other ([System.Drawing.Color]::SeaGreen) ([System.Drawing.Color]::Black)
    [void][Rmw.Spi]::SystemParametersInfoW(0x14, 0, $other, 3)
    Start-Sleep -Seconds 75 # past the drift check
    Check 'fidelity:person-wallpaper-left-alone' ((Wallpaper-Now) -like '*Outra foto*') "wallpaper now: $(Wallpaper-Now)"
    # Their own later choice is theirs: a restore keeps it, and brings back all the rest.
    $r = Restore-Original
    Start-Sleep -Seconds 2
    $after = Look-Now
    $rest = Differences $person $after ($person.Keys | Where-Object { $_ -ne 'Wallpaper' })
    $registry = @(Residue $personRegistry @(Registry-Snapshot) | Where-Object { $_ -notmatch '\\Control Panel\\Desktop :: WallPaper' })
    Check 'fidelity:person-changes-kept-then-original' ($after.Wallpaper -like '*Outra foto*' -and $rest.Count -eq 0 -and $registry.Count -eq 0) "$($r.code): wallpaper $($after.Wallpaper); look: $(Short $rest); registry: $(Short $registry)"
    [void][Rmw.Spi]::SystemParametersInfoW(0x14, 0, $mine, 3) # the person puts their own picture back
    Start-Sleep -Seconds 2
}

# ---- 7. Windows' pointer size: what sizes Windows takes (the owner: "no size below 32") ---------------------------------------
Add-Type -Namespace Rmw -Name Cursor -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct POINT { public int X; public int Y; }
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct CURSORINFO { public int cbSize; public int flags; public System.IntPtr hCursor; public POINT ptScreenPos; }
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct ICONINFO { public bool fIcon; public int xHotspot; public int yHotspot; public System.IntPtr hbmMask; public System.IntPtr hbmColor; }
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct BITMAP { public int bmType; public int bmWidth; public int bmHeight; public int bmWidthBytes; public ushort bmPlanes; public ushort bmBitsPixel; public System.IntPtr bmBits; }
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO info);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool GetIconInfo(System.IntPtr icon, out ICONINFO info);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern int GetObjectW(System.IntPtr obj, int size, ref BITMAP bitmap);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern bool DeleteObject(System.IntPtr obj);
public static int ShownWidth() {
    CURSORINFO info = new CURSORINFO(); info.cbSize = System.Runtime.InteropServices.Marshal.SizeOf(typeof(CURSORINFO));
    if (!GetCursorInfo(ref info) || info.hCursor == System.IntPtr.Zero) return -1;
    ICONINFO icon; if (!GetIconInfo(info.hCursor, out icon)) return -2;
    BITMAP bitmap = new BITMAP();
    System.IntPtr shown = icon.hbmColor != System.IntPtr.Zero ? icon.hbmColor : icon.hbmMask;
    GetObjectW(shown, System.Runtime.InteropServices.Marshal.SizeOf(typeof(BITMAP)), ref bitmap);
    if (icon.hbmColor != System.IntPtr.Zero) DeleteObject(icon.hbmColor);
    if (icon.hbmMask != System.IntPtr.Zero) DeleteObject(icon.hbmMask);
    return bitmap.bmWidth;
}
'@
Step 'pointer-sizes' {
    [void][Rmw.Cursor]::SetCursorPos(400, 400) # over the desktop: the arrow
    $seen = foreach ($size in 16, 24, 32, 48) {
        [void][Rmw.Spi]::SetUInt(0x2029, 0, [IntPtr]$size, 0) # reloads the pointers at that size (in memory only)
        Start-Sleep -Milliseconds 500
        [void][Rmw.Cursor]::SetCursorPos(401, 401)
        Start-Sleep -Milliseconds 200
        "$size->$(Cursor-Size-Now), drawn $([Rmw.Cursor]::ShownWidth()) px"
    }
    [void][Rmw.Spi]::SetUInt(0x2029, 0, [IntPtr]64, 0)
    [void][Rmw.Spi]::SetUInt(0x57, 0, [IntPtr]::Zero, 0)
    Note 'pointer-sizes' ($seen -join '; ')
}

# ---- 8. the data folder lost while a rice was on (uninstalled without restoring, data removed), then the app again: --------
# the "original" it now takes is the rice's look; restoring it must never mean Windows' defaults.
Step 'data-lost' {
    $r = Apply (Join-Path $riceRoot 'Sakura Dawn')
    Start-Sleep -Seconds 3
    $sakura = Look-Now
    Stop-Core
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    $r = Apply (Join-Path $riceRoot 'Mocha Kawaii')
    $r = Restore-Original
    Start-Sleep -Seconds 2
    $after = Look-Now
    $defaults = @()
    if ("$($after.Theme)" -match '\\Resources\\Themes\\(aero|dark)\.theme') { $defaults += "theme $($after.Theme)" }
    if ("$($after.Wallpaper)" -match '\\Web\\Wallpaper\\Windows\\img0') { $defaults += "wallpaper $($after.Wallpaper)" }
    Check 'fidelity:data-lost-never-windows-defaults' ($defaults.Count -eq 0) ($defaults -join '; ')
    Note 'data-lost-vs-sakura' (Short (Differences $sakura $after))
}

# ---- the end ---------------------------------------------------------------------------------------------------------------
Stop-Core
Check 'fidelity:taskbar-at-the-end' (Native-Taskbar) ''
if (Test-Path (Join-Path $data 'logs')) {
    Remove-Item (Join-Path $Out 'fidelity-logs') -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'fidelity-logs') -Recurse -Force
}
$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'fidelity.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
