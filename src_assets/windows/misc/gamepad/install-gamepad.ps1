# Installs the ViGEmBus driver (virtual gamepads) unless version 1.17 or later is installed.
# Waits for the installer and exits with its exit code: 0 done, 3010 or 1641 restart needed.
$ErrorActionPreference = 'Stop'

try {
    $sys = Join-Path $env:SystemRoot 'System32\drivers\ViGEmBus.sys'
    if (Test-Path -LiteralPath $sys) {
        $info = (Get-Item -LiteralPath $sys).VersionInfo
        $version = New-Object Version($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
        if ($version -ge [Version]'1.17') {
            Write-Host "ViGEmBus $version is already installed."
            exit 0
        }
    }
} catch {
    Write-Host "Could not read the installed ViGEmBus version ($($_.Exception.Message)); installing."
}

$installer = Join-Path $PSScriptRoot 'vigembus_installer.exe'
if (-not (Test-Path -LiteralPath $installer)) {
    Write-Host "ViGEmBus installer not found: $installer"
    exit 2
}
# The installer is an Advanced Installer bootstrapper: /quiet and /norestart go to Windows Installer
$process = Start-Process -FilePath $installer -ArgumentList '/quiet', '/norestart' -Wait -PassThru
Write-Host "ViGEmBus installer exit code: $($process.ExitCode)"
exit $process.ExitCode
