# GeoCoin on Apple Silicon

The macOS build targets arm64 and macOS 14.0 or later. Install Xcode and
Homebrew, then run from the repository root:

```sh
git submodule update --init --recursive
bash scripts/bootstrap-macos.sh --profile release
make portable-all
```

The bootstrap uses the macOS vcpkg manifest for Qt, OpenSSL and the other
release dependencies. The Core application runs its node inside the app
process. The GPU miner supports Metal and OpenCL.

The output directory is `out/portable/`:

| File | Contents |
| --- | --- |
| `GeoCoin-macos-arm64-portable.tar.gz` | Core, node and miners |
| `GeoCoin-macos-arm64-admin-portable.tar.gz` | Headless node and administration tools |
| `GeoCoin-macos-arm64-sdk-portable.tar.gz` | `libgeo.dylib` and C headers |
| `GeoCoin-macos-arm64.dmg` | `GeoCoin Core.app` and a link to Applications |

`GeoCoin Core.app` is also available directly in the output directory.
For a headless build use the `macos-arm64-headless` configure and build preset.
For the C SDK configure with `cmake --preset macos-arm64-release -S libgeo` and
build `libgeo/out/build/macos-arm64-release`.

Local packages are signed ad hoc. To use an installed Developer ID Application
identity, set `GEOCOIN_MACOS_SIGN_IDENTITY`. Set `GEOCOIN_MACOS_NOTARY_PROFILE`
to a Keychain profile registered with `notarytool` to notarize the app and DMG.
