#!/usr/bin/env bash
# Runs inside a clean distro container. The repository is mounted at /src.
set -euo pipefail

SRC="${GEOCOIN_SOURCE_DIR:-/src}"
BUILD_DIR="${GEOCOIN_BUILD_DIR:-/tmp/geocoin-build}"
WITH_GUI="${GEOCOIN_CONTAINER_GUI:-OFF}"
JOBS="${GEOCOIN_DEPENDENCY_JOBS:-$(nproc)}"

if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo 'GEOCOIN_DEPENDENCY_JOBS must be a positive integer' >&2
  exit 2
fi
if [[ "$WITH_GUI" != "ON" && "$WITH_GUI" != "OFF" ]]; then
  echo 'GEOCOIN_CONTAINER_GUI must be ON or OFF' >&2
  exit 2
fi

# shellcheck disable=SC1091
. /etc/os-release
printf '[container-build] %s gui=%s\n' "${PRETTY_NAME}" "$WITH_GUI"

export DEBIAN_FRONTEND=noninteractive
export TZ=UTC
ln -sfn /usr/share/zoneinfo/UTC /etc/localtime

rewrite_apt_files() {
  local file
  shopt -s nullglob
  for file in /etc/apt/sources.list /etc/apt/sources.list.d/*; do
    [[ -f "$file" ]] || continue
    sed -i "$@" "$file"
  done
  shopt -u nullglob
}

# Interim Ubuntu releases move from archive.ubuntu.com to old-releases after EOL.
if [[ "${ID:-}" == "ubuntu" ]] && ! apt-get update; then
  printf '[container-build] pointing Ubuntu apt at old-releases.ubuntu.com\n'
  rewrite_apt_files \
    -e 's|https\?://archive.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g' \
    -e 's|https\?://security.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g'
  apt-get update
fi

bootstrap=(bash "$SRC/scripts/bootstrap-linux.sh" --profile dev)
if [[ "$WITH_GUI" == "ON" ]]; then
  bootstrap+=(--with-gui)
fi
"${bootstrap[@]}"

cmake -S "$SRC" -B "$BUILD_DIR" -G Ninja \
  -D CMAKE_BUILD_TYPE=Release \
  -D CMAKE_PREFIX_PATH=/usr/local \
  -D BUILD_TOOLS=ON \
  -D BUILD_NODE_CLI=ON \
  -D BUILD_MULTISIG_CLI=OFF \
  -D BUILD_TESTS=OFF \
  -D BUILD_QT_GUI="$WITH_GUI"

targets=(geocoin-node)
if [[ "$WITH_GUI" == "ON" ]]; then
  targets+=(geocoin-qt)
fi
cmake --build "$BUILD_DIR" --target "${targets[@]}" --parallel "$JOBS"

"$BUILD_DIR/geocoin-node" --help >/dev/null
if [[ "$WITH_GUI" == "ON" ]]; then
  test -x "$BUILD_DIR/geocoin-core"
fi

printf '[container-build] rocksdb %s (%s)\n' \
  "$(pkg-config --modversion rocksdb)" \
  "$(pkg-config --variable=libdir rocksdb)"
if grep -q '^ROCKSDB_LIBRARY:' "$BUILD_DIR/CMakeCache.txt"; then
  grep -E '^ROCKSDB_(LIBRARY|INCLUDE_DIR):' "$BUILD_DIR/CMakeCache.txt"
fi
printf '[container-build] OK %s\n' "${PRETTY_NAME}"
