<#
.SYNOPSIS
    Configures, builds and (optionally) tests a CMake preset inside the VS developer environment.
.EXAMPLE
    .\scripts\build.ps1                     # debug build
    .\scripts\build.ps1 -Preset asan -Test  # AddressSanitizer build + tests
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'asan', 'tidy')]
    [string] $Preset = 'debug',
    [switch] $Test,
    [switch] $Fresh,
    [string] $Target,
    # Extra configure arguments, e.g. -CMakeArgs '-DRMW_BUILD_SPIKES=ON'
    [string[]] $CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vsdev.ps1')

$root = Split-Path $PSScriptRoot -Parent

# Some vcpkg ports (meson-based ones such as tomlplusplus) break when vcpkg's default cache
# folders under %LOCALAPPDATA% contain an apostrophe or a space (e.g. C:\Users\o'neil).
# In that case keep vcpkg's downloads, binary cache and registry cache inside build\.vcpkg.
if ($env:LOCALAPPDATA -match "['\s]") {
    $vcpkgCache = Join-Path $root 'build\.vcpkg'
    foreach ($name in 'downloads', 'archives', 'registries') {
        New-Item -ItemType Directory -Force (Join-Path $vcpkgCache $name) | Out-Null
    }
    if (-not $env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $vcpkgCache 'downloads' }
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $vcpkgCache 'archives' }
    if (-not $env:X_VCPKG_REGISTRIES_CACHE) { $env:X_VCPKG_REGISTRIES_CACHE = Join-Path $vcpkgCache 'registries' }
}
Push-Location $root
try {
    $configureArgs = @('--preset', $Preset) + $CMakeArgs
    if ($Fresh) { $configureArgs += '--fresh' }
    cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "Configure failed (preset '$Preset')." }

    $buildArgs = @('--build', '--preset', $Preset)
    if ($Target) { $buildArgs += @('--target', $Target) }
    cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed (preset '$Preset')." }

    if ($Test) {
        ctest --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "Tests failed (preset '$Preset')." }
    }
}
finally {
    Pop-Location
}
