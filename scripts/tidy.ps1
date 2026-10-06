<#
.SYNOPSIS
    Runs clang-tidy (.clang-tidy) on first-party sources, in parallel, using the tidy preset's
    compile_commands.json. Any finding fails the run.
.DESCRIPTION
    clang-tidy reads the real MSVC command lines from the compilation database. (Running it through
    CMAKE_CXX_CLANG_TIDY instead loses every '/'-style flag such as /EHsc — an LLVM limitation with
    cl.exe command lines — and reports bogus errors.)
.EXAMPLE
    .\scripts\tidy.ps1                     # libs, apps and tests
    .\scripts\tidy.ps1 -Path libs\domain   # one area
#>
[CmdletBinding()]
param(
    # Only files under these folders (relative to the repository root).
    [string[]] $Path = @('libs', 'apps', 'tests'),
    [int] $Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

$root = $script:RepoRoot
Push-Location $root
try {
    cmake --preset tidy | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Configure failed (preset 'tidy')." }
    # Generated headers the sources include (the settings window's Slint code): made, not compiled.
    if (Test-Path (Join-Path $root 'build\tidy\build.ninja')) {
        $generated = Select-String -Path (Join-Path $root 'build\tidy\build.ninja') -Pattern '^build (apps\\settings\\app\.h)[ :]' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -Unique
        foreach ($header in $generated) { cmake --build build\tidy -- $header | Out-Null }
    }

    $tidy = Join-Path $env:VCINSTALLDIR 'Tools\Llvm\x64\bin\clang-tidy.exe'
    if (-not (Test-Path $tidy)) { $tidy = (Get-Command clang-tidy).Source }

    $prefixes = $Path | ForEach-Object { (Join-Path $root $_).TrimEnd('\') + '\' }
    $database = Get-Content (Join-Path $root 'build\tidy\compile_commands.json') -Raw | ConvertFrom-Json
    $files = $database | ForEach-Object { [IO.Path]::GetFullPath($_.file) } | Sort-Object -Unique |
        Where-Object { $file = $_; $prefixes | Where-Object { $file.StartsWith($_, 'OrdinalIgnoreCase') } } |
        Where-Object { $_ -match '\.(c|cc|cpp|cxx)$' } # C++ only (no .rc resource scripts)
    if (-not $files) { Write-Host 'No files to check.'; return }

    $outDir = Join-Path $root 'build\tidy\tidy-logs'
    if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
    New-Item -ItemType Directory $outDir | Out-Null

    $commonArgs = @('-p', 'build\tidy', '--quiet', '--warnings-as-errors=*',
        '--extra-arg=-Wno-unknown-argument', '--extra-arg=-Wno-unused-command-line-argument')
    $runs = New-Object System.Collections.Generic.List[object]
    $index = 0
    foreach ($file in $files) {
        while (@($runs | Where-Object { -not $_.Process.HasExited }).Count -ge $Jobs) {
            Start-Sleep -Milliseconds 100
        }
        $log = Join-Path $outDir ('{0:D3}.txt' -f $index++)
        $process = Start-Process -FilePath $tidy -ArgumentList ($commonArgs + "`"$file`"") -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        $null = $process.Handle # makes ExitCode available after exit
        $runs.Add([pscustomobject]@{ File = $file; Process = $process; Log = $log })
    }

    $failed = 0
    foreach ($run in $runs) {
        $run.Process.WaitForExit()
        $findings = Get-Content $run.Log, "$($run.Log).err" -ErrorAction SilentlyContinue |
            Where-Object { $_ -notmatch '^\d+ (warnings?|errors?)( and \d+ errors?)? generated\.$' -and
                $_ -notmatch '^(Suppressed \d+ warnings|Use -header-filter|Error while processing)' -and $_ }
        if ($run.Process.ExitCode -ne 0 -or $findings) {
            $failed++
            Write-Host "--- $($run.File)" -ForegroundColor Yellow
            $findings | ForEach-Object { Write-Host $_ }
        }
    }
    Write-Host ('clang-tidy: {0} file(s) checked, {1} with findings.' -f $runs.Count, $failed)
    if ($failed -gt 0) { throw 'clang-tidy reported findings.' }
}
finally {
    Pop-Location
}
