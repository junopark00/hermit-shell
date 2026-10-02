#requires -Version 5.1
<#
.SYNOPSIS
One command to build Shell from source and install it on this PC.
.DESCRIPTION
1. Checks for an elevated PowerShell before doing anything (install needs it).
2. Builds a portable ZIP with Build-Shell.ps1. A failed build stops here; the service is not touched.
3. If the built version equals the installed version, stops without reinstalling (use -Force to
   reinstall anyway). Versions come from `git describe`, so any new commit changes the version.
4. Installs with Install-Shell.ps1: a first installation when Shell is not installed yet, otherwise
   an update with a full backup, program files replaced, config\ and drivers\ untouched and an
   automatic restore on failure. Keeps the original backup plus the newest -KeepBackups.

Run it from an elevated PowerShell in the repository root:

  powershell -ExecutionPolicy Bypass -File .\shell\Update-Shell.ps1
.PARAMETER SourceDir
Source tree to build, for example another worktree. Defaults to this repository.
.PARAMETER NoService
Testing only: skip the admin check and service handling (use with -InstallDir/-BackupRoot).
#>
[CmdletBinding()]
param(
    [string]$SourceDir,
    [switch]$Force,
    [ValidateRange(1, 100)][int]$KeepBackups = 5,
    [string]$InstallDir = "$env:ProgramFiles\Shell",
    [string]$BackupRoot = "$env:ProgramData\Shell\backups",
    [switch]$NoService
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $NoService) {
    $p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an elevated PowerShell (Run as administrator). Nothing was built or changed.'
    }
}

$repo = if ($SourceDir) { (Resolve-Path $SourceDir).Path } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$zip = Join-Path $repo 'build\cpack_artifacts\Shell.zip'
$buildStart = Get-Date

Write-Host '== Build =='
$buildArgs = @{ Package = $true }
if ($SourceDir) { $buildArgs.SourceDir = $repo }
& (Join-Path $PSScriptRoot 'Build-Shell.ps1') @buildArgs
if (-not (Test-Path -LiteralPath $zip) -or (Get-Item -LiteralPath $zip).LastWriteTime -lt $buildStart) {
    throw "Build did not produce a fresh package at $zip. Nothing was installed."
}

$built = (Get-Item -LiteralPath (Join-Path $repo 'build\shell.exe')).VersionInfo.ProductVersion
$installedExe = Join-Path $InstallDir 'shell.exe'
$installed = if (Test-Path -LiteralPath $installedExe) { (Get-Item -LiteralPath $installedExe).VersionInfo.ProductVersion } else { $null }
if ($built -like '*dirty*') { Write-Warning "Building from uncommitted changes ($built); commit first so the version identifies the code." }
if ($built -eq $installed -and -not $Force) {
    Write-Host "Installed version is already $installed. Nothing to install (use -Force to reinstall)."
    return
}

Write-Host "== Install $built (installed: $installed) =="
$installArgs = @{ ZipPath = $zip; InstallDir = $InstallDir; BackupRoot = $BackupRoot; KeepBackups = $KeepBackups }
if ($NoService) { $installArgs.NoService = $true }
& (Join-Path $PSScriptRoot 'Install-Shell.ps1') @installArgs | Format-List
