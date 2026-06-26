#!/usr/bin/env bash
set -euo pipefail

expand_tilde() {
  local value="$1"
  case "$value" in
    "~") printf '%s\n' "$HOME" ;;
    "~/"*) printf '%s\n' "$HOME/${value#"~/"}" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CALIB_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_PROJECT="${ROOT_PROJECT:-$HOME/src/xr_tracking}"
ROOT_PROJECT="$(expand_tilde "$ROOT_PROJECT")"
PACKAGE_ROOT="${PACKAGE_ROOT:-$ROOT_PROJECT/out/xreal_ultra}"
PACKAGE_ROOT="$(expand_tilde "$PACKAGE_ROOT")"

RECORDER="${RECORDER:-$CALIB_DIR/record_unified_kalibr_dataset.py}"
RECORDER="$(expand_tilde "$RECORDER")"

# Prefer the package-local thin venv created by install_xreal_ultra_out.sh.
# Fall back to the old capture_service venv only for legacy source-tree use.
if [[ -x "$PACKAGE_ROOT/bin/python-runtime/venv/bin/python" ]]; then
  PYTHON_BIN="${PYTHON_BIN:-$PACKAGE_ROOT/bin/python-runtime/venv/bin/python}"
elif [[ -x "${VENV_DIR:-$HOME/.venvs/xreal_capture_service}/bin/python" ]]; then
  PYTHON_BIN="${PYTHON_BIN:-${VENV_DIR:-$HOME/.venvs/xreal_capture_service}/bin/python}"
else
  PYTHON_BIN="${PYTHON_BIN:-python3}"
fi
PYTHON_BIN="$(expand_tilde "$PYTHON_BIN")"

export PYTHONPATH="$PACKAGE_ROOT/bin/python:$PACKAGE_ROOT/bin/python/capture_service:$ROOT_PROJECT:$ROOT_PROJECT/capture_service:${PYTHONPATH:-}"
export PYTHONUNBUFFERED="${PYTHONUNBUFFERED:-1}"

SECONDS_TOTAL="${SECONDS_TOTAL:-90}"
TRANSPORT="${TRANSPORT:-shm}"
REGISTRY="${REGISTRY:-/tmp/capture_service_streams.json}"
TCP_HOST="${TCP_HOST:-127.0.0.1}"
TCP_PORT="${TCP_PORT:-45660}"
CAM0_STREAM="${CAM0_STREAM:-camera0}"
CAM1_STREAM="${CAM1_STREAM:-camera1}"
IMU_STREAM="${IMU_STREAM:-imu0}"

# capture_service_cpp is the default runtime backend. Set START_CAPTURE_SERVICE=1
# to let the recorder launch and optionally stop it around the recording.
START_CAPTURE_SERVICE="${START_CAPTURE_SERVICE:-0}"
STOP_CAPTURE_SERVICE="${STOP_CAPTURE_SERVICE:-$START_CAPTURE_SERVICE}"
CAPTURE_SERVICE_IMPL="${CAPTURE_SERVICE_IMPL:-cpp}"
PUBLISH="${PUBLISH:-shm}"
EXPECT_WIDTH="${EXPECT_WIDTH:-480}"
EXPECT_HEIGHT="${EXPECT_HEIGHT:-640}"
WARMUP_SECONDS="${WARMUP_SECONDS:-2.0}"

if [[ ! -f "$RECORDER" ]]; then
  echo "[ERROR] recorder script not found: $RECORDER" >&2
  exit 1
fi

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1 && [[ ! -x "$PYTHON_BIN" ]]; then
  echo "[ERROR] Python not found: $PYTHON_BIN" >&2
  exit 1
fi

echo "[guided] XREAL unified 480x640 Kalibr dataset recording"
echo "[guided] ROOT_PROJECT=$ROOT_PROJECT"
echo "[guided] PACKAGE_ROOT=$PACKAGE_ROOT"
echo "[guided] PYTHON_BIN=$PYTHON_BIN"
echo "[guided] RECORDER=$RECORDER"
echo "[guided] capture_service_impl=$CAPTURE_SERVICE_IMPL transport=$TRANSPORT publish=$PUBLISH start=$START_CAPTURE_SERVICE"
echo

if [[ "$START_CAPTURE_SERVICE" != "1" ]]; then
  echo "[guided] Check stream registry:"
  "$PYTHON_BIN" - <<PY
import json
from pathlib import Path
registry = Path("$REGISTRY")
if not registry.exists():
    raise SystemExit(f"registry not found: {registry}; start capture_service or set START_CAPTURE_SERVICE=1")
j = json.loads(registry.read_text())
for sid in ("$CAM0_STREAM", "$CAM1_STREAM", "$IMU_STREAM"):
    s = j["streams"][sid]
    print(sid, s.get("width"), "x", s.get("height"), "payload", s["payload_size"], s.get("format_name"))
PY
  echo
fi

echo "[guided] Before Start:"
echo "  1. AprilGrid should be fixed and not movable."
echo "  2. Move only glasses."
echo "  3. Grid should be visible in both camera streams."
echo "  4. Movements should be smooth, without jerking."
echo "  5. Do not get too close: keep most of the grid in frame."
echo
read -rp "[guided] Press Enter to start ${SECONDS_TOTAL}s recording..."

OUT_LOG="/tmp/xreal_unified480_guided_record.log"
rm -f "$OUT_LOG"

args=(
  "$RECORDER"
  --transport "$TRANSPORT"
  --registry "$REGISTRY"
  --tcp-host "$TCP_HOST"
  --tcp-port "$TCP_PORT"
  --cam0-stream "$CAM0_STREAM"
  --cam1-stream "$CAM1_STREAM"
  --imu-stream "$IMU_STREAM"
  --seconds "$SECONDS_TOTAL"
  --expect-width "$EXPECT_WIDTH"
  --expect-height "$EXPECT_HEIGHT"
  --warmup-seconds "$WARMUP_SECONDS"
)

if [[ "$START_CAPTURE_SERVICE" == "1" ]]; then
  args+=(
    --start-capture-service
    --package-root "$PACKAGE_ROOT"
    --capture-service-impl "$CAPTURE_SERVICE_IMPL"
    --publish "$PUBLISH"
  )
  if [[ "$STOP_CAPTURE_SERVICE" == "1" ]]; then
    args+=(--stop-capture-service)
  fi
fi

"$PYTHON_BIN" "${args[@]}" 2>&1 | tee "$OUT_LOG" &
REC_PID=$!
GUIDE_START="$(date +%s)"

cleanup() {
  if kill -0 "$REC_PID" 2>/dev/null; then
    echo
    echo "[guided] stopping recorder pid=$REC_PID"
    kill "$REC_PID" 2>/dev/null || true
  fi
}
trap cleanup INT TERM

say_at() {
  local target_s="$1"
  local msg="$2"

  while true; do
    if ! kill -0 "$REC_PID" 2>/dev/null; then
      echo
      echo "[guided] recorder already finished before T+${target_s}s"
      return 1
    fi

    local now
    now="$(date +%s)"
    local elapsed=$((now - GUIDE_START))
    local remain=$((target_s - elapsed))

    if (( remain <= 0 )); then
      break
    fi

    if (( remain > 1 )); then
      sleep 1
    else
      sleep "$remain"
    fi
  done

  printf '\a'
  echo
  echo "================================================================"
  echo "[T+${target_s}s] $msg"
  echo "================================================================"

  command -v notify-send >/dev/null 2>&1 && notify-send "XREAL calibration" "$msg" || true
}

echo
echo "[guided] Recording started, pid=$REC_PID"
echo "[guided] Watch xreal_slam_viewer and keep AprilGrid in both cameras."

say_at 0 "Start: Aim the glasses at the center of the AprilGrid. 2-3 seconds, almost calmly."
say_at 5 "Yaw: Slowly rotate the glasses left and right, keeping the grid in the frame at all times."
say_at 15 "Pitch: Slowly tilt the glasses up and down, without any sudden movements."
say_at 25 "Roll: Slowly rotate the glasses around the optical axis: left/right tilt."
say_at 35 "Translation X/Y: Move the glasses left and right and up and down, keeping the target steady."
say_at 50 "Translation Z: Slowly move closer to and further from the AprilGrid, keeping the target angles in mind."
say_at 65 "Combined arcs: Smoothly yaw+pitch+translation, different areas of the frame."
say_at 80 "Final: Return the grid closer to the center, making small, smooth turns."
say_at 88 "Almost done: keep the target visible, do not block the cameras."

wait "$REC_PID"

echo
echo "[guided] Recorder finished."
echo "[guided] Last lines of log:"
tail -20 "$OUT_LOG" || true

echo
echo "[guided] Last dataset:"
ls -1dt "$HOME"/xreal_records/xreal_unified480_calib_* 2>/dev/null | head -n1 || true
