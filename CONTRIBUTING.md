# Contributing to Shell

Thank you for your interest in Shell. Bug reports, fixes, documentation improvements and translations
are welcome.

## Before you start

- Search the [issues](https://github.com/junopark00/hermit-shell/issues) first; someone may already be
  working on the same thing.
- For anything larger than a small fix, open an issue to discuss the change before writing code.
- Shell targets **Windows 10 and 11 (x64)** only. Changes to the inherited Linux and macOS code are not
  accepted unless they are needed to keep the shared code compiling.
- Keep compatibility with GameStream clients. Protocol names such as `SUNSHINE_SERVER_FREE`, the
  `SUNSHINE_*` environment variables for apps and the default ports are part of that contract and must
  not be renamed.
- Report security issues privately, as described in [SECURITY.md](SECURITY.md), not in public issues.

## Building

See [docs/building.md](docs/building.md). In short, with MSYS2 UCRT64 and the official Node.js installed:

```powershell
git clone --recurse-submodules https://github.com/junopark00/hermit-shell.git
cd hermit-shell
.\shell\Build-Shell.ps1 -Package
```

## Testing

Run the tests that cover your change before opening a pull request:

```powershell
# Installer (first installation, update, rollback, uninstall, Apollo migration) against scratch folders
powershell -NoProfile -ExecutionPolicy Bypass -File .\shell\Test-InstallShell.ps1

# Other PowerShell tools
powershell -NoProfile -ExecutionPolicy Bypass -File .\shell\Test-ShellConnect.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\shell\Test-HermitStats.ps1
pwsh -NoProfile -File .\shell\Test-ShellRecovery.ps1    # PowerShell 7.4 or later
```

The installer tests never touch the real installation, service or machine-wide settings.

For C++ changes, build the unit tests (configure with `-DBUILD_TESTS=ON`) and run
`build\tests\test_shell.exe`. For streaming changes, also test a real session with Hermit and with
Moonlight, and describe what you tested in the pull request: GPU and encoder, client, resolution,
frame rate, bitrate and network.

## Coding style

- **C++**: C++20, formatted with the repository's `.clang-format`. Follow the style of the surrounding
  code; keep platform-specific code under `src/platform/windows`.
- **PowerShell**: scripts must run on **Windows PowerShell 5.1** (except the recovery module, which
  needs 7.4). Use `Set-StrictMode -Version Latest` and `$ErrorActionPreference = 'Stop'`, keep script
  files ASCII-only, and give every script that changes the system a `-NoService` (or similar) mode
  that tests can run against scratch folders.
- **Web UI**: Vue 3 and Bootstrap. Every user-facing string goes through vue-i18n: add it to
  `src_assets/common/assets/web/public/assets/locale/en.json` and, if you can, `ko.json`.
- **Text**: code comments, log messages, documentation and default strings are in English. Korean
  appears only as a translation (`ko.json`, the Korean tray strings) and in `README.ko.md`.
- Comments explain why the code does something, not what it does.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request focused on one change; split unrelated changes.
3. Write commit messages with a short summary line (under 72 characters) and a body that explains the
   reason for the change.
4. Update the documentation (`README.md`, `README.ko.md`, `docs/`) when behavior or options change.
5. Make sure the build and the relevant tests pass.

By submitting a contribution you agree that it is licensed under the
[GNU General Public License v3.0](LICENSE), the license of this project.
