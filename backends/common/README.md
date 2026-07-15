# Backend capture profiles

`capture_service_cpp` publishes a top-level `profile` value in the capture
registry. Backend launchers resolve an exact profile file with the same name:

```text
capture registry:  "profile": "leap_motion_uvc"
backend profile:   configs/profiles/leap_motion_uvc.env
```

Resolution order is:

```text
backend-specific override / CAPTURE_PROFILE
-> capture registry .profile
-> xreal_air2ultra_unified_480 compatibility fallback
```

Profile names are restricted to letters, digits, `_`, `-`, and `.`. A missing
backend profile is an explicit startup error; no approximate matching is used.

Backends that only consume stereo frames (`mercury_hand_tracking`, `xr_video`,
and `xr_spatial`) do not require `imu0`. Basalt and `imu_3dof` still require an
IMU profile. `xr_spatial` uses `SPATIAL_POSE_INPUT=none` and
`SPATIAL_MAP_FRAME=camera` for camera-coordinate operation without IMU or HMD
pose.
