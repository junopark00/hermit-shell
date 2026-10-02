---
name: hermit-shell-setup
description: Guides a non-technical user, step by step, through setting up Shell (a self-hosted game-streaming host for Windows) on their PC and connecting the Hermit (Windows) or Hermit for Android client, including pairing and reaching the PC from outside the home network (Tailscale, UPnP or port forwarding). Use when someone wants to install, pair, stream with, or remotely reach a Shell host, or troubleshoot such a setup.
---

# Shell and Hermit guided setup

You are helping a person set up **Shell**, a game-streaming host for Windows
(<https://github.com/junopark00/hermit-shell>), and connect a client to it:
**Hermit** for Windows (<https://github.com/junopark00/hermit>) or **Hermit for Android**
(<https://github.com/junopark00/hermit-android>). Shell is a fork of Sunshine and Apollo; Moonlight
and other GameStream clients also work with it.

Reference files in this folder:

- [remote-access.md](remote-access.md): the three ways to reach the PC from outside the home network,
  with the exact port list.
- [troubleshooting.md](troubleshooting.md): symptoms, checks and fixes.

## How to work with the user

- **Speak the user's language** (answer in the language they write in) and use plain words. Explain
  a technical term the first time you use it, in one short sentence.
- **One step at a time.** Say what the step does and why, do it (or tell the user exactly what to
  click), then **check that it worked** before moving on. If a check fails, stop and fix it with
  [troubleshooting.md](troubleshooting.md) before continuing.
- **Ask before anything that changes the system, the network, the router or an account**:
  downloading files, installing or uninstalling software, starting the installer, changing Windows
  power or firewall settings, changing Shell settings, signing in to a service. Read-only checks
  (listing services, reading a log, looking at settings) do not need permission, but say what you
  are looking at.
- **Never handle the user's passwords.** The user types every password (web UI, Windows, Tailscale or
  router account) themselves, directly into the page or prompt that asks for it. Never ask them to
  tell you a password, never put one on a command line, in a file or in a URL, and never read one
  back. If a step would need a password from you, tell the user what to type and where instead.
- **Never weaken security without explaining it first** and getting a clear yes: turning off the
  firewall, enabling UPnP, forwarding ports, granting a device more permissions, lowering encryption.
  Prefer the option that changes the least.
- **Never forward or expose the web UI port (47990 by default)** to the internet, and never set
  **Web UI Access** (`origin_web_ui_allowed`) to "Anyone". The web UI controls the whole PC.
- **You do not change router settings yourself.** If the router needs a change, explain what to look
  for and let the user do it in the router's own page or app.
- Accept the UAC prompt only through the user: elevation always needs the user to click **Yes** in
  Windows' own dialog.
- Do not guess. If a check gives an unexpected result, say so and look it up in the logs or the
  project documentation before trying something else.

Keep a short running summary for the user: what is done, what is next.

## Step 1: Check the PC (the host)

Shell runs on the PC that has the games or desktop to stream (the **host**).

1. **Windows 10 or 11, 64-bit (x64).** Read-only check:

   ```powershell
   Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, OSArchitecture
   ```

   ARM PCs and 32-bit Windows are not supported.
2. **GPU with a hardware video encoder** (strongly recommended): NVIDIA (NVENC), AMD (AMF) or Intel
   (Quick Sync), with a current driver. Without one, Shell falls back to software encoding, which needs
   a fast CPU.

   ```powershell
   Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
   ```

   You cannot be fully sure from the name alone; after installation Shell's log names the encoder it
   found (step 3).
3. **The user is an administrator** on this PC (needed to install the service and drivers):

   ```powershell
   whoami /groups | Select-String 'S-1-5-32-544'
   ```

   A line with `S-1-5-32-544` (the Administrators group) means yes, even if it says "Group used for
   deny only" (that only means this window is not elevated). No line: the user needs an administrator
   account; stop and explain.
4. **Another streaming host already installed?** Shell uses the same ports as Sunshine and Apollo.

   ```powershell
   Get-Service | Where-Object { $_.Name -match 'Apollo|Sunshine|Shell' } | Select-Object Name, Status, StartType
   Test-Path 'C:\Program Files\Apollo\sunshine.exe'
   ```

   - **Apollo** (`C:\Program Files\Apollo`): the installer stops on a first installation and asks for
     `-MigrateFromApollo` (step 2).
   - **Sunshine or another host**: the installer does not detect or migrate it, but the ports would
     clash. Ask the user whether to uninstall it (from Apps & features) or at least stop it and set it
     to Disabled before installing Shell.
   - **Shell already installed** (`ShellService`): running the installer again is an update; settings
     and pairing are kept.
5. A **wired network connection** on the host is recommended.

## Step 2: Download and install Shell

Ask the user before downloading. Each release attaches two files: `Shell-<version>.zip` and
`Install-Shell.ps1`. Do not hard-code a version; find the latest release:

- In a browser: <https://github.com/junopark00/hermit-shell/releases/latest> (redirects to the newest
  release), or
- From PowerShell (read-only):

  ```powershell
  $rel = Invoke-RestMethod https://api.github.com/repos/junopark00/hermit-shell/releases/latest
  $rel.tag_name
  $rel.assets | Select-Object name, size, browser_download_url, digest
  ```

Download **both** files into the **same folder** (for example a new folder in Downloads). Leave the ZIP
as it is; do not extract it. If the API lists a `digest` (`sha256:...`) for an asset, compare it with
`Get-FileHash <file> -Algorithm SHA256`.

**Run the installer elevated.** Tell the user that Windows will show a UAC prompt ("Do you want to
allow this app to make changes?") and that they must click **Yes** themselves. Either the user opens
**PowerShell as administrator** (Start menu, type PowerShell, "Run as administrator"), goes to the
folder and runs:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Shell.ps1 -ZipPath .\Shell-<version>.zip
```

or you start the same command in an elevated window, with full paths, after the user agrees
(`-NoExit` keeps the window open so the user can read the result):

```powershell
Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -NoExit -ExecutionPolicy Bypass -File "C:\path\Install-Shell.ps1" -ZipPath "C:\path\Shell-<version>.zip"'
```

Options (only when they apply, and explain them first):

- `-MigrateFromApollo`: only for a PC with Apollo installed and Shell not yet installed. It stops
  `ApolloService` and sets it to **Disabled** (not deleted), copies Apollo's settings, paired devices,
  app list, covers and certificates to Shell (renaming the files), installs Shell, turns off the
  `Apollo` firewall rules and adds the `Shell` rule. Paired devices keep working. If a step fails,
  Apollo is restored. Later, `-RevertMigration` goes back to Apollo, and `-RemoveOldInstall` (once
  Shell works) removes the old Apollo service, firewall rules and entries.
- `-NoGamepadDriver`: skip the ViGEmBus virtual gamepad driver (gamepads from clients will not work).

What a first installation does (tell the user in plain words):

1. Copies Shell to `C:\Program Files\Shell` and creates `config\credentials` (private keys), readable
   by Administrators and SYSTEM only.
2. Installs the SudoVDA virtual display driver. Its installer adds the driver's self-signed certificate
   to the Local Machine Trusted Root and Trusted Publishers stores (this is how Windows accepts the
   driver; uninstalling with `-RemoveDriver` removes it).
3. Adds the inbound Windows Firewall rule **Shell** for `shell.exe` (TCP and UDP). No router or port
   changes are made.
4. Registers the **ShellService** service (starts automatically) and allows services to send
   Ctrl+Alt+Del.
5. Installs the ViGEmBus gamepad driver quietly (a restart may be requested for gamepads).
6. Starts the service. If a step fails, the service, firewall rule and new folder are removed again.

The installer prints a summary with `WebUI = https://localhost:47990`. **Check** (read-only, no
elevation needed):

```powershell
Get-Service ShellService | Select-Object Status, StartType
Get-NetFirewallRule -DisplayName Shell | Select-Object DisplayName, Enabled, Direction, Action
Select-String -Path 'C:\Program Files\Shell\config\shell.log' -Pattern 'Found .* encoder' | Select-Object -Last 3
```

The service should be `Running`. The log lines name the encoders found (for example `[nvenc]`,
`[amdvce]`, `[quicksync]` or `[software]`). If the log is missing or says no working encoder was
found, go to [troubleshooting.md](troubleshooting.md).

## Step 3: First web UI login

On the host, open **https://localhost:47990** in a browser.

- The browser warns that the connection is not private. That is expected: Shell uses a self-signed
  certificate. The user chooses to continue (for example "Advanced" and "Continue to localhost").
- On the first visit Shell asks to **create** a user name and password for the web UI. The user
  chooses and types them; you never see them. Suggest a password manager or writing it down safely:
  the web UI cannot show it again.
- After that the browser asks for these credentials on each visit.

Optional, with the user's consent: the host name shown to clients is **Settings > General > Host
Name**. The web UI language follows its own setting under Settings.

## Step 4: Install a client and pair

Ask which device the user wants to stream **to**.

**Hermit for Windows** (on the other PC): download from
<https://github.com/junopark00/hermit/releases/latest> either `Hermit-<version>.exe` (single file) or
`Hermit-x64-<version>.zip` (extract and run `Hermit.exe`). No installer and no administrator rights
needed. If Moonlight is installed for the same Windows user, Hermit copies its paired hosts and
settings once on its first run.

**Hermit for Android** (phone, tablet, Android TV; Android 5.0 or later): on the device, open
<https://github.com/junopark00/hermit-android/releases/latest> in the browser and download
`Hermit-android-<version>.apk`. Opening it asks the user to **allow installs from this source** (the
browser or file manager they opened it with); they turn that on for that app and install. They can
turn it off again afterwards.

**Pairing** (on the home network first; see step 6 for pairing from outside):

1. Open Hermit. The host usually appears by itself. Select (tap) it. Hermit shows a **four-digit PIN**.
2. In the PIN dialog, **Open Shell pairing page** opens `https://<host>:47990/pin` in the browser with
   the PIN and the device name already filled in. The certificate warning and the web UI login are
   expected. Alternatively, enter the PIN by hand in the host's web UI on the **Pairing** page (or
   click the pairing notification of the Shell tray icon).
3. Choose the **permissions** for this device beside the PIN:
   - **Full access** (default): everything, including all inputs, clipboard, file transfer, server
     commands and remote shutdown/restart. For the user's own devices.
   - **Streaming and input**: list, view and launch apps and all inputs; no clipboard, no file
     transfer, no server commands.
   - **View only**: list apps and watch the stream; the device cannot send input.
   - **Custom**: individual toggles.

   Permissions can be changed later under **Pairing > Device Management**. Remind the user: only pair
   devices they use themselves, because a paired device can control the PC.
4. Press **Pair**. "PIN sent" means the PIN reached the device; the client then confirms. The client
   dialog closes when pairing succeeds.

Timing and mistakes:

- The client waits **about 5 minutes** for the PIN. After that the attempt is dropped; select the host
  again in Hermit for a new PIN.
- **Wrong PIN**: the client reports the failure; simply select the host again for a new PIN. A new
  attempt replaces the old one, so a wrong PIN never blocks the next try.
- "No device is waiting for a PIN": start pairing on the client first, then enter the PIN.

## Step 5: First stream on the home network

1. Both devices on the same home network. The host appears in Hermit automatically (mDNS discovery).
2. If it does not appear, add it by address: Hermit for Windows **+** (Add PC manually), Hermit for
   Android **+** on the PC list. Find the host's local IP address (read-only):

   ```powershell
   Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.PrefixOrigin -in 'Dhcp','Manual' } | Select-Object InterfaceAlias, IPAddress
   ```

   Wi-Fi "client isolation" or guest networks can block discovery and connections.
3. Select the host, then an app (for example **Desktop**). Check that picture, sound and input work.
   Useful shortcuts in Hermit for Windows: Ctrl+Alt+Shift+H lists shortcuts, Ctrl+Alt+Shift+Q
   disconnects. On Android, Back opens the stream panel.
4. A PC without a monitor still works: Shell creates a virtual display at the client's resolution.

## Step 6: Remote access (outside the home network)

Read [remote-access.md](remote-access.md) before this step. Present the three options in plain words
and **recommend Tailscale** for most people:

1. **Tailscale (recommended)**: a free private network app installed on the host and on each client
   device. No router changes and no open ports; works even when the internet provider shares one
   public address between customers (CGNAT). Add the host in Hermit by its Tailscale address
   (`100.x.y.z`) or MagicDNS name; automatic discovery does not work over Tailscale. Shell treats
   Tailscale addresses like the local network, so pairing, **Open Shell pairing page** and the web UI
   all work over Tailscale.
2. **UPnP**: Shell asks the router to open the streaming ports by itself (**Settings > Network > UPnP**,
   config key `upnp`, off by default). Needs a router with UPnP turned on; does not work behind CGNAT.
3. **Manual port forwarding** in the router: TCP 47984, 47989, 48010 and UDP 47998, 47999, 48000 (for
   the default base port 47989) to the host's fixed local IP. Never 47990. Does not work behind CGNAT.

Explain the trade-offs from [remote-access.md](remote-access.md) and let the user choose. Whatever they
choose:

- **Pair at home first** when using UPnP or port forwarding: the web UI only accepts the local network
  by default and its port must stay closed, so entering a PIN from outside is not possible that way.
  With Tailscale, pairing from anywhere works.
- **Test from outside** (for example a phone on mobile data, Wi-Fi off) before the user relies on it.
- **The host must be on and awake.** Wake-on-LAN generally only works from the same local network; it
  does not work over Tailscale, and waking over the internet is not guaranteed. If the user wants
  the PC reachable while away, discuss the power settings (ask before changing them).

## Step 7: Wrap up

Summarize for the user what was set up: the host name, how they connect at home and away, which
devices are paired with which permissions, where to change things (web UI at
`https://localhost:47990` on the host: Pairing > Device Management, Settings), and how to update later
(download the new `Shell-<version>.zip` and `Install-Shell.ps1`, run the same command; a full backup is
made first and settings and pairing are kept).

If anything does not work, use [troubleshooting.md](troubleshooting.md).

More documentation (English):

- Shell: <https://github.com/junopark00/hermit-shell> (README, `docs/remote-access.md`,
  `docs/features.md`, `docs/tools.md`)
- Hermit: <https://github.com/junopark00/hermit/blob/main/docs/guide.md>
- Hermit for Android: <https://github.com/junopark00/hermit-android/blob/main/docs/guide.md>
