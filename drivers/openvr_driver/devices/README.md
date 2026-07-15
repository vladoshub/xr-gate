# OpenVR device settings overlays

Each optional named device profile lives at:

```text
<profile>/settings/default.vrsettings
```

Only values that differ from `resources/settings/default.vrsettings` need to be
included. Build scripts deep-merge the overlay and then apply explicit
environment overrides.

Minimal example:

```json
{
  "xr_tracking": {
    "serialNumber": "my-glasses-hmd-001",
    "modelNumber": "My SBS Glasses",
    "windowWidth": 3200,
    "windowHeight": 900,
    "renderWidth": 1600,
    "renderHeight": 900,
    "ipdMeters": 0.064,
    "fovHorizontalDeg": 52,
    "fovVerticalDeg": 30
  },
  "steamvr": {
    "windowWidth": 3200,
    "windowHeight": 900,
    "renderWidth": 1600,
    "renderHeight": 900
  }
}
```

The renderer converts `fovHorizontalDeg` / `fovVerticalDeg` into the tangents
required by OpenVR `GetProjectionRaw`. Asymmetric profiles may use
`fovLeftDeg`, `fovRightDeg`, `fovUpDeg`, and `fovDownDeg`. Raw
`projectionLeft/Right/Top/Bottom` values remain supported.
