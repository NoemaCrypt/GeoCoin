#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="$root/out/macos-deps"
checkout="$deps/vcpkg"
profile=dev
dry_run=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      [[ $# -ge 2 ]] || { echo "--profile needs a value" >&2; exit 2; }
      profile="$2"
      shift 2
      ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help)
      echo "Usage: scripts/bootstrap-macos.sh [--profile dev|ci|release] [--dry-run]"
      exit 0
      ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

run() {
  if [[ "$dry_run" == 1 ]]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
  echo "Build GeoCoin macOS releases from a native Apple Silicon shell." >&2
  exit 1
}
xcrun --find clang >/dev/null

case "$profile" in
  dev)
    run brew install cmake ninja pkg-config qt@5 rocksdb boost openssl@3 libqrencode
    ;;
  ci|release)
    missing_tools=()
    for formula in cmake ninja pkgconf autoconf autoconf-archive automake libtool; do
      if ! brew list --versions "$formula" >/dev/null 2>&1; then
        missing_tools+=("$formula")
      fi
    done
    if [[ ${#missing_tools[@]} -gt 0 ]]; then
      run env HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 brew install "${missing_tools[@]}"
    fi
    for command in python3 git; do
      command -v "$command" >/dev/null || {
        echo "Missing build tool: $command." >&2
        exit 1
      }
    done

    packaging="$deps/packaging"
    if [[ ! -x "$packaging/bin/python" ]]; then
      run python3 -m venv "$packaging"
    fi
    run "$packaging/bin/python" -m pip --disable-pip-version-check install \
      -r "$root/scripts/macos-dependencies/packaging-requirements.txt"

    revision="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["builtin-baseline"])' "$root/scripts/macos-dependencies/vcpkg.json")"
    if [[ ! -d "$checkout/.git" ]]; then
      run mkdir -p "$checkout"
      run git -C "$checkout" init
    fi
    if [[ "$(git -C "$checkout" rev-parse HEAD 2>/dev/null || true)" != "$revision" ]]; then
      run git -C "$checkout" fetch --depth 1 https://github.com/microsoft/vcpkg.git "$revision"
      run git -C "$checkout" checkout --detach "$revision"
    fi

    if [[ ! -x "$checkout/vcpkg" || "$checkout/scripts/vcpkg-tool-metadata.txt" -nt "$checkout/vcpkg" ]]; then
      run "$checkout/bootstrap-vcpkg.sh" -disableMetrics
    fi
    export VCPKG_DISABLE_METRICS=1
    run "$checkout/vcpkg" install \
      --x-manifest-root="$root/scripts/macos-dependencies" \
      --x-install-root="$deps/installed" \
      --overlay-ports="$root/scripts/vcpkg-ports" \
      --overlay-triplets="$root/scripts/vcpkg-triplets" \
      --triplet=arm64-osx-geocoin --host-triplet=arm64-osx-geocoin
    ;;
  *) echo "--profile must be dev, ci or release" >&2; exit 2 ;;
esac
