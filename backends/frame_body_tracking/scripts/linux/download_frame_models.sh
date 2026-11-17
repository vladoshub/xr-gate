#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}"
# shellcheck source=/dev/null
source "$ROOT_PROJECT/devices/common/linux/scripts/build/dependency_policy.sh"

upstream_url="https://github.com/abcamiletto/frame.git"
upstream_ref="e769c2f9068806101f6897e0b62b03e8c5280abd"
checkpoint_url="https://edmond.mpg.de/api/access/datafile/311764"
checkpoint_sha256="296d1e4216f8f9e13e39e4761ad8080622eb08bb6d10fd742d75818c53879406"
exporter="$SCRIPT_DIR/export_frame_backbone.py"
requirements="$SCRIPT_DIR/requirements-frame-export.txt"
cache="${XR_DEPENDENCY_CACHE_ROOT:-$ROOT_PROJECT/build/.dependency-cache}/frame"
model_name="frame_backbone.onnx"
model_dir="${XR_BIN_ROOT:-$ROOT_PROJECT/bin}/body-tracking-models/frame"

export_key="$(cat "$exporter" "$requirements" | sha256sum)"
cached_model="$cache/$upstream_ref-$checkpoint_sha256-${export_key%% *}"
mkdir -p "$cache"
exec 9>"$cache/lock"
flock 9

if [[ "$XR_UPDATE_DEPS" == "1" || ! -s "$cached_model/$model_name" ]]; then
  xr_require_network "FRAME ONNX export is missing: $cached_model/$model_name"
  export_dir="$(mktemp -d "$cache/export.XXXXXXXX")"
  trap 'rm -rf -- "$export_dir"' EXIT

  checkpoint="${XR_FRAME_CHECKPOINT:-$cache/backbone-$checkpoint_sha256.ckpt}"
  checkpoint="${checkpoint/#\~/${HOME:-~}}"
  if [[ -z "${XR_FRAME_CHECKPOINT:-}" ]]; then
    if [[ -f "$checkpoint" ]] && ! printf '%s  %s\n' "$checkpoint_sha256" "$checkpoint" | sha256sum --check --status; then
      rm -f -- "$checkpoint"
    fi
    xr_download "$checkpoint_url" "$checkpoint"
  fi
  if ! printf '%s  %s\n' "$checkpoint_sha256" "$checkpoint" | sha256sum --check --status; then
    echo "[frame_models][ERROR] checkpoint checksum mismatch: $checkpoint" >&2
    exit 1
  fi

  upstream="$cache/source"
  ref="$(xr_ensure_git_source "$upstream" "$upstream_url" "$upstream_ref" 1 "$XR_UPDATE_DEPS" 1)"
  git -C "$upstream" archive "$ref" LICENSE src/framevision | tar -x -C "$export_dir"

  "${XR_FRAME_EXPORT_PYTHON:-python3}" -m venv "$export_dir/venv"
  "$export_dir/venv/bin/python" -m pip install --disable-pip-version-check -r "$requirements"
  echo "[frame_models] exporting the FRAME backbone with ONNX opset 17"
  mkdir -p "$export_dir/model" "$cached_model"
  "$export_dir/venv/bin/python" "$exporter" --source "$export_dir" \
    --checkpoint "$checkpoint" --output "$export_dir/model/$model_name"

  cp "$export_dir/LICENSE" "$export_dir/model/FRAME-LICENSE.txt"
  cat > "$export_dir/model/SOURCE.txt" <<EOF_SOURCE
FRAME: Floor-aligned Representation for Avatar Motion from Egocentric Video.
Andrea Boscolo Camiletto, Jian Wang, Eduardo Alvarado, Rishabh Dabral,
Thabo Beeler, Marc Habermann and Christian Theobalt, CVPR 2025.
Project: https://vcai.mpi-inf.mpg.de/projects/FRAME/
Source: $upstream_url at $upstream_ref
Dataset and checkpoint terms: https://doi.org/10.17617/3.XARMQA
Original backbone.ckpt: $checkpoint_url
SHA-256 of backbone.ckpt: $checkpoint_sha256
License: CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)

The pretrained backbone was converted to ONNX (opset 17) for XR Gate.
The model retains the upstream terms. The FRAME source license is included
in FRAME-LICENSE.txt. Keep this notice with copies of the exported model.
EOF_SOURCE
  (cd "$export_dir/model" && sha256sum "$model_name" > SHA256SUMS.txt)
  mv -f "$export_dir/model/"{SOURCE.txt,FRAME-LICENSE.txt,SHA256SUMS.txt,"$model_name"} "$cached_model/"
fi

mkdir -p "$model_dir"
install_dir="$(mktemp -d "$model_dir/.install.XXXXXXXX")"
trap 'rm -rf -- "${export_dir:-}" "$install_dir"' EXIT
install -m 644 "$cached_model/"{SOURCE.txt,FRAME-LICENSE.txt,SHA256SUMS.txt,"$model_name"} "$install_dir/"
mv -f "$install_dir/"{SOURCE.txt,FRAME-LICENSE.txt,SHA256SUMS.txt,"$model_name"} "$model_dir/"
echo "[frame_models] installed $model_dir/$model_name"
