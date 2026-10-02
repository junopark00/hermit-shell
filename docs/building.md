# Building Shell

Shell is built on Windows with the MSYS2 UCRT64 toolchain (GCC), CMake and Ninja. The web UI is built
with the official Node.js. `shell\Build-Shell.ps1` runs the whole build from Windows PowerShell 5.1.

Only Windows x64 builds are supported. The Linux and macOS build files inherited from upstream are still
in the tree but are not maintained or tested.

## Prerequisites

### MSYS2

Install [MSYS2](https://www.msys2.org) to `C:\msys64` (the default):

```powershell
winget install --id MSYS2.MSYS2 -e
```

Open the **MSYS2 UCRT64** shell, update it, then install the build dependencies:

```bash
pacman -Syu
pacman -S --needed \
  git \
  mingw-w64-ucrt-x86_64-boost \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-cppwinrt \
  mingw-w64-ucrt-x86_64-curl-winssl \
  mingw-w64-ucrt-x86_64-MinHook \
  mingw-w64-ucrt-x86_64-miniupnpc \
  mingw-w64-ucrt-x86_64-nlohmann_json \
  mingw-w64-ucrt-x86_64-onevpl \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-toolchain
```

`mingw-w64-ucrt-x86_64-boost` is optional: when the installed Boost is not the exact version Shell
expects, CMake downloads the Boost sources instead. `mingw-w64-ucrt-x86_64-nsis` is only needed for the
NSIS installer package, and `doxygen` and `graphviz` only for the API documentation.

### Node.js

Install the official Node.js LTS (it must end up in `C:\Program Files\nodejs`):

```powershell
winget install --id OpenJS.NodeJS.LTS -e
```

Do not use MSYS2's `mingw-w64-ucrt-x86_64-nodejs`: it is built against a libstdc++ with a
`std::bad_weak_ptr` regression that crashes Node during startup.

### Source

Clone the repository with its submodules (the path must not contain spaces):

```powershell
git clone --recurse-submodules https://github.com/junopark00/hermit-shell.git C:\src\hermit-shell
```

For an existing clone: `git submodule update --init --recursive`.

The build version comes from the latest `v*` tag reachable from the checked-out commit
(`git describe --tags --dirty --match v*`): a build of tag `v1.0.0` is `1.0.0`, a build three commits
later is `1.0.0-3-g1234abc`, and uncommitted changes add `-dirty`. A clone without tags, or a source
archive without the `.git` folder, cannot be built with `Build-Shell.ps1`; fetch the tags first
(`git fetch --tags`).

## Build

From Windows PowerShell in the repository root:

```powershell
# Build into .\build
.\shell\Build-Shell.ps1

# Build and create the portable package build\cpack_artifacts\Shell.zip
.\shell\Build-Shell.ps1 -Package
```

| Parameter | Default | Purpose |
|---|---|---|
| `-Package` | off | Create the portable ZIP with CPack |
| `-BuildType` | `Release` | `Release`, `RelWithDebInfo` or `Debug` |
| `-BuildDir` | `build` | Build folder inside the source tree |
| `-SourceDir` | this repository | Build another checkout or worktree |
| `-Msys2Root` | `C:\msys64` | MSYS2 installation |

The script checks the prerequisites, sets the version, configures with CMake
(`-G Ninja -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF`), builds with Ninja and, with `-Package`, runs
`cpack -G ZIP`. A build folder configured for another source path is recreated automatically. Nothing
is installed; to install the result, see [tools.md](tools.md#install-shellps1), or run
`.\shell\Update-Shell.ps1` to build and install in one step.

The first build downloads the npm packages of the web UI and the ViGEmBus installer (checked against a
fixed SHA-256 hash), plus the Boost and nlohmann/json sources when MSYS2 does not provide matching
versions.

## Building by hand

Inside the MSYS2 UCRT64 shell, with the official Node.js on `PATH`:

```bash
export PATH="/c/Program Files/nodejs:$PATH"
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
ninja -C build
cpack -G ZIP --config ./build/CPackConfig.cmake
```

Build options are listed in [cmake/prep/options.cmake](../cmake/prep/options.cmake). Without the
`BRANCH` and `BUILD_VERSION` environment variables (set by `Build-Shell.ps1`), CMake derives the version
from `git describe` itself.

## Tests

- PowerShell tools: see [tools.md](tools.md#tests).
- C++ unit tests: configure with `-DBUILD_TESTS=ON` and run `build\tests\test_shell.exe`.
- Web UI only: `npm install` and `npm run build` in the repository root build the pages into
  `build\assets\web`.
