<#
    M8 system tests (packages). Run ONLY inside Windows Sandbox (run-in-sandbox.ps1 -Suite
    packages): it applies a rice.

    Acceptance (docs/06 section 4, docs/04 M8): a rice packed by `rmw pack` comes back whole through
    `rmw import` and applies; a plugin packed by `rmw plugin pack` installs from its .rmwp. Hostile
    packages - zip slip, alternate data streams, absolute names, a deflate bomb, a plugin script
    inside a rice, a renamed executable, something that is not a zip at all - are refused by the
    import worker (a process of its own), nothing lands outside the staging folder, nothing is
    left behind, and the core keeps running.
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
trap {
    $results.Add([pscustomobject]@{ name = 'script:error'; ok = $false; detail = ($_.Exception.ToString() + ' at ' + $_.InvocationInfo.PositionMessage) })
    continue
}

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem, System.Drawing
$rmw = Join-Path $Bin 'rmw.exe'
$data = Join-Path $Work 'data'
$files = Join-Path $Work 'packages'
New-Item -ItemType Directory -Force $files | Out-Null

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
$utf8 = New-Object Text.UTF8Encoding $false
function Bytes([string] $Text) { return $utf8.GetBytes($Text) }
function Png([int] $Width, [int] $Height) {
    $bitmap = New-Object System.Drawing.Bitmap $Width, $Height
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.Clear([System.Drawing.Color]::FromArgb(255, 40, 20, 60))
    $g.Dispose()
    $stream = New-Object IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return $stream.ToArray()
}
# A zip made with .NET, which writes any name it is given (like a hostile tool would).
function New-Zip([string] $Path, [object[]] $Entries) {
    if (Test-Path $Path) { Remove-Item $Path -Force }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew)
    $zip = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
    foreach ($entry in $Entries) {
        $level = if ($entry.store) { [IO.Compression.CompressionLevel]::NoCompression } else { [IO.Compression.CompressionLevel]::Optimal }
        $writer = $zip.CreateEntry($entry.name, $level).Open()
        if ($entry.bytes) { $writer.Write($entry.bytes, 0, $entry.bytes.Length) }
        if ($entry.zeros) {
            $block = New-Object byte[] (1MB)
            for ($i = 0; $i -lt $entry.zeros; $i++) { $writer.Write($block, 0, $block.Length) }
        }
        $writer.Dispose()
    }
    $zip.Dispose()
    $stream.Dispose()
}
$riceToml = "schema = 1`n`n[meta]`nname = `"Sakura Test`"`nauthor = `"Sandbox`"`n`n[appearance]`nmode = `"dark`"`n`n[[wallpaper.monitor]]`nmatch = `"all`"`nsource = `"assets/sakura.png`"`n"
[byte[]] $mp4Head = [byte[]]@(0, 0, 0, 24) + (Bytes 'ftypisom') + (New-Object byte[] 12)
function Rice-Count { @(Get-ChildItem (Join-Path $data 'rices') -Directory -ErrorAction SilentlyContinue).Count }
function Leftovers { @(Get-ChildItem (Join-Path $data 'imports') -Force -ErrorAction SilentlyContinue) }
# Where an escaping file would land: next to the data, the work folder, the drive's root, %TEMP%.
function Escaped {
    $places = @($Work, $data, (Join-Path $data 'rices'), 'C:\', 'C:\rmw', $env:TEMP, $env:USERPROFILE, (Join-Path $env:USERPROFILE 'Desktop'))
    @($places | ForEach-Object { Get-ChildItem $_ -Filter 'rmw-escape*' -Force -ErrorAction SilentlyContinue })
}

# ---- a rice: pack, import, apply ----------------------------------------------------------------
$source = Join-Path $files 'Sakura Test'
New-Item -ItemType Directory -Force (Join-Path $source 'assets') | Out-Null
[IO.File]::WriteAllText((Join-Path $source 'rice.toml'), $riceToml, $utf8)
[IO.File]::WriteAllBytes((Join-Path $source 'assets\sakura.png'), (Png 640 360))
[IO.File]::WriteAllText((Join-Path $source 'LICENSE'), "CC0 1.0`n", $utf8)
New-Item -ItemType Directory -Force (Join-Path $source '.git') | Out-Null
[IO.File]::WriteAllText((Join-Path $source '.git\config'), "[core]`n", $utf8)

$r = Invoke-Rmw @('pack', $source)
$riceFile = Join-Path $files 'Sakura Test.rice'
Check 'packages:pack-rice' ($r.code -eq 0 -and (Test-Path $riceFile)) $r.output
$listed = @()
if (Test-Path $riceFile) {
    $zip = [IO.Compression.ZipFile]::OpenRead($riceFile)
    $listed = @($zip.Entries | ForEach-Object { $_.FullName })
    $zip.Dispose()
}
Check 'packages:pack-leaves-editor-files-out' (($listed -join ',') -eq 'LICENSE,assets/sakura.png,rice.toml') ($listed -join ', ')

$before = Rice-Count
$started = Get-Date
$r = Invoke-Rmw @('import', $riceFile)
$importMs = [int]((Get-Date) - $started).TotalMilliseconds
$imported = Join-Path $data 'rices\Sakura Test'
Check 'packages:import-rice' ($r.code -eq 0 -and (Test-Path (Join-Path $imported 'assets\sakura.png'))) "$($r.output) ($importMs ms)"
$same = (Test-Path (Join-Path $imported 'rice.toml')) -and
        ((Get-FileHash (Join-Path $imported 'assets\sakura.png')).Hash -eq (Get-FileHash (Join-Path $source 'assets\sakura.png')).Hash)
Check 'packages:import-is-byte-identical' $same ''
Check 'packages:import-cleans-up' ((Leftovers).Count -eq 0) ((Leftovers | ForEach-Object Name) -join ', ')

$r = Invoke-Rmw @('import', $riceFile)
Check 'packages:import-twice-gets-a-new-folder' ($r.code -eq 0 -and (Test-Path (Join-Path $data 'rices\Sakura Test 2\rice.toml'))) $r.output

$r = Invoke-Rmw @('apply', $imported, '--wait')
Check 'packages:imported-rice-applies' ($r.code -eq 0) $r.output
$light = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -ErrorAction SilentlyContinue).AppsUseLightTheme
Check 'packages:imported-rice-is-dark' ($light -eq 0) "AppsUseLightTheme=$light"

# ---- the worker: a process of its own, gone when done --------------------------------------------
# A 300 MB video (stored) takes the worker a moment: long enough to see it.
$big = Join-Path $files 'big.rice'
New-Zip $big @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml.Replace('Sakura Test', 'Big Video')) },
               @{ name = 'assets/sakura.png'; bytes = (Png 64 64) },
               @{ name = 'assets/loop.mp4'; bytes = $mp4Head; zeros = 300; store = $true })
$job = Start-Job -ScriptBlock { param($rmw, $big, $data) & $rmw import $big --data-dir $data 2>&1 | Out-String } -ArgumentList $rmw, $big, $data
$worker = $null
$peak = 0
[void](Wait-Until {
    $found = Get-CimInstance Win32_Process -Filter "Name = 'rmw-core.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match '--import-worker' } | Select-Object -First 1
    if ($found) {
        $script:worker = $found
        $p = Get-Process -Id $found.ProcessId -ErrorAction SilentlyContinue
        if ($p) { $script:peak = [math]::Max($script:peak, $p.PeakPagedMemorySize64) }
    }
    $script:worker -and -not (Get-Process -Id $script:worker.ProcessId -ErrorAction SilentlyContinue)
} 120)
$output = Receive-Job $job -Wait
Remove-Job $job -Force
$parent = if ($worker) { Get-CimInstance Win32_Process -Filter "ProcessId = $($worker.ParentProcessId)" -ErrorAction SilentlyContinue }
Check 'packages:worker-is-its-own-process' ($worker -and $parent -and $parent.Name -eq 'rmw-core.exe' -and $parent.CommandLine -notmatch '--import-worker') ($(if ($worker) { $worker.CommandLine } else { 'not seen' }))
Check 'packages:worker-ends' ($worker -and -not (Get-Process -Id $worker.ProcessId -ErrorAction SilentlyContinue)) ''
Check 'packages:worker-memory-under-cap' ($peak -lt 384MB) ('{0:N1} MB committed at most' -f ($peak / 1MB))
Check 'packages:big-video-imported' (Test-Path (Join-Path $data 'rices\Big Video\assets\loop.mp4')) $output.Trim()

# ---- hostile packages ------------------------------------------------------------------------------
$hostile = [ordered]@{
    'zip-slip'       = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = '../../../../rmw-escape-slip.txt'; bytes = (Bytes 'x') })
    'backslashes'    = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = '..\..\..\rmw-escape-back.txt'; bytes = (Bytes 'x') })
    'absolute'       = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'C:/rmw-escape-abs.txt'; bytes = (Bytes 'x') })
    'stream'         = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'assets/sakura.png:rmw-escape-ads.txt'; bytes = (Bytes 'x') })
    'device-name'    = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'assets/CON.png'; bytes = (Png 8 8) })
    'short-name'     = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'PROGRA~1/rmw-escape.png'; bytes = (Png 8 8) })
    'bomb'           = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'assets/loop.mp4'; bytes = $mp4Head; zeros = 600 })
    'script-in-rice' = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'main.luau'; bytes = (Bytes 'return {}') })
    'renamed-exe'    = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'assets/sakura.png'; bytes = [byte[]](@(0x4D, 0x5A) + (New-Object byte[] 200)) })
    'huge-image'     = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'assets/sakura.png'; bytes = [byte[]](@(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52, 0, 0, 0xEA, 0x60, 0, 0, 0xEA, 0x60, 8, 6, 0, 0, 0) + (New-Object byte[] 4)) })
    'twice'          = @(@{ name = 'rice.toml'; bytes = (Bytes $riceToml) }, @{ name = 'assets/a.png'; bytes = (Png 8 8) }, @{ name = 'ASSETS/A.PNG'; bytes = (Png 8 8) })
    'no-manifest'    = @(@{ name = 'assets/sakura.png'; bytes = (Png 8 8) })
}
$countBefore = Rice-Count
foreach ($name in $hostile.Keys) {
    $file = Join-Path $files "$name.rice"
    New-Zip $file $hostile[$name]
    $r = Invoke-Rmw @('import', $file)
    Check "packages:refuses-$name" ($r.code -ne 0) $r.output
}
$junk = Join-Path $files 'junk.rice'
[IO.File]::WriteAllBytes($junk, (Bytes 'PK this is not a zip at all'))
$r = Invoke-Rmw @('import', $junk)
Check 'packages:refuses-not-a-zip' ($r.code -ne 0) $r.output
Check 'packages:nothing-escaped' ((Escaped).Count -eq 0) ((Escaped | ForEach-Object FullName) -join ', ')
Check 'packages:refused-leave-nothing' ((Leftovers).Count -eq 0 -and (Rice-Count) -eq $countBefore) ("imports: " + ((Leftovers | ForEach-Object Name) -join ', ') + "; rices $countBefore -> $(Rice-Count)")
$r = Invoke-Rmw @('status')
Check 'packages:core-still-running' ($r.code -eq 0) ''

# ---- plugins: .rmwp -----------------------------------------------------------------------------------
$plugin = Join-Path $Examples 'clock'
$rmwp = Join-Path $files 'clock.rmwp'
$r = Invoke-Rmw @('plugin', 'pack', $plugin, '--out', $rmwp)
Check 'packages:pack-plugin' ($r.code -eq 0 -and (Test-Path $rmwp)) $r.output
$r = Invoke-Rmw @('plugin', 'install', $rmwp, '--yes')
Check 'packages:install-rmwp' ($r.code -eq 0) $r.output
$r = Invoke-Rmw @('plugin', 'list')
Check 'packages:rmwp-installed' ($r.output -match 'io\.github\.angellm2\.clock') $r.output
Check 'packages:rmwp-files' (Test-Path (Join-Path $data 'plugins\io.github.angellm2.clock\widget.slint')) ''

$evilPlugin = Join-Path $files 'evil.rmwp'
New-Zip $evilPlugin @(@{ name = 'plugin.toml'; bytes = [IO.File]::ReadAllBytes((Join-Path $plugin 'plugin.toml')) },
                      @{ name = 'main.luau'; bytes = [IO.File]::ReadAllBytes((Join-Path $plugin 'main.luau')) },
                      @{ name = '../rmw-escape-plugin.luau'; bytes = (Bytes 'return {}') })
$r = Invoke-Rmw @('plugin', 'install', $evilPlugin, '--yes')
Check 'packages:refuses-hostile-rmwp' ($r.code -ne 0 -and (Escaped).Count -eq 0) $r.output
$exePlugin = Join-Path $files 'exe.rmwp'
New-Zip $exePlugin @(@{ name = 'plugin.toml'; bytes = [IO.File]::ReadAllBytes((Join-Path $plugin 'plugin.toml')) },
                     @{ name = 'helper.exe'; bytes = [byte[]](@(0x4D, 0x5A) + (New-Object byte[] 64)) })
$r = Invoke-Rmw @('plugin', 'install', $exePlugin, '--yes')
Check 'packages:refuses-exe-in-rmwp' ($r.code -ne 0) $r.output
Check 'packages:plugin-imports-clean' ((Leftovers).Count -eq 0) ((Leftovers | ForEach-Object Name) -join ', ')

$r = Invoke-Rmw @('restore', '--original', '--wait')
Invoke-Rmw @('shutdown') | Out-Null
Start-Sleep -Seconds 2
if (Test-Path (Join-Path $data 'logs')) { Copy-Item (Join-Path $data 'logs') (Join-Path $Out 'logs') -Recurse -Force }

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Out 'packages.json') -Encoding utf8
$results | ForEach-Object { '{0} {1} {2}' -f ($(if ($_.ok) { 'PASS' } else { 'FAIL' })), $_.name, $_.detail }
if (@($results | Where-Object { -not $_.ok }).Count -gt 0) { exit 1 }
exit 0
