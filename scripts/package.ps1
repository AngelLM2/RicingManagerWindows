<#
.SYNOPSIS
    Builds what a release ships: the per-user installer, the official rices (.rice), the example
    plugins (.rmwp), SHA256SUMS.txt and an SPDX SBOM. Nothing is published or uploaded.
.DESCRIPTION
    1. release build (tests run) and the `rices` target (the official rices drawn and packed);
    2. optional signing of the programs (scripts\sign.ps1, -Sign);
    3. the installer, compiled by Inno Setup (scripts\fetch-deps.ps1 -Name innosetup);
    4. the example plugins packed with `rmw plugin pack`;
    5. SHA256SUMS.txt and sbom.spdx.json (our programs, the vcpkg ports they link, Slint).
    Out: build\package\<version>\
.EXAMPLE
    .\scripts\package.ps1
    .\scripts\package.ps1 -NoBuild          # reuse build\release as it is
    .\scripts\package.ps1 -Sign             # sign with the project's certificate (sign.ps1)
#>
[CmdletBinding()]
param(
    [switch] $NoBuild,
    [switch] $Sign
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$root = $script:RepoRoot

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^\s+VERSION\s+(\d+\.\d+\.\d+)' |
    Select-Object -First 1).Matches[0].Groups[1].Value
$build = Join-Path $root 'build\release'
$bin = Join-Path $build 'bin'
$rices = Join-Path $build 'rices'
$out = Join-Path $root "build\package\$version"
Write-Host "Ricing Manager $version"

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset release -Test
    Push-Location $root
    try {
        cmake --build --preset release --target rices
        if ($LASTEXITCODE -ne 0) { throw 'Drawing or packing the official rices failed.' }
    }
    finally { Pop-Location }
}
$programs = 'rmw-core.exe', 'rmw.exe', 'rmw-settings.exe', 'rmw-bar.exe', 'rmw-plugin-host.exe', 'rmw-wallpaper.exe'
foreach ($program in $programs + 'slint_cpp.dll') {
    if (-not (Test-Path (Join-Path $bin $program))) { throw "Missing $program in $bin (build the release preset with the Slint SDK)." }
}
if (-not (Test-Path (Join-Path $rices 'diva-stage.rice'))) { throw "No official rices in $rices (cmake --build --preset release --target rices)." }

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

if ($Sign) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Files ($programs | ForEach-Object { Join-Path $bin $_ })
}

# ---- the installer -------------------------------------------------------------------------------------
& (Join-Path $PSScriptRoot 'fetch-deps.ps1') -Name innosetup
$iscc = Join-Path $root 'build\.deps\innosetup-6.7.3\ISCC.exe'
& $iscc /Q "/DAppVersion=$version" "/DBinDir=$bin" "/DRicesDir=$rices" "/DSourceDir=$root" "/DOutDir=$out" `
    (Join-Path $root 'installer\RicingManager.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup could not build the installer.' }
$setup = Join-Path $out "RicingManager-$version-setup.exe"
if ($Sign) { & (Join-Path $PSScriptRoot 'sign.ps1') -Files $setup }

# ---- rices and plugins to share -----------------------------------------------------------------------------
Get-ChildItem $rices -Filter '*.rice' | Copy-Item -Destination $out
$rmw = Join-Path $bin 'rmw.exe'
foreach ($plugin in Get-ChildItem (Join-Path $root 'plugins\examples') -Directory) {
    $output = & $rmw plugin pack $plugin.FullName --out (Join-Path $out "$($plugin.Name).rmwp") 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Packing the plugin $($plugin.Name) failed: $output" }
}

# ---- checksums and the bill of materials ------------------------------------------------------------------------
$files = Get-ChildItem $out -File | Sort-Object Name
$sums = $files | ForEach-Object { '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant(), $_.Name }
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"))

$packages = New-Object System.Collections.Generic.List[object]
$relationships = New-Object System.Collections.Generic.List[object]
$packages.Add([ordered]@{
    name = 'ricing-manager-windows'; SPDXID = 'SPDXRef-ricing-manager'; versionInfo = $version
    downloadLocation = 'https://github.com/AngelLM2/RicingManagerWindows'; licenseConcluded = 'GPL-3.0-or-later'
    licenseDeclared = 'GPL-3.0-or-later'; copyrightText = 'NOASSERTION'; filesAnalyzed = $false
})
$relationships.Add([ordered]@{ spdxElementId = 'SPDXRef-DOCUMENT'; relationshipType = 'DESCRIBES'; relatedSpdxElement = 'SPDXRef-ricing-manager' })
$share = Join-Path $root 'build\vcpkg_installed\x64-windows-static-hardened\share'
foreach ($port in Get-ChildItem $share -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'vcpkg.spdx.json') }) {
    if ($port.Name -eq 'gtest') { continue } # tests only
    $spdx = Get-Content (Join-Path $port.FullName 'vcpkg.spdx.json') -Raw | ConvertFrom-Json
    $main = $spdx.packages | Where-Object { $_.SPDXID -eq 'SPDXRef-port' } | Select-Object -First 1
    $id = "SPDXRef-port-$($port.Name)"
    $packages.Add([ordered]@{
        name = $main.name; SPDXID = $id; versionInfo = $main.versionInfo; downloadLocation = $main.downloadLocation
        licenseConcluded = $main.licenseConcluded; licenseDeclared = 'NOASSERTION'; copyrightText = 'NOASSERTION'
        filesAnalyzed = $false; homepage = $main.homepage
    })
    $relationships.Add([ordered]@{ spdxElementId = 'SPDXRef-ricing-manager'; relationshipType = 'STATIC_LINK'; relatedSpdxElement = $id })
}
$packages.Add([ordered]@{
    name = 'slint'; SPDXID = 'SPDXRef-slint'; versionInfo = '1.18.1'; downloadLocation = 'https://github.com/slint-ui/slint/releases/tag/v1.18.1'
    licenseConcluded = 'GPL-3.0-only'; licenseDeclared = 'GPL-3.0-only OR LicenseRef-Slint-Royalty-free-2.0 OR LicenseRef-Slint-commercial'
    copyrightText = 'NOASSERTION'; filesAnalyzed = $false
})
$relationships.Add([ordered]@{ spdxElementId = 'SPDXRef-ricing-manager'; relationshipType = 'DYNAMIC_LINK'; relatedSpdxElement = 'SPDXRef-slint' })
$sbom = [ordered]@{
    spdxVersion = 'SPDX-2.3'; dataLicense = 'CC0-1.0'; SPDXID = 'SPDXRef-DOCUMENT'
    name = "ricing-manager-windows-$version"
    documentNamespace = "https://github.com/AngelLM2/RicingManagerWindows/sbom/$version-$([guid]::NewGuid())"
    creationInfo = [ordered]@{ created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); creators = @('Tool: scripts/package.ps1') }
    packages = $packages; relationships = $relationships
}
[IO.File]::WriteAllText((Join-Path $out 'sbom.spdx.json'), ($sbom | ConvertTo-Json -Depth 6))

# ---- winget (to submit to microsoft/winget-pkgs once the release is published; not done here) ----------------------
$winget = Join-Path $out 'winget'
New-Item -ItemType Directory -Force $winget | Out-Null
$setupHash = (Get-FileHash -Algorithm SHA256 $setup).Hash
foreach ($template in Get-ChildItem (Join-Path $root 'installer\winget') -Filter '*.yaml') {
    $text = (Get-Content $template.FullName -Raw).Replace('{{VERSION}}', $version)
    $text = $text.Replace('{{DATE}}', (Get-Date).ToString('yyyy-MM-dd')).Replace('{{SHA256}}', $setupHash)
    [IO.File]::WriteAllText((Join-Path $winget $template.Name), $text)
}

Write-Host "`nPackaged in $out"
Get-ChildItem $out -File | ForEach-Object { '  {0,-40} {1,8:N1} MB' -f $_.Name, ($_.Length / 1MB) }
if (-not $Sign) { Write-Host '(unsigned: run with -Sign once the certificate is set up; see scripts\sign.ps1)' }
