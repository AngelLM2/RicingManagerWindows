<#
.SYNOPSIS
    Runs a test suite inside Windows Sandbox: a disposable Windows where tests may change real
    system settings (cursors, wallpaper, theme, taskbar) without touching the host (docs/08 §4).
.DESCRIPTION
    The build output is mapped read-only, results come back through build\sandbox\<preset>.
    The sandbox shuts itself down when the suite finishes (unless -KeepOpen).
    Requires the "Windows Sandbox" optional feature. Only one sandbox can run at a time.
.EXAMPLE
    .\scripts\sandbox.ps1 -Preset debug -Suite unit
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'asan')]
    [string] $Preset = 'debug',
    [ValidateSet('unit', 'system', 'bar', 'plugins', 'tray', 'video', 'spike')]
    [string] $Suite = 'unit',
    # For -Suite spike: executable path relative to the build folder, e.g. spikes\s3-taskbar\spike_s3_taskbar.exe
    [string] $Spike,
    # Arguments for the spike executable (letters, digits, spaces and dashes only)
    [string] $SpikeArgs = '',
    [int] $TimeoutMinutes = 20,
    [switch] $KeepOpen
)

$ErrorActionPreference = 'Stop'

$sandboxExe = Join-Path $env:WINDIR 'System32\WindowsSandbox.exe'
if (-not (Test-Path $sandboxExe)) {
    throw 'Windows Sandbox is not enabled (Windows Features > "Windows Sandbox").'
}

$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root "build\$Preset"
$runnerDir = Join-Path $root 'tests\system\sandbox'
$pluginsDir = Join-Path $root 'plugins'
$outDir = Join-Path $root "build\sandbox\$Preset"

if (-not (Test-Path (Join-Path $buildDir 'tests'))) {
    throw "Build '$Preset' not found. Run .\scripts\build.ps1 -Preset $Preset first."
}

function Wait-SandboxClosed([int] $Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while (Get-Process -Name 'WindowsSandbox*' -ErrorAction SilentlyContinue) {
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep -Seconds 2
    }
    return $true
}

# Only one Windows Sandbox can run at a time: wait for a previous run to finish closing.
if (-not (Wait-SandboxClosed 120)) { throw 'Another Windows Sandbox is still running.' }

if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Force $outDir | Out-Null

if ($Suite -eq 'spike') {
    if (-not $Spike -or $Spike -notmatch '^[\w\-\\.]+\.exe$' -or $Spike -match '\.\.') {
        throw '-Spike must be a relative path to an .exe inside the build folder.'
    }
    if (-not (Test-Path (Join-Path $buildDir $Spike))) { throw "Spike not found: $Spike" }
    if ($SpikeArgs -notmatch '^[\w\- ]*$') { throw '-SpikeArgs may contain only letters, digits, spaces and dashes.' }
}

$shutdown = if ($KeepOpen) { '' } else { ' -Shutdown' }
$spikeArg = if ($Suite -eq 'spike') { " -Spike $Spike" } else { '' }
# Double quotes: with "powershell -File" single quotes would reach the script literally.
if ($Suite -eq 'spike' -and $SpikeArgs) { $spikeArg += " -SpikeArgs `"$SpikeArgs`"" }
$logon = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\rmw\runner\run-in-sandbox.ps1 -Suite $Suite$spikeArg$shutdown"

function Escape-Xml([string] $text) { [System.Security.SecurityElement]::Escape($text) }

$wsb = @"
<Configuration>
  <Networking>Disable</Networking>
  <VGpu>Enable</VGpu>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <PrinterRedirection>Disable</PrinterRedirection>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <MemoryInMB>4096</MemoryInMB>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$(Escape-Xml $buildDir)</HostFolder>
      <SandboxFolder>C:\rmw\build</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(Escape-Xml $runnerDir)</HostFolder>
      <SandboxFolder>C:\rmw\runner</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(Escape-Xml $pluginsDir)</HostFolder>
      <SandboxFolder>C:\rmw\plugins</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(Escape-Xml $outDir)</HostFolder>
      <SandboxFolder>C:\rmw\out</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>$(Escape-Xml $logon)</Command>
  </LogonCommand>
</Configuration>
"@

$wsbPath = Join-Path (Split-Path $outDir -Parent) "rmw-$Preset.wsb"
[System.IO.File]::WriteAllText($wsbPath, $wsb, [System.Text.UTF8Encoding]::new($false))

Write-Host "Starting Windows Sandbox ($Suite tests, preset $Preset)..."
$startedAt = Get-Date
Start-Process -FilePath $sandboxExe -ArgumentList "`"$wsbPath`""

$donePath = Join-Path $outDir 'done.json'
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while (-not (Test-Path $donePath)) {
    if ((Get-Date) -gt $deadline) { throw "Timed out after $TimeoutMinutes minutes waiting for $donePath" }
    Start-Sleep -Seconds 3
}

$result = Get-Content $donePath -Raw | ConvertFrom-Json
if (-not $KeepOpen -and -not (Wait-SandboxClosed 120)) {
    # The guest did not shut down (seen after a test restarted its Explorer): close the session we
    # opened — only processes started after us, never a sandbox the user opened.
    Write-Warning 'The sandbox did not shut down by itself; closing the session this script opened.'
    Get-Process -Name 'WindowsSandbox*' -ErrorAction SilentlyContinue |
        Where-Object { $_.StartTime -ge $startedAt } | Stop-Process -ErrorAction SilentlyContinue
    [void](Wait-SandboxClosed 60)
}
Get-Content (Join-Path $outDir "$Suite.log") -ErrorAction SilentlyContinue | Select-Object -Last 40
Write-Host "Sandbox suite '$Suite' finished with exit code $($result.exitCode)."
exit [int]$result.exitCode
