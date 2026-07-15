# OpenVR device identity overlays

A named device profile may provide:

```text
<profile>/settings/default.vrsettings
```

Use this overlay for device identity and settings that are not part of display
geometry, for example:

```json
{
  "xr_tracking": {
    "serialNumber": "my-glasses-hmd-001",
    "modelNumber": "My SBS Glasses"
  }
}
```

Resolution, layout, refresh rate, IPD, lens metadata, and per-eye FOV belong in
the display/optics YAML selected with `XR_OPENVR_DISPLAY_CONFIG`. The build
renderer merges the identity overlay first, then applies the display config,
then explicit environment overrides.
