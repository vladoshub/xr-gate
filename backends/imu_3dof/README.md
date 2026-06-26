# IMU 3DoF Backend

Lightweight fallback HMD orientation backend.

It reads `imu0` from `capture_service`, runs a gyro+accelerometer AHRS filter, and publishes an HMD pose contract with synthetic/fixed position.

## Main stream

```text
/tmp/tracking_streams.json : hmd_pose_3dof
```

## Use cases

```text
- 3DoF fallback when Basalt is unavailable
- quick headset orientation mode
- 3DoF spatial passthrough together with xr_spatial pose_input=none
```

## Package output

```text
out/xreal_ultra/bin/backends/imu_3dof/
```
