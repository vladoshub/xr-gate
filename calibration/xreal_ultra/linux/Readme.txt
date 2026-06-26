# XREAL Air 2 Ultra unified 480x640 calibration flow

This folder records a Kalibr-compatible stereo+IMU dataset from the XR package runtime.
The default capture backend is now `capture_service_cpp`; the old Python/GStreamer backend remains an explicit fallback.

Assumptions:

```text
repo:        ~/src/xr_tracking
package:     ~/src/xr_tracking/out/xreal_ultra
streams:     camera0, camera1, imu0
transport:   shm on Linux, tcp when explicitly requested
resolution:  480x640 GRAY8
```

## 0. Install Kalibr

```bash
cd ~/src/xr_tracking/calibration/xreal_ultra/linux
XREAL_CALIB_DIR="$HOME/xreal_calib" ./scripts/install_kalibr.sh
```

## 1. Build the XR package

```bash
cd ~/src/xr_tracking
./devices/xreal_ultra/linux/scripts/install_xreal_ultra_out.sh
```


```bash
cd ~/src/xr_tracking

pkill -TERM -f capture_service_cpp || true
pkill -TERM -f capture_service\.cli || true
sleep 1
pkill -KILL -f capture_service_cpp || true
pkill -KILL -f capture_service\.cli || true

rm -f /tmp/capture_service_streams.json
rm -f /dev/shm/cap_xreal_air2ultra_linux_camera0
rm -f /dev/shm/cap_xreal_air2ultra_linux_camera1
rm -f /dev/shm/cap_xreal_air2ultra_linux_imu0
rm -f /dev/shm/cap_xreal_air2ultra_linux_xreal_raw_hid
```


## 2. Record dataset

Recommended: let the recorder start and stop `capture_service_cpp` around the recording.

```bash
cd ~/src/xr_tracking/calibration/xreal_ultra/linux
START_CAPTURE_SERVICE=1 \
CAPTURE_SERVICE_IMPL=cpp \
TRANSPORT=shm \
PUBLISH=shm \
SECONDS_TOTAL=90 \
./scripts/start_record.sh
```

Alternative: start the full XR stack/package separately, then record from existing SHM streams.

```bash
cd ~/src/xr_tracking/out/xreal_ultra
./run_xr_client.sh

# new terminal
cd ~/src/xr_tracking/calibration/xreal_ultra/linux
TRANSPORT=shm SECONDS_TOTAL=90 ./scripts/start_record.sh
```



# check video in new terminal
```bash
# new terminal after start record
cd ~/src/xr_tracking
./capture_client/debug/direct_slam_viewer_shm.sh
```

The recorder writes datasets to:

```text
~/xreal_records/xreal_unified480_calib_<timestamp>/
```

The output includes `camera_timestamps.csv`, `imu.csv`, `streams.txt`, `record_metadata.json`, and `frame_quality.csv`.

## 3. Convert dataset to ROS bag

```bash
cd ~/src/xr_tracking/calibration/xreal_ultra/linux
ROOT_PROJECT="$HOME/src/xr_tracking" \
XREAL_CALIB_DIR="$HOME/xreal_calib" \
XREAL_RECORDS_DIR="$HOME/xreal_records" \
./scripts/run_convertation_to_ros.sh
```

## 4. Check AprilGrid physical size

Example helper:

```bash
ROW_MM=171.1
COL_MM=243.7

python3 - <<PY
row_mm = float("$ROW_MM")
col_mm = float("$COL_MM")

tag_from_row = row_mm / 6.6
tag_from_col = col_mm / 9.4
avg = (tag_from_row + tag_from_col) / 2

print("tag from row mm:", tag_from_row)
print("tag from col mm:", tag_from_col)
print("avg tag mm:", avg)
print("tagSize meters:", avg / 1000.0)
PY
```

Then update `tagSize` in `$XREAL_CALIB_DIR/targets/aprilgrid_a4_5x7_25mm.yaml`.

## 5. Run camera-only calibration

```bash
cd ~/src/xr_tracking/calibration/xreal_ultra/linux
XREAL_CALIB_DIR="$HOME/xreal_calib" ./scripts/run_kalibr_camera.sh
```

Ignore `TypeError: plotting not available` if calibration still completes. After `Calibration complete.`:

```bash
XREAL_CALIB_DIR="$HOME/xreal_calib" \
FINAL_CAM_DIR="$HOME/xreal_calib/final/xreal_air2ultra/ZBBM5DZFMP/unified_480_ccw90" \
./scripts/save_camera_calibration.sh
```

## 6. Run camera-IMU calibration

```bash
UNIFIED_CALIB_DIR="$HOME/xreal_calib/final/xreal_air2ultra/ZBBM5DZFMP/unified_480_ccw90" \
XREAL_CALIB_DIR="$HOME/xreal_calib" \
XREAL_SERIAL="ZBBM5DZFMP" \
FROM="3" \
TO="60" \
MAX_ITER="6" \
TIMEOFFSET_PADDING="0.5" \
./scripts/run_kalibr_imu_camera.sh
```

## 7. Convert Kalibr result to Basalt/Mercury JSON

```bash
CONVERTER="$HOME/src/xr_tracking/calibration/xreal_ultra/linux/kalibr_to_basalt_unified_json.py" \
ROOT_PROJECT="$HOME/src/xr_tracking" \
XREAL_CALIB_DIR="$HOME/xreal_calib" \
XREAL_SERIAL="ZBBM5DZFMP" \
CALIB_PROFILE_NAME="unified_480_ccw90" \
./scripts/convert_basalt.sh
```
