#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 22.04 ships RocksDB 6.11: its headers fail with Clang/C++20 and
# WriteBufferManager cannot enforce the shared memory budget with write stalls.
MIN_VERSION=6.29.5
SOURCE_VERSION=8.9.1
SOURCE_SHA256=c22d2097e7aa75629612fd020499bdae0d3e321c7bc4361960c42aaf9cbd6dc1

if pkg-config --atleast-version="$MIN_VERSION" rocksdb; then
  printf '[bootstrap-rocksdb] using RocksDB %s\n' "$(pkg-config --modversion rocksdb)"
  exit 0
fi

JOBS="${GEOCOIN_DEPENDENCY_JOBS:-4}"
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo 'GEOCOIN_DEPENDENCY_JOBS must be a positive integer' >&2
  exit 2
fi
if [[ "$EUID" -eq 0 ]]; then
  SUDO=()
else
  SUDO=(sudo)
fi

"${SUDO[@]}" apt-get update
"${SUDO[@]}" apt-get install -y zlib1g-dev libbz2-dev liblz4-dev libzstd-dev

WORK_DIR="$(mktemp -d -t geocoin-rocksdb.XXXXXXXX)"
trap 'rm -rf -- "$WORK_DIR"' EXIT
printf '[bootstrap-rocksdb] building pinned RocksDB %s\n' "$SOURCE_VERSION"
curl --fail --location --retry 3 \
  "https://codeload.github.com/facebook/rocksdb/tar.gz/refs/tags/v${SOURCE_VERSION}" \
  --output "$WORK_DIR/source.tar.gz"
printf '%s  %s\n' "$SOURCE_SHA256" "$WORK_DIR/source.tar.gz" | sha256sum --check --status
tar -xzf "$WORK_DIR/source.tar.gz" -C "$WORK_DIR"

cmake -S "$WORK_DIR/rocksdb-$SOURCE_VERSION" -B "$WORK_DIR/build" -G Ninja \
  -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=/usr/local \
  -D CMAKE_INSTALL_LIBDIR=lib -D CMAKE_POSITION_INDEPENDENT_CODE=ON \
  -D ROCKSDB_BUILD_SHARED=ON \
  -D PORTABLE=ON -D USE_RTTI=ON -D WITH_SNAPPY=ON \
  -D WITH_ZLIB=ON -D WITH_BZ2=ON -D WITH_LZ4=ON -D WITH_ZSTD=ON \
  -D WITH_TESTS=OFF -D WITH_BENCHMARK_TOOLS=OFF -D WITH_CORE_TOOLS=OFF \
  -D WITH_TOOLS=OFF -D WITH_TRACE_TOOLS=OFF -D WITH_GFLAGS=OFF \
  -D WITH_LIBURING=OFF -D FAIL_ON_WARNINGS=OFF
cmake --build "$WORK_DIR/build" --parallel "$JOBS"
"${SUDO[@]}" cmake --install "$WORK_DIR/build"
"${SUDO[@]}" ldconfig

# CMake finds RocksDB with find_library, not pkg-config. Leave the distro
# package in place and the next configure can still compile against 6.11.
distro_rocksdb=()
while IFS= read -r package; do
  [[ -n "$package" ]] && distro_rocksdb+=("$package")
done < <(dpkg-query -W -f='${Package}\n' 'librocksdb*' 2>/dev/null || true)
if [[ ${#distro_rocksdb[@]} -gt 0 ]]; then
  printf '[bootstrap-rocksdb] removing distro packages: %s\n' "${distro_rocksdb[*]}"
  "${SUDO[@]}" apt-get remove -y "${distro_rocksdb[@]}"
  "${SUDO[@]}" ldconfig
fi

pkg-config --atleast-version="$MIN_VERSION" rocksdb
printf '[bootstrap-rocksdb] installed RocksDB %s from %s\n' \
  "$(pkg-config --modversion rocksdb)" \
  "$(pkg-config --variable=libdir rocksdb)"
