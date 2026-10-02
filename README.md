# Shell

**Shell is a self-hosted game-streaming host for Windows.** It streams your desktop, games and
applications to [Hermit](https://github.com/junopark00/hermit) (Windows) and
[Hermit for Android](https://github.com/junopark00/hermit-android), and to any other client that speaks
the GameStream protocol, such as [Moonlight](https://moonlight-stream.org).

Shell is a fork of [Sunshine](https://github.com/LizardByte/Sunshine) by way of
[Apollo](https://github.com/ClassicOldSong/Apollo). It keeps their low-latency streaming core and adds
a virtual display that follows the client, live bitrate changes, remote power control, a status
dashboard and a safer installer.

[한국어](README.ko.md)

| Dashboard | Applications | Pairing |
|---|---|---|
| ![Shell dashboard](docs/images/shell-dashboard.png) | ![Applications](docs/images/shell-applications.png) | ![Pairing](docs/images/shell-pairing.png) |

## Highlights

- **Hardware-accelerated streaming** with NVIDIA NVENC, AMD AMF and Intel Quick Sync (H.264, HEVC and AV1
  where the GPU supports them), with software encoding as a fallback. HDR and up to 4:4:4 chroma on
  supported hardware.
- **Virtual display** (SudoVDA): each session can get a display that matches the client's resolution and
  refresh rate, also on a PC without a monitor. When a client resumes at another size, the virtual display
  is resized (or recreated) to match. When a physical monitor is turned on, the desktop is handed back to
  it automatically.
- **Live bitrate changes** while streaming, without restarting the stream (NVIDIA encoders).
- **Remote shutdown and restart** from the client, with a warning when other devices are connected.
- **Clipboard sync**: text in both directions; images and files with Hermit.
- **Ctrl+Alt+Del** from the client opens the Windows secure screen (sent by the Shell service).
- **Per-device permissions** for input, clipboard, file transfer, app launch and viewing.
- **Status dashboard** in the web UI: active streams with resolution, bitrate, round-trip time and frame
  recovery requests, paired devices, session history and install backups.
- **Adaptive packet pacing** that spreads each video frame over time, which reduces burst loss on
  internet connections.
- **Safe installer**: every update makes a full backup of the installation, a failed update restores the
  previous files automatically, and any backup can be rolled back with one command.
- **Recovery tools**: diagnostics report and encrypted backup and restore of settings, pairing and
  certificates.
- **English and Korean** user interface: the web UI follows its language setting (other translations
  inherited from upstream are partial); the tray and notifications are in Korean when Windows' system
  display language is Korean, and in English otherwise.

## Compatibility

Shell works with any GameStream client. Some features need client support:

| Feature | Hermit | Moonlight and other GameStream clients |
|---|---|---|
| Streaming, input, gamepads, audio, HDR | Yes | Yes |
| Virtual display at the client's resolution | Yes | Yes |
| Text clipboard sync | Yes | Clients with the Apollo clipboard extension |
| Image and file clipboard | Yes | No |
| Live bitrate change | Yes | No (reconnect to change the bitrate) |
| Remote shutdown and restart | Yes | No |
| Ctrl+Alt+Del opens the secure screen | Yes | When the client sends the key combination |

Shell is **Windows only**. The Linux and macOS code inherited from upstream is still in the tree but is
neither maintained nor tested.

## Requirements

- Windows 10 or Windows 11, 64-bit (x64).
- A GPU with a hardware video encoder is strongly recommended: NVIDIA (NVENC), AMD (AMF) or Intel
  (Quick Sync), with a current driver. HEVC and AV1 need a GPU that encodes them. Without a hardware
  encoder, Shell falls back to software encoding, which needs a fast CPU.
- Administrator rights to install the service, the virtual display driver and the firewall rule.
- A wired network connection on the host is recommended.

## Installation

1. Download `Shell-1.1.0.zip` and `Install-Shell.ps1` from the
   [latest release](https://github.com/junopark00/hermit-shell/releases/latest) into the same folder.
   Leave the ZIP file as it is.
2. Open **PowerShell as administrator** in that folder and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install-Shell.ps1 -ZipPath .\Shell-1.1.0.zip
   ```

The installer copies Shell to `C:\Program Files\Shell`, installs the SudoVDA virtual display driver and
the ViGEmBus virtual gamepad driver, adds the firewall rule `Shell`, registers and starts `ShellService`,
and allows the service to send Ctrl+Alt+Del. Add `-NoGamepadDriver` to skip ViGEmBus.

Coming from Apollo? Add `-MigrateFromApollo` to move its settings, paired devices and apps to Shell.
Apollo is disabled, not deleted; see [docs/tools.md](docs/tools.md#migrating-from-apollo).

## First run and pairing

1. Open the web UI at **https://localhost:47990** on the host. The certificate is self-signed, so the
   browser shows a warning the first time; continue to the page.
2. Create the administrator user name and password.
3. In Hermit or Moonlight, add the PC (it is usually found automatically on the local network). The
   client shows a four-digit PIN.
4. Enter the PIN on the **Pairing** page of the web UI (or click the pairing notification of the Shell
   tray icon). Give the device a name you will recognize and choose its permissions beside the PIN:
   **Full access** (the default), **Streaming and input** (apps, viewing, launching and all inputs, but
   no clipboard, file transfer or server commands), **View only** or **Custom** with the individual
   toggles. The page remembers the last choice. The client waits about 5 minutes for the PIN; after
   that, start pairing again on the client.

The permissions of a paired device can be changed at any time under **Device Management** on the same
page. Hermit and Hermit for Android open the Pairing page with the PIN and device name already filled
in; you only check the permissions and press **Pair**.

## Configuration

All settings are in the web UI under **Settings**. They are stored in
`C:\Program Files\Shell\config\shell.conf`, next to the paired devices (`shell_state.json`), the app
list (`apps.json`) and the log (`shell.log`).

Settings specific to Shell:

- **Video Packet Pacing (Mbps)** (`video_pacing_mbps`): the rate at which the packets of one video frame
  are sent. `0` (default) picks three times the stream bitrate, between 100 and 800 Mbps. `800` matches
  upstream behavior and gives the lowest latency on a clean LAN.
- **Release the virtual display when a monitor turns on** (`virtual_display_release_on_monitor`, on by
  default).

See [docs/features.md](docs/features.md) for the details of each feature.

## Updating, rollback and uninstall

Run these from an elevated PowerShell in the folder with `Install-Shell.ps1` (use the script of the
release you install; in the source tree it is `shell\Install-Shell.ps1`). If PowerShell refuses to run
it, start it with `powershell -ExecutionPolicy Bypass -File .\Install-Shell.ps1 ...`.

```powershell
# Update: full backup, then the new program files (settings, pairing and drivers are kept)
.\Install-Shell.ps1 -ZipPath .\Shell-<version>.zip

# Roll back to a backup (the update prints the exact command)
.\Install-Shell.ps1 -Rollback -BackupPath C:\ProgramData\Shell\backups\<folder>

# Uninstall; -RemoveDriver also removes the virtual display driver, its driver package and certificate
.\Install-Shell.ps1 -Uninstall -RemoveDriver
```

Uninstalling moves the installation folder, including settings and pairing, to
`C:\ProgramData\Shell\backups`, where you can delete it. That folder, like `config\credentials`, is
readable by Administrators and SYSTEM only, because it holds copies of the private keys.

## Documentation

- [Features](docs/features.md): virtual display, power control, live bitrate, clipboard, session history.
- [Tools](docs/tools.md): installer, diagnostics and backups, connection checks and Wake-on-LAN, stream
  statistics.
- [Building](docs/building.md): building Shell from source.
- [Contributing](CONTRIBUTING.md) and [Security policy](SECURITY.md).

## Building from source

Shell is built on Windows with MSYS2 (UCRT64) and the official Node.js:

```powershell
git clone --recurse-submodules https://github.com/junopark00/hermit-shell.git
cd hermit-shell
.\shell\Build-Shell.ps1 -Package
```

The portable package is written to `build\cpack_artifacts\Shell.zip`. See
[docs/building.md](docs/building.md) for the prerequisites.

## Privacy and network

Shell has no telemetry, analytics or automatic update checks. It talks to the clients you pair, and
otherwise only to:

- **Game covers**: when you search for a cover in the **Apps** page, your browser loads the game list
  from the LizardByte GameDB (`raw.githubusercontent.com`) and cover images from IGDB
  (`images.igdb.com`). When you pick a cover, the host downloads that image from IGDB.
- **UPnP** (off by default): when enabled, Shell asks your router to forward its ports.

Building Shell downloads the npm packages of the web UI, the ViGEmBus installer from its GitHub release
and, when they are not installed, the Boost and nlohmann/json sources.

## Security notes

- **Private keys**: `config\credentials` and the install backups in `C:\ProgramData\Shell\backups`
  are readable by Administrators and SYSTEM only. Updates also fix installations made before this.
- **Certificates**: the SudoVDA driver is signed with its developer's self-signed certificate. The
  installer adds `sudovda.cer` to the Local Machine **Trusted Root Certification Authorities** and
  **Trusted Publishers** stores so that Windows accepts the driver. `Install-Shell.ps1 -Uninstall
  -RemoveDriver` removes it again.
- **Remote shutdown and restart** need the app launch permission of the device. Grant it only to devices
  you own.
- **Ctrl+Alt+Del**: the installer sets the `SoftwareSASGeneration` policy so that services may send the
  secure attention sequence. Uninstalling leaves the policy in place; remove it under
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System` if you no longer need it.
- **Web UI**: protected by the user name and password you create, served over HTTPS and by default
  reachable from the local network only. Keep it that way, and pair only devices you trust.
- Report vulnerabilities privately, as described in [SECURITY.md](SECURITY.md).

## License and credits

Shell is free software under the [GNU General Public License v3.0](LICENSE).

Shell builds on the work of:

- [Sunshine](https://github.com/LizardByte/Sunshine) by LizardByte and contributors.
- [Apollo](https://github.com/ClassicOldSong/Apollo) by ClassicOldSong (Yukino Song) and contributors.
- [Moonlight](https://moonlight-stream.org) and moonlight-common-c by the Moonlight project.
- [SudoVDA](https://github.com/SudoMaker/SudoVDA) by SudoMaker and [nefcon](https://github.com/nefarius/nefcon)
  and ViGEm by Nefarius.

Third-party components and their licenses are listed in [NOTICE](NOTICE).

## Disclaimer

Shell is an independent project. It is not affiliated with, endorsed by or sponsored by NVIDIA,
LizardByte or the Moonlight project. GameStream is a trademark of NVIDIA Corporation. All other
trademarks belong to their respective owners.
