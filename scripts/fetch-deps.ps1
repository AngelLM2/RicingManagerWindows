<#
.SYNOPSIS
    Downloads pinned non-vcpkg dependencies, verifies their SHA-256 and extracts them into
    build\.deps\ — installers are never executed.
.EXAMPLE
    .\scripts\fetch-deps.ps1 -Name slint
#>
[CmdletBinding()]
param(
    [ValidateSet('slint')]
    [string] $Name = 'slint'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

# Pinned versions. Update version, URL and hash together, in a reviewed commit.
$deps = @{
    slint = @{
        Version = '1.18.1'
        Url     = 'https://github.com/slint-ui/slint/releases/download/v1.18.1/Slint-cpp-1.18.1-win64-MSVC-AMD64.exe'
        Sha256  = 'eea71789a54982bc1c164d9e1d2c7f00fead1415e9ac396ccb48676f2f66a7a9'
        Folder  = 'slint-1.18.1'
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

# Extract (never run) the installer with vcpkg's own 7-Zip.
$sevenZip = (& "$env:VCPKG_ROOT\vcpkg.exe" fetch 7zip | Select-Object -Last 1).Trim()
if (-not (Test-Path $sevenZip)) { throw "Could not locate 7-Zip through 'vcpkg fetch 7zip'." }

if (Test-Path $target) { Remove-Item $target -Recurse -Force }
& $sevenZip x $file "-o$target" -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7-Zip failed to extract $file" }
Remove-Item (Join-Path $target '$PLUGINSDIR') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $target 'Uninstall.exe') -Force -ErrorAction SilentlyContinue

New-Item -ItemType File (Join-Path $target '.complete') | Out-Null
Write-Host "$Name $($dep.Version) extracted to $target"
