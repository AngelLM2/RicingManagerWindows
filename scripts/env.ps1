<#
.SYNOPSIS
    Shared environment setup for the build scripts (dot-source it): VS developer shell and
    vcpkg cache locations.
#>
. (Join-Path $PSScriptRoot 'vsdev.ps1')

$script:RepoRoot = Split-Path $PSScriptRoot -Parent

# Some vcpkg ports (meson-based ones such as tomlplusplus) break when vcpkg's default cache
# folders under %LOCALAPPDATA% contain an apostrophe or a space (e.g. C:\Users\o'neil).
# In that case keep vcpkg's downloads, binary cache and registry cache inside build\.vcpkg.
if ($env:LOCALAPPDATA -match "['\s]") {
    $vcpkgCache = Join-Path $script:RepoRoot 'build\.vcpkg'
    # Loop variable deliberately unusual: this file is dot-sourced into scripts with their own params.
    foreach ($rmwCacheFolder in 'downloads', 'archives', 'registries') {
        New-Item -ItemType Directory -Force (Join-Path $vcpkgCache $rmwCacheFolder) | Out-Null
    }
    if (-not $env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $vcpkgCache 'downloads' }
    if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $vcpkgCache 'archives' }
    if (-not $env:X_VCPKG_REGISTRIES_CACHE) { $env:X_VCPKG_REGISTRIES_CACHE = Join-Path $vcpkgCache 'registries' }
}
