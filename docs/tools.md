# Tools

The `shell` folder of the repository holds PowerShell tools for installing, updating, diagnosing and
backing up a Shell host, and for checking connections and stream quality. Run them from the repository
root (or the extracted source archive of a release).

| File | Purpose | PowerShell |
|---|---|---|
| `Install-Shell.ps1` | Install, update, roll back, uninstall; migrate from Apollo | Windows PowerShell 5.1 |
| `Build-Shell.ps1` | Build from source with MSYS2, portable ZIP | 5.1 |
| `Update-Shell.ps1` | Build and install in one step | 5.1 |
| `ShellRecovery.psm1`, `Invoke-ShellRecovery.ps1` | Diagnostics report, encrypted settings backup and restore | 7.4 or later |
| `ShellConnect.psm1`, `Invoke-ShellConnect.ps1` | Connection checks, Wake-on-LAN, readiness wait | 5.1 or 7 |
| `Get-HermitStats.ps1` | Session statistics from client logs | 5.1 |
| `Test-*.ps1`, `tests/` | Tests for the tools above | |

If PowerShell refuses to run a downloaded script, start it with
`powershell -NoProfile -ExecutionPolicy Bypass -File <script> ...`.

## Install-Shell.ps1

Run it from an **elevated** PowerShell (Run as administrator). Each release attaches
`Install-Shell.ps1` next to `Shell-<version>.zip`; in the source tree it is `shell\Install-Shell.ps1`.
The examples below assume the script and the ZIP are in the current folder.

### Install and update

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Shell.ps1 -ZipPath .\Shell-1.1.0.zip
```

The ZIP must contain a single top folder `Shell` with `shell.exe`; anything else is rejected before
any change is made.

**First installation** (Shell is not installed yet):

1. Copies the program files and the `drivers` folder to `C:\Program Files\Shell` and creates
   `config\credentials`, readable by Administrators and SYSTEM only (Users may list it).
2. Installs the SudoVDA virtual display driver. Its installer adds the driver's self-signed certificate
   (`sudovda.cer`) to the Local Machine Trusted Root Certification Authorities and Trusted Publishers
   stores, then creates the device with `nefconc.exe`.
3. Adds the inbound firewall rule `Shell` for `shell.exe` (TCP and UDP).
4. Registers `ShellService` (automatic start) and allows services to send Ctrl+Alt+Del.
5. Runs the ViGEmBus installer for virtual gamepads quietly (`/quiet /norestart`) and waits for it;
   if it needs a restart, a warning says so (`-NoGamepadDriver` skips it).
6. Starts the service.

If a step fails, the service, the firewall rule and the new folder are removed again; installed drivers
stay.

**Update** (Shell is installed):

1. Stops the service.
2. Copies the whole installation, including `config`, to
   `C:\ProgramData\Shell\backups\<yyyyMMdd-HHmmss>-<old version>` and checks the file count.
3. Replaces the program files. `config` and `drivers` are never touched, so settings, pairing and the
   virtual display driver stay as they are.
4. Starts the service. If anything fails after the service was stopped, the previous program files are
   restored and the service is started again.

**Permissions.** `config\credentials` holds the private keys and the backup folder holds copies of
them, so neither inherits permissions: only Administrators and SYSTEM can read them. An update applies
this to installations and backup folders made before (which inherited read access for Users from
Program Files and ProgramData), and folders moved into the backups (uninstall, `-RemoveOldInstall`)
are reset to inherit the backup folder's permissions.

`-KeepBackups N` deletes older backups after a successful update, keeping the oldest one (the original
installation) and the newest N. Folders without a `backup-manifest.json` are never touched.

### Roll back

```powershell
.\Install-Shell.ps1 -Rollback -BackupPath C:\ProgramData\Shell\backups\<folder>
```

Restores the program files of a backup. Add `-RestoreConfig` to restore the settings of that backup as
well. The update prints the exact command (`RollbackCommand`), and the web UI home page can copy it.

### Uninstall

```powershell
.\Install-Shell.ps1 -Uninstall [-RemoveDriver]
```

Stops and removes `ShellService` and the firewall rule, puts back the NVIDIA driver settings Shell
changed, and moves the installation folder, with settings, pairing and certificates, to
`C:\ProgramData\Shell\backups\uninstalled-<time>`. `-RemoveDriver` also removes the SudoVDA device,
its driver package from the DriverStore (`pnputil /delete-driver`) and its certificate from both
certificate stores. The ViGEmBus driver stays; remove it from
**Apps & features** if nothing else uses it. The Ctrl+Alt+Del policy is left as it is.

### Migrating from Apollo

```powershell
.\Install-Shell.ps1 -ZipPath .\Shell-1.1.0.zip -MigrateFromApollo
```

For a PC with Apollo installed (`ApolloService`, `C:\Program Files\Apollo`). A first installation
stops when it finds Apollo, because both use the same ports; migration only runs when asked for:

1. Stops `ApolloService` and sets it to Disabled (it is not deleted).
2. Copies `config` (settings, pairing, app list, covers, certificates) and `drivers` to Shell, renaming
   `sunshine.conf` to `shell.conf`, `sunshine_state.json` to `shell_state.json` and the key
   `sunshine_name` to `shell_name`. Paths into the Apollo folder and the `APOLLO_APP_*` and
   `APOLLO_CLIENT_*` variables in commands are updated. Paired devices keep working.
3. Installs Shell, registers and starts `ShellService`, disables the `Apollo` firewall rules and adds
   the `Shell` rules.

If a step fails, Apollo is restored and started again. Afterwards:

- `.\Install-Shell.ps1 -RevertMigration` goes back to Apollo (Shell's folder is kept aside;
  changes made in Shell are not carried back).
- `.\Install-Shell.ps1 -RemoveOldInstall`, once Shell works, deletes the Apollo service, firewall
  rules, Start menu folder, Apps & features entry and PATH entries, and moves the Apollo folder into
  the backups.

### Options for testing

`-NoService` skips the administrator check and every change outside the given folders (service,
firewall, drivers, certificates); with `-InstallDir`, `-BackupRoot` and `-OldInstallDir` it runs
against scratch folders. `-SasPolicyKey` points the Ctrl+Alt+Del policy step at another registry key.
`shell\Test-InstallShell.ps1` uses these.

## Build-Shell.ps1 and Update-Shell.ps1

```powershell
# Build and create the portable ZIP (build\cpack_artifacts\Shell.zip)
.\shell\Build-Shell.ps1 -Package

# Build and install in one step (elevated): skips the install when the version is unchanged
.\shell\Update-Shell.ps1
```

`Update-Shell.ps1` checks for an elevated PowerShell first, builds, and installs with
`Install-Shell.ps1` (keeping the original backup and the newest five; `-KeepBackups` changes that). A
failed build never touches the installed service. See [building.md](building.md) for the prerequisites.

## Diagnostics and configuration backups

Requires PowerShell 7.4 or later.

```powershell
.\shell\Invoke-ShellRecovery.ps1 -Action Diagnose
```

The report lists the Windows version, GPUs and drivers, network adapters and link speeds, the Shell
version and service state, which configuration files exist, and the state of the Tailscale service if
present. It does not record public IP addresses, host names, MAC addresses, settings or certificates.

**Full backup** (settings, app list, pairing state, certificates and the cover images in
`config\covers`; may need an elevated PowerShell):

```powershell
.\shell\Invoke-ShellRecovery.ps1 -Action Backup -Path D:\Backups\shell.apbackup -Portable
```

`-Portable` asks for a password and encrypts with AES-256-GCM; the key is derived with PBKDF2-SHA256
(random salt, 600,000 iterations). Such a backup can be restored after reinstalling Windows or under
another account. A forgotten password cannot be recovered. Never put the password on the command line.

Without `-Portable` the backup is protected with Windows DPAPI for the current user. That is meant for
local recovery only; **do not rely on it to survive a Windows reinstall.**

**Settings only** (`shell.conf` and `apps.json`, no pairing or certificates):

```powershell
.\shell\Invoke-ShellRecovery.ps1 -Action Backup -SettingsOnly -Path D:\Backups\settings.apbackup
```

**Restore** into a new folder:

```powershell
.\shell\Invoke-ShellRecovery.ps1 -Action Recover -Path D:\Backups\shell.apbackup -Destination D:\Backups\recovered -Portable
```

The destination must not exist; it is created readable by the current account only. Paths, duplicates,
hashes and the encryption are verified before anything is written. Existing files are never
overwritten and nothing is applied to the host automatically: copy the files into
`C:\Program Files\Shell\config` yourself with the service stopped.

Limits:

- Configurations that point to files elsewhere (`file_apps`, `file_state`, `pkey`, `cert`,
  `credentials_file`) are rejected rather than backed up incompletely.
- Logs, the session history (`session_history.jsonl`), drivers, games, external scripts and firewall
  or router settings are not backed up.
- Only `.png` files directly in `config\covers` are backed up; app images elsewhere are not, and cover
  names with characters other than letters, digits and `_ . ~ % -` are skipped with a warning.
- A backup holds at most 1000 files and 64 MB (16 MB per file); larger configurations are rejected.
- Avoid backups while settings are being changed; changes during the copy are detected, but the files
  are not an atomic snapshot of a running service.
- Symbolic links and junctions are not supported.
- A disk error during a restore can leave some files in the new folder.

## Connection checks and Wake-on-LAN

`ShellConnect.psm1` and `Invoke-ShellConnect.ps1` work in Windows PowerShell 5.1 and PowerShell 7.

Wake-on-LAN in Moonlight-based clients sends a magic packet (6 bytes `FF` and the MAC address 16 times,
102 bytes) to UDP ports 9 and 47009 and, for the default base port, 47998, 47999, 48000, 48002 and
48010, addressed to every known host address, 255.255.255.255 and each interface's subnet broadcast.
The MAC address is only sent in the paired HTTPS `serverinfo` answer; plain HTTP answers carry the
placeholder `00:00:00:00:00:00`. These tools use the same ports, so their results compare directly
with client behavior.

```powershell
# Is the host answering? (HTTP serverinfo, HTTPS port, address classification; read-only)
.\shell\Invoke-ShellConnect.ps1 -Action Probe -Target 192.168.0.10
.\shell\Invoke-ShellConnect.ps1 -Action Probe -Target host.example.com

# On the host: adapter Wake-on-LAN settings, wake from S5, Fast Startup (read-only)
.\shell\Invoke-ShellConnect.ps1 -Action Readiness

# Wake, wait until ready, report. -LanAddress is only used to tell whether this machine is on the host's subnet.
.\shell\Invoke-ShellConnect.ps1 -Action Sequence -Target host.example.com -MacAddress AA:BB:CC:DD:EE:FF `
    -LanAddress 192.168.0.10 -Scenario LongOffline -NoBroadcast -TimeoutSec 240 -OutputPath wake.json
```

The `Verdict` of a sequence:

- `AlreadyOnline`: the host answered before the wake; nothing about Wake-on-LAN was tested.
- `WokeOnLan`: a broadcast was used, or the packet was sent from the same subnet or to a private
  address. **Not proof that waking over the internet works.**
- `WokeUnknownPath`: the host woke, but the sender's location is unknown (no `-LanAddress`). Sending to
  a public address from inside the host's local network only tests NAT loopback.
- `WokeViaRemotePath`: sent only directly to non-private addresses from outside the host's subnet, and
  the host answered. The only result that shows remote wake works.
- `WakeNotConfirmed`: no answer in time. Lost packets, an unsupported power state and a slow boot are
  not told apart.

To verify remote wake, run `-Scenario JustShutDown` and `-Scenario LongOffline` from a device outside
the local network. `Sent` only means the local socket accepted the datagram. `Readiness` reads the
driver's registry keywords (`*WakeOnMagicPacket`, `S5WakeOnLan`, ...) because
`Get-NetAdapterPowerManagement` fails with some drivers; BIOS/UEFI settings and router forwarding are
not checked.

## Stream statistics

`Get-HermitStats.ps1` extracts the "Global video stats" blocks that Hermit and moonlight-qt write to
their log at the end of each session, as a table and JSON. The Windows clients keep their logs as
`%TEMP%\Hermit-<number>.log` or `%TEMP%\Moonlight-<number>.log` and only keep the last ten, so collect
them soon after a test. Android and iOS clients do not write these logs.

```powershell
# On the client PC: all recent logs, labeled "baseline"
.\shell\Get-HermitStats.ps1 -Label baseline -OutputPath stats-baseline.json

# One log file
.\shell\Get-HermitStats.ps1 -Path $env:TEMP\Hermit-1790000000.log
```

Each record holds the block's end time, bitrate, packet size and loss counts, resolution, codec and
frame rates (received, decoded, rendered), host processing latency (min, max, average), frames dropped
by the network and by jitter, round-trip time and variance, average decode, queue and render times,
unrecoverable frames, packets lost beyond error correction (median and 90th percentile) and key frame
(IDR) requests.

To compare two settings, run each for at least 30 minutes with the same game, scene, client and
network, and look at loss, jitter drops and latency, not just the average frame rate. The block is only
written when a session ends, so end the stream after each run.

## Tests

```powershell
.\shell\Test-InstallShell.ps1     # PowerShell 5.1 or later
.\shell\Test-ShellConnect.ps1     # PowerShell 5.1 or later
.\shell\Test-HermitStats.ps1      # PowerShell 5.1 or later
.\shell\Test-ShellRecovery.ps1    # PowerShell 7.4 or later
```

- `Test-InstallShell.ps1` runs the installer with `-NoService` against fake installations in a
  temporary folder: ZIP validation, first installation, update scope, protected `config` and `drivers`,
  backups, rollback, pruning, uninstall, the Ctrl+Alt+Del policy step (on a scratch registry key) and
  the Apollo migration. The real installation and service are never touched.
- `Test-ShellConnect.ps1` uses a loopback-only mock `serverinfo` server and UDP listener: magic packet
  layout, address classification, XML parsing (DTDs rejected), answer validation, readiness after a
  delayed start, timeouts and verdict rules. Packets go to 127.0.0.1 only.
- `Test-HermitStats.ps1` checks synthetic and real log formats and both log file names.
- `Test-ShellRecovery.ps1` checks byte-exact restores for both encryption modes, wrong passwords,
  tampering, path traversal, duplicates, hash mismatches, existing-file protection, covers (and the
  files that are left out), settings-only backups and rejected custom paths.
- `shell/tests/clipboard_image_test.cpp` checks the clipboard image conversion, path checks and the
  file archive encoding with synthetic data (the build command is at the top of the file).
