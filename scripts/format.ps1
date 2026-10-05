<#
.SYNOPSIS
    Formats first-party C++ sources with clang-format (.clang-format), or checks them with -Check.
#>
[CmdletBinding()]
param(
    [switch] $Check
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vsdev.ps1')

$root = Split-Path $PSScriptRoot -Parent
$files = Get-ChildItem -Path (Join-Path $root 'libs'), (Join-Path $root 'apps'), (Join-Path $root 'tests') `
    -Recurse -Include '*.cpp', '*.hpp', '*.h' -File |
    Where-Object { $_.FullName -notmatch '\\(build|vcpkg_installed)\\' } |
    ForEach-Object { $_.FullName }

if (-not $files) { return }

[string[]] $arguments = if ($Check) { '--dry-run', '--Werror' } else { '-i' }
clang-format @arguments $files
if ($LASTEXITCODE -ne 0) {
    throw 'clang-format reported differences. Run .\scripts\format.ps1 to fix them.'
}
