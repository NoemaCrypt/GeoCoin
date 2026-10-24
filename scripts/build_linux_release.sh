#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${PRESET:-linux-release-gui-gpu}"
GEO_BUILD_DIR="${GEO_BUILD_DIR:-$ROOT_DIR/out/build/linux-release-gui-gpu}"
LIBGEO_BUILD_DIR="${LIBGEO_BUILD_DIR:-$ROOT_DIR/libgeo/out/build/linux-release}"
DO_BOOTSTRAP=0
DO_SMOKE=1
DO_PACKAGE=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: scripts/build_linux_release.sh [options]

Options:
  --bootstrap       Run scripts/bootstrap-linux.sh --profile release before configure.
  --no-bootstrap    Do not run bootstrap (default).
  --smoke           Run GUI and miner smoke checks after build (default).
  --no-smoke        Skip smoke checks.
  --package         Build the portable Linux bundle after successful build.
  --no-package      Do not package (default).
  --dry-run         Print commands and checks without executing build/package steps.
  -h, --help        Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bootstrap)
      DO_BOOTSTRAP=1
      shift
      ;;
    --no-bootstrap)
      DO_BOOTSTRAP=0
      shift
      ;;
    --smoke)
      DO_SMOKE=1
      shift
      ;;
    --no-smoke)
      DO_SMOKE=0
      shift
      ;;
    --package)
      DO_PACKAGE=1
      shift
      ;;
    --no-package)
      DO_PACKAGE=0
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

log() {
  printf '[linux-release] %s\n' "$*"
}

run() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

expect_cache_value() {
  local cache_file="$1"
  local key="$2"
  local expected="$3"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log "would verify ${key}=${expected} in ${cache_file}"
    return 0
  fi
  if [[ ! -f "$cache_file" ]]; then
    echo "CMake cache not found: $cache_file" >&2
    exit 1
  fi
  if ! grep -Eq "^${key}:[^=]*=${expected}$" "$cache_file"; then
    echo "Expected ${key}=${expected} in $cache_file" >&2
    grep -E "^${key}:" "$cache_file" >&2 || true
    exit 1
  fi
}

if [[ "$DO_BOOTSTRAP" -eq 1 ]]; then
  run "$ROOT_DIR/scripts/bootstrap-linux.sh" --profile release --require-backends cuda,hip,opencl
fi

log "configuring GeoCoin with strict release GPU/RocksDB flags"
run cmake --preset "$PRESET" -S "$ROOT_DIR" \
  -DGEOCOIN_ENABLE_CUDA=ON \
  -DGEOCOIN_ENABLE_HIP=ON \
  -DGEOCOIN_ENABLE_OPENCL=ON \
  -DGEOCOIN_ENABLE_ROCKSDB=ON \
  -DBUILD_GPU_MINER=ON \
  -DBUILD_QT_GUI=ON

expect_cache_value "$GEO_BUILD_DIR/CMakeCache.txt" GEOCOIN_ENABLE_CUDA ON
expect_cache_value "$GEO_BUILD_DIR/CMakeCache.txt" GEOCOIN_ENABLE_HIP ON
expect_cache_value "$GEO_BUILD_DIR/CMakeCache.txt" GEOCOIN_ENABLE_OPENCL ON
expect_cache_value "$GEO_BUILD_DIR/CMakeCache.txt" GEOCOIN_ENABLE_ROCKSDB ON

log "building GeoCoin release targets"
run cmake --build "$GEO_BUILD_DIR" --target geocoin-qt geocoin-node degree_node geocoin-rpc-miner geocoin-gpu-miner --parallel

log "configuring libgeo release"
run cmake --preset linux-release -S "$ROOT_DIR/libgeo"
log "building libgeo release"
run cmake --build "$LIBGEO_BUILD_DIR" --parallel

if [[ "$DO_SMOKE" -eq 1 ]]; then
  log "running geocoin-gpu-miner help smoke"
  run "$GEO_BUILD_DIR/geocoin-gpu-miner" --help
  if command -v xvfb-run >/dev/null 2>&1; then
    log "running GUI offscreen smoke"
    run xvfb-run -a "$GEO_BUILD_DIR/geocoin-core" -platform offscreen --smoke-test -no-auto-connect
  else
    log "xvfb-run not found; GUI offscreen smoke skipped"
  fi
fi

if [[ "$DO_PACKAGE" -eq 1 ]]; then
  log "packaging portable Linux release"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '+ GEO_BUILD_DIR=%q LIBGEO_BUILD_DIR=%q %q\n' \
      "$GEO_BUILD_DIR" \
      "$LIBGEO_BUILD_DIR" \
      "$ROOT_DIR/scripts/package_portable_linux.sh"
  else
    GEO_BUILD_DIR="$GEO_BUILD_DIR" \
      LIBGEO_BUILD_DIR="$LIBGEO_BUILD_DIR" \
      "$ROOT_DIR/scripts/package_portable_linux.sh"
  fi
fi

log "done"
