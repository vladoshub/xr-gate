# XREAL Ultra OpenVR profile

Device-specific defaults for the generic `xr_tracking` OpenVR driver.

The driver code remains generic; `build_driver.sh` overlays this profile onto
`resources/settings/default.vrsettings` when `XR_OPENVR_DEVICE=xreal_ultra`.
Frequency and display mode are still selected by the build/register scripts.
