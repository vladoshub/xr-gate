# Backend recovery regression checks

These changes do not change sensor axes, image rotations, camera/IMU time
offsets, extrinsics, or saved controller calibration. Recalibration is not
required. Existing Monado hand and xr_spatial scanner patches apply independently.

Fixed behavior:

- Both TCP capture constructors close sockets and join receiver threads on
  initialization failure instead of aborting in a joinable thread destructor.
- Video and Mercury reopen capture input after one second without stereo.
  Mercury recreates its tracker and velocity history and changes reset_counter
  when frames resume after reconnecting. Output video sequence stays continuous.
- Basalt restarts capture/VIO after stalled stereo, missing IMU coverage,
  IMU sequence loss/restart, nonmonotonic timestamps, or an IMU timestamp gap
  over 50 ms. Strict IMU checks are enabled for Basalt only. Automatic and
  control resets advance the published pose reset_counter without rewriting
  backend control/calibration files. Duration covers all recovery attempts.
- Runtime clears controller optical anchors, velocity and yaw state when the
  primary 6DoF HMD reset_counter changes, including automatic Basalt recovery.
  This repeats live alignment initialization; saved sensor/controller
  calibration parameters are not edited. Rebuild runtime_adapter as well.
- Basalt feeds IMU independently of images, initializes only after startup
  gates pass, and drains workers during shutdown. Incomplete IMU windows are
  never sent as valid measurements. Recovery resets tracking origin; existing
  runtime reset_counter handling remains responsible for recentering.
- Video recording checks image, CSV, and metadata writes. A failed recording
  stops with an error and finalized=false; video streaming continues. Generated
  recording directories are unique, and explicit existing recordings are
  rejected rather than overwritten.
- Mercury selects V2 automatically unless an explicit format was supplied;
  explicit V1 with Mercury is rejected. Placeholder mode keeps its V1 default.
  The quality-filter test reflects the existing disabled-by-default setting.

Build the backends using the normal project installers after applying the patch:

```bash
ROOT_PROJECT="$PWD" bash backends/xr_video/scripts/linux/install_xr_video.sh
ROOT_PROJECT="$PWD" bash backends/mercury_hand_tracking/scripts/linux/install_mercury.sh
ROOT_PROJECT="$PWD" bash backends/basalt_vio/scripts/linux/install_basalt.sh
ROOT_PROJECT="$PWD" bash runtime_adapters/xr_runtime_adapter/scripts/linux/install_xr_runtime_adapter.sh
```

Standalone tests need a C++17 compiler, CMake, TBB and nlohmann-json development
packages. Run them on Linux (TCP, POSIX SHM and /dev/full are exercised):

```bash
cmake -S tests/backend_recovery -B build/backend_recovery -DCMAKE_BUILD_TYPE=Release
cmake --build build/backend_recovery -j2
ctest --test-dir build/backend_recovery --output-on-failure
python3 tests/backend_recovery/shm_recovery_test.py \
  --video /path/to/xr_video_backend \
  --hands /path/to/capture_hand_tracking_backend \
  --mercury-library "$PWD/build/backend_recovery/libfake_mercury.so" \
  --runtime /path/to/xr_runtime_adapter
python3 backends/mercury_hand_tracking/tests/test_quality_filter.py
python3 backends/mercury_hand_tracking/tests/test_tracking_diagnostics.py
```

The fake Mercury library tests backend lifecycle and ABI plumbing; it does not
run the neural models. The feeder test uses real TBB queues with an estimator
test double. Full VIO optimization and hardware tracking need device testing.
