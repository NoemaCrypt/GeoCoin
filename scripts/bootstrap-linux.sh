#!/usr/bin/env bash
set -euo pipefail

PROFILE="dev"
DRY_RUN=0
WITH_GUI=0
REQUIRE_BACKENDS=""
ROCM_VERSION="${ROCM_VERSION:-7.2.4}"
CUDA_TOOLKIT_PACKAGE="${CUDA_TOOLKIT_PACKAGE:-cuda-toolkit-13-1}"

usage() {
  cat <<'EOF'
Usage: scripts/bootstrap-linux.sh [options]

Options:
  --profile dev|release              Dependency profile. release requires cuda,hip,opencl by default.
  --with-gui                         Install Qt and libqrencode. Implied by --profile release.
  --require-backends cuda,hip,opencl  Require and install/verify selected GPU SDK backends.
  --dry-run                          Print commands without executing package changes.
  -h, --help                         Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      PROFILE="${2:-}"
      shift 2
      ;;
    --require-backends)
      REQUIRE_BACKENDS="${2:-}"
      shift 2
      ;;
    --with-gui)
      WITH_GUI=1
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

if [[ "$PROFILE" != "dev" && "$PROFILE" != "release" ]]; then
  echo "--profile must be dev or release" >&2
  exit 2
fi

if [[ -z "$REQUIRE_BACKENDS" && "$PROFILE" == "release" ]]; then
  REQUIRE_BACKENDS="cuda,hip,opencl"
fi

if [[ "$PROFILE" == "release" ]]; then
  WITH_GUI=1
fi

if [[ ! -r /etc/os-release ]]; then
  echo "/etc/os-release is required for distro detection" >&2
  exit 1
fi

# shellcheck disable=SC1091
. /etc/os-release
OS_ID="${ID:-}"
OS_VERSION="${VERSION_ID:-}"
OS_CODENAME="${VERSION_CODENAME:-${UBUNTU_CODENAME:-}}"

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=()
else
  SUDO=(sudo)
fi

log() {
  printf '[bootstrap-linux] %s\n' "$*"
}

die() {
  echo "[bootstrap-linux] $*" >&2
  exit 1
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

run_root() {
  run "${SUDO[@]}" "$@"
}

backend_required() {
  local needle="$1"
  [[ ",${REQUIRE_BACKENDS// /}," == *",${needle},"* ]]
}

common_packages=(
  build-essential
  ca-certificates
  clang
  cmake
  curl
  file
  g++
  git
  gnupg
  librocksdb-dev
  libboost-dev
  libsnappy-dev
  libssl-dev
  ninja-build
  patchelf
  pkg-config
  python3
  python3-pip
  python3-venv
  wget
)

gui_packages=(
  libqrencode-dev
  ocl-icd-opencl-dev
  qtbase5-dev
  qtbase5-dev-tools
  qttools5-dev
  qttools5-dev-tools
  xvfb
)

opencl_packages=(
  clinfo
  ocl-icd-opencl-dev
  opencl-headers
)

version_ge() {
  local have="$1"
  local want="$2"
  [[ -n "$have" && -n "$want" ]] || return 1
  [[ "$(printf '%s\n%s\n' "$want" "$have" | sort -V | head -n1)" == "$want" ]]
}

require_supported_distro() {
  case "${OS_ID}:${OS_VERSION}" in
    ubuntu:20.10|ubuntu:21.04)
      echo "Unsupported Linux distribution: ${PRETTY_NAME:-${OS_ID} ${OS_VERSION}}" >&2
      echo "This release has no GCC 11 package, and the toolchain PPA does not publish it." >&2
      echo "Supported: Ubuntu 20.04, Ubuntu 21.10 and newer, Debian 12 and newer." >&2
      exit 1
      ;;
  esac
  case "$OS_ID" in
    ubuntu) version_ge "$OS_VERSION" "20.04" && return 0 ;;
    debian) version_ge "$OS_VERSION" "12" && return 0 ;;
  esac
  echo "Unsupported Linux distribution: ${PRETTY_NAME:-${OS_ID} ${OS_VERSION}}" >&2
  echo "Supported: Ubuntu 20.04, Ubuntu 21.10 and newer, Debian 12 and newer." >&2
  echo "CUDA auto-install remains Ubuntu 22.04, Ubuntu 24.04, and Debian 12." >&2
  echo "HIP auto-install remains Ubuntu 22.04 and Ubuntu 24.04." >&2
  exit 1
}

ensure_cxx20_compiler() {
  local major candidate
  major="$(g++ -dumpversion 2>/dev/null | cut -d. -f1 || true)"
  if [[ -n "$major" ]] && version_ge "$major" "11"; then
    log "C++ compiler $(g++ --version | head -n1)"
    return 0
  fi

  if [[ "$OS_ID" == "debian" && -n "${VERSION_CODENAME:-}" ]]; then
    local backports="/etc/apt/sources.list.d/${VERSION_CODENAME}-backports.list"
    if [[ ! -e "$backports" ]]; then
      log "enabling ${VERSION_CODENAME}-backports to find GCC 11 or newer"
      printf 'deb http://deb.debian.org/debian %s-backports main\n' "$VERSION_CODENAME" \
        | run_root tee "$backports" >/dev/null
      run_root apt-get update
    fi
  fi

  if [[ "$OS_ID" == "ubuntu" ]]; then
    log "no distro package provides GCC 11; enabling ppa:ubuntu-toolchain-r/test"
    run_root apt-get install -y software-properties-common
    run_root add-apt-repository -y ppa:ubuntu-toolchain-r/test
  fi

  for candidate in 11 12 13 14 15; do
    if apt-cache show "g++-${candidate}" 2>/dev/null | grep -q "^Package: g++-${candidate}$"; then
      log "installing g++-${candidate}; the default compiler is older than GCC 11"
      if [[ "$OS_ID" == "debian" && -n "${VERSION_CODENAME:-}" && -e "/etc/apt/sources.list.d/${VERSION_CODENAME}-backports.list" ]]; then
        run_root apt-get install -y -t "${VERSION_CODENAME}-backports" "g++-${candidate}"
      else
        run_root apt-get install -y "g++-${candidate}"
      fi
      if [[ "$DRY_RUN" -eq 0 ]]; then
        run_root ln -sfn "/usr/bin/gcc-${candidate}" /usr/bin/gcc
        run_root ln -sfn "/usr/bin/g++-${candidate}" /usr/bin/g++
        run_root ln -sfn "/usr/bin/gcc-${candidate}" /usr/bin/cc
        run_root ln -sfn "/usr/bin/g++-${candidate}" /usr/bin/c++
      fi
      return 0
    fi
  done
  die "C++20 requires GCC 11 or newer, and this distribution has no such package"
}

nvcc_version() {
  if ! command -v nvcc >/dev/null 2>&1; then
    return 1
  fi
  nvcc --version | sed -nE 's/.*release ([0-9]+(\.[0-9]+)?).*/\1/p' | head -n1
}

have_cuda_131() {
  local ver
  ver="$(nvcc_version || true)"
  [[ -n "$ver" ]] && version_ge "$ver" "13.1"
}

cuda_repo_id() {
  case "${OS_ID}:${OS_VERSION}" in
    ubuntu:22.04) echo "ubuntu2204" ;;
    ubuntu:24.04) echo "ubuntu2404" ;;
    debian:12) echo "debian12" ;;
    *) return 1 ;;
  esac
}

install_cuda_toolkit() {
  local repo_id tmp_deb
  repo_id="$(cuda_repo_id)" || die "CUDA auto-install is unsupported on ${PRETTY_NAME:-$OS_ID $OS_VERSION}"
  tmp_deb="/tmp/cuda-keyring_1.1-1_all.deb"
  log "installing CUDA toolkit package ${CUDA_TOOLKIT_PACKAGE} from NVIDIA ${repo_id} repo"
  run curl -fsSL -o "$tmp_deb" "https://developer.download.nvidia.com/compute/cuda/repos/${repo_id}/x86_64/cuda-keyring_1.1-1_all.deb"
  run_root dpkg -i "$tmp_deb"
  run_root apt-get update
  run_root apt-get install -y "$CUDA_TOOLKIT_PACKAGE"
}

have_opencl_dev() {
  [[ -e /usr/include/CL/cl.h || -e /usr/include/CL/opencl.h ]] || return 1
  ldconfig -p 2>/dev/null | grep -q 'libOpenCL\.so'
}

install_rocm_hip_sdk() {
  if [[ "$OS_ID" != "ubuntu" ]]; then
    die "HIP is required but hipcc was not found. Debian auto-install is intentionally not attempted; install ROCm/HIP SDK first."
  fi
  if [[ "$OS_CODENAME" != "jammy" && "$OS_CODENAME" != "noble" ]]; then
    die "HIP is required but Ubuntu codename '$OS_CODENAME' is not supported by this bootstrap script."
  fi

  log "installing ROCm HIP SDK ${ROCM_VERSION} for Ubuntu ${OS_CODENAME}"
  run_root mkdir --parents --mode=0755 /etc/apt/keyrings
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "+ wget -qO - https://repo.radeon.com/rocm/rocm.gpg.key | gpg --dearmor | sudo tee /etc/apt/keyrings/rocm.gpg >/dev/null"
  else
    wget -qO - https://repo.radeon.com/rocm/rocm.gpg.key | gpg --dearmor | "${SUDO[@]}" tee /etc/apt/keyrings/rocm.gpg >/dev/null
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "+ sudo tee /etc/apt/sources.list.d/rocm.list"
  else
    cat <<EOF | "${SUDO[@]}" tee /etc/apt/sources.list.d/rocm.list >/dev/null
deb [arch=amd64 signed-by=/etc/apt/keyrings/rocm.gpg] https://repo.radeon.com/rocm/apt/${ROCM_VERSION} ${OS_CODENAME} main
deb [arch=amd64 signed-by=/etc/apt/keyrings/rocm.gpg] https://repo.radeon.com/graphics/${ROCM_VERSION}/ubuntu ${OS_CODENAME} main
EOF
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "+ sudo tee /etc/apt/preferences.d/rocm-pin-600"
  else
    cat <<'EOF' | "${SUDO[@]}" tee /etc/apt/preferences.d/rocm-pin-600 >/dev/null
Package: *
Pin: release o=repo.radeon.com
Pin-Priority: 600
EOF
  fi
  run_root apt-get update
  run_root apt-get install -y rocm-hip-sdk rocm-opencl-sdk
}

require_supported_distro
log "detected ${PRETTY_NAME:-${OS_ID} ${OS_VERSION}}"
run_root apt-get update
run_root apt-get install -y "${common_packages[@]}"
if [[ "$WITH_GUI" -eq 1 ]]; then
  log "installing Qt wallet and QR dependencies"
  run_root apt-get install -y "${gui_packages[@]}"
fi
if backend_required opencl; then
  log "installing OpenCL development headers and loader"
  run_root apt-get install -y "${opencl_packages[@]}"
fi
ensure_cxx20_compiler
run bash "$(dirname "${BASH_SOURCE[0]}")/bootstrap-rocksdb.sh"

if backend_required cuda; then
  if have_cuda_131; then
    log "CUDA toolkit $(nvcc_version) already satisfies >= 13.1"
  else
    install_cuda_toolkit
    if [[ "$DRY_RUN" -eq 0 ]] && ! have_cuda_131; then
      die "CUDA toolkit >= 13.1 is still not available after installation"
    fi
  fi
fi

if backend_required hip; then
  if command -v hipcc >/dev/null 2>&1; then
    log "HIP compiler already available: $(command -v hipcc)"
  else
    install_rocm_hip_sdk
    if [[ "$DRY_RUN" -eq 0 ]] && ! command -v hipcc >/dev/null 2>&1; then
      die "hipcc is still not available after ROCm HIP SDK installation"
    fi
  fi
fi

if backend_required opencl; then
  if [[ "$DRY_RUN" -eq 0 ]] && ! have_opencl_dev; then
    die "OpenCL headers/loader were installed but libOpenCL.so was not found by ldconfig"
  fi
  log "OpenCL development headers/loader are present or scheduled for installation"
fi

log "done"
