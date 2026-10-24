#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GEO_BUILD_DIR="${GEO_BUILD_DIR:-$ROOT_DIR/out/build/linux-release-gui}"
LIBGEO_BUILD_DIR="${LIBGEO_BUILD_DIR:-$ROOT_DIR/libgeo/out/build/linux-release}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/portable/GeoCoin-linux-x86_64}"
ARCHIVE_PATH="${ARCHIVE_PATH:-$ROOT_DIR/out/portable/GeoCoin-linux-x86_64-portable.tar.gz}"

log() {
  printf '[portable-linux] %s\n' "$*"
}

for required_binary in geocoin-core geocoin-node geocoin-rpc-miner; do
  if [[ ! -x "$GEO_BUILD_DIR/$required_binary" ]]; then
    echo "$required_binary was not found at $GEO_BUILD_DIR" >&2
    echo "Build that exact tree before packaging or set GEO_BUILD_DIR explicitly." >&2
    exit 1
  fi
done
if [[ ! -f "$LIBGEO_BUILD_DIR/libgeo.so" ]]; then
  echo "libgeo.so was not found at $LIBGEO_BUILD_DIR" >&2
  echo "Build that exact tree before packaging or set LIBGEO_BUILD_DIR explicitly." >&2
  exit 1
fi

if [[ -x "$GEO_BUILD_DIR/geocoin-gpu-miner" ]]; then
  log "optional GPU miner found and will be included"
else
  log "optional GPU miner not found; creating a CPU/RPC-miner bundle"
fi

if ! command -v patchelf >/dev/null 2>&1; then
  echo "patchelf is required to make the Linux portable bundle self-contained." >&2
  echo "Install it with scripts/bootstrap-ubuntu-22.04.sh or apt-get install patchelf." >&2
  exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/lib" "$OUT_DIR/plugins" \
         "$OUT_DIR/include/libgeo" "$OUT_DIR/config" "$OUT_DIR/docs" "$OUT_DIR/deploy"

for binary in geocoin-core geocoin-node genesis geocoin-rpc-miner geocoin-gpu-miner geocoin-db; do
  if [[ -f "$GEO_BUILD_DIR/$binary" ]]; then
    cp -f "$GEO_BUILD_DIR/$binary" "$OUT_DIR/bin/"
  fi
done
if [[ -x "$OUT_DIR/bin/geocoin-node" ]]; then
  ln -s geocoin-node "$OUT_DIR/bin/degree_node"
fi
if [[ -x "$OUT_DIR/bin/genesis" ]]; then
  ln -s genesis "$OUT_DIR/bin/mine_genesis"
  ln -s genesis "$OUT_DIR/bin/geocoin-miner"
fi

cp -f "$LIBGEO_BUILD_DIR/libgeo.so" "$OUT_DIR/lib/"
if [[ -f "$LIBGEO_BUILD_DIR/libgeo.a" ]]; then
  cp -f "$LIBGEO_BUILD_DIR/libgeo.a" "$OUT_DIR/lib/"
fi
cp -f "$ROOT_DIR/libgeo/include/libgeo/libgeo.h" "$OUT_DIR/include/libgeo/"
cp -f "$ROOT_DIR"/config/*.json "$OUT_DIR/config/"
cp -a "$ROOT_DIR/deploy/." "$OUT_DIR/deploy/"
cp -a "$ROOT_DIR/docs/." "$OUT_DIR/docs/"
if [[ -f "$ROOT_DIR/WHATS_NEW.txt" ]]; then
  cp -f "$ROOT_DIR/WHATS_NEW.txt" "$OUT_DIR/"
fi
cp -f "$ROOT_DIR/README.md" "$OUT_DIR/"
mkdir -p "$OUT_DIR/licenses"
cp -f "$ROOT_DIR/geographiclib/LICENSE.txt" "$OUT_DIR/licenses/GeographicLib.txt"
cp -f "$ROOT_DIR/third_party/boost-geometry.LICENSE.txt" "$OUT_DIR/licenses/Boost.txt"
if [[ -f "$ROOT_DIR/out/release-manifest.json" ]]; then
  cp -f "$ROOT_DIR/out/release-manifest.json" "$OUT_DIR/"
fi
if [[ -f "$ROOT_DIR/out/release-sbom.json" ]]; then
  cp -f "$ROOT_DIR/out/release-sbom.json" "$OUT_DIR/"
fi

QT_PLUGIN_DIR="$(qtpaths --plugin-dir 2>/dev/null || qmake -query QT_INSTALL_PLUGINS)"
for plugin_dir in platforms imageformats iconengines styles; do
  if [[ -d "$QT_PLUGIN_DIR/$plugin_dir" ]]; then
    mkdir -p "$OUT_DIR/plugins/$plugin_dir"
    cp -a "$QT_PLUGIN_DIR/$plugin_dir/." "$OUT_DIR/plugins/$plugin_dir/"
  fi
done

cat > "$OUT_DIR/bin/qt.conf" <<'EOF'
[Paths]
Prefix=.
Plugins=../plugins
EOF

cat > "$OUT_DIR/geocoin-core" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$SELF_DIR/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$SELF_DIR/plugins/platforms"
exec "$SELF_DIR/bin/geocoin-core" "$@"
EOF
chmod +x "$OUT_DIR/geocoin-core"

cat > "$OUT_DIR/degree_node" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -x "$SELF_DIR/bin/degree_node" ]]; then
  echo "degree_node is not included in this bundle. Use ./geocoin-node or rebuild with BUILD_NODE_CLI=ON." >&2
  exit 127
fi
exec "$SELF_DIR/bin/degree_node" "$@"
EOF
chmod +x "$OUT_DIR/degree_node"

cat > "$OUT_DIR/geocoin-node" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -x "$SELF_DIR/bin/geocoin-node" ]]; then
  echo "geocoin-node is not included in this bundle. Rebuild with BUILD_NODE_CLI=ON." >&2
  exit 127
fi
exec "$SELF_DIR/bin/geocoin-node" "$@"
EOF
chmod +x "$OUT_DIR/geocoin-node"

if [[ -x "$OUT_DIR/bin/geocoin-gpu-miner" ]]; then
  cat > "$OUT_DIR/geocoin-gpu-miner" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$SELF_DIR/bin/geocoin-gpu-miner" "$@"
EOF
  chmod +x "$OUT_DIR/geocoin-gpu-miner"
fi

cat > "$OUT_DIR/geocoin-rpc-miner" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -x "$SELF_DIR/bin/geocoin-rpc-miner" ]]; then
  echo "geocoin-rpc-miner is not included in this bundle. Rebuild with BUILD_TOOLS=ON." >&2
  exit 127
fi
exec "$SELF_DIR/bin/geocoin-rpc-miner" "$@"
EOF
chmod +x "$OUT_DIR/geocoin-rpc-miner"

cat > "$OUT_DIR/geocoin-db" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -x "$SELF_DIR/bin/geocoin-db" ]]; then
  echo "geocoin-db is not included in this bundle. Rebuild with BUILD_TOOLS=ON." >&2
  exit 127
fi
exec "$SELF_DIR/bin/geocoin-db" "$@"
EOF
chmod +x "$OUT_DIR/geocoin-db"

cat > "$OUT_DIR/geocoin-miner" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -x "$SELF_DIR/bin/geocoin-miner" ]]; then
  echo "geocoin-miner is not included in this bundle. Rebuild with BUILD_GENESIS_CLI=ON." >&2
  exit 127
fi
exec "$SELF_DIR/bin/geocoin-miner" "$@"
EOF
chmod +x "$OUT_DIR/geocoin-miner"

cat > "$OUT_DIR/README.txt" <<'EOF'
GeoCoin Linux portable bundle

This package ships the GeoCoin binaries together with their non-system runtime libraries.
The bundle is intended to run on common x86_64 Linux systems without installing Qt,
OpenSSL, Curl, RocksDB, and libgeo runtime libraries separately.

The GPU miner is optional and is present only when the selected build tree
produced bin/geocoin-gpu-miner. GPU drivers are always provided by the
OS/vendor driver stack:
- CUDA requires an installed NVIDIA driver.
- OpenCL requires a vendor OpenCL ICD from the installed GPU driver.
- HIP requires an AMD driver/runtime stack compatible with the GPU.

Entry points:
- ./geocoin-core: desktop wallet GUI
- ./geocoin-node: canonical full node, sync, peers, wallet RPC
- ./degree_node: compatibility alias for the same full node role
- ./geocoin-gpu-miner: optional CUDA/HIP/OpenCL solo miner; present only in GPU-enabled builds
- ./geocoin-rpc-miner: RPC getblocktemplate/submitblock mining helper; use miner RPC credentials
- ./geocoin-miner: standalone diagnostic miner
- ./geocoin-db: database schema, check, backup, and restore

Do not launch these for normal wallet/node work:
- ./bin/genesis
- ./bin/mine_genesis

SDK:
- ./lib/libgeo.so is the C SDK library for external apps and is not started directly.

These wrapper scripts set library/plugin paths before launching the real binaries.
Runtime dependency evidence is saved in ./runtime-dependencies.txt.
EOF

declare -A COPIED_DEPS=()

should_skip_dep() {
  local base
  base="$(basename "$1")"
  case "$base" in
    linux-vdso.so.*|ld-linux*.so*|libc.so.*|libpthread.so.*|libm.so.*|libdl.so.*|librt.so.*|libutil.so.*|libresolv.so.*|libnsl.so.*|libcrypt.so.*)
      return 0
      ;;
    # GPU driver libraries must come from the target host driver stack, not from
    # the build machine. Bundling them can hide the installed NVIDIA driver.
    libcuda.so*|libnvidia-*.so*)
      return 0
      ;;
  esac
  return 1
}

is_elf_file() {
  local target="$1"
  [[ -f "$target" ]] && file -b "$target" 2>/dev/null | grep -q 'ELF'
}

copy_deps_recursive() {
  local target="$1"
  local line=""
  if ! is_elf_file "$target"; then
    return 0
  fi
  while IFS= read -r line; do
    local resolved=""
    if [[ "$line" == *"=>"* ]]; then
      resolved="$(sed -E 's/.*=> ([^ ]+) .*/\1/' <<<"$line")"
    else
      resolved="$(awk '{print $1}' <<<"$line")"
    fi

    if [[ -z "$resolved" || "$resolved" == "statically" || "$resolved" == "not" ]]; then
      continue
    fi
    if [[ ! -f "$resolved" ]]; then
      continue
    fi
    if should_skip_dep "$resolved"; then
      continue
    fi
    if [[ -n "${COPIED_DEPS[$resolved]:-}" ]]; then
      continue
    fi

    COPIED_DEPS["$resolved"]=1
    local dest="$OUT_DIR/lib/$(basename "$resolved")"
    if [[ "$(realpath -m "$resolved")" == "$(realpath -m "$dest")" ]]; then
      continue
    fi
    cp -L "$resolved" "$dest"
    copy_deps_recursive "$dest"
  done < <(ldd "$target" 2>/dev/null || true)
}

while IFS= read -r target; do
  copy_deps_recursive "$target"
done < <(find "$OUT_DIR/bin" "$OUT_DIR/lib" -maxdepth 1 -type f)

while IFS= read -r plugin_file; do
  copy_deps_recursive "$plugin_file"
done < <(find "$OUT_DIR/plugins" -type f \( -name '*.so' -o -name '*.so.*' \))

set_elf_rpath() {
  local target="$1"
  local rpath="$2"
  if is_elf_file "$target"; then
    patchelf --set-rpath "$rpath" "$target"
  fi
}

while IFS= read -r target; do
  set_elf_rpath "$target" '$ORIGIN/../lib'
done < <(find "$OUT_DIR/bin" -maxdepth 1 -type f)

while IFS= read -r target; do
  set_elf_rpath "$target" '$ORIGIN'
done < <(find "$OUT_DIR/lib" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \))

while IFS= read -r plugin_file; do
  plugin_dir="$(dirname "$plugin_file")"
  rel_to_lib="$(realpath --relative-to="$plugin_dir" "$OUT_DIR/lib")"
  set_elf_rpath "$plugin_file" "\$ORIGIN/$rel_to_lib"
done < <(find "$OUT_DIR/plugins" -type f \( -name '*.so' -o -name '*.so.*' \))

{
  echo "GeoCoin Linux portable runtime dependency report"
  echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "Bundled libraries:"
  find "$OUT_DIR/lib" -maxdepth 1 -type f -printf '  %f\n' | sort
  echo
  echo "ldd output:"
} > "$OUT_DIR/runtime-dependencies.txt"

missing_deps=0
while IFS= read -r target; do
  if ! is_elf_file "$target"; then
    continue
  fi
  {
    echo
    echo "== ${target#$OUT_DIR/} =="
    ldd "$target" || true
  } >> "$OUT_DIR/runtime-dependencies.txt"
  if ldd "$target" 2>/dev/null | grep -q 'not found'; then
    echo "$(basename "$target") still has unresolved runtime dependencies:" >&2
    ldd "$target" >&2
    missing_deps=1
  fi
done < <(find "$OUT_DIR/bin" "$OUT_DIR/lib" "$OUT_DIR/plugins" -type f)

if [[ "$missing_deps" -ne 0 ]]; then
  exit 1
fi

mkdir -p "$(dirname "$ARCHIVE_PATH")"
rm -f "$ARCHIVE_PATH"
tar -C "$(dirname "$OUT_DIR")" -czf "$ARCHIVE_PATH" "$(basename "$OUT_DIR")"

log "bundle directory: $OUT_DIR"
log "archive: $ARCHIVE_PATH"
