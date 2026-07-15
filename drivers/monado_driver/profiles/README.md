# Monado display profiles

`start.sh` can source one profile before launching `monado-service`:

```bash
XR_MONADO_DISPLAY_PROFILE=my_glasses bin/drivers/monado_driver/start.sh
```

The matching file is `profiles/my_glasses.env`. An explicit path has priority:

```bash
XR_MONADO_DISPLAY_PROFILE_FILE=/path/my_glasses.env bin/drivers/monado_driver/start.sh
```

Profile files should assign defaults with `${VAR:-value}` so explicit caller environment variables remain authoritative.

Common variables:

```bash
XR_MONADO_EYE_WIDTH=1920
XR_MONADO_EYE_HEIGHT=1080
XR_MONADO_WINDOW_WIDTH=3840
XR_MONADO_WINDOW_HEIGHT=1080
XR_MONADO_FOV_HORIZONTAL_DEG=52
XR_MONADO_FOV_VERTICAL_DEG=30
XR_MONADO_REFRESH_HZ=60
XR_MONADO_IPD_M=0.064
```

Directional FOV half-angles are also supported:

```bash
XR_MONADO_FOV_LEFT_DEG=26
XR_MONADO_FOV_RIGHT_DEG=26
XR_MONADO_FOV_UP_DEG=15
XR_MONADO_FOV_DOWN_DEG=15
```

Existing `XR_MONADO_FOV_LEFT/RIGHT/UP/DOWN` values remain raw radians and override degree aliases.
