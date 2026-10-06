<#
.SYNOPSIS
    Signs files with the project's code-signing certificate (Certum, decision D5) and checks the
    signature. Prepared, not run by default: scripts\package.ps1 -Sign calls it.
.DESCRIPTION
    The certificate is the maintainer's own (Certum Open Source Code Signing, on the SimplySign
    cloud card or a physical card): it never lives in the repository or in CI. Before signing:
      1. connect the card (SimplySign Desktop: log in, the virtual card appears in the store);
      2. find the certificate's SHA-1 thumbprint: Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert
      3. set RMW_SIGN_THUMBPRINT to it (or pass -Thumbprint).
    Every file gets a SHA-256 signature and an RFC 3161 timestamp from Certum, so it stays valid
    after the certificate expires.
.EXAMPLE
    $env:RMW_SIGN_THUMBPRINT = '0123...'; .\scripts\sign.ps1 -Files build\release\bin\rmw.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string[]] $Files,
    [string] $Thumbprint = $env:RMW_SIGN_THUMBPRINT,
    [string] $Timestamp = 'http://time.certum.pl'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

if (-not $Thumbprint) { throw 'No certificate: set RMW_SIGN_THUMBPRINT (see Get-Help .\scripts\sign.ps1).' }
$certificate = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Thumbprint -eq $Thumbprint }
if (-not $certificate) { throw "Certificate $Thumbprint is not in the current user's store (is the card connected?)." }

$signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if (-not $signtool) {
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
}
if (-not $signtool) { throw 'signtool.exe not found (Windows SDK).' }
$signtool = if ($signtool -is [System.Management.Automation.CommandInfo]) { $signtool.Source } else { $signtool.FullName }

foreach ($file in $Files) {
    if (-not (Test-Path $file)) { throw "Not found: $file" }
    & $signtool sign /sha1 $Thumbprint /fd sha256 /tr $Timestamp /td sha256 /d 'Ricing Manager' $file
    if ($LASTEXITCODE -ne 0) { throw "Signing failed: $file" }
    & $signtool verify /pa /q $file
    if ($LASTEXITCODE -ne 0) { throw "The signature does not verify: $file" }
    Write-Host "signed $file"
}
