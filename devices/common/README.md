# Common device runtime

This directory contains hardware-neutral environment defaults, runtime launchers
and shared Linux build/package tooling.

A concrete device profile, such as `devices/xreal_ultra/xreal_ultra.env`, sources
`common.env` and supplies capture configuration, calibration and display geometry.

`xr_client` uses:

- `{common_scripts}` for generic capture/backend/runtime launchers;
- `{device_scripts}` for hardware-specific helpers;
- `{scripts}` as a legacy alias for `{device_scripts}`.

Shared Linux tooling is split by purpose:

```text
linux/scripts/build/    generic device build/package implementation
linux/scripts/ci/       generic local GitHub Actions runner
linux/scripts/release/  generic runtime archive extractor
linux/scripts/runtime/  generic Ubuntu runtime dependency installer
```

Device folders keep thin naming/profile wrappers and optional hooks for hardware
components, udev rules, permissions and device-only package content.
