# Features

This page describes what Shell adds to the Sunshine and Apollo streaming core, how each feature
behaves and which client endpoints it uses.

## Names and locations

| Item | Value |
|---|---|
| Service | `ShellService` (`tools\shellsvc.exe` runs `shell.exe` in the signed-in user's session) |
| Clipboard helper | `tools\shellclip.exe`, run by `shell.exe` as the signed-in user (see [Clipboard](#clipboard)) |
| Install folder | `C:\Program Files\Shell` |
| Settings, pairing, apps, log | `config\shell.conf`, `config\shell_state.json`, `config\apps.json`, `config\shell.log` |
| Session history | `config\session_history.jsonl` |
| Webhook secret | `config\credentials\webhook_secret` (only with a secret set; see [Webhooks](#webhooks)) |
| Install backups | `C:\ProgramData\Shell\backups` |
| Firewall rule | `Shell` (inbound, TCP and UDP, for `shell.exe`) |
| Web UI | `https://localhost:47990` |
| Environment for apps | `SHELL_APP_*` and `SHELL_CLIENT_*`; the `SUNSHINE_*` names are set as well, for scripts written for Sunshine-based hosts |
| Protocol | GameStream compatible: mDNS `_nvstream._tcp`; TCP 47984, 47989, 47990 (web UI) and 48010, UDP 47998-48000 (default ports; see [remote-access.md](remote-access.md)) |

The host name shown to clients is **Settings > General > Host Name** (`shell_name`). A configuration
file that still uses the Sunshine key `sunshine_name` is read as well.

## Web UI

- **Home** shows a summary line (host, version, platform, state, running app, connected and paired
  devices) and these tables:
  - **Streaming now**: one row per active stream with device, app, elapsed time, resolution and frame
    rate, bitrate, round-trip time and recovery requests, refreshed every 2 seconds. A stream can be
    disconnected from its row; the app on the host is paused or closed according to its settings.
    When the encoder keeps using more than 65% of the frame interval, a warning line under the row
    recommends a faster preset (see [Encode time guard](#encode-time-guard)).
  - **Paired devices** with their state and permissions.
  - **Recent sessions**: the last 20 sessions from the session history (up to 100 with "Show more").
  - **Install backups** made by `Install-Shell.ps1`, newest first, each with a "Copy rollback command"
    button. The web UI does not run the rollback itself, because the service stops during a rollback.
  - **Virtual display** (Windows hosts): whether the virtual display is in use, released or not in
    use, how many monitors are on, the automatic release setting and the driver state. While the
    driver is not usable the card shows only the driver state with what to do (see
    [Driver state](#driver-state)).
- **Bitrate** in the live table follows changes made during the stream. Hover over it to see the
  encoder target, the packet pacing and whether live changes are supported.
- **Round-trip time** comes from the control connection (ENet pings about every 0.5 seconds). It shows
  `-` when no answer arrived for 2 seconds.
- **Recovery requests** count the key frame and reference frame invalidation requests of the client,
  which it sends mostly after losing frames. After the first minute the last minute's count is colored:
  0 green, 1 to 3 yellow, more red.
- **Theme**: neutral grey layers with one teal accent, in light and dark. Fonts (IBM Plex Sans) are served
  by the host, so the UI works without internet access.
- **Access**: **Settings > Network > Web UI Access** (`origin_web_ui_allowed`: `pc`, `lan` (default) or
  `wan`) decides which addresses may open the web UI; the check runs before the sign-in. A browser that
  opens a page (`/`, `/pin`, ...) from outside that range, for example a phone on the local network while
  the setting is `pc` (what Hermit's **Open Shell pairing page** runs into), gets a 403 page in English and
  Korean that names the allowed scope and where the request came from, gives the local URL to use on the
  host PC itself and the setting to change, and advises against `wan`. The page is built into the server
  (no web UI assets), and `/api/...` requests from outside the range get a bare 403 as before.

## Pairing

The **Pairing** page (`/pin`) takes the four-digit PIN shown by the client, an optional device name and
the permissions the device gets, so a new device is ready to use without a second visit to the device
editor.

| Preset | Permissions |
|---|---|
| **Full access** (default) | Everything: all inputs, clipboard read and write, file upload and download, server commands, and listing, viewing and launching apps |
| **Streaming and input** | Listing, viewing and launching apps and all inputs; no clipboard, file transfer or server commands |
| **View only** | Listing apps and viewing streams (what Apollo gives a second device) |
| **Custom** | The same toggles as the device editor |

- The summary beside the preset shows what the device will get. The last choice, including the custom
  toggles, is kept in the browser (`localStorage`) and is the starting point for the next pairing.
- Permissions can be changed later under **Device Management** on the same page.
- `POST /api/pin` takes `{"pin": "1234", "name": "My phone", "perm": <uint32>}`. `perm` is the
  permission bit set (the values of the `PERM` enum in `src/crypto.h`, masked with `_all`; full access is
  `0x071F1F00`). Without it the first paired device gets every permission and later ones list and view,
  as before.
- `POST /api/pin` replies `{"status": <bool>, "reason": "<reason>"}`. The host cannot tell a wrong PIN
  when it is entered: it hands the PIN to the device, which checks it and then finishes the pairing or,
  for a wrong PIN, ends it. So `sent` (`status` true) means the PIN reached the waiting device, not
  that it is paired; the page says so, asks to start pairing again on the device if it reports a wrong
  PIN, and reloads the device list 3 and 10 seconds later so a finished pairing shows up. The other
  reasons (`status` false) are `no-client` (no device is waiting for a PIN), `invalid-pin` (not four
  digits), `failed` (the waiting device's request was invalid; it was told pairing failed) and
  `rate-limited` (the waiting device's address is over the pairing limit below; its attempt was ended
  with "Too many pairing attempts; try again later", and the page asks to start again later).
- A device that starts pairing again replaces its earlier unfinished attempt (after a wrong PIN or a
  dropped connection), so a wrong PIN never blocks the next try. If the earlier attempt is still
  waiting for a PIN, its request is answered with a failed pairing (`status_code` 400, "Superseded by
  a newer pairing attempt") instead of being left to time out.
- A device waits up to 5 minutes for the PIN. Its request then times out (the HTTP server's
  300-second limit) and Shell drops the attempt, so a PIN entered later never goes to a device that
  gave up; start pairing again on the device. Both Hermit clients stop waiting a little earlier, at
  295 seconds, and then send `/unpair`, so an un-cancelled pairing from them ends about 5 minutes
  after it started with "Unfinished pairing dropped at the client's request" in the log. With
  several devices waiting, the PIN goes to the one that started pairing last.
- `GET /unpair?uniqueid=<id>` on the HTTP port: clients send it after a pairing failed on their side
  (a wrong PIN), when the user cancels pairing or they stop waiting for the PIN at 295 seconds (both
  Hermit clients) and from their own Unpair command. Shell drops the unfinished pairing of that
  uniqueid, answering a request it kept waiting for a PIN, and replies `<root status_code="200"/>`,
  so the client reports the wrong PIN. It never removes a paired device: without an unfinished
  pairing the reply is `status_code` 400 ("Unpair this device in the host's web UI"), so a client's
  Unpair command shows an error instead of a false success. Paired devices are removed under
  **Device Management**. Over HTTPS `/unpair` still answers 404.
- `/unpair` is accepted only from the address that started the unfinished pairing (the peer address
  of the socket; Shell reads no forwarding headers, a reverse proxy is not supported). From any other
  address the reply is the same `status_code` 400 as without an unfinished pairing, so the request
  does not tell whether one exists; the log gets one line at info level. Both Hermit clients send
  `/unpair` from the device that started the pairing, so nothing changes for them.
- **Pairing limit per address**: a client address gets `pair_rate_limit_count` pairing attempts per
  `pair_rate_limit_seconds` (defaults 10 per 60 seconds; the window starts with the address's first
  attempt). The start of a pairing (`/pair` getservercert) counts as one attempt; cancelling it
  with `/unpair` does not, so a pairing that a device cancels or fails with a wrong PIN costs one
  attempt. Over the limit,
  `/pair` and `/unpair` answer `status_code` 400 ("Too many pairing attempts; try again later"), the
  shape of a failed pairing, so clients show their usual pairing failure and no attempt is kept; a
  later phase of a pairing already under way is failed the same way; and a pairing that is waiting
  for the PIN is ended with that message when the PIN is entered (reason `rate-limited`). The first
  refusal of an address in a window is logged as a warning. The keys are set in `config\shell.conf`
  (there is no toggle on the config page, which keeps keys it does not show):
  `pair_rate_limit_count = 10` and `pair_rate_limit_seconds = 60`; 0 in either turns the limit off.
- Limitation: unfinished pairing attempts are told apart only by the client's uniqueid. Stock
  Moonlight-style clients (and older Hermit for Android) all send the same one, `0123456789ABCDEF`,
  so a pairing attempt from one such device replaces another such device's unfinished attempt, and
  `/unpair` (unauthenticated, over HTTP) from the same address cancels the unfinished attempt of that
  uniqueid whichever device behind that address started it. Paired devices are identified by their
  certificate and are never affected.
  Hermit for Windows sends a per-install id, and Hermit for Android sends one for pairing (the
  shared id with NVIDIA GameStream hosts).
- **Prefill for companion clients**: Hermit and Hermit for Android open
  `https://<host>:47990/pin#pin=1234&name=<URL-encoded device name>`. The fragment never leaves the
  browser; the page fills the fields from it, focuses the **Pair** button and removes the fragment from
  the address (so a reload starts clean). Nothing is sent until the user presses **Pair**.

### QR pairing

**Pair with QR** on the same page creates a QR code that a Hermit client uses to pair in one step: no
PIN, no device name to type, the certificate pinned from the first contact, and the permissions
chosen beside the PIN applied. Hermit for Android scans the code; Hermit on the host PC takes the
same link pasted into its "add PC" field. Standard PIN pairing is unchanged, and a QR code exists
only after a signed-in administrator creates one.

**Link.** `hermit://pair?v=1&host=<host>&port=<HTTP port>&fp=<64 hex>&token=<26 base32>&name=<name>`,
keys in this order, RFC 3986 percent-encoding, at most 320 bytes (`name` is cut first).

| Key | Value |
|---|---|
| `v` | `1`; a client refuses other values |
| `host` | An IPv4 address, a bracketed IPv6 address or a host name (no zone id). The page offers the host's LAN and Tailscale IPv4 addresses (`GET /api/pair/qr/hosts`), the first one by default; another address can be typed |
| `port` | The HTTP port (47989 by default); the client reads the HTTPS port from `serverinfo` |
| `fp` | SHA-256 of the DER server certificate, 64 lower-case hex characters (what `openssl x509 -fingerprint -sha256` prints, without colons). The client compares it with the certificate it receives and stops before the challenge on a mismatch |
| `token` | 16 random bytes as 26 upper-case base32 characters (RFC 4648, no padding). It is the one-time secret and stands in for the PIN |
| `name` | The host name (**Host Name** in Settings), at most 32 characters, display only |

The link carries no permissions (they stay in the host's record, so a changed link cannot raise
them), no PIN, no credentials and no expiry (the host keeps the time).

**Record.** The host keeps one QR record, in memory only (not in `shell_state.json`; a restart drops
it): the token, its id, the permissions, the creation time and the state `pending`, `in_use` (a
device is pairing with it, bound to that device's uniqueid and address), `used` or `expired`.
Creating a new code replaces the previous one; a device still pairing with the old one is answered
"Superseded by a new QR code". The record id is `SHA-256("hermit-qr-id" ‖ token)`, first 16 hex
characters; the client computes it from the token and sends only the id over plain HTTP, never the
token.

**Protocol.** The client first reads `serverinfo` over HTTP: a host with QR pairing answers
`<qrPairing>1</qrPairing>` (HTTP and HTTPS, public like the rest of the HTTP `serverinfo`). Without
it the client stops before any pairing request ("update the host"). It then sends `/pair`
getservercert with `qr=<id>` and its real `devicename`, and the token in place of the PIN in its key
derivation (`SHA-256(salt ‖ PIN)` takes any length, so nothing changes in the key derivation on
either side). The host checks the record in this order and answers the first failure as a failed
pairing (`status_code` 400, `paired` 0) whose `status_message` starts with `QR_<reason>: ` and which
carries the attribute `qr_reason`; clients map the prefix to their own text:

| Order | Check | `qr_reason` | `status_message` |
|---|---|---|---|
| 1 | No record, or another id | `none` | `QR_none: No QR pairing is pending; create one in the web UI` |
| 2 | The client address is not local (`net::from_address` says WAN) | `network` | `QR_network: QR pairing is accepted from the local network only` |
| 3 | More than 120 seconds since the code was created | `expired` | `QR_expired: QR code expired; create a new one` |
| 4 | Already used, or in use by another device (another uniqueid or another address) | `used` | `QR_used: QR code already used; create a new one` |

A wrong id and a WAN address learn nothing about the record (they are answered before its state is
looked at). When the checks pass, the record becomes `in_use`, bound to the device's uniqueid and
address, and the host answers getservercert at once with the token as the PIN: no PIN wait, no tray
notification. The rest of the pairing (clientchallenge, serverchallengeresp, clientpairingsecret,
the HTTPS `pairchallenge`) is unchanged; when it succeeds the record becomes `used` and the device
is stored with the record's permissions. One token admits exactly one client: a session whose record
is no longer bound to it when it finishes (replaced, cancelled or expired meanwhile) is refused with
`QR_used` and nothing is stored. A pairing that fails (a damaged token, a dropped
connection, `/unpair` from the device) hands the record back as `pending`, so a retry within the 120
seconds works. `enable_pairing = false` turns QR pairing off with the rest: `POST /api/pair/qr`
answers 403, `/pair` as before.

**Address binding.** The uniqueid and the record id travel over plain HTTP, so a QR session can be
replaced, continued or cancelled only from the address it started from: a getservercert (with or
without `qr=`), a later pairing phase or `/unpair` with that uniqueid from another address is answered
`QR_used` (or, for `/unpair`, the usual 400) and changes nothing. Two devices behind one shared
address are not told apart; the 120-second window and the device list limit what that allows.

**Lifetime.** A QR session that stalls after getservercert (the client stopped at a certificate
mismatch and its `/unpair` was lost) is dropped when the record's 120 seconds end, and the record
becomes `expired`. The pairing limit per address applies to QR pairing as to PIN pairing, with the
same message.

**Web API** (web UI login required; all four answer with `Cache-Control: no-store`):

| Request | Body | Reply |
|---|---|---|
| `POST /api/pair/qr` | `{"perm": <uint32>, "host": "192.168.0.10"}` (`perm` as for `/api/pin`) | `{"status": true, "link": "hermit://pair?...", "expires_in": 120}`; the link is returned here once and never again. 403 when pairing is disabled, 400 for a bad `host` |
| `GET /api/pair/qr` | — | `{"status": true, "state": "none\|pending\|in_use\|used\|expired", "remaining": <seconds>, "paired_name": "<device>"}`. No token, link or id. `used` with the device's name is shown for 60 seconds after the pairing |
| `GET /api/pair/qr/hosts` | — | `{"status": true, "hosts": ["192.168.0.10", "100.64.1.2"]}`: LAN and Tailscale IPv4 addresses of the host, private ranges first, without loopback and link-local addresses |
| `DELETE /api/pair/qr` | — | `{"status": true, "cancelled": <bool>}`; drops the record and the sessions bound to it |

The page draws the code with the bundled `qrcode-generator` library (MIT, no dependencies, no
network), shows a 120-second countdown and the link with a **Copy** button, polls the state every
2 seconds and, once `used`, shows the device's name (as text) and reloads the device list. The token
is shown once, in the code and the link, and is not kept by the page once the record ends.

**Log.** "QR pairing created from [addr] with preset all|stream|view|custom, expires in 120 s",
"QR pairing started for [name] from [addr]", "QR pairing completed for [name] from [addr]",
"QR pairing refused from [addr]: <reason>", expiry and cancellation, all at info level. The token
and the link are never logged at any level; the id may appear in the debug-level request dump.

**Security notes.** The token has 128 bits, so the offline guessing that a 4-digit PIN allows is
pointless, and the host hands nothing over without the token (a device that starts pairing at the
same moment cannot take a PIN meant for another). A photo or screen capture of the code is usable
only within 120 seconds, from a local or Tailscale address, and only once; a pairing it makes shows
up on the page and in the device list, where it can be removed. Do not send the link by message or
e-mail; copy it only to paste into Hermit on the host PC (Windows' clipboard history and cloud sync
can keep it). Clients that do not know `qr=` (Moonlight) are unaffected: without it `/pair` runs the
PIN flow. Hosts before Shell 1.3.0 have no `qrPairing` in `serverinfo`, and the clients stop there.

## Virtual display

Shell uses the SudoVDA driver to create a virtual display for a session, at the resolution and refresh
rate the client asks for. A client can be set to always use the virtual display (per device, on the
**Pairing** page); apps can be set to use it, and `headless_mode` uses it whenever no monitor is on.

### Driver state

Shell opens the SudoVDA device when it starts and checks the driver's protocol version; while the
device is open a watchdog ping keeps running. The **Virtual display** card on the web UI home page
shows the result, and `/api/shell/live` carries it as `vdisplay.driver_state` (`vdisplay.driver` stays
the boolean "usable").

| State | Meaning | What to do |
|---|---|---|
| `ok` | The device opened and the protocol version matches. | Nothing. |
| `missing` | The device could not be opened: the driver is not installed, not started yet, or was removed (`Install-Shell.ps1 -Uninstall -RemoveDriver`). A first installation whose driver step ended with "A restart of Windows completes the driver installation" (installer exit code 3010) shows this until Windows is restarted. | Restart Windows after a fresh installation. Otherwise install Shell again: `Install-Shell.ps1` installs the driver on a first installation (an update never touches `drivers\`), or run `drivers\sudovda\install.bat` from the extracted release ZIP as administrator. |
| `incompatible` | The device opened, but the driver's protocol version does not fit this Shell: another major version, or a driver older than the one this Shell was built with. An update keeps the installed driver, so this appears when a Shell version needs a newer driver than the one on the PC (also after a migration from Apollo, which keeps Apollo's driver). Right after a driver reinstall it can also mean the previous driver is still loaded until a restart. | Restart Windows if the driver was just installed. If it persists, install the driver of this release: `Install-Shell.ps1 -Uninstall -RemoveDriver` and install again, or run `drivers\sudovda\install.bat` from the extracted release ZIP as administrator. |
| `watchdog` | The driver stopped answering the watchdog ping four times in a row; Shell closed the device. | Restart Windows. |
| `unknown` | Not checked yet (only for a moment at start-up). | Wait. |

Shell tries the driver again when an app that needs the virtual display is launched and when the app
list is reloaded, so a driver that starts working later is picked up without restarting the service.
`Invoke-ShellRecovery.ps1 -Action Diagnose` reports the device from the Windows side
(`VirtualDisplayDriver`: present, status, problem code, driver version; see [tools.md](tools.md)).

**When a launch is refused** because no display could be captured (HTTP 503), the message names the
reason when the virtual display is involved: the driver state as above ("No virtual display: the
virtual display driver is not installed or not running ..."), "The virtual display could not be
created", or "The virtual display \\.\DISPLAYn was not active after N ms". Hermit shows this text in
its launch error ("Host returned error: ... (Error 503)"); the host log has the same line
(`Launch refused: ...`). With a monitor on, a launch that asked for the virtual display streams the
monitor instead, as before.

### Resuming at another resolution

When a client resumes a running app at a different resolution, Shell changes the virtual display to
the new mode (scaling and the doubled refresh rate option are applied as on a first launch). The driver
only offers the modes of the size it was created with, so when the new mode is not available Shell
recreates the virtual display at the new size under the same ID. Windows of the running app move to the
new display. Desktop apps are unaffected; some full-screen games switch to windowed mode or minimize
when the display changes.

### Releasing the virtual display when a monitor turns on

If the PC was started with all monitors off and a client connects, the virtual display becomes the only
and therefore primary display. While the virtual display is in use, Shell checks every 2 seconds how
many other displays are active. When that number goes from zero to one or more (a monitor was turned
on) and nobody is streaming, Shell removes the virtual display: Windows moves the windows to the
monitor and makes it primary. The app keeps running and a tray notification explains what happened.

- A monitor turned on during a stream is handled after that stream ends.
- Nothing is released during the first 30 seconds after a client connects.
- Monitors that were already on when the session started do not trigger a release.
- Release by hand: tray menu **Release Virtual Display** or the virtual display card on the web UI
  home page. Active streams are disconnected first; the app keeps running.
- After a release, the next connection recreates the virtual display if no monitor is on. With a
  monitor on, Shell streams the monitor like a regular host, unless the client, the app or
  `headless_mode` asks for the virtual display.
- Turn the automatic release off with `virtual_display_release_on_monitor = disabled`.

## Video packet pacing

`video_pacing_mbps` (**Settings > Advanced > Video Packet Pacing**) limits how fast the packets of one
video frame are sent. Sending each frame as a burst at full link speed causes burst loss on many
internet paths; spreading it reduces that loss.

- `0` (default, automatic): three times the session bitrate, at least 100 and at most 800 Mbps
  (50 Mbps streams are paced at 150 Mbps, 20 Mbps streams at 100 Mbps). The bitrate is the one the
  client requested, limited by `max_bitrate`.
- `1` or more: that rate. `800` matches upstream behavior.
- Trade-off: with automatic pacing an average frame takes about 40% of the frame interval to send. Very
  large frames (full-screen changes, 5 to 10 times the average) can delay the next 2 to 4 frames
  slightly. On a clean LAN, `800` gives the lowest latency.
- The host log prints the setting and the effective rate at the start of each session, for example
  `Video pacing [auto 150 Mbps, effective ~146 Mbps]`, together with the FEC percentage and the packet
  size.

## Encode time guard

Shell measures how long the encoder takes for each frame and compares it with the frame interval.
The interval is the one at which frames were really delivered during the second (the second's
length ÷ the frames encoded in it), never shorter than the requested one (1000 ms ÷ the frame rate
the client asked for: 8.3 ms at 120 fps): a 120 fps request that only gets 60 fps is judged against
16.7 ms, so a 6 ms encode counts as 36%, not 72%. Once a second it takes the 90th percentile of that
second's encode times (the slowest tenth, mostly key frames, does not count) as a percentage of the
interval. Seconds with fewer than 10 encoded frames (static content, start-up) are not evaluated. The measurement is the wall time of the encode call on the encoder
thread, which for NVENC includes waiting for the encoded frame; the colour conversion before it is
not counted. The cost is two timestamps per frame and one selection over at most 1,024 samples per
second; nothing is logged per frame.

- Over 65% for 10 consecutive seconds: the stream has exceeded its encode budget. The host log gets
  one warning per stream (`Encoding uses 78% of the frame budget at 3840x2160@120 HEVC preset P7, 2 NVENC
  engine(s); a faster preset is recommended`), the dashboard's **Streaming now** table shows a warning line under
  the row and the session record notes it. Under 50% for 10 consecutive seconds clears the mark
  again (an `info` line); seconds between 50% and 65% end both streaks.
- `GET /api/shell/live` session entries carry `encode_budget_pct` (the last evaluated second) and
  `encode_budget_exceeded`; the [session record](#session-history) carries `encode_budget_max_pct`
  and `encode_budget_exceeded` (true when the budget was exceeded at any time). All of them are
  omitted until the first second has been evaluated.
- `nvenc_preset_guard` (**Settings > NVIDIA NVENC Encoder > Encode time guard**):
  - `notify` (default): only the warning above.
  - `adjust`: in addition, the next stream with the same resolution, frame rate, codec, HDR and
    chroma format starts one NVENC preset faster than the one that exceeded the budget (P7 → P6 → …
    → P1, never below P1), and the
    log says so (`NVENC preset guard: the next 3840x2160@120 HEVC stream starts with preset P6 (this one
    used P7, nvenc_preset is 7)`). A running stream is never changed: reconfiguring the encoder
    mid-stream risks a broken picture. The adjustment lives in memory only; a Shell restart or a
    change of `nvenc_preset` starts again from the configured preset. It applies to the NVENC
    encoder; other encoders get the warning only.
- The NVENC encoder's start-up log line (`NvEnc: created encoder ...`) also names the number of
  encoder engines on the GPU (`engines 2`, from `NV_ENC_CAPS_NUM_ENCODER_ENGINES`) when the driver
  reports it. It is informational only.

## Live bitrate changes

Hermit's stream panel changes the bitrate during a stream with `GET /actions/bitrate?kbps=N` (paired
client, its own stream only).

- NVIDIA encoders apply the new average bitrate and VBV size with `nvEncReconfigureEncoder`: no encoder
  reset, no forced key frame, no visible pause.
- The ratio between requested and encoded bitrate from the session start (FEC, audio and packet
  overhead) is kept; `max_bitrate` applies, and automatic packet pacing is recalculated. The new value
  survives an encoder reinitialization.
- Reply: `bitrate=<encoding kbps>`. `403` when the client is not streaming, `501` for other encoders
  (the client then reconnects with the new bitrate).

## Live FEC changes

A client raises or lowers the forward error correction of its own stream during the stream with
`GET /actions/fec?percent=N` (paired client with the view permission, its own running stream only).
Without a request nothing changes: every frame uses `fec_percentage` from the settings, as before.

- `percent`: an integer `1` to `100`. Values above `100` are clamped to `100`; `0` or `default`
  returns to the configured `fec_percentage`. Anything else (empty, a sign, letters) gets `400`
  with a plain-text reason.
- Reply: `percent=<effective>` (plain text), the percentage the next frame uses. `403` (`not
  streaming`) when the caller has no running stream, `401` without the view permission.
- The value applies from the next video frame: no encoder reconfiguration, no key frame, no pause.
  Everything downstream is unchanged: the shard layout per FEC block, the parity minimum per frame
  and the "no FEC for an oversized frame" fallback.
- Bandwidth cost: parity packets add `N`% to the video packets, so 50% at 35 Mbps sends about
  52 Mbps on the wire (the encoder bitrate is not reduced; a client that wants the total constant
  lowers the bitrate with `/actions/bitrate` as well).
- Why a client raises it: frame loss with a flat round-trip time (no queueing) is link loss, not
  congestion. Lowering the bitrate then only costs picture quality; more parity lets the client
  rebuild the lost packets instead. Loss with a rising round-trip time is congestion and still calls
  for a lower bitrate.
- The log notes a change (`FEC for [Tablet] changed to 50% (was 20%)`), at most one line per
  500 ms per session; a change inside that window is logged, with the latest value, when the window
  opens. A request that leaves the percentage unchanged is not logged. `GET
  /api/shell/live` session entries carry `fec_percent` (the percentage the next frame uses). The
  [session record](#session-history) keeps `fec_percent` (the configured value) and adds
  `fec_percent_max`, the highest percentage the session used.
- `serverinfo` over HTTPS carries `<LiveFec>1</LiveFec>` (the protocol version). Its presence is the
  contract: a client sends `/actions/fec` only to a host that advertises it. The plain-HTTP
  `serverinfo` never carries it. (`/actions/bitrate` has no such field; clients detect it from the
  reply.)

## Local cursor

In remote desktop mouse mode the cursor in the video trails the mouse by the capture interval, the
encode, the network and the client's V-Sync (about 25 to 40 ms at 60 fps). A client that supports the
local cursor draws the mouse pointer itself with the operating system's cursor: Shell sends the
pointer's **shape, hotspot and visibility** (never its position, which the client knows) over the
control connection and leaves the pointer out of the captured video. The picture keeps its delay, the
cursor does not.

Nothing changes for clients that do not ask for it: stock Moonlight, Hermit in game mouse mode and
older Hermit versions keep seeing the cursor in the video exactly as before. The feature needs the
Desktop Duplication capture method (`capture = ddx` or automatic); Windows.Graphics.Capture does not
report pointer shapes.

- `local_cursor` (**Settings > Advanced > Local cursor**, Windows): `auto` (default) offers it to
  clients that ask; `off` never does, and the cursor is always drawn into the video.
- `serverinfo` over HTTPS carries `LocalCursor` = the newest protocol version the host speaks (`1`)
  unless the setting is `off`. The plain-HTTP `serverinfo` never carries it.
- `GET /actions/cursor` (HTTPS, paired client with the view permission, its own running stream):
  - `mode=local&v=1` asks for the local cursor; `mode=host` returns to the default. Hermit calls it
    once its control connection is up, on every mouse mode toggle, and when its setting changes;
    a session that ends is reset by the host. Reply `mode=local` or `mode=host`, the latter with
    `reason=off` (setting) or `reason=version` (`v` newer than the host supports) when the host
    declines; `403` when the client is not streaming; `409` when the capture backend does not report
    pointer shapes (WGC). Older Shell versions answer `404`, which the client treats as "unsupported".
  - `shape=<id>` returns the current pointer shape when its id matches, as one `CURSOR_SHAPE` payload
    (below, `offset` 0 and all pixels, `application/octet-stream`), else `404`. Clients use it when a
    `CURSOR_STATE` names a shape they do not hold.
- **Composing rule.** One capture thread serves every session, so the pointer can only be left out
  of the video for everyone at once. It is left out while *every* session asked for the local cursor;
  as soon as one session did not (a phone in touch mode, stock Moonlight), the pointer is drawn into
  the video for all of them and the sessions that asked are told so with `host_composing=1` (they then
  hide their own cursor and see the host's). The decision is recomputed whenever a session starts,
  ends or changes its mode.
- **Messages** (control stream, host to client, AES-GCM encrypted like every other message, reliable
  and in order on channel 0; all integers little-endian; only to sessions that asked):
  - `0x3100 CURSOR_SHAPE`: `version u8` (1), `shape_id u32`, `width u16`, `height u16`, `hotspot_x u16`,
    `hotspot_y u16`, `offset u32`, then the pixel bytes `[offset, offset + n)` of the image, `n` being
    the rest of the message. Pixels are top-down rows of B, G, R, A with straight (not premultiplied)
    alpha, `width * height * 4` bytes in all, each dimension 1 to 256, hotspot inside the shape. The
    encrypted control packet has a 16-bit length, so an image larger than 61,440 bytes arrives in
    several messages with increasing `offset` and the same header (up to 123x123 fits in one message;
    124x124x4 = 61,504 bytes needs two); it is complete when `offset + n == width * height * 4`.
  - `0x3101 CURSOR_STATE`: `version u8` (1), `shape_id u32`, `visible u8` (0/1), `host_composing u8`
    (0/1). `shape_id` 0 means "no shape" (the client shows its default arrow; this happens before the
    first shape was captured or when the shape is larger than 256x256). The client ignores messages
    whose `version` is not 1.
- **Shape ids and caching.** `shape_id` is the 32-bit FNV-1a hash of `width`, `height`, `hotspot_x`,
  `hotspot_y` (each as a little-endian u16, in that order) followed by the pixel bytes; 0 is mapped
  to 1. Shell remembers the last 128 ids it sent to a session (4 bytes each; more frames than any
  cursor pack animates) and sends only `CURSOR_STATE` for a shape the client already holds, so an
  animated cursor costs one `CURSOR_SHAPE` per frame in its first cycle and short states afterwards;
  a pack with more than 128 frames keeps sending shapes. The list is cleared on every `mode=local`,
  so a client that dropped its cache gets every shape again. A client that receives an unknown id
  fetches it with `?shape=<id>` and keeps showing the previous shape meanwhile.
- **Conversion.** Colour pointers are sent as captured. Monochrome (AND/XOR) and masked colour pointers
  are flattened to straight alpha; pixels that would invert the screen (the I-beam over text, for
  example) cannot be drawn by a static cursor and become opaque white with a one-pixel black
  outline on the transparent pixels around them, the usual remote-desktop stand-in for XOR cursors,
  which stays visible on light and dark backgrounds (a masked-colour XOR pixel with RGB 0 leaves the
  screen unchanged and is transparent). Shapes wider or taller than 256 pixels are not sent (one
  warning in the log; the client shows its default arrow).
- **Rates.** Per session at most 20 `CURSOR_SHAPE` and 60 `CURSOR_STATE` messages per second; a
  visibility or composing change is never held back by either limit: while a new shape waits, such a
  change goes out as a state with the shape the client has. When a limit holds a message back, the
  newest shape or state goes out on the next pass, never a stale one. While a session uses the local
  cursor the control thread wakes every 10 ms instead of waiting up to 150 ms for the next packet.
- **Mode change.** On `mode=local` the host sends the current shape and state on the control thread's
  next pass: within about 150 ms at most, usually sooner because the client's own input packets wake
  the thread. A capture restart (resolution change) delivers the pointer shape again and the host
  repeats the state with its id.
- The log notes every change (`Cursor for [Tablet]: local; host composing: no`). `GET /api/shell/live`
  session entries carry `cursor` (`local` or `host`) and `cursor_composing` (whether the host still
  draws the pointer into the video); the dashboard's video detail names the mode. The
  [session record](#session-history) carries `cursor`: `local` when the client drew the cursor itself
  at any time.

## Remote shutdown, restart, sleep and hibernation

In Hermit, right-click the PC in the PC list (long-press on Android) and choose to shut it down,
restart it, put it to sleep or hibernate it. Sleep and hibernation are offered only when the host
reports them as available.

- `GET /actions/power?action=query` tells the client whether it may do this and which other devices are
  streaming; Hermit names them and asks again before going ahead. The reply also says what this PC
  can do:
  - `sleep=1|0`: S1-S3 sleep (`GetPwrCapabilities`), or modern standby (S0 low-power idle, reported
    through `CallNtPowerInformation(PlatformInformation)`, which also sleeps through `SetSuspendState`).
  - `hibernate=1|0`: S4 is supported and the hibernation file exists (`powercfg /hibernate on`).
  - `wol=yes|no|unknown`: whether a connected wired Ethernet adapter wakes the PC on a magic packet.
    Read-only: the adapter's standardized driver setting `*WakeOnMagicPacket` (its network class
    registry key, matched by `NetCfgInstanceId`) plus whether Windows lets the device wake the PC
    (`DevicePowerEnumDevices`, the list `powercfg /devicequery wake_armed` shows). `yes` when both
    hold for one adapter, `no` when the setting is off on every wired adapter that has it, `unknown`
    when nothing could be read (no wired adapter with the setting, or the wake permission could not
    be confirmed). The clients warn before sleep or hibernation when it is not `yes`.
- `GET /actions/power?action=shutdown[&force=1]` shuts the PC down after 5 seconds (`InitiateShutdownW`,
  full shutdown); `action=restart` restarts it. By default running apps are asked to close first, so an
  app with unsaved work can stop the shutdown; `force=1` closes those apps as well.
- `GET /actions/power?action=sleep` puts the PC to sleep and `action=hibernate` hibernates it
  (`SetSuspendState`, 3 seconds after the reply so it reaches the client). Nothing is closed; `force`
  is ignored. Hibernation is refused with `409` and a reason when the hibernation file is absent, and
  both are refused with `409` when the query reports them unavailable. Streams of other devices end
  when the PC suspends; the client warns about them the same way as for a shutdown.
- Only paired devices with the **launch apps** permission can do this; each action is logged with the
  device's name. To turn the PC on again, use Wake-on-LAN (from sleep this needs the adapter's
  Wake on Magic Packet setting and the "Allow this device to wake the computer" permission; from
  hibernation and shutdown also the firmware's wake-on-LAN setting).

## Ctrl+Alt+Del

Windows ignores Ctrl+Alt+Del sent as ordinary input. When Shell runs as a service and receives
Ctrl+Alt+Del from a client, the service calls `SendSAS`, which opens the secure screen (lock, switch
user, Task Manager, sign-out). This needs the `SoftwareSASGeneration` policy to allow services, which
`Install-Shell.ps1` sets on install and update. When Shell is not running as a service, or the policy is
missing, the keys are passed through unchanged and Shell logs a warning once.

## Remapped keys

`keybindings` (and the **Right Alt → Win** option, which is a remap of `0xA5` to `0x5B`) change the key
that reaches the host. Shell keeps two things straight (taken from upstream Sunshine PR #5558):

- Modifier state follows the key the client is holding, not the remapped host key. While a remapped Alt
  is held, other keys are no longer wrapped in an extra synthetic Alt press and release.
- When a stream ends or a client drops, Shell releases the host key that was actually pressed, so a
  remapped modifier such as Win is not left held.

## Virtual audio sink with several clients

On Windows, when **Keep virtual sink as default** is on and another program changes the default playback
device, Shell switches back to the virtual sink. With several clients streaming at once, each client's
capture thread could try this at the same time. The switch is now serialised with a lock, and the sink to
restore is read when the change happens rather than when the capture started, so a client that joined
before the first client finished switching still restores it after the first client leaves (from
foundation-sunshine PR #1099).

## Clipboard

The upstream `/actions/clipboard` endpoint carries text only. On Windows Shell adds:

| Request | Content | Permission |
|---|---|---|
| `GET type=info` | `seq=<change number>`, `type=<text\|image\|files\|other\|none>`, `files=stream` | Clipboard read |
| `GET`/`POST type=image` | PNG; reads the "PNG" format or converts CF_DIB, writes both "PNG" and CF_DIBV5 | Clipboard read / write |
| `GET`/`POST type=files` | An "APCF" archive (up to 256 MB and 1,000 files) | Clipboard plus file download / upload |
| `GET type=filelist` | The copied files and folders as a text list (up to 4 GB and 1,000 items), see below | Clipboard read plus file download |
| `GET type=filedata&snapshot=<id>&index=<n>[&offset=<bytes>]` | The bytes of one file from that list | Clipboard read plus file download |

- `POST` replies with the new change number (`seq=`), so a client does not read back what it wrote.
- `type=other` means the clipboard holds content in formats Shell does not sync (a program's own
  formats, RTF or HTML only): a change nothing can be fetched for, which clients do not replace with
  an older copy of theirs. `type=none` means the clipboard is empty, which is not a change: a copy
  waiting on the client is still sent. Shell 1.1.0 answered `none` for both.
- `POST type=image` answers 413 when the PNG is over 32 MB or has more than 8192 × 8192 pixels
  (width × height, read from its header), 422 `image-not-convertible` when the data is not a PNG at
  all, and 500 when a PNG cannot be decoded or placed. `GET type=image` answers 413 when the
  clipboard holds an image whose PNG is over 32 MB, and 422 `image-not-convertible` when it holds an
  image whose data cannot be read or converted to PNG; an empty 200 reply means the clipboard holds
  no image.
- `POST type=files` answers 413 when the archive is over its limits (256 MB of file data, 1,000
  entries), 422 when a path in it cannot be created as it is (`unsupported-name` for a path Windows
  does not allow, `duplicate-name` for two paths that differ only in case or a file also used as a
  folder) and 400 for a malformed archive, including one with no entries.
- **Clipboard helper.** Shell runs as SYSTEM (started by the service in the user's session) and
  then does not see a copy made in File Explorer: Windows lists only OLE's "DataObject" format to
  it, so the copied files looked like `type=other`. Like Remote Desktop with `rdpclip.exe`, Shell
  therefore starts `tools\shellclip.exe` as the signed-in user, with the user's own token (never
  elevated), no window and no console, and every clipboard read and write goes through it: it
  answers requests from one pipe it inherits as its standard input and output and does nothing else
  (it opens nothing by name and never runs or opens the paths it carries). The pipe has a random
  name, allows SYSTEM and Administrators only, takes one client, and that client end is opened by
  Shell before the helper starts. Each request has a deadline (4 s, 12 s for images); a helper that
  does not answer in time is restarted and the client gets 503 `clipboard-busy`. The helper is
  started again when it exits (after 1 s, doubling up to 30 s), stopped and started again when the
  console session changes, and stopped when Shell stops; it also exits by itself when Shell is gone.
  While it is not running (nobody signed in, the file missing, a start failure), Shell reads the
  clipboard itself as before: text and images work, files copied in File Explorer are not seen.
  The dashboard shows the helper's state (**Clipboard helper**), the log says when it starts, stops
  or fails and, once per reason, when Shell falls back to reading the clipboard itself; the
  `type=other` diagnostics line says whether the helper or Shell answered. Shell started by hand
  (not as a service) needs no helper and does not start one.
- Received files are checked first (paths, duplicates, size), then extracted with the signed-in user's
  rights to `%LOCALAPPDATA%\Temp\ShellClipboard` and placed on the clipboard as CF_HDROP ("copy").
  Files that are sent are read with the user's rights; symbolic links and junctions are not
  followed. Other reparse points, such as OneDrive Files On-Demand placeholders, are ordinary files:
  reading one fetches its content.
- When another program holds the clipboard, Shell retries for about 0.3 seconds. If it is still
  held, `GET type=text`, `type=image`, `type=files` and `type=filelist` and every `POST` answer 503
  with the body `clipboard-busy` and a line of text; clients keep their last change number and try
  again later. `type=info` reads the change number and the content type together with the clipboard
  open, so the type always belongs to that number (a program that writes the clipboard empties it
  first and adds its formats afterwards); it retries for about 0.3 seconds and otherwise answers the same
  503. `type=filedata` never waits for the clipboard; it answers the same 503 only while an earlier
  read of the same file is stalled (see below). An empty 200
  reply (`type=text`, `type=image`, `type=files`, `type=filelist`) means only that the clipboard
  holds nothing of that type. `GET type=text` answers 500 (plain text) when the clipboard lists text
  that cannot be read and 413 when the text is over 64 MB; real text is a 200 with the text.
- Every 422 body starts with a machine-readable reason on a line of its own, then a line of text
  for people. Clients branch on the first line:

  | Reason | Meaning |
  |---|---|
  | `unsupported-name` | A name the other side cannot create (reserved names such as `CON`, a trailing dot or space, characters Windows does not allow, over 255 UTF-16 code units) |
  | `duplicate-name` | Two names that differ only in case, or a file also used as a folder |
  | `nothing-to-copy` | Nothing left to copy once links and junctions are skipped |
  | `image-not-convertible` | `GET type=image`: the clipboard image cannot be read or converted to PNG; `POST type=image`: the data is not a PNG |

  413, 400 and 500 bodies are plain text.
- `GET type=files` and `GET type=filelist` fail with 413 when the copy is over the size or item
  limit, 422 when the files cannot be copied as they are, and 500 when no user is signed in or a
  file cannot be read. 422 covers a name Windows paths do not allow (`unsupported-name`), two names
  that differ only in case, compared with the Unicode uppercase mapping and not only ASCII
  (`duplicate-name`), and nothing left to copy once links are skipped (`nothing-to-copy`).
- `files=stream` in the `type=info` reply means the host has `type=filelist` and `type=filedata`.
  Clients read the reply lines by key, so older clients ignore it. Hermit then puts host files on the
  local clipboard as virtual files and downloads each file only while it is pasted; older Hermit
  versions keep using `type=files`. Hermit for Android does not sync files.
- `type=filelist` expands folders like `type=files` (same name checks, links and junctions skipped,
  1,000 items, read with the signed-in user's rights) but reads no file data. The reply is UTF-8 text:

  ```text
  seq=<change number>
  snapshot=<16 hex digits>
  entries=<item count>
  bytes=<total file bytes>
  d<TAB>0<TAB><last write, Unix ms><TAB>Folder
  f<TAB><size><TAB><last write, Unix ms><TAB>Folder/report.pdf
  ```

  One line per item, folders before their contents, relative paths with `/`. An empty reply means
  the clipboard holds no files; 413 means the list is over 4 GB or 1,000 items (422, 500 and 503 as
  above).
- Shell keeps the newest two lists per paired device in memory (by device UUID), so a paste that is
  still copying from the previous list keeps working after the client fetches a new one. A list
  belongs to the stream session it was made in and to the user signed in at the console then: once
  that stream ends (or the device reconnects), or another user signs in, the list is gone (410) and
  the client has to fetch a new one.
- `type=filedata` takes the item's position in the list (`index`, from 0) and answers with
  `Content-Length` and the file read from disk in 256 KB chunks, each sent after the previous one
  left, so a slow client holds the host back through TCP flow control and the file is never held in
  memory. The file is opened with the user's rights and must still have the listed size and last
  write time. Errors: 400 bad arguments, 404 the index is not a file of the list, 409 the file is gone
  or changed, 410 unknown, dropped or expired list, 416 offset past the end, 500 the file cannot be
  opened, 503 `clipboard-busy` an earlier read of the same file is stalled (clients retry after a
  short wait). If the file cannot be read to the end, the connection closes early.
- Walking folders for `type=filelist`, reading files for `GET type=files` and checking and unpacking
  `POST type=files` run on a separate worker thread, one at a time, so other requests from any
  device are not held up meanwhile.
- Opening and reading the files for `type=filedata` run on four other threads, so a long job on that
  worker (such as a 256 MB `type=files` archive) does not hold up downloads. Each file is read in
  order, one chunk after the previous one was sent, and uses one thread at a time. A read that stalls
  (a OneDrive placeholder being fetched, a slow network share) holds up only its own thread; the
  other downloads continue on the rest. While a read of a file has been stuck in one open or read
  call for more than 3 seconds, a new `type=filedata` request for the same file (same device, list
  and index), such as a retry after the client gave up on the stalled read, gets 503
  `clipboard-busy` instead of a second thread, and the client retries after a short wait. A new
  request that only overlaps an earlier read briefly (Explorer seeking within a file, a resume right
  after a cut while one chunk is still being read) goes through; the file is then read twice for a
  moment. Only a request that arrives within the first 3 seconds of a stall can take another thread
  for that file, so holding up all downloads takes several stalled reads at once.
- The HTTPS server ends a request whose upload or response takes longer than 30 minutes (1,800
  seconds), which leaves a 256 MB `type=files` transfer room down to about 1.2 Mbps. Longer
  `type=filedata` downloads are cut; clients continue with `offset=<bytes received>`. The 4 GB limit
  for lists only bounds what one copy can bring over (about 18 minutes at 30 Mbps); file data is never
  buffered.
- Shell logs each list (device, item count, total bytes) and the end of each file download (device,
  index, bytes, time), never names or contents.
- Clients that only know `type=text` get real text as before. They also see 503 when the clipboard
  is busy and 500 when it lists text that cannot be read, where they used to get an empty 200.

## Session history

At the end of every stream Shell appends one JSON line to `session_history.jsonl` in the config folder.
When the file exceeds 1 MB or 5,000 lines, the older half is dropped (written to a temporary file and
swapped in). A failure to write is logged and never affects streaming.

```json
{"started":"2026-09-30T21:04:11","ended":"2026-09-30T22:15:40","duration_s":4289,"client":"Tablet","client_uuid":"0123-ABCD","app":"Desktop","width":2560,"height":1440,"fps":120,"bitrate_kbps":50000,"codec":"HEVC","hdr":false,"pacing_mbps":150,"pacing_auto":true,"fec_percent":20,"fec_percent_max":20,"cursor":"host","end_reason":"disconnect","encode_budget_max_pct":41,"encode_budget_exceeded":false}
```

| Field | Meaning |
|---|---|
| `started`, `ended` | Host local time (`yyyy-MM-ddTHH:mm:ss`); the start is when the client finished the stream setup (RTSP ANNOUNCE) |
| `duration_s` | Length in seconds |
| `client`, `client_uuid` | Device name given at pairing and its UUID (omitted when unknown) |
| `app` | The app that was streamed |
| `width`, `height`, `fps` | Resolution and frame rate requested by the client |
| `bitrate_kbps` | Requested bitrate limited by `max_bitrate` (before the FEC and audio share) |
| `codec`, `hdr` | H.264, HEVC or AV1, and whether the client asked for 10-bit (HDR) |
| `pacing_mbps`, `pacing_auto`, `fec_percent` | Packet pacing, whether it was automatic, configured FEC percentage |
| `fec_percent_max` | Highest FEC percentage the session used; equals `fec_percent` unless the client raised it (see [Live FEC changes](#live-fec-changes)) |
| `end_reason` | `disconnect` (the client closed the connection), `timeout` (no answer), `host` (stopped on the host, including a revoked view permission), `app_exit` (the app ended), `client_quit` (the client asked to quit the app); omitted when unknown |
| `encode_budget_max_pct` | Highest per-second encode time of the session as a percentage of the frame interval (see [Encode time guard](#encode-time-guard)); omitted when no full second was measured |
| `encode_budget_exceeded` | `true` when the encoder stayed above 65% of the frame interval for 10 seconds at any time; omitted with `encode_budget_max_pct` |
| `cursor` | `local` when the client drew the mouse cursor itself at any time (see [Local cursor](#local-cursor)), else `host` |

A network loss can also end as `disconnect` when the control connection times out first, so
`disconnect` does not always mean a clean exit.

The web UI reads the history with `GET /api/shell/sessions` (signed in): `{"status":true,"sessions":[...]}`,
newest first, at most 100 entries; lines that are not valid JSON are skipped.

## Webhooks

Off by default. When **Settings > Network > Webhooks** has a URL, Shell POSTs one JSON object per
event to it. Nothing else changes: with no URL no connection is ever opened, and the code only
queues events; one background thread sends them, so a slow or unreachable receiver never delays
pairing, streaming or the web UI.

| Key | Default | Meaning |
|---|---|---|
| `webhook_url` | empty (off) | `https://` URL that receives the events |
| `webhook_events` | `pairing,stream,app` | Event groups to send, comma separated; `none` turns every group off (the web UI writes it when no group is checked, since an empty value is not saved); unknown names are ignored with a warning. `test` is always sent |
| `webhook_include_client_ip` | `disabled` | Add `client_ip` to pairing and stream events |
| `webhook_allow_http_private` | `disabled` | Accept an `http://` URL whose host is a literal private, link-local or Tailscale IP address (the ranges of `net::from_address`: 10/8, 172.16/12, 192.168/16, 100.64/10, 169.254/16, fc00::/7, fe80::/64) or `localhost` |
| secret | not set | HMAC-SHA256 key, stored as `webhook_secret` next to the key configured by `pkey` (default `config\credentials\webhook_secret`), not in `shell.conf`; see below |

**URL rules.** The URL is checked when Shell starts (after **Apply**): `https://` is required, or
`http://` under the rule above; a user name or password in the URL (`user:pw@host`), a space or a
control character, an empty host or a bad port are refused. A refused URL is logged once as a
warning and leaves webhooks off; the settings page shows the reason.

**Delivery.** Each event is one `POST` with `Content-Type: application/json` and
`User-Agent: Shell/<version>`. The queue holds 64 events; when it is full the oldest is dropped
(one warning until the queue drains). Limits per event: 5 s to connect, 10 s in all, at most 2
retries after 2 s and 8 s, and only for connection errors (DNS, connect, timeout, reset) and HTTP
5xx; 4xx and other errors are not retried. Redirects are never followed: a 3xx answer is a failure.
No proxy is ever used, whatever `http_proxy`, `https_proxy` or `all_proxy` say in the environment.
TLS: the libcurl Shell links uses Schannel, so the certificate chain and the host name are verified
by Windows against its certificate store (`CURLSSLOPT_NATIVE_CA` is also set for an OpenSSL build),
TLS 1.2 or newer, SNI set from the host name. Revocation is checked on a best-effort basis
(`CURLSSLOPT_REVOKE_BEST_EFFORT`): a revocation server that cannot be reached does not fail the
delivery, a certificate known to be revoked does. A self-signed receiver needs its CA in the Local
Machine Trusted Root store, or a private address over `http://`. A failure is logged once as a
warning; the same failure is not logged again until a delivery succeeds. Shell never reads the
response body.

**Signature.** With a secret set, every request also carries
`X-Shell-Timestamp: <Unix seconds>` and `X-Shell-Signature: sha256=<hex>` where `<hex>` is the
lowercase HMAC-SHA256 of the exact request body with the secret as the key. The receiver recomputes
it over the raw body and compares; the timestamp lets it refuse old requests. The secret is written
through `POST /api/shell/webhook/secret` (`{"secret":"..."}`; empty removes it; signed in) into
`webhook_secret` next to the key configured by `pkey` (default `config\credentials`), where it
inherits the folder's permissions (Administrators and SYSTEM only). The web UI never reads it back and only shows "set" or "not set". A new secret is used
from the next event on, with no restart.

**Events.** Every object starts with `event`, `time` (ISO 8601 UTC) and `host` (the host name shown
to clients), followed by the event's fields, and `client_ip` last when the option is on:

| `event` | Fields | When |
|---|---|---|
| `pairing.succeeded` | `client` (device name) | A device finished pairing |
| `pairing.failed` | `client`, `reason` | A pairing attempt failed: `No PIN was entered in time`, `PIN or certificate check failed` (a wrong PIN), `Too many pairing attempts; try again later`, or the protocol error that ended it. Never the PIN or any key material |
| `stream.started` | `client`, `client_uuid`, `app`, `width`, `height`, `fps`, `bitrate_kbps`, `codec`, `hdr` | A stream is running (the first part of the session record) |
| `stream.ended` | The whole [session record](#session-history): `started`, `ended`, `duration_s`, `client`, `client_uuid`, `app`, `width`, `height`, `fps`, `bitrate_kbps`, `codec`, `hdr`, `pacing_mbps`, `pacing_auto`, `fec_percent`, `fec_percent_max`, `cursor`, `end_reason`, `encode_budget_*` | A stream ended; sent with the same JSON builder as `session_history.jsonl` |
| `app.started` | `app` | An app was launched for a client (also `Remote Input`) |
| `app.ended` | `app` | The app was closed or ended |
| `test` | `message` | **Send test event** on the settings page |

```json
{"event":"pairing.succeeded","time":"2026-10-12T09:50:00Z","host":"GAMING-PC","client":"Tablet"}
{"event":"pairing.failed","time":"2026-10-12T09:51:30Z","host":"GAMING-PC","client":"Phone","reason":"No PIN was entered in time","client_ip":"192.168.1.20"}
{"event":"stream.started","time":"2026-10-12T10:00:02Z","host":"GAMING-PC","client":"Tablet","client_uuid":"0123-ABCD","app":"Desktop","width":2560,"height":1440,"fps":120,"bitrate_kbps":50000,"codec":"HEVC","hdr":false}
{"event":"stream.ended","time":"2026-10-12T11:11:33Z","host":"GAMING-PC","started":"2026-10-12T19:00:02","ended":"2026-10-12T20:11:33","duration_s":4291,"client":"Tablet","client_uuid":"0123-ABCD","app":"Desktop","width":2560,"height":1440,"fps":120,"bitrate_kbps":50000,"codec":"HEVC","hdr":false,"pacing_mbps":150,"pacing_auto":true,"fec_percent":20,"fec_percent_max":20,"cursor":"host","end_reason":"disconnect"}
{"event":"app.started","time":"2026-10-12T10:00:01Z","host":"GAMING-PC","app":"Desktop"}
{"event":"test","time":"2026-10-12T09:49:00Z","host":"GAMING-PC","message":"Test event from the Shell web UI"}
```

`started` and `ended` inside `stream.ended` stay in host local time, like the session record;
`time` is UTC.

**What leaves the PC.** Only to the URL you set: the host name, device names, app names, the
stream figures above and, when you turn it on, the client IP. No PIN, no certificate, no key, no
password and no log line is ever sent. The receiver's answers are discarded.

**Web UI.** `GET /api/shell/webhook` (signed in) returns `{"status":true,"enabled":...,"url_error":
null|"...","host":"...","secret_set":...,"events":[...],"queued":n,"sent":n,"failed":n,"dropped":n,
"last":{"event","time","ok","http_code","error","attempts"}|null}`. `POST /api/shell/webhook/test`
queues a `test` event; its result shows up in `last` once the thread is done. The settings page
polls for it after **Send test event**.

## Icons

The tray and web UI icons are 16x16 pixel art (a spiral seashell). The pixel grid lives in
`shell/branding/deploy.py`; the tray states reuse it with a colored dot (streaming coral, paused amber,
locked grey). `shell/branding/pixelart.py` scales the pixels without a renderer and writes the PNG, ICO
and SVG files:

```powershell
python shell/branding/deploy.py
```
