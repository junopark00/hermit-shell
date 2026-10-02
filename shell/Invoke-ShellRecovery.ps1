#requires -Version 7.4
[CmdletBinding()]
param(
    [ValidateSet('Diagnose', 'Backup', 'Recover')][string]$Action = 'Diagnose',
    [string]$InstallDirectory = "$env:ProgramFiles\Shell",
    [string]$Path,
    [string]$Destination,
    [switch]$Portable,
    [switch]$SettingsOnly
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ShellRecovery.psm1') -Force
switch ($Action) {
    'Diagnose' {
        $report = Get-ShellDiagnostic -InstallDirectory $InstallDirectory | ConvertTo-Json -Depth 6
        if ($Path) {
            $stream = [IO.File]::Open([IO.Path]::GetFullPath($Path), [IO.FileMode]::CreateNew)
            try { $bytes = [Text.Encoding]::UTF8.GetBytes($report); $stream.Write($bytes) } finally { $stream.Dispose() }
        }
        $report
    }
    'Backup' {
        if (-not $Path) { throw 'Specify -Path for the new encrypted backup file.' }
        $password = if ($Portable) { Read-Host 'Backup password (at least 12 characters)' -AsSecureString } else { $null }
        Backup-ShellConfig -ConfigDirectory (Join-Path $InstallDirectory 'config') -OutputPath $Path -Password $password -SettingsOnly:$SettingsOnly
    }
    'Recover' {
        if (-not $Path -or -not $Destination) { throw 'Specify -Path and a new -Destination folder.' }
        $password = if ($Portable) { Read-Host 'Backup password' -AsSecureString } else { $null }
        Expand-ShellBackup -BackupPath $Path -Destination $Destination -Password $password
    }
}
