# Remote access

At home, Hermit finds Shell on the local network by itself. To stream from outside the home network,
the client needs a way to reach the host. There are three, from simplest to most manual:

| | Tailscale (recommended) | UPnP | Manual port forwarding |
|---|---|---|---|
| Router changes | None | UPnP must be on in the router | Forwarding rules in the router |
| Ports open to the internet | None | Streaming ports, opened by Shell | Streaming ports |
| Works behind CGNAT | Yes | No | No |
| Extra app on each device | Tailscale | None | None |
| Pairing from outside | Yes | No, pair at home | No, pair at home |
| Address used by the client | Tailscale IP or MagicDNS name | Public IP or DDNS name | Public IP or DDNS name |

**Tailscale is recommended for most people**: nothing on the router changes, no port is open to the
internet, and it also works when the internet provider shares one public address between customers
(CGNAT), where UPnP and port forwarding cannot work.

Whichever way you choose, **never expose the web UI port (47990)** to the internet and leave
**Settings > Network > Web UI Access** at its default, local network only.

## Tailscale

[Tailscale](https://tailscale.com) connects your own devices in a private, encrypted network (a
tailnet). It is a separate service with its own account.

1. Install Tailscale on the host PC and sign in.
2. Install Tailscale on each client device (Windows, Android) and sign in to the **same tailnet**.
3. Look up the host's Tailscale address (`100.x.y.z`, shown in the Tailscale app or by
   `tailscale ip -4` on the host) or its MagicDNS name (on by default for tailnets created since
   October 2022: the machine name, or `<machine>.<tailnet>.ts.net`;
   [MagicDNS](https://tailscale.com/kb/1081/magicdns)).
4. Add the host in Hermit by that address: **+** (Add PC manually) in Hermit for Windows, **+** on the
   PC list in Hermit for Android. Automatic discovery (mDNS) does not work across Tailscale, so the
   host does not appear by itself.

How Shell treats Tailscale connections:

- Shell classifies Tailscale's address range `100.64.0.0/10` (and `fc00::/7`, which includes
  Tailscale's IPv6 range) as **local network** (`src/network.cpp`, `net::from_address`).
- So the web UI, which by default accepts the local network only, is reachable at
  `https://<Tailscale address>:47990` from devices in the tailnet, and **pairing over Tailscale works**,
  including **Open Shell pairing page** in Hermit.
- The **LAN Encryption Mode** applies (default: video not encrypted by Shell; the control stream is
  encrypted for clients that support it). Tailscale encrypts all traffic between the devices itself.
- The installer's firewall rule `Shell` applies to every network profile, so the Tailscale adapter
  needs no extra rule.

Notes:

- `tailscale ping <host>` shows whether the path is direct (the peer's IP) or relayed through a DERP
  server (shown by its city code; [CLI](https://tailscale.com/kb/1080/cli)). Relayed connections work
  but add latency and limit bandwidth.
- Wake-on-LAN does not work over Tailscale: a sleeping or shut-down PC is not running Tailscale.
- Device keys expire after 180 days by default; for an always-on host you can disable key expiry under
  **Machines → the host's menu → Disable key expiry** in the admin console
  ([Key expiry](https://tailscale.com/kb/1028/key-expiry)).
- On Windows, Tailscale disconnects when the user signs out or the PC restarts until someone signs in.
  **Run unattended** (tray icon → **Preferences**) keeps the host connected
  ([Run unattended](https://tailscale.com/kb/1088/run-unattended)).
- On Android only one VPN app can be active at a time
  ([Android docs](https://developer.android.com/develop/connectivity/vpn)), so Tailscale cannot run
  alongside another VPN app.

## UPnP

**Settings > Network > UPnP** (`upnp`, off by default) makes Shell ask the router to forward the
streaming ports listed below. Press **Save** and then **Apply** (which restarts Shell and ends running
streams).

- The router must support UPnP and have it turned on, and the home must have a public address (no
  CGNAT).
- Shell maps the six streaming ports with a one-hour lease, checks and renews them every 2 minutes
  and removes them when it stops.
  The web UI port is only mapped when **Web UI Access** is set to allow anyone, which you should not do
  (`src/upnp.cpp`).
- The log shows `Completed UPnP port mappings to ...` when the router accepted them.
- Trade-off: with UPnP on, any program on the home network can open router ports without asking.
- Connect from outside by the home's public IP or a DDNS name, as with manual forwarding.

## Manual port forwarding

1. Give the host a fixed local IP address, preferably with a DHCP reservation in the router.
2. Forward these ports in the router to that address (same external and internal port):

   | Protocol | Default port | Base port offset | Use |
   |---|---|---|---|
   | TCP | 47984 | −5 | HTTPS |
   | TCP | 47989 | 0 | HTTP |
   | TCP | 48010 | +21 | RTSP |
   | UDP | 47998 | +9 | Video |
   | UDP | 47999 | +10 | Control |
   | UDP | 48000 | +11 | Audio |

   **Do not forward TCP 47990** (base + 1): it is the web UI.
3. The base port is **Settings > Network > Port** (`port`, default 47989). If you change it, every port
   moves with it, and clients connect with `<address>:<base port>`.
4. From outside, connect by the home's public IP or a DDNS name. Hermit also remembers the home's
   public address when it finds the host on the local network, so a host paired at home is often
   reachable from outside without adding it again.
5. Connections from internet addresses use the **WAN Encryption Mode** (default: video encrypted for
   clients that support it).

Some guides also list UDP 48002 and 48010; Shell does not listen on them (Moonlight-based clients send
Wake-on-LAN packets there, see [tools.md](tools.md#connection-checks-and-wake-on-lan)).

### CGNAT

If the provider uses CGNAT, nothing from the internet reaches the home router, and neither UPnP nor
port forwarding can work. Signs: the WAN address on the router's status page differs from the address
a "what is my IP" site shows, or it is in `100.64.0.0/10` or a private range. Tailscale works in this
case; some providers also give a public address on request.

### Pairing from outside

The web UI only accepts the local network by default and its port stays closed, so with UPnP or port
forwarding, **pair while at home** (or enter the PIN on the host itself). Over Tailscale, pairing
works from anywhere.

## Wake-on-LAN

Hermit and Hermit for Android can send Wake-on-LAN packets. They reliably wake the PC only from the
same local network, and only when the network adapter and BIOS/UEFI allow it. Over Tailscale they do
not work, and over the internet it depends on the router. See
[tools.md](tools.md#connection-checks-and-wake-on-lan) for checking readiness and testing remote
wake.
