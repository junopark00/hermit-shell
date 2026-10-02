# Security Policy

## Supported versions

| Version | Supported |
|---|---|
| 1.0.x | Yes |
| Earlier builds | No |

Security fixes are released as new 1.0.x versions. Please update to the latest release before reporting
an issue.

## Reporting a vulnerability

**Do not report security vulnerabilities in public issues, discussions or pull requests.**

Report them privately through GitHub's private vulnerability reporting: open the
[Security Advisories](https://github.com/junopark00/hermit-shell/security/advisories/new) page of this
repository and choose **Report a vulnerability**.

Please include:

- the Shell version (shown on the web UI home page) and the Windows version,
- the affected component (for example the web UI, the pairing or streaming protocol, the service or
  `Install-Shell.ps1`),
- steps to reproduce, a proof of concept if you have one, and the impact you expect.

You will receive an acknowledgement within 7 days. We will investigate, keep you informed of the
progress, and credit you in the advisory unless you prefer otherwise. Please give us a reasonable time to
release a fix before disclosing the issue publicly.

## Scope

In scope: Shell itself, including the web UI, the GameStream endpoints, the clipboard, power and
bitrate extensions, the Windows service and the PowerShell tools in this repository.

Out of scope, please report to the respective projects:

- the Hermit clients ([Windows](https://github.com/junopark00/hermit),
  [Android](https://github.com/junopark00/hermit-android)),
- third-party components such as the SudoVDA driver, ViGEmBus, FFmpeg or OpenSSL, unless Shell uses them
  in an unsafe way.

## Hardening recommendations

- Keep the web UI reachable from the local network only (the default) and use a strong password.
- Pair only devices you own, and grant each device only the permissions it needs. Remote shutdown and
  restart require the app launch permission.
- Do not expose the GameStream ports to the internet without a VPN or another access control in front
  of them.
