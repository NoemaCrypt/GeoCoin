# Build And Run

This document describes the supported local build paths for this branch and explains what each binary is for.

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

### Full node compatibility alias

- `degree_node` on Linux
- `degree_node.exe` on Windows

This is the same full node role under the old alias name. You do not need to run both.

Prefer:

- `geocoin-node`

Use `degree_node` only for older scripts or bundles. `degree-node` is not a built binary name.

### Genesis and mining helpers

- `genesis`
- `mine_genesis`
- `geocoin-miner`

These are helper tools for genesis or mining-related workflows.

Do not use them for normal wallet or normal node startup.

`mine_genesis` accepts:

```text
mine_genesis <threads> <bits-hex> <start-nonce> <auto|native|legacy|openssl> <degree|degree-test>
```

The committed values are documented in the repository README. The helper
verifies every found header with the network-specific proof-of-work limit before
printing a nonce/hash for source code. Re-mining consensus data invalidates all
existing datadirs for that network.

### SDK library

- `geo.dll` on Windows
- `libgeo.so` on Linux

This is the C SDK for external apps.

It is a library, not a standalone program. You do not launch it directly.

## Typical Startup Order

### Normal desktop usage

1. Start the full node if you want a real local backend.
2. Start the GUI wallet.

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

- `geo.lib`
- static SDK headers/libraries

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
echo smoke-pass>rpc.pass
echo miner-pass>rpc-miner.pass
geocoin-node.cmd --data-dir .\data --network degree-test --rpc --rpc-user smoke --rpc-pass-file .\rpc.pass --rpc-miner-user miner --rpc-miner-pass-file .\rpc-miner.pass --activation-manifest .\config\activation-manifest.testing-testnet.json
```

What this does:

- starts the local node
- creates or uses `.\data`
- enables RPC for local testing
- enables a separate `miner` RPC role for external `geocoin-rpc-miner` / `geocoin-gpu-miner`

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
printf '%s\n' smoke-pass > rpc.pass
printf '%s\n' miner-pass > rpc-miner.pass
chmod 600 rpc.pass rpc-miner.pass
./geocoin-node --data-dir ./data --network degree-test --rpc --rpc-user smoke --rpc-pass-file ./rpc.pass --rpc-miner-user miner --rpc-miner-pass-file ./rpc-miner.pass --activation-manifest ./config/activation-manifest.testing-testnet.json
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

- run `degree_node`

If you want to integrate from another app:

- use `geo.dll` or `libgeo.so`

If you want normal day-to-day usage:

- run the node first
- then run the GUI
