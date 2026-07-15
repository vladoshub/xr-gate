#!/usr/bin/env bash
set -euo pipefail

: "${XR_ROOT_PROJECT:?XR_ROOT_PROJECT is required}"
: "${XR_OUT_ROOT:?XR_OUT_ROOT is required}"
: "${XR_OUT_BIN_ROOT:?XR_OUT_BIN_ROOT is required}"

copy_runtime_file() {
  local src="$1" dst="$2"
  [[ -f "$src" ]] || return 0
  mkdir -p "$(dirname "$dst")"
  cp -a "$src" "$dst"
  chmod +x "$dst" 2>/dev/null || true
}

write_root_launcher() {
  local name="$1" target="$2"
  cat > "$XR_OUT_ROOT/$name" <<EOF_LAUNCHER
#!/usr/bin/env bash
set -euo pipefail
PACKAGE_ROOT="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
exec "\$PACKAGE_ROOT/$target" "\$@"
EOF_LAUNCHER
  chmod +x "$XR_OUT_ROOT/$name"
}

copy_runtime_file \
  "$XR_ROOT_PROJECT/drivers/steam_vr/scripts/linux/restore_xreal_desktop.sh" \
  "$XR_OUT_BIN_ROOT/scripts/drivers/steam_vr/restore_xreal_desktop.sh"

if [[ -x "$XR_OUT_BIN_ROOT/scripts/drivers/steam_vr/restore_xreal_desktop.sh" ]]; then
  write_root_launcher \
    "run_openvr_restore_desktop.sh" \
    "bin/scripts/drivers/steam_vr/restore_xreal_desktop.sh"
fi
