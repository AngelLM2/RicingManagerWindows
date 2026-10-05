<#
.SYNOPSIS
    Enters the Visual Studio x64 developer environment in the current PowerShell session.
.DESCRIPTION
    Dot-source it (". .\scripts\vsdev.ps1") so the environment stays in your session.
    Requires Visual Studio 2022+ or Build Tools with the "Desktop development with C++" workload.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    return
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found. Install Visual Studio 2022+ or Build Tools with the C++ workload.'
}

$installPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installPath) {
    throw 'No Visual Studio installation with the MSVC x64 toolset was found.'
}

# VsDevCmd looks for vswhere on PATH; make it findable to avoid a spurious warning.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
Import-Module (Join-Path $installPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if (-not $env:VCPKG_ROOT) {
    throw 'VCPKG_ROOT is not set. Install the vcpkg component of Visual Studio or set VCPKG_ROOT.'
}
