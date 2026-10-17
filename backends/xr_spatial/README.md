# XR Spatial Backend

Optional backend for live stereo depth, spatial proxy mesh, and lightweight backend-only scan experiments.

It reads stereo frames from `capture_service`, optionally reads HMD pose, computes stereo depth, publishes an organized-grid spatial proxy mesh, and can save simple scan artifacts to disk.

## Main streams

```text
/tmp/runtime_tracking_streams.json : spatial_proxy_mesh
/tmp/runtime_tracking_streams.json : runtime_spatial_summary
```

## Common modes

```text
SPATIAL_POSE_INPUT=shm   # 6DoF/world-space mesh and scanner
SPATIAL_POSE_INPUT=none  # camera-relative live passthrough / 3DoF debug mode
```

## Package output

```text
out/xr-gate/bin/backends/xr_spatial/
```

`xr_spatial` is optional and must not sit between tracking backends and `xr_runtime_adapter`.

## Automatic capture profiles and camera-only mode

The launcher reads the top-level `profile` value from
`/tmp/capture_service_streams.json` and loads the exact matching file from
`configs/profiles/<profile>.env`. Explicit `XR_SPATIAL_PROFILE` or
`CAPTURE_PROFILE` remains higher priority. When the field is absent, the
existing `xreal_air2ultra_unified_480` profile is used.

`xr_spatial` consumes stereo frames and an optional HMD pose stream; it does not
consume raw `imu0`. For camera-coordinate live depth, a profile can set:

```bash
SPATIAL_POSE_INPUT=none
SPATIAL_MAP_FRAME=camera
```

The existing Basalt-style calibration JSON with `value0.T_imu_cam` remains
supported unchanged. A stereo-only JSON may instead contain
`value0.T_cam1_cam0`, using the convention:

```text
X_cam1 = T_cam1_cam0 * X_cam0
```

It must still contain two entries in `value0.intrinsics` and
`value0.resolution`.

## Capture transport

SHM remains the default and keeps the existing local pipeline unchanged:

```bash
./scripts/linux/start_xr_spatial_shm.sh
```

For direct `capture_service_cpp` TCP input, use the thin wrapper:

```bash
CAPTURE_TCP_HOST=192.168.1.20 \
CAPTURE_TCP_PORT=45660 \
./scripts/linux/start_xr_spatial_tcp.sh
```

The TCP transport subscribes only to `camera0` and `camera1`; `imu0` is not
requested. Pose input remains independent: `SPATIAL_POSE_INPUT=shm` can still
consume a local `hmd_pose`, while `SPATIAL_POSE_INPUT=none` keeps output in
camera coordinates. Automatic TCP profile probing remains disabled unless
`CAPTURE_PROFILE_PROBE_ENABLED=1` is explicitly set.

## Scanner recovery and output

The launchers use the tracking sensor's `BASALT_CALIB` as the default calibration.
An explicit `CALIB_JSON` still takes priority. This keeps the scanner on the same
camera intrinsics and camera-to-IMU transform as the selected Basalt profile,
including XREAL device-time calibration.

All launchers also accept `--calib /path/to/calibration.json` (or `--calib=...`).
The last explicit CLI value overrides both environment and profile paths before
checking the file. Paths containing spaces are supported when quoted.

With tracked pose input, camera timestamps are shifted by the calibration's
`value0.cam_time_offset_ns` before matching frames to poses:
`t_imu = t_camera + offset`. Source image timestamps remain unchanged. A frame
is consumed only once even when `DEPTH_RATE_HZ=0` disables rate limiting.

A scan stops before integrating a changed pose `reset_counter`, a restarted
pose sequence, or a large pose-source jump. The accumulated part is saved in
the selected output directory; worlds from different tracking epochs are not
merged. The jump thresholds are `QUALITY_REINIT_DELTA_M/DEG` and apply even
when the optional depth quality gate is disabled. Runtime mode clears its map
and tracking origin on the same discontinuities and keeps running.

Stalled camera SHM streams are reopened using the current capture registry.
`CAPTURE_REATTACH_ON_STALE_MS=500` controls the timeout; `0` disables reopening.
Restarted camera sequence numbers are accepted when their timestamps advance.
A scan ending with stalled capture, no integrated points, or an interruption
is marked incomplete rather than silently reported as a successful full scan.

Depth errors drop the individual source frame. After 30 consecutive errors the
scanner saves its accumulated data and exits with an error. Other processing
exceptions also finalize the accumulated scan. Each output is saved separately,
so a failed artifact does not prevent attempts to save the others. Write/flush
failures are reported, including a full output disk.

`scan_metadata.json` records `complete`, `stop_reason`, `frame_errors`,
`capture_reattachments`, `camera_time_offset_ns`, `observed_points`, and
`reservoir_sampling`. Exit codes are `0` for a completed scan, `2` for a saved
incomplete scan, and `1` for processing or output errors.

`MAX_ACCUMULATED_POINTS` must be positive. Once this memory budget is reached,
uniform reservoir sampling keeps a bounded sample of the entire scan, allowing
new areas to replace a portion of earlier points. It does not preserve every
observation. Voxel PLY `observations` counts retained source points, including
points from the same frame; it is not a count of independent frame detections.
Scan mode accumulates points even if `live_depth_grid` is selected for display.
Explicit CLI arguments override profile-derived arguments in the launchers.

## CPU usage and thread count

OpenCV uses one thread by default. Select a limit with `--num-threads N` or
`XR_SPATIAL_NUM_THREADS=N` in a launcher's environment (including the `xr_client`
service environment). An explicit CLI value has priority. `0` restores OpenCV's
automatic parallelism; negative values are rejected. The backend logs the
requested and effective counts and records them in scan metadata. This controls
OpenCV work, not CPU affinity or transport threads.

```bash
./scripts/linux/start_xr_spatial_shm.sh --num-threads 2 --calib /path/to/calib.json
./scripts/linux/start_xr_spatial_scan.sh --num-threads 1 --calib /path/to/calib.json
```

Stereo synchronization caches image pairs within `STEREO_POSE_SYNC_SCAN_BACK`
(default 16), so pose polling and depth rate limiting reuse payloads already
read from the capture transport. Only new sequences are copied. Memory use is
bounded by this window (about 10 MB of GRAY8 pixels for 16 pairs at 480x640).
Camera-only mode keeps one pair. The cache is cleared when cameras are reopened
or their sequence restarts; pose resets still stop scans or clear runtime maps.
Timestamp matching, calibration transforms, SGBM resolution/disparity settings,
and the default depth rate remain unchanged.

## Scanner regression tests

Requires the same C++/OpenCV dependencies as the backend and Python 3:

```bash
cmake -S backends/xr_spatial -B build/tests/xr_spatial -DXR_SPATIAL_BUILD_TESTS=ON
cmake --build build/tests/xr_spatial -j2
ctest --test-dir build/tests/xr_spatial --output-on-failure
```

The tests exercise the production scanner and SGBM with synthetic stereo images
and real SHM rings. They cover calibration selection, camera/IMU time offsets,
pose resets and jumps, camera SHM replacement, frame errors, duplicate frames,
point budgets, live-grid scanning, and failed output writes. No camera or HMD
hardware is needed.
