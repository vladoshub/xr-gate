# Common device runtime

This directory contains hardware-neutral environment defaults and launch wrappers.

A concrete device profile, such as `devices/xreal_ultra/xreal_ultra.env`, sources
`common.env` and supplies capture configuration, calibration and display geometry.

`xr_client` uses:

- `{common_scripts}` for generic capture/backend/runtime launchers;
- `{device_scripts}` for hardware-specific helpers;
- `{scripts}` as a legacy alias for `{device_scripts}`.

The compatibility wrappers under `devices/xreal_ultra/linux/scripts` remain so
existing commands do not break, but delegate immediately to this directory.
