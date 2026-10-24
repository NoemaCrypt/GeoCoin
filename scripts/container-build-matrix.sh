#!/usr/bin/env bash
# Build GeoCoin from a clean distro image using scripts/bootstrap-linux.sh.
# Default images are the releases whose clean container build has passed.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITH_GUI=OFF
JOBS="${GEOCOIN_DEPENDENCY_JOBS:-$(nproc)}"

usage() {
  cat <<'EOF'
Usage: scripts/container-build-matrix.sh [options]

Options:
  --image <ref>     Run one image (repeatable). Default: the releases listed below.
  --with-gui        Install Qt and build geocoin-core as well as geocoin-node
  -h, --help        Show this help

Requires Docker. The source tree is mounted read-only; the build stays inside
the container. GEOCOIN_DEPENDENCY_JOBS limits compile parallelism.
EOF
}

IMAGES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      IMAGES+=("${2:-}")
      shift 2
      ;;
    --with-gui)
      WITH_GUI=ON
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

if [[ ${#IMAGES[@]} -eq 0 ]]; then
  IMAGES=(
    ubuntu:20.04
    ubuntu:21.10
    ubuntu:22.04
    ubuntu:22.10
    ubuntu:23.04
    ubuntu:23.10
    ubuntu:24.04
    ubuntu:24.10
    ubuntu:25.04
    ubuntu:25.10
    ubuntu:26.04
    debian:12
    debian:13
  )
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required" >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  echo "docker daemon is not available" >&2
  exit 1
fi

fail=0
for image in "${IMAGES[@]}"; do
  printf '[container-build] starting %s\n' "$image"
  if ! docker run --rm \
      -v "$ROOT_DIR":/src:ro \
      -e GEOCOIN_SOURCE_DIR=/src \
      -e GEOCOIN_CONTAINER_GUI="$WITH_GUI" \
      -e GEOCOIN_DEPENDENCY_JOBS="$JOBS" \
      -e DEBIAN_FRONTEND=noninteractive \
      -e TZ=UTC \
      "$image" \
      bash /src/scripts/container-build-inner.sh
  then
    printf '[container-build] FAILED %s\n' "$image" >&2
    fail=1
  fi
done

exit "$fail"
