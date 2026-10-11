CAL_COMMON=~/src/xr-gate/calibration/common/linux
CALIB_TARGET=xreal_ultra_device_time
CALIB_PROFILE_NAME=unified_480_ccw90_device_time
CALIB_UNIT_ID=ZBBM5DZFMP



FORCE_TARGET_CONFIGS=1 \
"$CAL_COMMON/calibrate.sh" \
--target "$CALIB_TARGET" \
install


CONFIG_PATH=~/src/xr-gate/devices/xreal_ultra/configs/capture_service/xreal_air2ultra_linux_unified_480_device_time_shm.yaml ~/src/xr-gate/out/xr-gate/bin/scripts/capture_service_cpp/start_capture_service_cpp.sh
CAPTURE_CLIENT_ROOT=~/src/xr-gate/capture_client ~/src/xr-gate/capture_client/debug/direct_slam_viewer_shm.sh
RECORD_MODE=stereo_imu START_CAPTURE_SERVICE=1 SECONDS_TOTAL=90 ~/src/xr-gate/calibration/common/linux/calibrate.sh   --target xreal_ultra_device_time   record


DS="$(ls -1dt "$HOME/xr_records/$CALIB_TARGET"/* | head -n1)"
echo "$DS"

wc -l "$DS/camera_timestamps.csv"
wc -l "$DS/imu.csv"
head -3 "$DS/imu.csv"
cat "$DS/record_metadata.json"


4. BAG="$HOME/xr_calib/$CALIB_TARGET/bags/$(basename "$DS")_stereo_imu.bag"

DS="$DS" \
BAG="$BAG" \
NO_IMU=0 \
"$CAL_COMMON/calibrate.sh" \
--target "$CALIB_TARGET" \
bag


5.
BAG="$BAG" \
BAG_FREQ=4.0 \
APPROX_SYNC=0.005 \
"$CAL_COMMON/calibrate.sh" \
--target "$CALIB_TARGET" \
camera


6.

BAG="$BAG" \
"$CAL_COMMON/calibrate.sh" \
--target "$CALIB_TARGET" \
save-camera

7.
BAG="$BAG" \
FROM=3 \
TO=87 \
MAX_ITER=7 \
TIMEOFFSET_PADDING=0.5 \
"$CAL_COMMON/calibrate.sh" \
--target "$CALIB_TARGET" \
imu-camera


"$CAL_COMMON/calibrate.sh" \
--target "$CALIB_TARGET" \
convert-runtime


ROOT="$HOME/src/xr-gate"


SRC="$HOME/xr_calib/xreal_ultra_device_time/final/xreal_air2ultra/ZBBM5DZFMP/$CALIB_PROFILE_NAME"


DST="$ROOT/devices/xreal_ultra/configs/calibration_dataset/final/xreal_air2ultra/ZBBM5DZFMP/$CALIB_PROFILE_NAME"

#Example
#DST="$ROOT/devices/leap_motion_uvc_nrf54l15/configs/calibration_dataset/final/$CALIB_TARGET/$CALIB_UNIT_ID/$CALIB_PROFILE_NAME"

mkdir -p "$DST"


install -m 0644 \
"$SRC/basalt_calib_${CALIB_PROFILE_NAME}.json" \
"$DST/basalt_calib_640x480.json"

install -m 0644 \
"$SRC/mercury_calib_${CALIB_PROFILE_NAME}.json" \
"$DST/mercury_calib_640x480.json"

install -m 0644 \
"$SRC/basalt_vio_config_${CALIB_PROFILE_NAME}.json" \
"$DST/basalt_vio_config_640x480.json"

install -m 0644 \
"$SRC/basalt_vo_config_${CALIB_PROFILE_NAME}.json" \
"$DST/basalt_vo_config_640x480.json"  
  
  
 