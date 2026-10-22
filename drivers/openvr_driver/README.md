# OpenVR Driver

SteamVR/OpenVR driver for the XR runtime streams produced by `xr_runtime_adapter`.

The driver is intentionally thin. It does not read cameras, IMU, Basalt, Mercury, or raw device data. It only consumes runtime-normalized HMD/controller streams.

## Windows TCP driver

Windows builds default to one shared application TCP connection at
`127.0.0.1:45810` (`xr_gate_app_v1`). Linux `auto` retains its existing POSIX SHM
readers, settings and dependency requirements. Windows explicit `udp` remains
available. See the Windows setup, CI, BAT installer, and Direct Mode instructions:
[English](docs/WINDOWS.en.md) / [Русский](docs/WINDOWS.ru.md).

TCP settings in the `xr_tracking` section are `runtimeTcpHost`, `runtimeTcpPort`,
`runtimeTcpReconnectMs`, and `runtimeTcpTimeoutMs`. Only IPv4 literals and
`localhost` are accepted. All head/hand/controller readers share the connection.
Frames are decoded on a background thread, cached without a queue, and aged
using local receive time plus age calculated entirely in the server clock
domain. Disconnect clears the cache immediately; reconnect requires a fresh
compatible hello and resets the sequence baseline. Missing streams remain
missing and the TCP path bypasses the local controller-input hold policy.

Build C++/Python network tests with `-DXR_OPENVR_BUILD_TESTS=ON`. Linux driver
builds with this option disabled do not compile or link the TCP client/parser.

## Controller compatibility

The same virtual controller input path is used on Windows and Linux. SteamVR
1.26+ automatic rebinding selects the highest-priority layout for which the
application supplies bindings: Oculus Touch, Index, Vive, then Simple Controller.
`resources/input/xr_hand_controller_remapping.json` owns that order and all
source-path conversions. It contains no game IDs or game action names. The
driver keeps its own device identity; simulation is configured per binding.

Touch, direction and physical click are separate inputs. Moving or touching a
pad does not also press its centre. Vive D-pad click bindings become D-pad touch
bindings, preserving their parameters; ordinary button clicks remain physical.
The joystick and trackpad expose the same normalized runtime axes. Analog
trigger/grip values are preserved; a digital-only press supplies a full pull.
Touch inputs use runtime touch flags, presses, or analog activity. Trackpad force
is a binary representation of the physical switch, not measured pressure.

Settings in the `xr_tracking` section of `default.vrsettings`:

| Setting | Default | Effect |
| --- | --- | --- |
| `handControllerLeftABXYAlias` | `true` | Alias left A/B and X/Y so existing runtime mappings also work with Touch/Index layouts. Disable for four independent left face buttons. |
| `handControllerTrackpadClickFromTouch` | `false` | Opt in to the old synthetic trackpad click for older custom bindings. Can make direction and general click actions fire together. |
| `handControllerInputAxisThreshold` | `0.05` | Neutral axis region and threshold for synthesized stick touch. |
| `handControllerInputClickThreshold` | `0.55` | Analog trigger/grip click threshold. |

The physical-device mapping still belongs in the controller backend config.
Application actions still belong to the game's bindings. Existing user-saved
bindings take precedence over automatic rebinding; after upgrading, select the
game's default bindings if an older generated/custom binding remains active.
Restart SteamVR after changing a driver resource or setting. A new DLL/.so and
the matching resources are required; copying JSON onto an old binary does not
change its input semantics. See [the Russian setup guide](docs/CONTROLLER_INPUT.ru.md).

The functional tests cover normalized input semantics and the binding resource
contract. To additionally check exported application bindings, run
`python3 tests/controller_bindings_test.py --bindings-zip /path/to/exports.zip`
from this directory. This checks destination capabilities, not SteamVR's actual
rebinding engine or in-game behavior. Games requiring controls absent from the
physical device still need a backend mapping; skeletal fingers and real pressure
sensing are not provided by these compatibility layouts.

## Expected runtime streams

```text
/tmp/runtime_tracking_streams.json : runtime_hmd_pose
/tmp/runtime_tracking_streams.json : runtime_controller_state
/tmp/runtime_tracking_streams.json : runtime_hand_tracking        # optional fallback
```

## Linux SHM restart recovery

The pose, hand and controller readers check whether their mapped SHM object is
still linked and has its original size before reading it. When a runtime
publisher unlinks/recreates its segment, the driver closes the old mapping and
opens the current segment automatically, including a restarted sequence number.
Without this check, an old mapping remains readable with frozen data and the
controller poses can remain unavailable until SteamVR is restarted. Missing,
truncated or incompatible segments are rejected and retried on subsequent frames.
The usual sample-age checks still apply to a stopped publisher.

`XR_OPENVR_BUILD_TESTS=ON` also enables the `openvr_posix_shm` test on Linux. It
uses the production readers with real SHM rings to cover publisher replacement,
live updates, missing segments, truncation, invalid headers and stale samples.

For an Alyx failure, distinguish a reversed trackpad axis in SteamVR's input
test from reversed player movement. The driver forwards the runtime stick X/Y
values directly; its log reports them as `runtime controller left ... stick=(x,y)`.
Reversed movement alone can also involve the controller pose or game bindings,
so restart recovery is not proof of the cause of an axis inversion.

Before restarting SteamVR during a failure, save `vrserver.txt` from Steam's
`logs` directory. On Linux, this command also shows whether its runtime SHM file
descriptors refer to unlinked objects (marked `(deleted)`):

```bash
ls -l /proc/"$(pgrep -n vrserver)"/fd | grep -E 'runtime_(hmd_pose|hand_tracking|controller_state)'
```

## Package driver directory

Historical XREAL packages keep their existing names, for example:

```text
out/xr-gate/bin/drivers/openvr_driver_60HZ/xr_tracking/
```

Custom device profiles are namespaced, for example:

```text
out/my_glasses/bin/drivers/openvr_driver_my_glasses_72HZ/xr_tracking/
```

The final `xr_tracking/` directory is the SteamVR driver package registered manually or with `vrpathreg`.

## Display and optics config

OpenVR build and registration use:

```text
drivers/openvr_driver/configs/display/default.yaml
```

Build with another config:

```bash
XR_OPENVR_DISPLAY_CONFIG=/path/cardboard.yaml \
drivers/openvr_driver/scripts/build_and_register_driver.sh
```

The schema is:

```yaml
display:
  width_px: 3840
  height_px: 1080
  layout: side_by_side_horizontal
  eye_width_px: 1920
  eye_height_px: 1080
  refresh_hz: 60
  rotation_deg: 0

optics:
  ipd_m: 0.064
  inter_lens_distance_m: 0.064
  screen_to_lens_distance_m: 0.0
  eye_to_lens_distance_m: 0.0

  left_eye:
    lens_center_uv: [0.5, 0.5]
    fov_deg:
      left: 45.0
      right: 45.0
      up: 45.0
      down: 45.0

  right_eye:
    lens_center_uv: [0.5, 0.5]
    fov_deg:
      left: 45.0
      right: 45.0
      up: 45.0
      down: 45.0
```

The checked-in default reproduces the previous hardcoded OpenVR package values.
The normalized config is rendered into `default.vrsettings`, and a copy is
stored in the assembled driver package at:

```text
xr_tracking/resources/settings/display_config.yaml
```

Registration uses the rendered settings from that package. Changing the YAML
therefore requires rebuilding the OpenVR package, but not changing driver source.

An explicit frequency override remains supported and has priority over
`display.refresh_hz`:

```bash
XR_OPENVR_DISPLAY_CONFIG=/path/cardboard.yaml \
XR_OPENVR_DISPLAY_FREQUENCY_HZ=72 \
drivers/openvr_driver/scripts/build_and_register_driver.sh
```

Display geometry, layout, rotation, IPD, and asymmetric per-eye FOV are applied
to the OpenVR package. Lens centers, inter-lens distance, and screen/eye-to-lens
distances are loaded into the driver settings; while identity distortion is
active they are optics metadata for the future distortion implementation.


## Device identity overlays

Model/serial and other device-specific settings can still be supplied by:

```text
drivers/openvr_driver/devices/<profile>/settings/default.vrsettings
```

The display/optics YAML is applied after this overlay, so display geometry comes
from the selected YAML config. Existing environment overrides remain available
for one-off tests and have higher priority.

## Package naming

`XR_OPENVR_PACKAGE_TAG` can distinguish packages for the same profile, mode,
and frequency. Existing XREAL package names such as `openvr_driver_60HZ` remain
unchanged.

## Standalone source archive

Regenerate the repository's standalone source ZIP with legal files and without
build/cache/backup artifacts:

```bash
./drivers/openvr_driver/scripts/package_source_zip.sh
```
