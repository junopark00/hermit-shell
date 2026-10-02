#requires -Version 5.1
<#
.SYNOPSIS
Builds Shell locally with MSYS2 UCRT64 and the official Node.js.
.DESCRIPTION
Runs CMake configure, Ninja build and, with -Package, a portable ZIP via CPack. Requires MSYS2
at -Msys2Root with the packages listed in docs/building.md, official Node.js in Program Files
(MSYS2's nodejs crashes with its gcc-16 libstdc++), and initialized submodules:

  git submodule update --init --recursive

Nothing is installed or replaced; output stays in -BuildDir inside the source tree.

The version is taken from `git describe --tags --dirty --match v*` of the source tree: a build of
the v1.0.0 tag is 1.0.0, a build three commits later 1.0.0-3-g1234abc, so a local build of
unreleased code is never mistaken for a release. A v* tag is required.
.PARAMETER SourceDir
Source tree to build. Defaults to the repository containing this script; use it to build
another worktree.
.EXAMPLE
./shell/Build-Shell.ps1
.EXAMPLE
./shell/Build-Shell.ps1 -Package -SourceDir C:/src/shell-worktree
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$BuildType = 'Release',
    [switch]$Package,
    [string]$SourceDir,
    [string]$Msys2Root = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'

$repo = if ($SourceDir) { (Resolve-Path $SourceDir).Path } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
if ($repo -match '\s') { throw "Repository path contains spaces, which the MSYS2 build does not handle: $repo" }
if ($BuildDir -notmatch '^[A-Za-z0-9._/-]+$') { throw 'BuildDir may only contain letters, digits, dot, dash, underscore and slash.' }
# The build folder may be deleted and recreated below, so it must be a real subfolder of the repository.
if (@(($BuildDir -split '/') | Where-Object { $_ -eq '.' -or $_ -eq '..' -or $_ -eq '' }).Count -gt 0) { throw 'BuildDir must be a subfolder path without "." or ".." segments.' }

$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw "MSYS2 not found at $Msys2Root. Install it with: winget install --id MSYS2.MSYS2 -e" }
$nodeDir = Join-Path $env:ProgramFiles 'nodejs'
if (-not (Test-Path -LiteralPath (Join-Path $nodeDir 'node.exe'))) { throw 'Official Node.js not found. Install it with: winget install --id OpenJS.NodeJS.LTS -e' }

$missing = @(git -C $repo submodule status | Where-Object { $_ -match '^-' })
if ($missing.Count -gt 0) { throw "Submodules are not initialized. Run: git submodule update --init --recursive" }

# cmake/prep/build_version.cmake uses these when both BRANCH and BUILD_VERSION are set.
$describe = (git -C $repo describe --tags --dirty --match 'v*').Trim()
if (-not $describe) { throw 'git describe found no v* tag in the source tree.' }
$env:BRANCH = (git -C $repo rev-parse --abbrev-ref HEAD).Trim()
$env:BUILD_VERSION = $describe -replace '^v', ''
$env:COMMIT = (git -C $repo rev-parse HEAD).Trim()
Write-Host "Building $($env:BUILD_VERSION) from branch $($env:BRANCH) in $repo"

$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$env:MSYS2_PATH_TYPE = 'inherit'
$env:PATH = "$nodeDir;$env:PATH"

# CMake refuses a build folder configured for another source path (for example after the
# repository folder was moved), so start that folder over; the first build is then a full one.
$cache = Join-Path $repo "$BuildDir\CMakeCache.txt"
if (Test-Path -LiteralPath $cache) {
    $homeLine = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$' | Select-Object -First 1
    if ($homeLine) {
        $cached = $homeLine.Matches[0].Groups[1].Value.Trim() -replace '\\', '/' -replace '^/([a-zA-Z])/', '$1:/'
        if ($cached.TrimEnd('/') -ne ($repo -replace '\\', '/').TrimEnd('/')) {
            Write-Host "Build folder was configured for $cached; recreating it for $repo (full rebuild)."
            Remove-Item -LiteralPath (Join-Path $repo $BuildDir) -Recurse -Force
        }
    }
}

$unixRepo = (& $bash -lc "cygpath -u '$repo'").Trim()
$steps = @(
    "cd '$unixRepo'",
    "cmake -B '$BuildDir' -G Ninja -S . -DCMAKE_BUILD_TYPE=$BuildType -DBUILD_TESTS=OFF",
    "ninja -C '$BuildDir'"
)
if ($Package) { $steps += "cpack -G ZIP --config './$BuildDir/CPackConfig.cmake'" }

$sw = [Diagnostics.Stopwatch]::StartNew()
& $bash -lc ($steps -join ' && ')
$code = $LASTEXITCODE
$sw.Stop()
if ($code -ne 0) { throw "Build failed with exit code $code after $([int]$sw.Elapsed.TotalMinutes) min." }
Write-Host "Build finished in $([Math]::Round($sw.Elapsed.TotalMinutes, 1)) min."
# Report with Write-Host rather than pipeline output, so callers such as Update-Shell.ps1 can print
# their own result object without PowerShell formatting it under this table's columns.
if ($Package) {
    foreach ($z in Get-ChildItem -LiteralPath (Join-Path $repo "$BuildDir\cpack_artifacts") -Filter *.zip) {
        Write-Host "Package: $($z.FullName) ($([Math]::Round($z.Length / 1MB, 1)) MB)"
    }
}
