#requires -Version 5.1
<#
.SYNOPSIS
Installs, updates or uninstalls Shell from a portable ZIP build, with a full backup and rollback.
.DESCRIPTION
Install or update (-ZipPath <Shell-version.zip>):
  The ZIP must hold a single top folder "Shell" containing shell.exe; it is extracted to a staging
  folder first.

  First installation (Shell is not installed in -InstallDir yet):
    1. Copies the program files and the drivers folder, and creates config\ (with credentials\
       readable by Administrators and SYSTEM only).
    2. Installs the SudoVDA virtual display driver (drivers\sudovda\install.bat). This adds the
       driver's self-signed certificate (sudovda.cer) to the Local Machine "Trusted Root
       Certification Authorities" and "Trusted Publishers" stores, then creates the device.
    3. Adds the inbound firewall rule "Shell" for shell.exe (TCP and UDP).
    4. Registers ShellService (automatic start) and allows it to send Ctrl+Alt+Del (see below).
    5. Runs the ViGEmBus installer for virtual gamepads quietly and waits for it (skip with
       -NoGamepadDriver); a restart it asks for is reported as a warning.
    6. Starts the service. If a step before it fails, the service, the firewall rule and the new
       folder are removed again (the drivers stay installed).

  Update (Shell is installed):
    1. Stops the Shell service.
    2. Copies the whole install folder, including config, to a new timestamped backup folder.
    3. Copies the new program files over the install folder. config\ and drivers\ are never touched,
       so pairing, settings and the virtual display driver stay as they are.
    4. Starts the service. If anything fails after the service was stopped, the program files are
       restored from the backup and the service is started again.

  Permissions: config\credentials (private keys) and the backup folder (which holds copies of
  them) do not inherit permissions: only Administrators and SYSTEM can read them (Users may list the
  credentials folder). Install, update and migration apply this, and an update fixes an older
  installation or backup folder that still inherits read access for Users.

Rollback (-Rollback -BackupPath <folder>): restores the program files from a backup made by this script.
config\ is left as it is unless -RestoreConfig is given.

Uninstall (-Uninstall): stops and removes ShellService and the "Shell" firewall rule, puts back the
NVIDIA driver settings Shell changed, and moves the install folder (with config, pairing and
certificates) into the backup folder, from where it can be deleted. -RemoveDriver also removes the
SudoVDA device, its driver package (pnputil /delete-driver) and its certificate from the certificate
stores. The ViGEmBus driver stays (remove
it from Apps & features if nothing else uses it).

Migration from Apollo (-MigrateFromApollo, opt-in, with -ZipPath): when Apollo is installed
(service ApolloService, folder C:\Program Files\Apollo with sunshine.exe and sunshine.conf) and Shell
is not, moves that installation over instead of starting from scratch:
  1. Stops the Apollo service and sets it to Disabled (it is not deleted).
  2. Copies config\ (settings, pairing, app list, covers, certificates) and drivers\ to the new folder,
     renaming sunshine.conf -> shell.conf and sunshine_state.json -> shell_state.json and the
     sunshine_name key -> shell_name. Paired devices keep working without pairing again.
  3. Copies the program files, registers ShellService (automatic) and starts it. The "Apollo"
     firewall rules are turned off and "Shell" rules are added.
  If anything fails, the Apollo service, firewall rules and start type are put back and it is started again.
-RevertMigration goes back to Apollo (changes made in Shell since are not carried over).
-RemoveOldInstall, once Shell works, deletes the Apollo service and firewall rules and moves the
Apollo folder into the backup folder. Without -MigrateFromApollo, a first installation stops when it
finds Apollo, because both would use the same ports.

Install, update and migration also allow services to send Ctrl+Alt+Del (the SoftwareSASGeneration
policy under HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System gets the "services" bit),
so Shell can pass on a Ctrl+Alt+Del from a client; Windows ignores it as ordinary input. A failure
there only warns. Rollback, uninstall and -RevertMigration leave the policy as it is (remove the bit
by hand if it is not wanted).

-KeepBackups N (update only, after success): deletes older backups made by this script, keeping the
oldest one (the original installation) and the newest N. Folders without backup-manifest.json are
never touched. 0 (default) keeps everything.

Must run in an elevated PowerShell. -NoService and -InstallDir exist for testing against a scratch folder
(no service, firewall, driver or certificate changes); -SasPolicyKey (another registry key for the
Ctrl+Alt+Del policy) lets a test check that step with -NoService.
.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\Install-Shell.ps1 -ZipPath .\Shell-1.0.0.zip
.EXAMPLE
.\Install-Shell.ps1 -Rollback -BackupPath C:\ProgramData\Shell\backups\20260930-010203-1.0.0
.EXAMPLE
.\Install-Shell.ps1 -ZipPath .\Shell-1.0.0.zip -MigrateFromApollo
.EXAMPLE
.\Install-Shell.ps1 -Uninstall -RemoveDriver
#>
[CmdletBinding(DefaultParameterSetName = 'Deploy')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Deploy')][string]$ZipPath,
    [Parameter(Mandatory, ParameterSetName = 'Rollback')][switch]$Rollback,
    [Parameter(Mandatory, ParameterSetName = 'Rollback')][string]$BackupPath,
    [Parameter(ParameterSetName = 'Rollback')][switch]$RestoreConfig,
    [Parameter(ParameterSetName = 'Deploy')][ValidateRange(0, 100)][int]$KeepBackups = 0,
    [Parameter(ParameterSetName = 'Deploy')][switch]$NoGamepadDriver,
    [Parameter(ParameterSetName = 'Deploy')][switch]$MigrateFromApollo,
    [Parameter(Mandatory, ParameterSetName = 'RevertMigration')][switch]$RevertMigration,
    [Parameter(Mandatory, ParameterSetName = 'RemoveOldInstall')][switch]$RemoveOldInstall,
    [Parameter(Mandatory, ParameterSetName = 'Uninstall')][switch]$Uninstall,
    [Parameter(ParameterSetName = 'Uninstall')][switch]$RemoveDriver,
    [string]$InstallDir = "$env:ProgramFiles\Shell",
    [string]$BackupRoot = "$env:ProgramData\Shell\backups",
    [string]$ServiceName = 'ShellService',
    # The Apollo installation that -MigrateFromApollo moves over
    [string]$OldInstallDir = "$env:ProgramFiles\Apollo",
    [string]$OldServiceName = 'ApolloService',
    [string]$OldFirewallRule = 'Apollo',
    [string]$SasPolicyKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System',
    [switch]$NoService
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:SasKeyGiven = $PSBoundParameters.ContainsKey('SasPolicyKey')
# Shell reads the policy from HKLM only: another key is for tests against a scratch folder
if ($script:SasKeyGiven -and -not $NoService) { throw '-SasPolicyKey is only for tests (with -NoService).' }

# Folders inside the install dir that update and plain rollback never modify.
$script:Protected = @('config', 'drivers')
$script:FirewallRule = 'Shell'
# The SudoVDA device: hardware id and the Display device class
$script:VdaHardwareId = 'root\sudomaker\sudovda'
$script:DisplayClassGuid = '4D36E968-E325-11CE-BFC1-08002BE10318'
# Protect-Credentials: true to restrict even when the folder no longer inherits
$script:ForceAcl = $false

function Assert-Admin {
    if ($NoService) { return }
    $p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell (Run as administrator).'
    }
}

function Wait-ProcessGone([string]$Name) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (@(Get-Process -Name $Name -ErrorAction SilentlyContinue).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    if (@(Get-Process -Name $Name -ErrorAction SilentlyContinue).Count -gt 0) { throw "$Name.exe is still running after stopping the service." }
}

function Stop-Shell {
    if ($NoService) { return }
    $svc = Get-Service -Name $ServiceName -ErrorAction Stop
    if ($svc.Status -ne 'Stopped') {
        Write-Host "Stopping $ServiceName..."
        Stop-Service -Name $ServiceName -Force
        $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }
    # The service wrapper stops shell.exe; make sure nothing still holds the files.
    Wait-ProcessGone 'shell'
}

function Start-Shell {
    if ($NoService) { return }
    Write-Host "Starting $ServiceName..."
    Start-Service -Name $ServiceName
    (Get-Service -Name $ServiceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
}

function Register-ShellService {
    Write-Host "Registering $ServiceName"
    $bin = '"' + (Join-Path $InstallDir 'tools\shellsvc.exe') + '"'
    $null = New-Service -Name $ServiceName -BinaryPathName $bin -DisplayName 'Shell Service' -StartupType Automatic `
        -Description 'Shell game streaming host for Hermit and other Moonlight-compatible clients.'
}

function Remove-ShellService {
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        & sc.exe delete $ServiceName | Out-Null
    }
}

function Get-ProgramItems([string]$Dir) {
    @(Get-ChildItem -LiteralPath $Dir -Force | Where-Object { $script:Protected -notcontains $_.Name.ToLowerInvariant() })
}

function Copy-ProgramFiles([string]$From, [string]$To) {
    # Copy every top-level item except protected folders, replacing existing files.
    foreach ($item in Get-ProgramItems $From) {
        $target = Join-Path $To $item.Name
        if ($item.PSIsContainer) {
            $null = New-Item -ItemType Directory -Path $target -Force
            Copy-Item -Path (Join-Path $item.FullName '*') -Destination $target -Recurse -Force
        } else {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
}

function Remove-ProgramFiles([string]$Dir) {
    # Removes program files but keeps protected folders, so a restore does not leave stray new files.
    foreach ($item in Get-ProgramItems $Dir) { Remove-Item -LiteralPath $item.FullName -Recurse -Force }
}

function Get-ExeVersion([string]$Dir) {
    $exe = Join-Path $Dir 'shell.exe'
    if (Test-Path -LiteralPath $exe) { (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion } else { $null }
}

function Set-ShellFirewall {
    if ($NoService) { return }
    $exe = Join-Path $InstallDir 'shell.exe'
    Get-NetFirewallRule -DisplayName $script:FirewallRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    foreach ($protocol in 'TCP', 'UDP') {
        $null = New-NetFirewallRule -DisplayName $script:FirewallRule -Direction Inbound -Action Allow -Protocol $protocol -Program $exe -Enabled True
    }
}

function Remove-ShellFirewall {
    if ($NoService) { return }
    Get-NetFirewallRule -DisplayName $script:FirewallRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
}

# Lets services (Shell) send the secure attention sequence: SoftwareSASGeneration 1 = services,
# 2 = Ease of Access programs, 3 = both; the services bit is added to what is set
function Enable-ServiceSas {
    if ($NoService -and -not $script:SasKeyGiven) { return }
    try {
        if (-not (Test-Path -LiteralPath $SasPolicyKey)) { $null = New-Item -Path $SasPolicyKey -Force }
        # GetValue: the value is usually missing (Windows default), which StrictMode would not allow as a property
        $current = (Get-Item -LiteralPath $SasPolicyKey).GetValue('SoftwareSASGeneration', $null)
        $value = if ($null -eq $current) { 0 } else { [int]$current }
        if (($value -band 1) -eq 0) {
            Write-Host 'Allowing services to send Ctrl+Alt+Del (SoftwareSASGeneration)'
            Set-ItemProperty -LiteralPath $SasPolicyKey -Name 'SoftwareSASGeneration' -Value ($value -bor 1) -Type DWord
        }
    } catch {
        Write-Warning "Could not allow Ctrl+Alt+Del from Shell ($($_.Exception.Message)); Ctrl+Alt+Del from a client will do nothing."
    }
}

# Makes a folder private: no inherited entries, full control for Administrators and SYSTEM (by SID,
# since the group names are localized) and, with -UsersMayList, list access (not file read) for
# Users. Everything below it is reset to inherit only that. With -NoService (tests against scratch
# folders, run without elevation) the current user keeps full control as well.
function Set-PrivateAcl([string]$Path, [switch]$UsersMayList) {
    $grants = @('*S-1-5-32-544:(OI)(CI)(F)', '*S-1-5-18:(OI)(CI)(F)')
    if ($NoService) { $grants += '*' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value + ':(OI)(CI)(F)' }
    if ($UsersMayList) { $grants += '*S-1-5-32-545:(R)' }
    # Grant first, so removing the inherited entries never locks this script out
    & icacls.exe $Path /grant:r @grants /Q | Out-Null
    if ($LASTEXITCODE -eq 0) { & icacls.exe $Path /inheritance:r /Q | Out-Null }
    # Explicit entries for Users, Authenticated Users and Everyone (none is expected)
    if ($LASTEXITCODE -eq 0 -and -not $UsersMayList) { & icacls.exe $Path /remove:g '*S-1-5-32-545' '*S-1-5-11' '*S-1-1-0' /Q | Out-Null }
    if ($LASTEXITCODE -ne 0) { throw "Could not restrict the permissions of $Path (icacls exit code $LASTEXITCODE)." }
    if (@(Get-ChildItem -LiteralPath $Path -Force).Count -gt 0) {
        & icacls.exe (Join-Path $Path '*') /reset /T /C /Q | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not reset the permissions below $Path (icacls exit code $LASTEXITCODE)." }
    }
}

# credentials\ holds the private keys: it must not inherit read access for Users from Program Files
function Protect-Credentials {
    $credentials = Join-Path $InstallDir 'config\credentials'
    if (-not (Test-Path -LiteralPath $credentials)) { return }
    if ((Get-Acl -LiteralPath $credentials).AreAccessRulesProtected -and -not $script:ForceAcl) { return }
    Write-Host "Restricting access to $credentials"
    Set-PrivateAcl $credentials -UsersMayList
}

# Backups contain config\credentials; ProgramData would let Users read them. The folder is made
# private once (which also fixes the backups already in it); later backups inherit that.
function Protect-BackupRoot {
    $null = New-Item -ItemType Directory -Path $BackupRoot -Force
    if ((Get-Acl -LiteralPath $BackupRoot).AreAccessRulesProtected) { return }
    Write-Host "Restricting access to $BackupRoot"
    Set-PrivateAcl $BackupRoot
}

# A folder moved into the backups keeps the entries it had; make it inherit the backup folder's only
function Reset-ToInherited([string]$Path) {
    & icacls.exe $Path /reset /T /C /Q | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not reset the permissions of $Path (icacls exit code $LASTEXITCODE)." }
}

function Initialize-ConfigFolder {
    # Settings, pairing state and certificates live in config\; shell.exe fills it on first start
    $null = New-Item -ItemType Directory -Path (Join-Path $InstallDir 'config\credentials') -Force
    $script:ForceAcl = $true
    try { Protect-Credentials } finally { $script:ForceAcl = $false }
}

function Install-VirtualDisplayDriver {
    if ($NoService) { return }
    $script = Join-Path $InstallDir 'drivers\sudovda\install.bat'
    if (-not (Test-Path -LiteralPath $script)) { Write-Warning "No $script; the virtual display will not be available."; return }
    Write-Host 'Installing the SudoVDA virtual display driver (adds its certificate to the Root and TrustedPublisher stores)'
    & cmd.exe /d /c $script | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Warning "The driver installer ended with exit code $LASTEXITCODE; check the virtual display in Device Manager." }
}

function Install-GamepadDriver {
    if ($NoService -or $NoGamepadDriver) { return }
    $script = Join-Path $InstallDir 'scripts\install-gamepad.ps1'
    if (-not (Test-Path -LiteralPath $script)) { return }
    Write-Host 'Installing the ViGEmBus driver for virtual gamepads'
    # The script waits for the installer and returns its exit code
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script | Out-Host
    $code = $LASTEXITCODE
    if ($code -eq 3010 -or $code -eq 1641) {
        Write-Warning 'ViGEmBus was installed; restart Windows before using gamepads from a client.'
    } elseif ($code -ne 0) {
        Write-Warning "The ViGEmBus installer ended with exit code $code; gamepads from clients may not work."
    }
}

function Remove-VirtualDisplayDriver {
    $nefcon = Join-Path $InstallDir 'drivers\sudovda\nefconc.exe'
    if (Test-Path -LiteralPath $nefcon) {
        Write-Host 'Removing the SudoVDA virtual display device'
        & $nefcon --remove-device-node --hardware-id $script:VdaHardwareId --class-guid $script:DisplayClassGuid | Out-Host
    }
    # The driver package in the DriverStore: pnputil lists one block per package; the block that
    # names sudovda.inf holds its oem<N>.inf (labels are localized, so match the file names only)
    try {
        $blocks = ((& pnputil.exe /enum-drivers) -join "`n") -split "`n\s*`n"
        foreach ($block in $blocks) {
            if ($block -match '(?i)\bsudovda\.inf\b' -and $block -match '(?i)\b(oem\d+\.inf)\b') {
                $published = $matches[1]
                Write-Host "Removing the SudoVDA driver package $published"
                & pnputil.exe /delete-driver $published /uninstall /force | Out-Host
                if ($LASTEXITCODE -ne 0) { Write-Warning "pnputil could not remove $published (exit code $LASTEXITCODE)." }
            }
        }
    } catch { Write-Warning "Could not remove the SudoVDA driver package ($($_.Exception.Message))." }
    $cer = Join-Path $InstallDir 'drivers\sudovda\sudovda.cer'
    if (Test-Path -LiteralPath $cer) {
        $thumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2($cer)).Thumbprint
        foreach ($store in 'Root', 'TrustedPublisher') {
            $path = "Cert:\LocalMachine\$store\$thumb"
            if (Test-Path -LiteralPath $path) {
                Write-Host "Removing the SudoVDA certificate from LocalMachine\$store"
                Remove-Item -LiteralPath $path
            }
        }
    }
}

function Set-OldFirewall([bool]$Enabled) {
    if ($NoService) { return }
    $rules = Get-NetFirewallRule -DisplayName $OldFirewallRule -ErrorAction SilentlyContinue
    if ($rules) { if ($Enabled) { $rules | Enable-NetFirewallRule } else { $rules | Disable-NetFirewallRule } }
}

function Copy-OldConfig([string]$From, [string]$To) {
    # Settings, pairing state, app list, covers and certificates, under the new file names
    $null = New-Item -ItemType Directory -Path $To -Force
    Copy-Item -Path (Join-Path $From '*') -Destination $To -Recurse -Force
    foreach ($pair in @(@('sunshine.conf', 'shell.conf'), @('sunshine_state.json', 'shell_state.json'))) {
        $old = Join-Path $To $pair[0]
        if (Test-Path -LiteralPath $old) { Move-Item -LiteralPath $old -Destination (Join-Path $To $pair[1]) -Force }
    }
    Get-ChildItem -LiteralPath $To -Filter 'sunshine.log*' -File | Remove-Item -Force
    $conf = Join-Path $To 'shell.conf'
    if (Test-Path -LiteralPath $conf) {
        $lines = [IO.File]::ReadAllLines($conf) | ForEach-Object { $_ -replace '^(\s*)sunshine_name(\s*=)', '$1shell_name$2' }
        [IO.File]::WriteAllLines($conf, [string[]]$lines, (New-Object Text.UTF8Encoding($false)))
    }
    # Commands and covers may point into the Apollo folder (gone after -RemoveOldInstall) or use
    # the APOLLO_* variables (not set by Shell)
    foreach ($file in @($conf, (Join-Path $To 'apps.json'))) {
        if (-not (Test-Path -LiteralPath $file)) { continue }
        $text = [IO.File]::ReadAllText($file)
        $new = $text
        foreach ($sep in @('\', '\\', '/')) {
            $old = $OldInstallDir.Replace('\', $sep); $repl = $InstallDir.Replace('\', $sep)
            # Only the folder itself: followed by a separator, a quote or the end (not "Apollo Tools")
            $pattern = [regex]::Escape($old) + '(?=' + [regex]::Escape($sep) + '|/|"|''|\s*$)'
            $new = [regex]::Replace($new, $pattern, $repl.Replace('$', '$$'), 'IgnoreCase, Multiline')
        }
        # Only the variables Apollo set (not a host name such as APOLLO_PC)
        $new = [regex]::Replace($new, '\bAPOLLO_((?:APP|CLIENT)_[A-Z_]+)\b', 'SHELL_$1')
        if ($new -ne $text) { [IO.File]::WriteAllText($file, $new, (New-Object Text.UTF8Encoding($false))) }
    }
}

Assert-Admin
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$OldInstallDir = [IO.Path]::GetFullPath($OldInstallDir)
$installed = Test-Path -LiteralPath (Join-Path $InstallDir 'shell.exe')
$oldInstalled = Test-Path -LiteralPath (Join-Path $OldInstallDir 'sunshine.exe')

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $InstallDir)) { throw "No Shell installation at $InstallDir." }
    $driverRemoved = $false
    if (-not $NoService) {
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
            Stop-Shell
            & sc.exe delete $ServiceName | Out-Null
        }
        Remove-ShellFirewall
        # Puts back the NVIDIA driver settings Shell changed (nothing to do without an NVIDIA GPU)
        $exe = Join-Path $InstallDir 'shell.exe'
        if (Test-Path -LiteralPath $exe) {
            try { & $exe --restore-nvprefs-undo | Out-Null } catch { Write-Warning "Could not restore the NVIDIA settings ($($_.Exception.Message))." }
        }
        if ($RemoveDriver) {
            Remove-VirtualDisplayDriver
            $driverRemoved = $true
        }
    }
    # Kept, not deleted: the folder holds the settings, pairing state and certificates
    Protect-BackupRoot
    $moved = Join-Path $BackupRoot ('uninstalled-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Move-Item -LiteralPath $InstallDir -Destination $moved
    Reset-ToInherited $moved
    [pscustomobject]@{ Action = 'Uninstall'; RemovedService = $ServiceName; DriverRemoved = $driverRemoved; FolderMovedTo = $moved }
    return
}

if ($RevertMigration) {
    if (-not $oldInstalled) { throw "No Apollo installation at $OldInstallDir to go back to." }
    if (-not $NoService) {
        if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
            Stop-Shell
            & sc.exe delete $ServiceName | Out-Null
        }
        Remove-ShellFirewall
        Set-OldFirewall $true
        Set-Service -Name $OldServiceName -StartupType Automatic
        Start-Service -Name $OldServiceName
    }
    # Put the new folder aside (kept, with any settings changed since) so a later deploy migrates again
    $aside = $null
    if (Test-Path -LiteralPath $InstallDir) {
        $aside = $InstallDir + '.reverted-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Move-Item -LiteralPath $InstallDir -Destination $aside
    }
    [pscustomobject]@{ Action = 'RevertMigration'; Running = $OldServiceName; OldInstallDir = $OldInstallDir; NewFolderMovedTo = $aside }
    return
}

if ($RemoveOldInstall) {
    if (-not $installed) { throw "Shell is not installed at $InstallDir; the Apollo installation stays." }
    if (-not $NoService) {
        if ((Get-Service -Name $ServiceName -ErrorAction Stop).Status -ne 'Running') { throw "$ServiceName is not running; check Shell before removing the Apollo installation." }
        if (Get-Service -Name $OldServiceName -ErrorAction SilentlyContinue) {
            Stop-Service -Name $OldServiceName -Force -ErrorAction SilentlyContinue
        }
    }
    # The folder first: if it is locked, nothing else is removed and the old install stays usable
    $moved = $null
    if (Test-Path -LiteralPath $OldInstallDir) {
        $moved = Join-Path $BackupRoot ("old-install-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Protect-BackupRoot
        Move-Item -LiteralPath $OldInstallDir -Destination $moved
        Reset-ToInherited $moved
    }
    if (-not $NoService) {
        if (Get-Service -Name $OldServiceName -ErrorAction SilentlyContinue) { & sc.exe delete $OldServiceName | Out-Null }
        Get-NetFirewallRule -DisplayName $OldFirewallRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
        # Apollo's installer entry in Apps & features and its Start menu folder point into the moved folder
        foreach ($root in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall') {
            foreach ($key in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
                $uninstallString = [string](Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue).UninstallString
                if ($uninstallString -and $uninstallString.Trim('"').StartsWith($OldInstallDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    Remove-Item -LiteralPath $key.PSPath -Recurse -Force
                }
            }
        }
        $menu = Join-Path $env:ProgramData ('Microsoft\Windows\Start Menu\Programs\' + (Split-Path $OldInstallDir -Leaf))
        if (Test-Path -LiteralPath $menu) { Remove-Item -LiteralPath $menu -Recurse -Force }
        # Apollo's installer added its folders to the system PATH. Edit the raw registry value so
        # entries such as %SystemRoot%\system32 stay variables (and the value stays REG_EXPAND_SZ)
        $envKey = Get-Item -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
        $raw = [string]$envKey.GetValue('Path', $null, 'DoNotExpandEnvironmentNames')
        if ($raw) {
            $entries = $raw.Split(';')
            $kept = @($entries | Where-Object {
                $entry = [Environment]::ExpandEnvironmentVariables($_).Trim().TrimEnd('\')
                -not ($entry -and ($entry -ieq $OldInstallDir -or $entry.StartsWith($OldInstallDir + '\', [StringComparison]::OrdinalIgnoreCase)))
            })
            if ($kept.Count -ne $entries.Count) {
                Set-ItemProperty -LiteralPath $envKey.PSPath -Name Path -Value ($kept -join ';') -Type ExpandString
            }
        }
    }
    [pscustomobject]@{ Action = 'RemoveOldInstall'; RemovedService = $OldServiceName; OldFolderMovedTo = $moved }
    return
}

# A Shell folder without its service is what a revert or a failed migration leaves behind
$serviceMissing = -not $NoService -and -not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)
$migrate = $false
if ($MigrateFromApollo) {
    if (-not $oldInstalled) { throw "-MigrateFromApollo: no Apollo installation at $OldInstallDir (sunshine.exe not found)." }
    if ($installed -and -not $serviceMissing) { throw "Shell is already installed at $InstallDir; -MigrateFromApollo only applies before the first installation." }
    $migrate = $true
}
$fresh = -not $installed -and -not $migrate -and -not $Rollback
if ($fresh -and $oldInstalled) {
    throw "Found an Apollo installation at $OldInstallDir. Run again with -MigrateFromApollo to move its settings and pairing to Shell, or uninstall Apollo first (both use the same ports)."
}
if ($Rollback -and -not $installed) { throw "No Shell installation found at $InstallDir" }

if ($Rollback) {
    $BackupPath = [IO.Path]::GetFullPath($BackupPath)
    $manifestPath = Join-Path $BackupPath 'backup-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Not a backup made by this script (backup-manifest.json missing).' }
    $files = Join-Path $BackupPath 'files'
    if (-not (Test-Path -LiteralPath (Join-Path $files 'shell.exe'))) { throw 'Backup does not contain shell.exe.' }
    Stop-Shell
    Remove-ProgramFiles $InstallDir
    Copy-ProgramFiles $files $InstallDir
    if ($RestoreConfig) {
        $cfg = Join-Path $files 'config'
        if (-not (Test-Path -LiteralPath $cfg)) { throw 'Backup has no config folder.' }
        Copy-Item -Path (Join-Path $cfg '*') -Destination (Join-Path $InstallDir 'config') -Recurse -Force
    }
    Protect-Credentials
    Start-Shell
    [pscustomobject]@{ Action = 'Rollback'; InstallDir = $InstallDir; RestoredVersion = Get-ExeVersion $InstallDir; ConfigRestored = [bool]$RestoreConfig; Backup = $BackupPath }
    return
}

# --- Install, update or migrate: check and extract the ZIP ---
$ZipPath = [IO.Path]::GetFullPath($ZipPath)
if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) { throw "ZIP not found: $ZipPath" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    # Some ZIP writers (.NET Framework) store '\' as the separator; treat both the same.
    $names = @($zip.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
    $tops = @($names | ForEach-Object { ($_ -split '/')[0] } | Sort-Object -Unique)
    if ($tops.Count -ne 1 -or $tops[0] -ne 'Shell') { throw "Unexpected ZIP layout; top-level entries: $($tops -join ', ')" }
    if ($names -notcontains 'Shell/shell.exe') { throw 'ZIP has no Shell/shell.exe.' }
    foreach ($n in $names) { if ($n -match '(^|/)\.\.(/|$)' -or $n -match '^[/\\]' -or $n -match ':') { throw "Unsafe path in ZIP: $n" } }
} finally { $zip.Dispose() }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$staging = Join-Path ([IO.Path]::GetTempPath()) "shell-staging-$stamp"
[IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $staging)
$newFiles = Join-Path $staging 'Shell'
$newVersion = Get-ExeVersion $newFiles

if ($fresh) {
    $createdDir = -not (Test-Path -LiteralPath $InstallDir)
    $serviceCreated = $false
    $firewallSet = $false
    try {
        Write-Host "Installing Shell $newVersion to $InstallDir"
        $null = New-Item -ItemType Directory -Path $InstallDir -Force
        Copy-ProgramFiles $newFiles $InstallDir
        $drivers = Join-Path $newFiles 'drivers'
        if ((Test-Path -LiteralPath $drivers) -and -not (Test-Path -LiteralPath (Join-Path $InstallDir 'drivers'))) {
            Copy-Item -LiteralPath $drivers -Destination (Join-Path $InstallDir 'drivers') -Recurse
        }
        Initialize-ConfigFolder
        if ((Get-ExeVersion $InstallDir) -ne $newVersion) { throw 'Installed shell.exe does not report the new version.' }

        Install-VirtualDisplayDriver
        if (-not $NoService) {
            Set-ShellFirewall
            $firewallSet = $true
            Register-ShellService
            $serviceCreated = $true
        }
        Enable-ServiceSas
        Install-GamepadDriver
        Start-Shell
    } catch {
        $err = $_
        Write-Warning "Installation failed: $($err.Exception.Message). Removing what was installed."
        if ($serviceCreated) { try { Remove-ShellService } catch { Write-Warning "Could not remove $ServiceName ($($_.Exception.Message))." } }
        if ($firewallSet) { try { Remove-ShellFirewall } catch { Write-Warning "Could not remove the firewall rule ($($_.Exception.Message))." } }
        if ($createdDir -and (Test-Path -LiteralPath $InstallDir)) {
            try { Remove-Item -LiteralPath $InstallDir -Recurse -Force } catch { Write-Warning "Could not remove $InstallDir ($($_.Exception.Message))." }
        }
        throw $err
    } finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
    [pscustomobject]@{
        Action = 'Install'; InstallDir = $InstallDir; NewVersion = Get-ExeVersion $InstallDir
        WebUI = 'https://localhost:47990'; UninstallCommand = "& '$PSCommandPath' -Uninstall"
    }
    return
}

if ($migrate) {
    $oldExe = Join-Path $OldInstallDir 'sunshine.exe'
    $oldVersion = (Get-Item -LiteralPath $oldExe).VersionInfo.ProductVersion
    $oldStartType = $null
    $created = $false
    $oldStopped = $false
    $aside = $null
    try {
        if (-not $NoService) {
            $oldService = Get-Service -Name $OldServiceName -ErrorAction Stop
            $oldStartType = $oldService.StartType
            Write-Host "Stopping $OldServiceName..."
            if ($oldService.Status -ne 'Stopped') {
                Stop-Service -Name $OldServiceName -Force
                $oldService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
            }
            $oldStopped = $true
            Wait-ProcessGone 'sunshine'
        }

        # A Shell folder left without its service may hold settings changed since an earlier
        # migration; keep it aside instead of overwriting it
        if (Test-Path -LiteralPath $InstallDir) {
            $aside = $InstallDir + '.before-migration-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
            Write-Host "Keeping the existing $InstallDir as $aside"
            Move-Item -LiteralPath $InstallDir -Destination $aside
        }
        Write-Host "Moving settings and pairing from $OldInstallDir to $InstallDir"
        $null = New-Item -ItemType Directory -Path $InstallDir -Force
        Copy-OldConfig (Join-Path $OldInstallDir 'config') (Join-Path $InstallDir 'config')
        # The copied credentials\ would otherwise inherit read access for Users
        Initialize-ConfigFolder
        $oldDrivers = Join-Path $OldInstallDir 'drivers'
        if (Test-Path -LiteralPath $oldDrivers) {
            $null = New-Item -ItemType Directory -Path (Join-Path $InstallDir 'drivers') -Force
            Copy-Item -Path (Join-Path $oldDrivers '*') -Destination (Join-Path $InstallDir 'drivers') -Recurse -Force
        }
        Copy-ProgramFiles $newFiles $InstallDir
        if ((Get-ExeVersion $InstallDir) -ne $newVersion) { throw 'Installed shell.exe does not report the new version.' }

        if (-not $NoService) {
            Register-ShellService
            $created = $true
            Set-Service -Name $OldServiceName -StartupType Disabled
            Set-OldFirewall $false
            Set-ShellFirewall
            Enable-ServiceSas
        }

        $null = New-Item -ItemType Directory -Path (Split-Path $BackupRoot -Parent) -Force
        @{ MigratedUtc = [DateTime]::UtcNow.ToString('o'); From = $OldInstallDir; OldService = $OldServiceName; OldVersion = $oldVersion
           To = $InstallDir; NewVersion = $newVersion } | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path (Split-Path $BackupRoot -Parent) 'migration.json') -Encoding UTF8

        Start-Shell
    } catch {
        $err = $_
        Write-Warning "Migration failed: $($err.Exception.Message). Going back."
        # Each step on its own, so one failing does not skip the others; the error is reported in the end
        if ($created) {
            try { Remove-ShellService } catch { Write-Warning "Could not remove $ServiceName ($($_.Exception.Message))." }
        }
        # Put back the Shell folder that was kept aside, in place of the half-built one
        if ($aside -and (Test-Path -LiteralPath $aside)) {
            try {
                if (Test-Path -LiteralPath $InstallDir) { Remove-Item -LiteralPath $InstallDir -Recurse -Force }
                Move-Item -LiteralPath $aside -Destination $InstallDir
            } catch { Write-Warning "The earlier Shell folder stays at $aside ($($_.Exception.Message))." }
        }
        if ($oldStopped) {
            try {
                Remove-ShellFirewall
                Set-OldFirewall $true
                if ($oldStartType) { Set-Service -Name $OldServiceName -StartupType $oldStartType }
            } catch { Write-Warning "Could not restore the firewall rules or start type ($($_.Exception.Message))." }
            try { Start-Service -Name $OldServiceName }
            catch { Write-Warning "Could not start $OldServiceName ($($_.Exception.Message)); start it from Services." }
        }
        throw $err
    } finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
    [pscustomobject]@{
        Action = 'Migrate'; From = $OldInstallDir; InstallDir = $InstallDir; OldVersion = $oldVersion; NewVersion = Get-ExeVersion $InstallDir
        OldService = "$OldServiceName (disabled, kept)"; RevertCommand = "& '$PSCommandPath' -RevertMigration"
        CleanupCommand = "& '$PSCommandPath' -RemoveOldInstall"
    }
    return
}

# --- Update ---
if ($serviceMissing) {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    throw "Shell is installed at $InstallDir but $ServiceName is missing. Move that folder away (or run -Uninstall) and install again."
}
$oldVersion = Get-ExeVersion $InstallDir

$backup = Join-Path $BackupRoot "$stamp-$($oldVersion -replace '[^0-9A-Za-z.\-]', '_')"
if (Test-Path -LiteralPath $backup) { throw "Backup folder already exists: $backup" }

Protect-BackupRoot

$stopped = $false
try {
    Stop-Shell
    $stopped = $true

    Write-Host "Backing up $InstallDir to $backup"
    $null = New-Item -ItemType Directory -Path (Join-Path $backup 'files') -Force
    Copy-Item -Path (Join-Path $InstallDir '*') -Destination (Join-Path $backup 'files') -Recurse -Force
    $srcCount = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -File -Force).Count
    $bakCount = @(Get-ChildItem -LiteralPath (Join-Path $backup 'files') -Recurse -File -Force).Count
    if ($bakCount -ne $srcCount) { throw "Backup incomplete: $bakCount of $srcCount files copied." }
    @{
        CreatedUtc = [DateTime]::UtcNow.ToString('o'); InstallDir = $InstallDir; OldVersion = $oldVersion
        NewVersion = $newVersion; Zip = $ZipPath; ZipSha256 = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
        FileCount = $bakCount
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'backup-manifest.json') -Encoding UTF8

    Write-Host "Installing $newVersion over $oldVersion"
    Copy-ProgramFiles $newFiles $InstallDir
    if ((Get-ExeVersion $InstallDir) -ne $newVersion) { throw 'Installed shell.exe does not report the new version.' }
    # Installations made before credentials\ was restricted get fixed here
    Protect-Credentials
    Enable-ServiceSas

    Start-Shell
} catch {
    $err = $_
    if ($stopped -and (Test-Path -LiteralPath (Join-Path $backup 'files\shell.exe'))) {
        Write-Warning "Update failed: $($err.Exception.Message). Restoring previous program files."
        try {
            Stop-Shell
            Remove-ProgramFiles $InstallDir
            Copy-ProgramFiles (Join-Path $backup 'files') $InstallDir
        } finally { Start-Shell }
    } elseif ($stopped) {
        Start-Shell
    }
    throw $err
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}

$pruned = @()
if ($KeepBackups -gt 0) {
    # Only folders this script created (they carry a manifest); timestamp prefix sorts chronologically.
    $all = @(Get-ChildItem -LiteralPath $BackupRoot -Directory |
        Where-Object { (Test-Path -LiteralPath (Join-Path $_.FullName 'backup-manifest.json')) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'files\shell.exe')) } |
        Sort-Object Name)
    if ($all.Count -gt ($KeepBackups + 1)) {
        $keep = @($all[0]) + @($all | Select-Object -Last $KeepBackups)
        foreach ($dir in $all) {
            if ($keep.FullName -contains $dir.FullName -or $dir.FullName -eq $backup) { continue }
            Remove-Item -LiteralPath $dir.FullName -Recurse -Force
            $pruned += $dir.Name
        }
    }
}

[pscustomobject]@{
    Action = 'Update'; InstallDir = $InstallDir; OldVersion = $oldVersion; NewVersion = Get-ExeVersion $InstallDir
    Backup = $backup; PrunedBackups = $pruned
    RollbackCommand = "& '$PSCommandPath' -Rollback -BackupPath '$backup'"
}
