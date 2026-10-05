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
. (Join-Path $PSScriptRoot 'env.ps1')

$root = $script:RepoRoot
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
