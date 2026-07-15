# Backend capture profiles

`capture_service_cpp` publishes a top-level `profile` value in the capture
registry. Backend launchers resolve an exact profile file with the same name:

```text
capture registry:  "profile": "leap_motion_uvc"
backend profile:   configs/profiles/leap_motion_uvc.env
```

Resolution order is:

```text
--capture-profile / backend-specific environment override / CAPTURE_PROFILE
-> optional capture_tcp_probe result
-> local capture registry .profile
-> xreal_air2ultra_unified_480 compatibility fallback
```

The TCP metadata probe is deliberately disabled by default. Enable it for a
launcher with:

```bash
CAPTURE_PROFILE_PROBE_ENABLED=1 \
CAPTURE_PROFILE_PROBE_HOST=192.168.1.20 \
CAPTURE_PROFILE_PROBE_PORT=45660 \
./start_backend.sh
```

Optional settings are `CAPTURE_PROFILE_PROBE_BIN` and
`CAPTURE_PROFILE_PROBE_TIMEOUT_MS`. Probe failure is non-fatal: the resolver
continues with the local registry and then the compatibility fallback.

A launcher CLI override is consumed before starting the backend and is not
forwarded to the backend executable:

```bash
./start_backend.sh --capture-profile leap_motion_uvc_nrf54l15
```

Profile names are restricted to letters, digits, `_`, `-`, and `.`. A missing
backend profile is an explicit startup error; no approximate matching is used.

Backends that only consume stereo frames (`mercury_hand_tracking`, `xr_video`,
and `xr_spatial`) do not require `imu0`. Basalt and `imu_3dof` still require an
IMU profile. `xr_spatial` uses `SPATIAL_POSE_INPUT=none` and
`SPATIAL_MAP_FRAME=camera` for camera-coordinate operation without IMU or HMD
pose.
