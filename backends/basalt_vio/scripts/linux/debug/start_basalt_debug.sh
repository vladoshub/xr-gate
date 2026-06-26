#!/usr/bin/env bash
set -euo pipefail

XR="${XR:-$HOME/src/xr_tracking}"
BASALT_DIR="${BASALT_DIR:-$XR/bin/backends/basalt_vio}"
FINAL="${FINAL:-$XR/calibration_dataset/final/xreal_air2ultra/ZBBM5DZFMP/unified_480_ccw90}"
STARTUP_GATE="${STARTUP_GATE:-1}"
STARTUP_GATE_SCRIPT="${STARTUP_GATE_SCRIPT:-$XR/tools/xr_startup_gate.py}"

rm -rf /tmp/xr_basalt_unified_live
mkdir -p /tmp/xr_basalt_unified_live

if [[ "$STARTUP_GATE" == "1" ]]; then
  PYTHONPATH="$XR/capture_service:${PYTHONPATH:-}" \
    python3 "$STARTUP_GATE_SCRIPT" \
      --transport shm \
      --registry /tmp/capture_service_streams.json \
      --cam0-stream camera0 \
      --cam1-stream camera1 \
      --imu-stream imu0 \
      --visual-gate \
      --visual-good-frames 30 \
      --min-mean 22 \
      --min-stddev 10 \
      --max-black-fraction 0.60 \
      --max-white-fraction 0.15 \
      --min-corners 260 \
      --min-grid-cells 14 \
      --min-laplacian-stddev 16 \
      --print-every 5
fi

"$BASALT_DIR/capture_basalt_backend" \
  --transport shm \
  --registry /tmp/capture_service_streams.json \
  --cam0-stream camera0 \
  --cam1-stream camera1 \
  --imu-stream imu0 \
  --cam-calib "$FINAL/basalt_calib_unified_480_ccw90.json" \
  --config-path "$FINAL/basalt_vio_config_unified_480_ccw90.json" \
  --out-dir /tmp/xr_basalt_unified_live \
  --duration 0 \
  --image-scale 256 \
  --no-enforce-realtime \
  --save-trajectory \
  --trajectory /tmp/xr_basalt_unified_live/trajectory.csv \
  --print-every 30
