<#
    M6 system tests (video wallpapers). Run ONLY inside Windows Sandbox (run-in-sandbox.ps1
    -Suite video): it plays a video behind the desktop icons and restarts Explorer.

    Acceptance of M6 (docs/04): the video shows behind the icons; the budget of docs/01 (here: the
    Sandbox's GPU-PV; the user's PC is checked in the final real test); a fullscreen app -> the
    renderer pauses (0% CPU); killing rmw-wallpaper -> back in < 3 s; a hung window thread is found
    in < 2 s; Explorer restarting -> the video comes back by itself; restore -> gone.
    The test video is made here, with Windows' own H.264 encoder (rmw_video_test_tool).
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
# A mistake in this script is a failed check, not the end of the run (the logs still get copied).
trap {
    $results.Add([pscustomobject]@{ name = 'script:error'; ok = $false; detail = ($_ | Out-String).Trim() })
    continue
}

$rmw = Join-Path $Bin 'rmw.exe'
$tool = Join-Path $Bin 'rmw_video_test_tool.exe'
$data = Join-Path $Work 'data'
$videoLog = Join-Path $data 'logs\wallpaper.log'
$coreLog = Join-Path $data 'logs\core.log'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Rmw -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetDC(System.IntPtr window);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern int ReleaseDC(System.IntPtr window, System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("gdi32.dll")]
public static extern uint GetPixel(System.IntPtr dc, int x, int y);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetCursorPos(int x, int y);
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
function Video-Process { Get-Process rmw-wallpaper -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Log-Text([string] $File) { if (Test-Path $File) { Get-Content $File -Raw -ErrorAction SilentlyContinue } else { '' } }
function Log-Count([string] $File, [string] $Pattern) { ([regex]::Matches((Log-Text $File), $Pattern)).Count }
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
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
# Inside the video's magenta block (bottom-right), above the taskbar.
$probeX = $screen.Left + [int]($screen.Width * 0.92)
$probeY = $screen.Top + [int]($screen.Height * 0.80)
function Probe {
    $dc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    $color = [Rmw.Win]::GetPixel($dc, $probeX, $probeY)
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $dc)
    [pscustomobject]@{ r = $color -band 0xFF; g = ($color -shr 8) -band 0xFF; b = ($color -shr 16) -band 0xFF }
}
function Is-Magenta { $c = Probe; $c.r -gt 190 -and $c.g -lt 80 -and $c.b -gt 190 }
function Probe-Text { $c = Probe; '#{0:X2}{1:X2}{2:X2}' -f $c.r, $c.g, $c.b }
# CPU of a process over a while, in % of the whole machine.
function Cpu-Percent($Process, [double] $Seconds) {
    $Process.Refresh(); $before = $Process.TotalProcessorTime; $start = Get-Date
    Start-Sleep -Seconds $Seconds
    $Process.Refresh(); $after = $Process.TotalProcessorTime
    100.0 * ($after - $before).TotalSeconds / ((Get-Date) - $start).TotalSeconds / [Environment]::ProcessorCount
}

for ($i = 0; $i -lt 30 -and -not (Get-Process explorer -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }

# ---- the rice: a test video on every monitor, a still image under it --------------------------------
$rice = Join-Path $Work 'rice-video'
$assets = Join-Path $rice 'assets'
New-Item -ItemType Directory -Force $assets | Out-Null
$encoded = & $tool encode (Join-Path $assets 'loop.mp4') 20 2>&1 | Out-String
Check 'video:test-video-encoded' (Test-Path (Join-Path $assets 'loop.mp4')) $encoded.Trim()
$still = New-Object System.Drawing.Bitmap 64, 64
$g = [System.Drawing.Graphics]::FromImage($still); $g.Clear([System.Drawing.Color]::FromArgb(40, 60, 90)); $g.Dispose()
$still.Save((Join-Path $assets 'still.png'), [System.Drawing.Imaging.ImageFormat]::Png); $still.Dispose()
@'
schema = 1

[meta]
name = "Video test"

[wallpaper]
mode = "per-monitor"

[wallpaper.policy]
pause_on_fullscreen = true
pause_when_covered  = true
fps_limit           = 30

[[wallpaper.monitor]]
match          = "all"
kind           = "video"
source         = "assets/loop.mp4"
fallback_image = "assets/still.png"
fit            = "fill"
'@ | Out-File (Join-Path $rice 'rice.toml') -Encoding ascii

$before = Probe-Text
$r = Invoke-Rmw @('apply', $rice, '--wait')
Check 'video:apply' ($r.code -eq 0) $r.output
Check 'video:worker-running' (Wait-Until { $null -ne (Video-Process) } 15) ''
# The core keeps its own copy of the video, named by its hash.
$playing = Wait-Until { (Log-Text $videoLog) -match 'wallpaper: \w+\.mp4 on ' } 15
Check 'video:playing' $playing ((Log-Text $videoLog) -split "`n" | Select-Object -Last 5 | Out-String)
$visible = Wait-Until { Is-Magenta } 10
Check 'video:visible-behind-the-icons' $visible "before $before, now $(Probe-Text)"
Shot 'video-1-playing'
$s = Invoke-Rmw @('status')
Check 'video:status' ($s.output -match 'Live wallpaper: running' -and $s.output -match ': playing: 1920x1080') $s.output

# ---- the budget (docs/01: ~100 MB and <= 3% CPU at 1080p; ADR-0006: stop at 150 MB / 5%) ------------
$process = Video-Process
Start-Sleep -Seconds 3
$cpu = Cpu-Percent $process 10
$process.Refresh()
$private = $process.PrivateMemorySize64 / 1MB
Note 'budget' ('cpu {0:N2}% private {1:N1} MB, {2}' -f $cpu, $private, (((Log-Text $videoLog) -split "`n" | Where-Object { $_ -match 'on .* \(' } | Select-Object -Last 1)))
Check 'video:budget-cpu' ($playing -and $cpu -le 3.0) ('{0:N2}%' -f $cpu)
Check 'video:budget-memory' ($playing -and $private -le 150) ('{0:N1} MB (target 90 MB)' -f $private)

# ---- a fullscreen app in front: paused, nothing spent ---------------------------------------------------
$full = Start-Process $tool 'fullscreen' -PassThru
$paused = Wait-Until { (Log-Text $videoLog) -match 'paused: a fullscreen app is in front' } 5
Check 'video:pauses-for-fullscreen' $paused ''
Start-Sleep -Seconds 1
$process = Video-Process
$pausedCpu = Cpu-Percent $process 5
Check 'video:paused-costs-nothing' ($paused -and $pausedCpu -lt 0.5) ('{0:N2}%' -f $pausedCpu)
Stop-Process -Id $full.Id -Force
$playingAgain = Wait-Until { (Log-Count $videoLog ': playing \(') -ge 1 } 5
Check 'video:plays-again' $playingAgain ''

# ---- a maximized window: the desktop is covered -----------------------------------------------------
$cover = Start-Process $tool 'cover' -PassThru
Check 'video:pauses-when-covered' (Wait-Until { (Log-Text $videoLog) -match 'paused: the desktop is covered' } 5) ''
Stop-Process -Id $cover.Id -Force
Check 'video:plays-after-cover' (Wait-Until { (Log-Count $videoLog ': playing \(') -ge 2 } 5) ''

# ---- paused for 2 minutes: the decoder and the swap chain are freed ------------------------------------
$cover = Start-Process $tool 'cover' -PassThru
$released = Wait-Until { (Log-Text $videoLog) -match 'memory released' } 135
Start-Sleep -Seconds 1
$process = Video-Process
$releasedMemory = if ($process) { $process.Refresh(); $process.PrivateMemorySize64 / 1MB } else { -1 }
Check 'video:released-after-2-min' ($released -and $process -and $releasedMemory -lt $private) ('{0:N1} MB playing, {1:N1} MB released; {2}' -f $private, $releasedMemory, (((Log-Text $videoLog) -split "`n" | Select-Object -Last 4) -join ' | '))
Stop-Process -Id $cover.Id -Force
Check 'video:plays-after-release' (Wait-Until { Is-Magenta } 8) (Probe-Text)

# Until here nothing ended it: a fullscreen app, a covered desktop and a release are not hangs.
Check 'video:never-restarted-so-far' ((Log-Count $coreLog 'worker wallpaper .*restarting') -eq 0) ((Log-Text $coreLog) -split "`n" | Where-Object { $_ -match 'worker wallpaper' } | Out-String)

# ---- killed: back in < 3 s ------------------------------------------------------------------------------
$old = Video-Process
$killedAt = Get-Date
Stop-Process -Id $old.Id -Force
$back = Wait-Until { $p = Video-Process; $p -and $p.Id -ne $old.Id } 5
$backIn = ((Get-Date) - $killedAt).TotalSeconds
Check 'video:back-after-kill' ($back -and $backIn -lt 3) ('{0:N1} s' -f $backIn)
Check 'video:visible-after-kill' (Wait-Until { Is-Magenta } 8) (Probe-Text)

# ---- a hung window thread: found in < 2 s ---------------------------------------------------------------
$hangsBefore = Log-Count $coreLog 'worker wallpaper stopped responding'
$old = Video-Process
New-Item -ItemType File -Force (Join-Path $data 'wallpaper\test-hang') | Out-Null
$hungAt = $null
[void](Wait-Until { (Log-Text $videoLog) -match 'TEST HOOK' } 5)
$hook = [regex]::Match((Log-Text $videoLog), '(\d{4}-\d\d-\d\dT[\d:.]+)Z WARN[^\n]*TEST HOOK')
$found = Wait-Until { (Log-Count $coreLog 'worker wallpaper stopped responding') -gt $hangsBefore } 8
$detected = [regex]::Matches((Log-Text $coreLog), '(\d{4}-\d\d-\d\dT[\d:.]+)Z WARN[^\n]*worker wallpaper stopped responding')
$detail = ''
$quick = $false
if ($hook.Success -and $detected.Count -gt 0) {
    $delay = ([datetime]$detected[$detected.Count - 1].Groups[1].Value - [datetime]$hook.Groups[1].Value).TotalSeconds
    $quick = $delay -lt 2.0 # docs/04: < 2 s (1.5 s of silence, plus the checks)
    $detail = '{0:N1} s' -f $delay
}
Check 'video:hang-detected' ($found -and $quick) $detail
Check 'video:back-after-hang' (Wait-Until { $p = Video-Process; $p -and $p.Id -ne $old.Id } 8) ''

# ---- Explorer restarts: behind the icons again ------------------------------------------------------------
$reattached = Log-Count $videoLog 'Explorer restarted|the desktop is back'
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe | Out-Null }
Check 'video:explorer-restart-seen' (Wait-Until { (Log-Count $videoLog 'Explorer restarted|the desktop is back') -gt $reattached } 20) ''
Check 'video:visible-after-explorer' (Wait-Until { Is-Magenta } 20) (Probe-Text)
Start-Sleep -Seconds 2
Shot 'video-2-after-explorer'

# ---- restore: no video, Windows' wallpaper as before ------------------------------------------------------
$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'video:restore' ($r.code -eq 0) $r.output
Check 'video:worker-gone' (Wait-Until { $null -eq (Video-Process) } 10) ''
Check 'video:not-visible-after-restore' (Wait-Until { -not (Is-Magenta) } 10) (Probe-Text)
Shot 'video-3-restored'

# ---- M12: pictures and colors drawn by rmw-wallpaper, with effects and particles ---------------------------
function Is-Gray($c) { [Math]::Abs($c.r - $c.g) -lt 12 -and [Math]::Abs($c.g - $c.b) -lt 12 }
# A hash of the whole screen: two of them differ when anything moved.
function Screen-Hash {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $target = $graphics.GetHdc()
    $screenDc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    [void][Rmw.Win]::BitBlt($target, 0, 0, $bounds.Width, $bounds.Height, $screenDc, $bounds.X, $bounds.Y, 0x40CC0020)
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $screenDc)
    $graphics.ReleaseHdc($target)
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $graphics.Dispose(); $bitmap.Dispose()
    $sha = [System.Security.Cryptography.SHA1]::Create()
    $hash = [BitConverter]::ToString($sha.ComputeHash($stream.ToArray()))
    $stream.Dispose(); $sha.Dispose()
    $hash
}
# 48 pixels along a row, a third of the way in: where the stripes are.
function Row-Signature([int] $Y) {
    $dc = [Rmw.Win]::GetDC([IntPtr]::Zero)
    $values = for ($i = 0; $i -lt 48; $i++) { [Rmw.Win]::GetPixel($dc, $screen.Left + [int]($screen.Width / 3) + $i * 5, $Y) }
    [void][Rmw.Win]::ReleaseDC([IntPtr]::Zero, $dc)
    $values -join ','
}
function Write-LiveRice([string] $Monitor) {
    @"
schema = 1

[meta]
name = "Live test"

[wallpaper]
mode = "per-monitor"

[wallpaper.policy]
pause_on_fullscreen = false
pause_when_covered  = false
fps_limit           = 30

$Monitor
"@ | Out-File (Join-Path $live 'rice.toml') -Encoding ascii
}

$live = Join-Path $Work 'rice-live'
$liveAssets = Join-Path $live 'assets'
New-Item -ItemType Directory -Force $liveAssets | Out-Null
# Red: Windows alone shows it red; drawn with saturation 0 it is gray.
$red = New-Object System.Drawing.Bitmap 320, 180
$g = [System.Drawing.Graphics]::FromImage($red); $g.Clear([System.Drawing.Color]::FromArgb(200, 40, 40)); $g.Dispose()
$red.Save((Join-Path $liveAssets 'red.png'), [System.Drawing.Imaging.ImageFormat]::Png); $red.Dispose()
# Stripes, for the parallax: 6 pixels black, 6 white.
$stripes = New-Object System.Drawing.Bitmap 1920, 1080
$g = [System.Drawing.Graphics]::FromImage($stripes); $g.Clear([System.Drawing.Color]::White)
for ($x = 0; $x -lt 1920; $x += 12) { $g.FillRectangle([System.Drawing.Brushes]::Black, $x, 0, 6, 1080) }
$g.Dispose()
$stripes.Save((Join-Path $liveAssets 'stripes.png'), [System.Drawing.Imaging.ImageFormat]::Png); $stripes.Dispose()

# A picture with its colors changed: drawn once, then nothing spent.
Write-LiveRice @'
[[wallpaper.monitor]]
match   = "all"
kind    = "image"
source  = "assets/red.png"
fit     = "fill"
effects = { saturation = 0 }
'@
$r = Invoke-Rmw @('apply', $live, '--wait')
Check 'live:apply-picture' ($r.code -eq 0) $r.output
Check 'live:worker-for-a-picture' (Wait-Until { $null -ne (Video-Process) } 15) ''
$grayShown = Wait-Until { $c = Probe; (Is-Gray $c) -and $c.r -gt 40 } 15
Check 'live:effects-visible' $grayShown "probe $(Probe-Text) (Windows alone shows #C82828)"
Shot 'video-live-1-gray-picture'
$s = Invoke-Rmw @('status')
Check 'live:status-picture' ($s.output -match ': playing: picture 320x180, effects') $s.output
Start-Sleep -Seconds 2
$p = Video-Process
$stillCpu = if ($p) { Cpu-Percent $p 4 } else { 100 }
Check 'live:still-picture-costs-nothing' ($grayShown -and $stillCpu -lt 0.5) ('{0:N2}%' -f $stillCpu)

# A color with a vignette and two particle layers: frames keep coming, within the budget.
Write-LiveRice @'
[[wallpaper.monitor]]
match     = "all"
kind      = "color"
color     = "#204060"
effects   = { vignette = 0.6 }
particles = [{ kind = "snow", count = 300 }, { kind = "sparkles", count = 60 }]
'@
$r = Invoke-Rmw @('apply', $live, '--wait')
Check 'live:apply-particles' ($r.code -eq 0) $r.output
$drawing = Wait-Until { (Invoke-Rmw @('status')).output -match ': playing: color #204060, effects, 2 particle layers' } 15
Check 'live:status-particles' $drawing (Invoke-Rmw @('status')).output
Start-Sleep -Seconds 2
$first = Screen-Hash
Start-Sleep -Milliseconds 500
$second = Screen-Hash
Check 'live:particles-move' ($drawing -and $first -ne $second) ''
Shot 'video-live-2-particles'
$p = Video-Process
$particlesCpu = if ($p) { Cpu-Percent $p 5 } else { 100 }
$p = Video-Process
$particlesMemory = if ($p) { $p.Refresh(); $p.PrivateMemorySize64 / 1MB } else { 0 }
Check 'live:particles-budget-cpu' ($drawing -and $particlesCpu -le 5.0) ('{0:N2}% (360 particles at 30 fps)' -f $particlesCpu)
Note 'particles-memory' ('{0:N1} MB' -f $particlesMemory)

# The parallax: the picture leans away from the mouse, then rests.
Write-LiveRice @'
[[wallpaper.monitor]]
match   = "all"
kind    = "image"
source  = "assets/stripes.png"
fit     = "fill"
effects = { parallax = 1 }
'@
$r = Invoke-Rmw @('apply', $live, '--wait')
Check 'live:apply-parallax' ($r.code -eq 0) $r.output
$row = $screen.Top + [int]($screen.Height * 0.4)
Wait-Until { (Invoke-Rmw @('status')).output -match ': playing: picture 1920x1080, effects' } 15 | Out-Null
[void][Rmw.Win]::SetCursorPos($screen.Left + 5, $row)
Start-Sleep -Seconds 3
$atLeft = Row-Signature $row
[void][Rmw.Win]::SetCursorPos($screen.Right - 5, $row)
Start-Sleep -Seconds 3
$atRight = Row-Signature $row
Check 'live:parallax-follows-the-mouse' ($atLeft -ne $atRight) ''
Shot 'video-live-3-parallax'
$p = Video-Process
$restingCpu = if ($p) { Cpu-Percent $p 4 } else { 100 }
Check 'live:parallax-rests' ($restingCpu -lt 0.5) ('{0:N2}% with the mouse still' -f $restingCpu)

$r = Invoke-Rmw @('restore', '--original', '--wait')
Check 'live:restore' ($r.code -eq 0) $r.output
Check 'live:worker-gone' (Wait-Until { $null -eq (Video-Process) } 10) ''

Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'video.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
