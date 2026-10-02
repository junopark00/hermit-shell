# Remote access: reaching the host from outside the home network

Three ways, from simplest and safest to most manual. Recommend **Tailscale** to most people. You never
change router settings yourself; you explain and the user does it.

| | Tailscale | UPnP | Manual port forwarding |
|---|---|---|---|
| Router changes | None | Router must have UPnP on | User adds rules in the router |
| Ports open to the internet | None | Streaming ports, opened by Shell | Streaming ports |
| Works behind CGNAT | Yes | No | No |
| Extra app on each device | Yes (Tailscale) | No | No |
| Pairing from outside | Yes | No (pair at home) | No (pair at home) |
| How the client finds the host | Tailscale IP or MagicDNS name | Public IP or DDNS name | Public IP or DDNS name |

**CGNAT** (carrier-grade NAT): some internet providers, especially mobile and some fiber or cable
providers, share one public address between many customers. Then nothing from the internet can reach
the home router, so UPnP and port forwarding cannot work. Signs: the "WAN" or "Internet" address on
the router's status page differs from the address a "what is my IP" website shows, or it starts with
`100.64` to `100.127`, `10.`, `172.16` to `172.31` or `192.168.`. Ask the user to compare them; the
provider can sometimes give a public address on request.

## Option 1: Tailscale (recommended)

Tailscale (<https://tailscale.com>) builds a private, encrypted network (a "tailnet") between the
user's own devices. It is a separate product with its own account and terms; the user signs in
themselves (for example with a Google, Microsoft, Apple or GitHub account). Ask before installing it.

Steps:

1. **Host PC**: install Tailscale for Windows from <https://tailscale.com/download> and sign in.
2. **Each client device**: install Tailscale (Windows installer, or the Google Play app on Android)
   and sign in **to the same account / tailnet**. On Android only one VPN app can be active at a time
   ([Android docs](https://developer.android.com/develop/connectivity/vpn)), so Tailscale cannot run
   together with another VPN app.
3. **Find the host's Tailscale address**: in the Tailscale app or admin console, or on the host
   (read-only):

   ```powershell
   tailscale ip -4
   ```

   It is a `100.x.y.z` address ([`tailscale ip`](https://tailscale.com/kb/1080/cli)). With MagicDNS,
   on by default for tailnets created since October 2022
   ([MagicDNS](https://tailscale.com/kb/1081/magicdns)), the host's machine name also works, or its
   full name `<machine>.<tailnet>.ts.net`.
4. **Add the host in Hermit by that address**: Hermit for Windows **+** (Add PC manually), Hermit for
   Android **+** on the PC list. Automatic discovery (mDNS) does not cross Tailscale, so the host does
   not appear by itself. A host already paired at home is the same host: adding it by its Tailscale
   address uses the existing pairing.
5. **Test from outside**: for example the phone on mobile data with Wi-Fi off, Tailscale on.

How Shell treats Tailscale (verified in the source):

- Shell classifies `100.64.0.0/10` (Tailscale's IPv4 range) and `fc00::/7` (which includes Tailscale's
  IPv6 range) as **LAN**, like `192.168.x.x` (`src/network.cpp`, `lan_ips_v4` and `lan_ips_v6`, used by
  `net::from_address`).
- Consequences:
  - **Web UI**: with the default **Web UI Access** = LAN (`origin_web_ui_allowed = lan`), the web UI at
    `https://<tailscale address>:47990` is reachable from devices in the tailnet (still protected by
    the web UI password). Nothing needs to change.
  - **Pairing over Tailscale works**, including **Open Shell pairing page** (it opens the address
    Hermit reached the host at, port 47990).
  - **Encryption**: the **LAN Encryption Mode** applies (default: Shell does not encrypt video; the
    control stream is always encrypted for clients that support it). The Tailscale tunnel itself
    encrypts all traffic between the devices, so this is not a gap. **WAN Encryption Mode** is not
    used for Tailscale connections.
- The Windows Firewall rule **Shell** (added by the installer) allows `shell.exe` on all network
  profiles, so the Tailscale adapter needs no extra rule.

Tips:

- Speed: `tailscale ping <host>` from the client shows whether the connection is **direct** (the
  peer's IP) or relayed **via DERP** (a relay, shown with its city code such as `nyc` or `fra`;
  [CLI](https://tailscale.com/kb/1080/cli)). A relayed connection works but adds latency and limits bandwidth; it often
  becomes direct after a moment. Some strict networks (company, hotel) only allow relays.
- **Key expiry**: device keys expire after 180 days by default and the device then has to sign in
  again. For an always-on host the user may choose **Machines → the host's menu → Disable key
  expiry** in the admin console ([Key expiry](https://tailscale.com/kb/1028/key-expiry)); explain the
  trade-off and let the user decide.
- **Run unattended** (Windows): without it, Tailscale disconnects when the user signs out or the PC
  restarts, until someone signs in again. Right-click the Tailscale tray icon → **Preferences** →
  **Run unattended** keeps it connected ([Run unattended](https://tailscale.com/kb/1088/run-unattended)).
  Explain it and let the user decide.
- **Wake-on-LAN does not work over Tailscale**: a PC that sleeps or is shut down is not running
  Tailscale, and the wake packet ("magic packet") only works inside the local network. Keep the host
  on (or wake it from a device at home) when using it remotely.

## Option 2: UPnP

UPnP lets a program ask the home router to open ports automatically.

- **Where**: Shell web UI, **Settings > Network > UPnP** (config key `upnp`, **off by default**). Turn
  it on, press **Save**, then **Apply** (Apply restarts Shell and ends running streams).
- **What it opens**: the streaming ports listed below (TCP 47984, 47989, 48010 and UDP 47998, 47999,
  48000 for the default base port). The web UI port is only added if **Web UI Access** is set to
  "Anyone" (`origin_web_ui_allowed = wan`), which must not be done. Shell renews the mappings
  (one-hour lease, checked every 2 minutes) and removes them when it stops normally.
- **Needs**: a router that supports UPnP (often called "UPnP" or "UPnP IGD") with it turned on, and a
  real public address (no CGNAT). The Shell log shows `Completed UPnP port mappings to ...` when it
  worked; otherwise it logs why.
- **Trade-off**: UPnP on the router lets any program on the home network open ports without asking,
  including unwanted software. Many people leave it off for that reason. Explain this before the user
  turns it on in the router.
- Then connect from outside by the home's public IP or a DDNS name (see option 3, step 4).

## Option 3: Manual port forwarding

The user adds forwarding rules in the router to the host PC. Explain each part; do not do it for them.

1. **Fixed local IP for the host.** Best done as a **DHCP reservation** ("static lease", "address
   reservation") in the router for the host's network adapter, so the address never changes. Find the
   current address and MAC (read-only):

   ```powershell
   Get-NetAdapter -Physical | Where-Object Status -eq 'Up' | Select-Object Name, MacAddress
   Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias, IPAddress
   ```

2. **Forward these ports** to that local IP (same external and internal port):

   | Protocol | Port (default) | Offset from base port | Purpose |
   |---|---|---|---|
   | TCP | 47984 | base − 5 | HTTPS (paired client requests) |
   | TCP | 47989 | base | HTTP (server info, pairing) |
   | TCP | 48010 | base + 21 | RTSP (stream setup) |
   | UDP | 47998 | base + 9 | Video |
   | UDP | 47999 | base + 10 | Control |
   | UDP | 48000 | base + 11 | Audio |

   - **Do not forward TCP 47990** (base + 1): it is the web UI.
   - The base port is **Settings > Network > Port** (`port`, default 47989). Leave it at the default
     unless there is a reason; if it is changed, all ports above move with it, and the client must
     connect with `<address>:<base port>`.
   - Some guides (including Moonlight's) also list UDP 48002 and 48010. Shell does not listen on
     them; Moonlight-based clients only send Wake-on-LAN packets there.
3. **CGNAT**: if the provider uses CGNAT, forwarding cannot work (see the top of this page).
   Tailscale still does.
4. **Address to use from outside**: the home's public IP (it can change; check the router's status
   page), or better a **DDNS** name (many routers have a built-in dynamic DNS service). Hermit also
   remembers the home network's public address when it finds the host at home (it asks a public STUN
   server), so a host paired at home can often be reached from outside without adding it again.
5. **Encryption**: connections from internet addresses use **WAN Encryption Mode** (default: video
   encrypted for clients that support it).

Pairing: the web UI only accepts the local network by default and its port stays closed, so **pair at
home first**, or enter the PIN on the host itself (at `https://localhost:47990`).

## Wake-on-LAN

Hermit (**Wake PC**) and Hermit for Android (PC menu on the PC list) can send a Wake-on-LAN packet. It
reliably works only from the same local network, and only if the PC's network adapter and BIOS/UEFI
allow it. Over Tailscale it does not work; over the internet it depends on the router and is often
impossible. Shell's source tree has a read-only readiness check
(`shell\Invoke-ShellConnect.ps1 -Action Readiness`, see `docs/tools.md` in the Shell repository) for
users who want to look into it.

## Sources in the Shell repository

- Address classification: `src/network.cpp` (`lan_ips_v4`, `lan_ips_v6`, `from_address`,
  `encryption_mode_for_address`, `map_port`).
- Web UI access check: `src/confighttp.cpp` (`checkIPOrigin`).
- Defaults: `src/config.cpp` (`origin_web_ui_allowed` "lan", base port 47989, `lan_encryption_mode` 0,
  `wan_encryption_mode` 1, `upnp` off).
- UPnP mappings: `src/upnp.cpp`.
- Port offsets: `src/nvhttp.h`, `src/confighttp.h`, `src/rtsp.h`, `src/stream.h`; the web UI's
  **Settings > Network** shows the same table.
