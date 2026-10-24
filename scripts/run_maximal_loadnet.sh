#!/usr/bin/env bash
# Maximal isolated loadnet: 4 foundation managers, 4 archive nodes, 1000 accounts.
# Rebuilds node/miner/signing-tool, then runs the live Rust harness with NFT,
# sidechain, query, chaos, restart, reorg, crash and transfer rounds.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/cmake-build-release}"
JOBS="${JOBS:-$(nproc)}"
STAMP="$(date +%Y%m%d-%H%M%S)"
WORK_DIR="${WORK_DIR:-${ROOT}/loadnet-runs/maximal-4n-1000-${STAMP}}"

cd "${ROOT}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON
cmake --build "${BUILD_DIR}" \
  --target geocoin-node geocoin-rpc-miner sidechain_test_tool \
  --parallel "${JOBS}"

HARNESS_TARGET="${ROOT}/tools/loadnet-stress/target"
CARGO_TARGET_DIR="${HARNESS_TARGET}" cargo test --manifest-path "${ROOT}/tools/loadnet-stress/Cargo.toml"
CARGO_TARGET_DIR="${HARNESS_TARGET}" cargo build --release --manifest-path "${ROOT}/tools/loadnet-stress/Cargo.toml"
HARNESS="${HARNESS_TARGET}/release/geocoin-loadnet-stress"

mkdir -p "${WORK_DIR}"
echo "[maximal-loadnet] work-dir=${WORK_DIR}"
echo "[maximal-loadnet] node=${BUILD_DIR}/geocoin-node"
echo "[maximal-loadnet] harness=${HARNESS}"

exec "${HARNESS}" \
  --node-binary "${BUILD_DIR}/geocoin-node" \
  --miner-binary "${BUILD_DIR}/geocoin-rpc-miner" \
  --signing-tool "${BUILD_DIR}/sidechain_test_tool" \
  --activation-manifest "${ROOT}/config/activation-manifest.testing-loadnet.json" \
  --work-dir "${WORK_DIR}" \
  --nodes 4 \
  --accounts 1000 \
  --workers 16 \
  --rounds 3 \
  --live-features true \
  "$@"
