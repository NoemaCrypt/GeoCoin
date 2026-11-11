#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GEO_BUILD_DIR="${GEO_BUILD_DIR:-$ROOT_DIR/out/build/linux-release-gui}"
LIBGEO_BUILD_DIR="${LIBGEO_BUILD_DIR:-$ROOT_DIR/libgeo/out/build/linux-release}"
BUNDLE_PROFILE="${BUNDLE_PROFILE:-user}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
case "$BUNDLE_PROFILE" in
  user) SUFFIX="" ;;
  admin|sdk) SUFFIX="-$BUNDLE_PROFILE" ;;
  *) echo "BUNDLE_PROFILE must be user, admin or sdk" >&2; exit 2 ;;
esac
OUT_DIR="$(realpath -m "${OUT_DIR:-$ROOT_DIR/out/portable/GeoCoin-linux-x86_64$SUFFIX}")"
ARCHIVE_PATH="${ARCHIVE_PATH:-$ROOT_DIR/out/portable/GeoCoin-linux-x86_64$SUFFIX-portable.tar.gz}"
REPORT_PATH="${REPORT_PATH:-$ROOT_DIR/out/packaging/linux-$BUNDLE_PROFILE-runtime-dependencies.txt}"

log() { printf '[portable-linux] %s\n' "$*"; }

if ! command -v patchelf >/dev/null 2>&1; then
  echo "patchelf is required for portable runtime libraries." >&2
  exit 1
fi
case "$ROOT_DIR/" in
  "$OUT_DIR/"*) echo "Output directory must not contain the source tree" >&2; exit 2 ;;
esac
if [[ "$OUT_DIR" == "$(realpath -m "$GEO_BUILD_DIR")" || "$OUT_DIR" == "$(realpath -m "$LIBGEO_BUILD_DIR")" ]]; then
  echo "Output directory must differ from the build directories" >&2
  exit 2
fi
if [[ "$BUNDLE_PROFILE" == sdk && ! -f "$LIBGEO_BUILD_DIR/libgeo.so" ]]; then
  echo "libgeo.so was not found at $LIBGEO_BUILD_DIR" >&2
  exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/lib"
"$PYTHON_BIN" "$ROOT_DIR/scripts/release_bundle.py" stage --root "$ROOT_DIR" \
  --profile "$BUNDLE_PROFILE" --platform linux --build-dir "$GEO_BUILD_DIR" --output "$OUT_DIR"

if [[ "$BUNDLE_PROFILE" == sdk ]]; then
  cp -f "$LIBGEO_BUILD_DIR/libgeo.so" "$OUT_DIR/lib/"
fi

if [[ "$BUNDLE_PROFILE" == user ]]; then
  QT_PLUGIN_DIR="${GEOCOIN_QT_PLUGIN_DIR:-}"
  if [[ -z "$QT_PLUGIN_DIR" ]]; then
    QT_PLUGIN_DIR="$(qtpaths --plugin-dir 2>/dev/null || qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
  fi
  if [[ ! -d "$QT_PLUGIN_DIR/platforms" ]]; then
    echo "Qt platform plugins not found; set GEOCOIN_QT_PLUGIN_DIR for the selected build." >&2
    exit 1
  fi
  for plugin_dir in platforms imageformats iconengines styles; do
    if [[ -d "$QT_PLUGIN_DIR/$plugin_dir" ]]; then
      mkdir -p "$OUT_DIR/plugins/$plugin_dir"
      while IFS= read -r -d '' plugin_file; do
        cp -L "$plugin_file" "$OUT_DIR/plugins/$plugin_dir/"
      done < <(find "$QT_PLUGIN_DIR/$plugin_dir" -maxdepth 1 -name '*.so' -print0)
    fi
  done
  cat > "$OUT_DIR/bin/qt.conf" <<'EOF'
[Paths]
Prefix=.
Plugins=../plugins
EOF
fi

for binary in geocoin-core geocoin-node geocoin-rpc-miner geocoin-gpu-miner geocoin-db; do
  if [[ ! -x "$OUT_DIR/bin/$binary" ]]; then continue; fi
  cat > "$OUT_DIR/$binary" <<EOF
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="\$SELF_DIR/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
EOF
  if [[ "$binary" == geocoin-core ]]; then
    cat >> "$OUT_DIR/$binary" <<'EOF'
export QT_PLUGIN_PATH="$SELF_DIR/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$SELF_DIR/plugins/platforms"
EOF
  fi
  printf 'exec "$SELF_DIR/bin/%s" "$@"\n' "$binary" >> "$OUT_DIR/$binary"
  chmod +x "$OUT_DIR/$binary"
done
mkdir -p "$OUT_DIR/plugins"

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

notice_args=()
for dependency in "${!COPIED_DEPS[@]}"; do
  notice_args+=(--library "$dependency")
done
"$PYTHON_BIN" "$ROOT_DIR/scripts/release_bundle.py" notices --root "$ROOT_DIR" \
  --profile "$BUNDLE_PROFILE" --platform linux --output "$OUT_DIR" "${notice_args[@]}"

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

mkdir -p "$(dirname "$REPORT_PATH")"
{
  echo "GeoCoin Linux $BUNDLE_PROFILE runtime dependency report"
  echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$REPORT_PATH"

missing_deps=0
while IFS= read -r target; do
  if ! is_elf_file "$target"; then continue; fi
  {
    echo
    echo "== ${target#$OUT_DIR/} =="
    ldd "$target" || true
  } >> "$REPORT_PATH"
  if ldd "$target" 2>/dev/null | grep -q 'not found'; then
    echo "Unresolved runtime dependencies: ${target#$OUT_DIR/}" >&2
    missing_deps=1
  fi
done < <(find "$OUT_DIR/bin" "$OUT_DIR/lib" "$OUT_DIR/plugins" -type f)
if [[ "$missing_deps" -ne 0 ]]; then exit 1; fi

"$PYTHON_BIN" "$ROOT_DIR/scripts/release_bundle.py" check --root "$ROOT_DIR" \
  --profile "$BUNDLE_PROFILE" --platform linux --output "$OUT_DIR"
mkdir -p "$(dirname "$ARCHIVE_PATH")"
rm -f "$ARCHIVE_PATH"
tar -C "$(dirname "$OUT_DIR")" -czf "$ARCHIVE_PATH" "$(basename "$OUT_DIR")"
"$PYTHON_BIN" "$ROOT_DIR/scripts/release_bundle.py" check --root "$ROOT_DIR" \
  --profile "$BUNDLE_PROFILE" --platform linux --output "$ARCHIVE_PATH"
log "bundle directory: $OUT_DIR"
log "archive: $ARCHIVE_PATH"
log "build dependency report: $REPORT_PATH"
