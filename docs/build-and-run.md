# Build And Run

This document describes the supported local build paths for this branch and explains what each binary is for.

Initialize both pinned submodules before configuring: `git submodule update --init --recursive`.
NFT/cadastral geodesy uses GeographicLib **2.3.0**, commit
`849284b9620b2ee4f0b9841b6822c67f9d0fa0c2`. CMake requires that exact clean
checkout and builds it statically in binary64 precision with strict floating-point
options. It does not substitute a system installation or load external geodetic
datasets. The same dependency is linked into the node, wallet, and C SDK.

GeoJSON display clipping uses header-only Boost.Geometry (system Boost >= 1.71
or `boost-geometry` at the pinned vcpkg baseline). It is outside consensus.
Portable bundles include GeographicLib and Boost licences.

The supported code paths on this branch are:

- `geocoin/` for the node, wallet, RPC, consensus, and Qt GUI
- `libgeo/` for the C SDK

## What Each Binary Does

### Desktop wallet

- `geocoin-core` on Linux
- `geocoin-core.exe` on Windows

This is the desktop wallet application with the Qt interface.

Use it when you want:

- a wallet window
- balances and transaction history
- normal user interaction

This is the usual entry point for a desktop user.

### Full node

- `geocoin-node` on Linux
- `geocoin-node.exe` on Windows

This is the real blockchain node process.

It is responsible for:

- keeping the local blockchain state
- connecting to peers
- syncing blocks
- exposing RPC
- serving as the backend for wallet and testing flows

Start this when you need:

- a real local node
- wallet RPC
- chain sync
- node testing

### Genesis developer tool

The genesis generator is excluded from release packages and disabled by default.
Developers can build it explicitly with `-DBUILD_GENESIS_CLI=ON` and target
`genesis` (or `mine_genesis`). It prints parameters for source code; it is not
used for ordinary mining or node startup.

The working CPU miner is `geocoin-rpc-miner`; GPU mining uses
`geocoin-gpu-miner`. Both connect to a node using miner RPC credentials.

### SDK library

- `geo.dll` on Windows
- `libgeo.so` on Linux

This is the C SDK for external apps.

It is a library, not a standalone program. You do not launch it directly.

## Typical Startup Order

### Normal desktop usage

Start the GUI wallet; it starts its own local node. Run `geocoin-node` separately
only for headless operation or with a different data directory.

## Windows Build

The supported Windows path on this branch is:

- `ClangCL`
- `Visual Studio`
- `vcpkg x64-windows-static-release`
- static Qt/OpenSSL/RocksDB linkage

### Open the build shell

Open a Visual Studio x64 developer command prompt, or run:

```cmd
call D:\VS\VC\Auxiliary\Build\vcvarsall.bat x64
```

### Set required paths

Example:

```cmd
set REPO_ROOT=H:\PROJECTS\GeoCoin
set VCPKG_ROOT=H:\PROJECTS\VCPKG
```

Important:

- `VCPKG_ROOT` must not contain `+`. Static Qt can break on such paths.
- Do not use the old dynamic `QT_ROOT_DIR` release path for production Windows artifacts.

### Build the GUI and node

```cmd
cmake --preset windows-clang-static-release-gui -S .
cmake --build out\build\windows-clang-static-release-gui --config Release --parallel
```

This produces the main Windows binaries under:

```text
out\build\windows-clang-static-release-gui\
```

### Build the SDK

```cmd
cmake --preset windows-clang-static-release -S libgeo
cmake --build libgeo\out\build\windows-clang-static-release --config Release --parallel
```

This produces:

- `geo.dll` with statically linked dependencies
- `geo.lib`, its import library
- the public C API header

### Build the static Windows bundle

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_portable_windows.ps1
```

Or:

```powershell
make portable-windows
```

The final Windows bundle appears here:

```text
out\portable\GeoCoin-windows-x64-static
```

The archive appears here:

```text
out\portable\GeoCoin-windows-x64-static.zip
```

Static verification:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check_windows_static_imports.ps1 -BundleDir out\portable\GeoCoin-windows-x64-static
Get-ChildItem out\portable\GeoCoin-windows-x64-static -Recurse -Filter *.dll
```

## Windows Run Commands

### Start the full node

From the portable bundle root:

```cmd
geocoin-node.cmd --data-dir .\data --network degree
```

What this does:

- starts the production network node and synchronizes the chain
- creates or uses `.\data`

For server RPC configuration, use the admin package's deployment templates and
`geocoin-node --help`. Test manifests remain in the source repository.

### Start the GUI wallet

From the same bundle root:

```cmd
bin\geocoin-core.exe
```

The Windows GUI starts as a GUI subsystem binary and should not open a console window.
It starts its local full node backend for normal wallet usage.

## Linux / WSL Build

The supported Linux path is Ubuntu 22.04 or WSL Ubuntu 22.04.

### Install dependencies

```bash
chmod +x scripts/bootstrap-ubuntu-22.04.sh
./scripts/bootstrap-ubuntu-22.04.sh --with-gui
```

### Build the GUI and node

```bash
cmake --preset linux-release-gui -S .
cmake --build out/build/linux-release-gui --parallel
```

### Build the SDK

```bash
cmake --preset linux-release -S libgeo
cmake --build libgeo/out/build/linux-release --parallel
```

### Build the self-contained Linux bundle

```bash
make portable-linux
```

The final Linux bundle appears here:

```text
out/portable/GeoCoin-linux-x86_64
```

The archive appears here:

```text
out/portable/GeoCoin-linux-x86_64-portable.tar.gz
```

## Linux Run Commands

### Start the full node

From the portable bundle root:

```bash
./geocoin-node --data-dir ./data --network degree
```

### Start the GUI wallet

From the same bundle root:

```bash
./geocoin-core
```

## Smoke Commands

### GUI smoke

Windows:

```powershell
.\out\portable\GeoCoin-windows-x64-static\bin\geocoin-core.exe -platform offscreen --smoke-test -no-auto-connect
.\out\portable\GeoCoin-windows-x64-static\bin\geocoin-core.exe --gui-e2e-test --data-dir $env:TEMP\geocoin-gui-e2e -addnode=node1.geocoin.network:3030 -addnode=node2.geocoin.network:3030 -addnode=node3.geocoin.network:3030
```

Linux:

```bash
xvfb-run -a ./out/portable/GeoCoin-linux-x86_64/bin/geocoin-core -platform offscreen --smoke-test -no-auto-connect
```

### Full node smoke with mining and restart

Windows:

```powershell
Set-Content -Path "$env:TEMP\geocoin-rpc.pass" -Value "smoke-pass"
python scripts/node_rpc_smoke.py --node-binary .\out\portable\GeoCoin-windows-x64-static\bin\geocoin-node.exe --network degree-test --rpc-user smoke --rpc-pass-file "$env:TEMP\geocoin-rpc.pass" --mined-blocks 3 --confirm-blocks 1 --mining-threads 32 --timeout 1800
```

Linux:

```bash
printf '%s\n' smoke-pass > /tmp/geocoin-rpc.pass
chmod 600 /tmp/geocoin-rpc.pass
python3 scripts/node_rpc_smoke.py --node-binary ./out/portable/GeoCoin-linux-x86_64/bin/geocoin-node --network degree-test --rpc-user smoke --rpc-pass-file /tmp/geocoin-rpc.pass --mined-blocks 3 --confirm-blocks 1 --mining-threads 32 --timeout 1800
```

## Which Thing To Launch

If you want the wallet window:

- run `geocoin-core`

If you want a real full node:

- run `geocoin-node`

If you need the old compatibility alias:

- run `geocoin-node`

If you want to integrate from another app:

- use `geo.dll` or `libgeo.so`

If you want normal day-to-day usage:

- run the node first
- then run the GUI

## Release package profiles

Linux: set `BUNDLE_PROFILE=user`, `admin` or `sdk` when running
`scripts/package_portable_linux.sh`. Windows: pass `-BundleProfile user`,
`admin` or `sdk` to `scripts/package_portable_windows.ps1`.

The default user archive contains the wallet, node, CPU RPC miner and an
optional GPU miner. The admin archive contains the node, database tools,
CPU RPC miner and deployment templates. SDK headers and the shared C library
are distributed only in the SDK archive. Windows SDK dependencies are linked
statically into `geo.dll`; `geo.lib` is its import library.

Each archive includes its own short `README.txt` and dependency notices.
`config/release-bundles.json` lists the permitted application files. Packaging
validates both the staged directory and the actual archive. Linux dependency
reports stay in `out/packaging/`, outside the user download. The release workflow
builds all six archives and smoke-tests the packaged wallet and C SDK.
