#!/usr/bin/env bash
set -euo pipefail
source_dir="$1"
destination="$2"
[[ "$destination" == /* && "$destination" != / && "$destination" != /mnt/* ]] || {
  echo 'Choose an absolute directory in the Linux filesystem.' >&2
  exit 1
}
mkdir -p -- "$destination"
if [[ "$source_dir" != "$destination" ]]; then
  cp -a -- "$source_dir/." "$destination/"
fi
chmod +x -- "$destination/xr-gate"
find "$destination/scripts" "$destination/image" -name '*.sh' -exec chmod +x {} +
