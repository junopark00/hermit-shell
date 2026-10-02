# Features

This page describes what Shell adds to the Sunshine and Apollo streaming core, how each feature
behaves and which client endpoints it uses.

## Names and locations

| Item | Value |
|---|---|
| Service | `ShellService` (`tools\shellsvc.exe` runs `shell.exe` in the signed-in user's session) |
| Install folder | `C:\Program Files\Shell` |
| Settings, pairing, apps, log | `config\shell.conf`, `config\shell_state.json`, `config\apps.json`, `config\shell.log` |
| Session history | `config\session_history.jsonl` |
| Install backups | `C:\ProgramData\Shell\backups` |
| Firewall rule | `Shell` (inbound, TCP and UDP, for `shell.exe`) |
| Web UI | `https://localhost:47990` |
| Environment for apps | `SHELL_APP_*` and `SHELL_CLIENT_*`; the `SUNSHINE_*` names are set as well, for scripts written for Sunshine-based hosts |
| Protocol | GameStream compatible: mDNS `_nvstream._tcp`; TCP 47984, 47989, 47990 (web UI) and 48010, UDP 47998-48000, 48002 and 48010 (default ports) |

The host name shown to clients is **Settings > General > Host Name** (`shell_name`). A configuration
file that still uses the Sunshine key `sunshine_name` is read as well.

## Web UI

- **Home** shows a summary line (host, version, platform, state, running app, connected and paired
  devices) and these tables:
  - **Streaming now**: one row per active stream with device, app, elapsed time, resolution and frame
    rate, bitrate, round-trip time and recovery requests, refreshed every 2 seconds. A stream can be
    disconnected from its row; the app on the host is paused or closed according to its settings.
  - **Paired devices** with their state and permissions.
  - **Recent sessions**: the last 20 sessions from the session history (up to 100 with "Show more").
  - **Install backups** made by `Install-Shell.ps1`, newest first, each with a "Copy rollback command"
    button. The web UI does not run the rollback itself, because the service stops during a rollback.
- **Bitrate** in the live table follows changes made during the stream. Hover over it to see the
  encoder target, the packet pacing and whether live changes are supported.
- **Round-trip time** comes from the control connection (ENet pings about every 0.5 seconds). It shows
  `-` when no answer arrived for 2 seconds.
- **Recovery requests** count the key frame and reference frame invalidation requests of the client,
  which it sends mostly after losing frames. After the first minute the last minute's count is colored:
  0 green, 1 to 3 yellow, more red.
- **Theme**: neutral grey layers with one teal accent, in light and dark. Fonts (IBM Plex Sans) are served
  by the host, so the UI works without internet access.

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
  digits) and `failed` (the waiting device's request was invalid; it was told pairing failed).
- A device that starts pairing again replaces its earlier unfinished attempt (after a wrong PIN or a
  dropped connection), so a wrong PIN never blocks the next try. If the earlier attempt is still
  waiting for a PIN, its request is answered with a failed pairing (`status_code` 400, "Superseded by
  a newer pairing attempt") instead of being left to time out.
- A device waits up to 5 minutes for the PIN. Its request then times out (the HTTP server's
  300-second limit) and Shell drops the attempt, so a PIN entered later never goes to a device that
  gave up; start pairing again on the device. With several devices waiting, the PIN goes to the one
  that started pairing last.
- `GET /unpair?uniqueid=<id>` on the HTTP port: clients send it after a pairing failed on their side
  (a wrong PIN) and from their own Unpair command. Shell drops the unfinished pairing of that
  uniqueid, answering a request it kept waiting for a PIN, and replies `<root status_code="200"/>`,
  so the client reports the wrong PIN. It never removes a paired device: without an unfinished
  pairing the reply is `status_code` 400 ("Unpair this device in the host's web UI"), so a client's
  Unpair command shows an error instead of a false success. Paired devices are removed under
  **Device Management**. Over HTTPS `/unpair` still answers 404.
- **Prefill for companion clients**: Hermit and Hermit for Android open
  `https://<host>:47990/pin#pin=1234&name=<URL-encoded device name>`. The fragment never leaves the
  browser; the page fills the fields from it, focuses the **Pair** button and removes the fragment from
  the address (so a reload starts clean). Nothing is sent until the user presses **Pair**.

## Virtual display

Shell uses the SudoVDA driver to create a virtual display for a session, at the resolution and refresh
rate the client asks for. A client can be set to always use the virtual display (per device, on the
**Pairing** page); apps can be set to use it, and `headless_mode` uses it whenever no monitor is on.

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

## Remote shutdown and restart

In Hermit, right-click the PC in the PC list (long-press on Android) and choose to shut it down or
restart it.

- `GET /actions/power?action=query` tells the client whether it may do this and which other devices are
  streaming; Hermit names them and asks again before going ahead.
- `GET /actions/power?action=shutdown[&force=1]` shuts the PC down after 5 seconds (`InitiateShutdownW`,
  full shutdown); `action=restart` restarts it. By default running apps are asked to close first, so an
  app with unsaved work can stop the shutdown; `force=1` closes those apps as well.
- Only paired devices with the **launch apps** permission can do this. To turn the PC on again, use
  Wake-on-LAN.

## Ctrl+Alt+Del

Windows ignores Ctrl+Alt+Del sent as ordinary input. When Shell runs as a service and receives
Ctrl+Alt+Del from a client, the service calls `SendSAS`, which opens the secure screen (lock, switch
user, Task Manager, sign-out). This needs the `SoftwareSASGeneration` policy to allow services, which
`Install-Shell.ps1` sets on install and update. When Shell is not running as a service, or the policy is
missing, the keys are passed through unchanged and Shell logs a warning once.

## Clipboard

The upstream `/actions/clipboard` endpoint carries text only. On Windows Shell adds:

| Request | Content | Permission |
|---|---|---|
| `GET type=info` | `seq=<change number>`, `type=<text\|image\|files\|none>`, `files=stream` | Clipboard read |
| `GET`/`POST type=image` | PNG; reads the "PNG" format or converts CF_DIB, writes both "PNG" and CF_DIBV5 | Clipboard read / write |
| `GET`/`POST type=files` | An "APCF" archive (up to 256 MB and 1,000 files) | Clipboard plus file download / upload |
| `GET type=filelist` | The copied files and folders as a text list (up to 4 GB and 1,000 items), see below | Clipboard read plus file download |
| `GET type=filedata&snapshot=<id>&index=<n>[&offset=<bytes>]` | The bytes of one file from that list | Clipboard read plus file download |

- `POST` replies with the new change number (`seq=`), so a client does not read back what it wrote.
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
- Received files are checked first (paths, duplicates, size), then extracted with the signed-in user's
  rights to `%LOCALAPPDATA%\Temp\ShellClipboard` and placed on the clipboard as CF_HDROP ("copy").
  Files that are sent are read with the user's rights; symbolic links and junctions are not
  followed. Other reparse points, such as OneDrive Files On-Demand placeholders, are ordinary files:
  reading one fetches its content.
- When another program holds the clipboard, Shell retries for about 0.3 seconds. If it is still
  held, `GET type=text`, `type=image`, `type=files` and `type=filelist` and every `POST` answer 503
  with the body `clipboard-busy` and a line of text; clients keep their last change number and try
  again later. `type=info` and `type=filedata` never wait for the clipboard. An empty 200 reply
  (`type=text`, `type=image`, `type=files`, `type=filelist`) means only that the clipboard holds
  nothing of that type. For `type=text` this is what clients that only know text see as well; real
  text is still a 200 with the text.
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
  opened. If the file cannot be read to the end, the connection closes early.
- Walking folders for `type=filelist`, reading files for `GET type=files` and checking and unpacking
  `POST type=files` run on a separate worker thread, one at a time, so other requests from any
  device are not held up meanwhile.
- Opening and reading the files for `type=filedata` run on four other threads, so a long job on that
  worker (such as a 256 MB `type=files` archive) does not hold up downloads. Each file is read in
  order, one chunk after the previous one was sent, and uses one thread at a time. A read that stalls
  (a OneDrive placeholder being fetched, a slow network share) holds up only its own thread; the
  other downloads continue on the rest, and only four stalled files at once hold up all of them.
- The HTTPS server ends a request whose upload or response takes longer than 30 minutes (1,800
  seconds), which leaves a 256 MB `type=files` transfer room down to about 1.2 Mbps. Longer
  `type=filedata` downloads are cut; clients continue with `offset=<bytes received>`. The 4 GB limit
  for lists only bounds what one copy can bring over (about 18 minutes at 30 Mbps); file data is never
  buffered.
- Shell logs each list (device, item count, total bytes) and the end of each file download (device,
  index, bytes, time), never names or contents.
- Clients that only know `type=text` are unaffected.

## Session history

At the end of every stream Shell appends one JSON line to `session_history.jsonl` in the config folder.
When the file exceeds 1 MB or 5,000 lines, the older half is dropped (written to a temporary file and
swapped in). A failure to write is logged and never affects streaming.

```json
{"started":"2026-09-30T21:04:11","ended":"2026-09-30T22:15:40","duration_s":4289,"client":"Tablet","client_uuid":"0123-ABCD","app":"Desktop","width":2560,"height":1440,"fps":120,"bitrate_kbps":50000,"codec":"HEVC","hdr":false,"pacing_mbps":150,"pacing_auto":true,"fec_percent":20,"end_reason":"disconnect"}
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
| `pacing_mbps`, `pacing_auto`, `fec_percent` | Packet pacing, whether it was automatic, FEC percentage |
| `end_reason` | `disconnect` (the client closed the connection), `timeout` (no answer), `host` (stopped on the host, including a revoked view permission), `app_exit` (the app ended), `client_quit` (the client asked to quit the app); omitted when unknown |

A network loss can also end as `disconnect` when the control connection times out first, so
`disconnect` does not always mean a clean exit.

The web UI reads the history with `GET /api/shell/sessions` (signed in): `{"status":true,"sessions":[...]}`,
newest first, at most 100 entries; lines that are not valid JSON are skipped.

## Icons

The tray and web UI icons are 16x16 pixel art (a spiral seashell). The pixel grid lives in
`shell/branding/deploy.py`; the tray states reuse it with a colored dot (streaming coral, paused amber,
locked grey). `shell/branding/pixelart.py` scales the pixels without a renderer and writes the PNG, ICO
and SVG files:

```powershell
python shell/branding/deploy.py
```
