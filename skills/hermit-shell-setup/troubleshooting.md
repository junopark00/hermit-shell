# Troubleshooting

Work through the checks in order and fix one thing at a time. The read-only checks need no
permission; anything that changes the system needs the user's yes first.

## Where things are on the host

| Item | Location |
|---|---|
| Shell log | `C:\Program Files\Shell\config\shell.log`, also in the web UI under **Troubleshooting > Logs** |
| Settings | `C:\Program Files\Shell\config\shell.conf` (change them in the web UI, **Settings**) |
| Paired devices | `C:\Program Files\Shell\config\shell_state.json` (manage them in **Pairing > Device Management**) |
| Session history | `C:\Program Files\Shell\config\session_history.jsonl` |
| Install backups | `C:\ProgramData\Shell\backups` |
| Service | `ShellService` |
| Firewall rule | `Shell` (inbound, TCP and UDP, for `shell.exe`) |
| Web UI | `https://localhost:47990` |

Client logs: Hermit for Windows writes `%TEMP%\Hermit-<number>.log` per run (the last ten are kept).
Hermit for Android keeps crash reports on the device (shown in its crash dialog).

Read the end of the Shell log (read-only):

```powershell
Get-Content 'C:\Program Files\Shell\config\shell.log' -Tail 80
```

Logs contain host names and addresses. Do not paste them anywhere public without the user's consent.

## The basics

```powershell
Get-Service ShellService | Select-Object Status, StartType
Get-NetFirewallRule -DisplayName Shell | Select-Object DisplayName, Enabled, Action
Get-NetTCPConnection -State Listen -LocalPort 47984,47989,47990,48010 -ErrorAction SilentlyContinue |
    Select-Object LocalAddress, LocalPort, OwningProcess
```

- Service stopped: ask, then start it (`Start-Service ShellService`, elevated) or restart the PC. If it
  stops again, read the log.
- Firewall rule missing or disabled: running the installer again for the same version is an update
  and does not re-add the rule; ask before adding it back (elevated):
  `New-NetFirewallRule -DisplayName Shell -Direction Inbound -Action Allow -Protocol TCP -Program 'C:\Program Files\Shell\shell.exe'`
  and the same with `-Protocol UDP`. Third-party security suites can have their own firewall; the
  user has to allow `shell.exe` there.
- Ports not listening: another program (Sunshine, Apollo, an older host) may hold them. Look for
  `Couldn't bind` in the log and for other streaming hosts in the service list.

## Symptoms

| Symptom | Likely cause | What to do |
|---|---|---|
| Web UI does not open at `https://localhost:47990` | Service not running; or the browser stopped at the certificate warning | Check the service; continue past the warning (self-signed certificate) |
| Web UI forgotten password | | See "Forgotten web UI password" below |
| Web UI "403" / refused from another device | **Web UI Access** allows the local network only (default), and the device is not on it | Open it on the host, on the home network, or over Tailscale. Do not set it to "Anyone" |
| Host does not appear in Hermit | mDNS blocked (guest Wi-Fi, client isolation, VPN, different subnet); or **Settings > General > Enable Auto Discovery** turned off | Add the host by IP address (**+**) |
| "Unable to connect" / host shows offline | Host asleep or off; service stopped; firewall; wrong address | Wake the PC, check the basics, check the address; from outside, see [remote-access.md](remote-access.md) |
| "No video received from host" | UDP ports blocked (firewall, router forwarding, a network that blocks UDP) | Check the firewall rule; for remote use check forwarding or use Tailscale |
| Pairing: "No device is waiting for a PIN" | Pairing not started on the client, or it timed out (about 5 minutes) | Select the host in Hermit again, then enter the new PIN |
| Pairing: client says the PIN was wrong | Typo, or PIN entered on another host | Select the host again for a new PIN; **Open Shell pairing page** fills it in |
| **Open Shell pairing page** does nothing / cannot connect | No browser on the device; or the web UI is not reachable from this device (from the internet it never is by default) | Open the web UI on the host and enter the PIN there |
| Clipboard does not sync | Device lacks **Clipboard Read** / **Clipboard Set** permission (for example paired with **Streaming and input** or **View only**) | Web UI **Pairing > Device Management**: turn the toggles on for that device, save |
| Files do not copy (Hermit for Windows) | Missing **File Upload** / **File Download** permission (plus clipboard) | Same place. Hermit for Android does not sync files at all |
| Remote shutdown/restart not offered or refused | Device lacks **Launch Apps** permission | Turn it on in Device Management, only for the user's own devices |
| Keyboard, mouse or gamepad do nothing | Device paired **View only**, or input toggles off; gamepads need the ViGEmBus driver (restart after install) | Check Device Management; restart the host after the installer reported a ViGEmBus restart |
| Black screen or wrong display | Virtual display problems; a monitor turned on during the session | Try again; per device **Always create virtual display** (Device Management); tray **Release Virtual Display**; see the Shell `docs/features.md` virtual display section |
| Stream stutters or breaks up | Network loss, Wi-Fi, relayed Tailscale connection, bitrate too high | Lower the bitrate; wired host; `tailscale ping <host>` shows "via DERP" for a relayed path; Hermit's overlay (Ctrl+Alt+Shift+S) shows loss and latency |
| Log says no working encoder found | GPU driver missing or too old; no hardware encoder | Update the GPU driver from the vendor; without a hardware encoder Shell uses software encoding |
| Host unreachable after some time away | PC went to sleep | Ask whether to change the sleep settings on the host; Wake-on-LAN works from the home network only |

## Forgotten web UI password

The user sets a new user name and password; they type both themselves.

If they still know the current password, they change it in the web UI under **Change Password**.

If it is lost, `shell.exe --creds <username> <password>` writes new web UI credentials (documented in
Shell's [tools guide](https://github.com/junopark00/hermit-shell/blob/main/docs/tools.md#resetting-the-web-ui-password)).
It replaces only the stored user name and password hash in `config\shell_state.json`; paired devices
and settings stay. Shell reads the credentials at start, so the service is stopped first and started
again after. Let the user run this themselves in an elevated PowerShell window; the `Read-Host`
prompts keep the password out of the command history and out of your view:

```powershell
Stop-Service ShellService
$user = Read-Host 'New web UI user name'
$secret = Read-Host 'New web UI password' -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secret)
try {
    & "$env:ProgramFiles\Shell\shell.exe" --creds $user ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr))
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
}
Start-Service ShellService
```

Tell the user to avoid double quotes (`"`) in the new password (Windows PowerShell 5.1 does not pass
them to programs reliably). Then the user signs in at `https://localhost:47990` with the new credentials. If Shell was installed
somewhere other than `C:\Program Files\Shell`, adjust the path.

## Starting over

- Update or repair: run the installer of the latest release again (a backup is made first; settings
  and pairing are kept).
- Roll back: `.\Install-Shell.ps1 -Rollback -BackupPath C:\ProgramData\Shell\backups\<folder>`
  (the update prints the exact command; the web UI home page can copy it).
- Uninstall: `.\Install-Shell.ps1 -Uninstall` (add `-RemoveDriver` to also remove the virtual display
  driver and its certificate). The installation folder, with settings and pairing, is moved to
  `C:\ProgramData\Shell\backups`.

All of these change the system and need an elevated PowerShell and the user's yes.
