<#
.SYNOPSIS
    Downloads pinned non-vcpkg dependencies, verifies their SHA-256 (and, for signed ones, their
    Authenticode signer) and unpacks them into build\.deps\ — installers are never run on this PC.
.DESCRIPTION
    slint       the Slint C++ SDK, extracted with vcpkg's 7-Zip.
    innosetup   the Inno Setup compiler (ISCC) for the installer. Its own installer is in a format
                no extractor reads, so it runs inside a throwaway Windows Sandbox, which copies the
                installed folder back out; this PC only ever runs the compiler.
.EXAMPLE
    .\scripts\fetch-deps.ps1 -Name slint
    .\scripts\fetch-deps.ps1 -Name innosetup
#>
[CmdletBinding()]
param(
    [ValidateSet('slint', 'innosetup')]
    [string] $Name = 'slint'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

# Pinned versions. Update version, URL and hash together, in a reviewed commit.
$deps = @{
    slint     = @{
        Version = '1.18.1'
        Url     = 'https://github.com/slint-ui/slint/releases/download/v1.18.1/Slint-cpp-1.18.1-win64-MSVC-AMD64.exe'
        Sha256  = 'eea71789a54982bc1c164d9e1d2c7f00fead1415e9ac396ccb48676f2f66a7a9'
        Folder  = 'slint-1.18.1'
    }
    innosetup = @{
        Version = '6.7.3'
        Url     = 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe'
        Sha256  = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
        Signer  = 'CN=Pyrsys B.V.,'
        Folder  = 'innosetup-6.7.3'
    }
}

$dep = $deps[$Name]
$depsDir = Join-Path $script:RepoRoot 'build\.deps'
$target = Join-Path $depsDir $dep.Folder
if (Test-Path (Join-Path $target '.complete')) {
    Write-Host "$Name $($dep.Version) already present in $target"
    return
}

New-Item -ItemType Directory -Force (Join-Path $depsDir 'downloads') | Out-Null
$file = Join-Path $depsDir ('downloads\' + (Split-Path $dep.Url -Leaf))
if (-not (Test-Path $file)) {
    Write-Host "Downloading $($dep.Url)"
    Invoke-WebRequest -UseBasicParsing -Uri $dep.Url -OutFile $file
}

$hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
if ($hash -ne $dep.Sha256) {
    Remove-Item $file -Force
    throw "SHA-256 mismatch for $file (expected $($dep.Sha256), got $hash). The file was deleted."
}
if ($dep.Signer) {
    $signature = Get-AuthenticodeSignature $file
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate.Subject.StartsWith($dep.Signer)) {
        throw "Unexpected signature on $file ($($signature.Status), $($signature.SignerCertificate.Subject))."
    }
}

if (Test-Path $target) { Remove-Item $target -Recurse -Force }

if ($Name -eq 'innosetup') {
    if (-not (Get-Command 'WindowsSandbox.exe' -ErrorAction SilentlyContinue)) {
        throw 'Windows Sandbox is not enabled (Windows Features > "Windows Sandbox"); it unpacks Inno Setup.'
    }
    if (Get-Process -Name 'WindowsSandbox', 'WindowsSandboxClient', 'WindowsSandboxRemoteSession', 'vmmemWindowsSandbox' -ErrorAction SilentlyContinue) {
        throw 'Another Windows Sandbox is running.'
    }
    $work = Join-Path $depsDir 'innosetup-sandbox'
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
    New-Item -ItemType Directory -Force (Join-Path $work 'in'), (Join-Path $work 'out') | Out-Null
    Copy-Item $file (Join-Path $work 'in\setup.exe')
    # Installed in the Sandbox (portable: no uninstaller, no registry), copied out, and off it goes.
    [IO.File]::WriteAllText((Join-Path $work 'in\unpack.cmd'), @'
C:\is\in\setup.exe /PORTABLE=1 /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /NOICONS /DIR=C:\innosetup /LOG=C:\is\out\setup.log
robocopy C:\innosetup C:\is\out\innosetup /E /NFL /NDL /NJH /NJS
echo done > C:\is\out\done.txt
shutdown /s /t 3
'@)
    $wsb = Join-Path $work 'unpack.wsb'
    [IO.File]::WriteAllText($wsb, @"
<Configuration>
  <Networking>Disable</Networking>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MappedFolders>
    <MappedFolder><HostFolder>$(Join-Path $work 'in')</HostFolder><SandboxFolder>C:\is\in</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$(Join-Path $work 'out')</HostFolder><SandboxFolder>C:\is\out</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>cmd.exe /c C:\is\in\unpack.cmd</Command></LogonCommand>
</Configuration>
"@)
    Write-Host 'Unpacking Inno Setup in Windows Sandbox...'
    Start-Process $wsb
    $deadline = (Get-Date).AddMinutes(10)
    while (-not (Test-Path (Join-Path $work 'out\done.txt'))) {
        if ((Get-Date) -gt $deadline) { throw 'Windows Sandbox did not finish unpacking Inno Setup in 10 minutes.' }
        Start-Sleep -Seconds 3
    }
    $iscc = Join-Path $work 'out\innosetup\ISCC.exe'
    if (-not (Test-Path $iscc)) { throw "Inno Setup did not install in the Sandbox (see $work\out\setup.log)." }
    $signature = Get-AuthenticodeSignature $iscc
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate.Subject.StartsWith($dep.Signer)) {
        throw "Unexpected signature on ISCC.exe ($($signature.Status))."
    }
    Move-Item (Join-Path $work 'out\innosetup') $target
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
else {
    # Extract (never run) the installer with vcpkg's own 7-Zip.
    $sevenZip = (& "$env:VCPKG_ROOT\vcpkg.exe" fetch 7zip | Select-Object -Last 1).Trim()
    if (-not (Test-Path $sevenZip)) { throw "Could not locate 7-Zip through 'vcpkg fetch 7zip'." }
    & $sevenZip x $file "-o$target" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed to extract $file" }
    Remove-Item (Join-Path $target '$PLUGINSDIR') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $target 'Uninstall.exe') -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType File (Join-Path $target '.complete') | Out-Null
Write-Host "$Name $($dep.Version) unpacked to $target"
