#!/usr/bin/env bash
# Shared by source installers as well as the session wrapper. Existing sources
# stay at the requested ref; refreshing remote refs is an explicit operation.
export XR_UPDATE_DEPS="${XR_UPDATE_DEPS:-0}"
export XR_OFFLINE="${XR_OFFLINE:-0}"
case "$XR_UPDATE_DEPS:$XR_OFFLINE" in
  0:0|0:1|1:0) ;;
  *) echo "[dependencies][ERROR] expected XR_UPDATE_DEPS=0|1 and XR_OFFLINE=0|1 (update requires online mode)" >&2; return 2 ;;
esac
# Git checkouts must not implicitly download LFS objects during checkout/reset.
# Optional model downloads hydrate the local LFS cache explicitly instead.
export GIT_LFS_SKIP_SMUDGE=1 GIT_TERMINAL_PROMPT=0 VCPKG_DISABLE_METRICS=1
if [[ "$XR_OFFLINE" == 1 ]]; then
  export GIT_ALLOW_PROTOCOL="" CARGO_NET_OFFLINE=true RUSTUP_AUTO_INSTALL=0
  export X_VCPKG_ASSET_SOURCES='clear;x-block-origin'
fi

xr_require_network() {
  [[ "$XR_OFFLINE" != 1 ]] && return 0
  echo "[dependencies][ERROR] offline: $*. Run the same build once with XR_OFFLINE=0 to fill missing dependencies, or XR_UPDATE_DEPS=1 to refresh them." >&2
  return 2
}

xr_resolve_git_ref() {
  local dir="$1" ref="$2"
  if git -C "$dir" rev-parse --verify --quiet "$ref^{commit}" >/dev/null; then
    printf '%s\n' "$ref"
  elif git -C "$dir" rev-parse --verify --quiet "origin/$ref^{commit}" >/dev/null; then
    printf 'origin/%s\n' "$ref"
  else
    return 1
  fi
}

xr_ensure_git_source() {
  local dir="$1" url="$2" ref="$3" clone="${4:-1}" refresh="${5:-$XR_UPDATE_DEPS}" shallow="${6:-0}"
  local resolved
  if [[ ! -e "$dir/.git" ]]; then
    [[ "$clone" == 1 ]] || { echo "[dependencies][ERROR] source missing: $dir (cloning disabled)" >&2; return 2; }
    if [[ -d "$dir" && -n "$(find "$dir" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
      echo "[dependencies][ERROR] refusing a nonempty source directory without Git metadata: $dir" >&2
      return 2
    fi
    xr_require_network "source checkout missing: $dir" || return
    mkdir -p "$(dirname "$dir")"
    if [[ "$shallow" == 1 && "$ref" =~ ^[0-9a-fA-F]{40}$ ]]; then
      git init -q "$dir" || return
      git -C "$dir" remote add origin "$url" || return
      if ! git -C "$dir" fetch --depth=1 origin "$ref" >&2; then
        git -C "$dir" fetch --tags origin >&2 || return
      fi
    elif [[ "$shallow" == 1 ]]; then
      git clone --depth=1 --branch "$ref" "$url" "$dir" >&2 || return
    else
      git clone "$url" "$dir" >&2 || return
    fi
  elif [[ "$refresh" == 1 ]]; then
    xr_require_network "requested Git refresh: $dir" || return
    git -C "$dir" fetch --tags origin >&2 || return
  fi
  if ! resolved="$(xr_resolve_git_ref "$dir" "$ref")"; then
    xr_require_network "Git ref $ref missing in $dir" || return
    git -C "$dir" fetch origin "$ref" >&2 || return
    resolved="$(xr_resolve_git_ref "$dir" "$ref" || printf FETCH_HEAD)"
  fi
  printf '%s\n' "$resolved"
}

xr_update_submodules() {
  local dir="$1" args=()
  [[ "$XR_OFFLINE" != 1 ]] || args+=(--no-fetch)
  git -C "$dir" submodule update --init --recursive "${args[@]}" || {
    echo "[dependencies][ERROR] submodules incomplete in $dir; rerun with XR_OFFLINE=0 to prepare missing sources" >&2
    return 2
  }
}

xr_download() {
  local url="$1" destination="$2" temporary="$2.part.$$"
  [[ ! -s "$destination" || "$XR_UPDATE_DEPS" == 1 ]] || return 0
  xr_require_network "download missing or refresh requested: $destination" || return
  mkdir -p "$(dirname "$destination")"
  if curl --fail --location --retry 3 --output "$temporary" "$url"; then
    mv -f "$temporary" "$destination"
  else
    rm -f -- "$temporary"
    return 2
  fi
}
