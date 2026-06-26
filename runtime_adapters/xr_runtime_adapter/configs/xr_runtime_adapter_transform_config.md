# XR Runtime Adapter Transform Config

This document explains how to configure `xr_21_joint_hand_viewer_verified.json`.

The file defines coordinate transforms, orientation transforms, HMD-relative placement, spatial mesh runtime behavior, and controller override behavior for `xr_runtime_adapter`.

Typical package location:

```text
devices/xreal_ultra/configs/xr_runtime_adapter/xr_21_joint_hand_viewer_verified.json
```

Typical development-tree location:

```text
runtime_adapters/xr_runtime_adapter/configs/xr_21_joint_hand_viewer_verified.json
```

`xr_runtime_adapter` should receive this file through the runtime environment or its launch wrapper:

```bash
TRACKING_TRANSFORM_CONFIG=/path/to/xr_21_joint_hand_viewer_verified.json
```

---

## 1. Purpose

Different XR services can publish poses in different coordinate conventions:

```text
Basalt / VIO
imu_3dof_backend
Mercury hand tracking
xr_spatial
body tracker backends
controller override service
```

This JSON file tells `xr_runtime_adapter` how to convert those inputs into the runtime coordinate space used by OpenVR, Monado, debug viewers, and downstream consumers.

It is responsible for:

```text
- axis remapping
- axis inversion
- HMD height offset
- HMD-relative hand placement
- orientation basis correction
- controller/palm/wrist orientation correction
- spatial mesh transform and triangle winding
- 3DoF/camera-relative spatial passthrough mode
- controller override behavior
```

This file does not start services. It only configures how `xr_runtime_adapter` transforms data.

---

## 2. Top-level structure

```json
{
  "enabled": true,
  "streams": {
    "hmd": {},
    "hmd_3dof": {},
    "hand_tracking_21_joint": {},
    "hand_skeleton26": {},
    "body_trackers": {},
    "spatial_proxy_mesh": {}
  },
  "controller_override": {}
}
```

### `enabled`

```json
"enabled": true
```

Keep this set to `true` for the verified XREAL Ultra runtime profile.

If this is disabled, `xr_runtime_adapter` may fall back to default or identity transforms, and the verified coordinate corrections will not be applied.

---

## 3. `streams`

The `streams` object contains per-stream transform configuration.

Current stream blocks:

```text
hmd
  6DoF HMD pose, usually from Basalt/VIO.

hmd_3dof
  3DoF HMD pose, usually from imu_3dof_backend.

hand_tracking_21_joint
  Mercury 21-joint hand tracking stream.

hand_skeleton26
  Extended 26-joint hand skeleton stream.

body_trackers
  Generic body tracker stream.

spatial_proxy_mesh
  Mesh/points stream from xr_spatial.
```

Each stream can have some or all of these blocks:

```text
coordinate_transform
hmd_relative
orientation_transform
hand_orientation_offset
mesh_runtime
```

---

## 4. `coordinate_transform`

Example:

```json
"coordinate_transform": {
  "axis_map": ["x", "z", "y"],
  "invert_x": true,
  "invert_y": false,
  "invert_z": false,
  "rotation_deg": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.0
  },
  "scale": 1.0,
  "offset_m": {
    "x": 0.0,
    "y": 1.6,
    "z": 0.0
  }
}
```

This block transforms positions.

The usual transform order is conceptually:

```text
input position
  -> axis remap
  -> axis inversion
  -> optional rotation
  -> scale
  -> offset
  -> runtime position
```

### 4.1 `axis_map`

```json
"axis_map": ["x", "z", "y"]
```

`axis_map` defines which input axis becomes each output axis.

Examples:

```json
["x", "y", "z"]
```

No axis remapping.

```json
["x", "z", "y"]
```

Output `x` comes from input `x`, output `y` comes from input `z`, output `z` comes from input `y`.

Change this when an object appears rotated into the wrong coordinate plane.

Typical symptoms:

```text
- HMD moves forward but runtime moves it upward
- mesh lies on the wrong plane
- hands appear rotated around the wrong axis
- depth grid is vertical when it should be horizontal
```

### 4.2 `invert_x`, `invert_y`, `invert_z`

Example:

```json
"invert_x": true,
"invert_y": false,
"invert_z": false
```

These flags invert the selected output axis after axis remapping.

Use them when movement is correct in axis choice but wrong in direction.

Typical symptoms:

```text
Left/right mirrored:
  toggle invert_x.

Up/down reversed:
  toggle invert_y.

Forward/backward reversed:
  toggle invert_z.
```

Example:

```json
"invert_x": true
```

means the runtime `x` coordinate is multiplied by `-1`.

### 4.3 `rotation_deg`

```json
"rotation_deg": {
  "x": 0.0,
  "y": 0.0,
  "z": 0.0
}
```

This applies an additional positional rotation in degrees.

Prefer fixing coordinate conventions with:

```text
axis_map
invert_x / invert_y / invert_z
```

Use `rotation_deg` only when remapping and sign flips are not enough.

Useful diagnostic values:

```json
"rotation_deg": { "x": 90.0, "y": 0.0, "z": 0.0 }
```

```json
"rotation_deg": { "x": 0.0, "y": 90.0, "z": 0.0 }
```

```json
"rotation_deg": { "x": 0.0, "y": 0.0, "z": 90.0 }
```

### 4.4 `scale`

```json
"scale": 1.0
```

Position scale.

Keep this as:

```json
"scale": 1.0
```

for all current XREAL runtime streams.

Change it only if an input stream uses different units.

Example:

```json
"scale": 0.001
```

would convert millimeters to meters.

### 4.5 `offset_m`

```json
"offset_m": {
  "x": 0.0,
  "y": 1.6,
  "z": 0.0
}
```

Final position offset in meters.

For `hmd` and `hmd_3dof`, the current profile adds runtime height:

```json
"y": 1.6
```

This places the HMD approximately 1.6 meters above the runtime floor.

If the user appears too low:

```json
"offset_m": {
  "x": 0.0,
  "y": 1.75,
  "z": 0.0
}
```

If the user appears too high:

```json
"offset_m": {
  "x": 0.0,
  "y": 1.45,
  "z": 0.0
}
```

---

## 5. `hmd_relative`

Example:

```json
"hmd_relative": {
  "enabled": true,
  "offset_m": {
    "x": 0.0,
    "y": -0.05,
    "z": -0.15
  },
  "rotate_with_hmd_orientation": true
}
```

This block places a stream relative to the HMD.

It is mainly used for hand streams.

### 5.1 `enabled`

```json
"enabled": true
```

When enabled, the stream position is interpreted relative to the current HMD pose.

Recommended values:

```text
hmd:
  false

hmd_3dof:
  false

hand_tracking_21_joint:
  true

hand_skeleton26:
  true

world-space spatial mesh:
  false

camera-relative 3DoF spatial passthrough:
  handled through spatial_proxy_mesh.mesh_runtime.camera_relative_runtime
```

### 5.2 `offset_m`

Example for hands:

```json
"offset_m": {
  "x": 0.0,
  "y": -0.05,
  "z": -0.15
}
```

Meaning:

```text
x:
  left/right offset relative to the HMD

y:
  up/down offset relative to the HMD

z:
  forward/backward offset relative to the HMD
```

If hands are too close to the face:

```json
"z": -0.25
```

If hands are too far away:

```json
"z": -0.08
```

If hands are too high:

```json
"y": -0.10
```

If hands are too low:

```json
"y": 0.05
```

### 5.3 `rotate_with_hmd_orientation`

```json
"rotate_with_hmd_orientation": true
```

When enabled, the HMD-relative offset rotates with the HMD orientation.

For hands, this should usually be:

```json
"rotate_with_hmd_orientation": true
```

Otherwise, the hand offset may remain in world coordinates instead of following the head.

For world-space mesh, this should usually be:

```json
"rotate_with_hmd_orientation": false
```

---

## 6. `orientation_transform`

Example:

```json
"orientation_transform": {
  "enabled": true,
  "basis_rotation": {
    "rx_deg": -90.0,
    "ry_deg": 180.0,
    "rz_deg": 0.0
  }
}
```

This block transforms orientations, not positions.

It affects quaternions / rotations.

For `hmd` and `hmd_3dof`, the verified profile enables this block and applies:

```json
"basis_rotation": {
  "rx_deg": -90.0,
  "ry_deg": 180.0,
  "rz_deg": 0.0
}
```

Use this block when:

```text
- HMD looks in the wrong direction
- yaw/pitch/roll are mapped incorrectly
- 3DoF orientation is rotated by 90 or 180 degrees
- SteamVR sees the headset as tilted or facing backwards
```

Important rule:

```text
hmd and hmd_3dof should usually use compatible orientation_transform settings.
```

Otherwise switching between 6DoF and 3DoF may cause a sudden orientation jump.

---

## 7. `hand_orientation_offset`

Example:

```json
"hand_orientation_offset": {
  "enabled": true,
  "multiply_order": "post",
  "apply_to": {
    "controller": true,
    "palm": true,
    "wrist": true,
    "joints": false
  },
  "left": {
    "enabled": true,
    "rx_deg": 0.0,
    "ry_deg": 0.0,
    "rz_deg": 0.0
  },
  "right": {
    "enabled": true,
    "rx_deg": 0.0,
    "ry_deg": 0.0,
    "rz_deg": 0.0
  }
}
```

This block is for final hand/controller orientation tuning.

Use it when:

```text
- controller model is visually rotated
- palm orientation is wrong
- wrist roll is wrong
- left and right hands require different orientation corrections
```

Do not use this block to fix major coordinate-frame errors. Fix those first with:

```text
coordinate_transform
hmd_relative
orientation_transform
```

### 7.1 `multiply_order`

```json
"multiply_order": "post"
```

Usually keep:

```json
"multiply_order": "post"
```

Try `"pre"` only if the correction is being applied in the wrong local/global frame.

### 7.2 `apply_to`

```json
"apply_to": {
  "controller": true,
  "palm": true,
  "wrist": true,
  "joints": false
}
```

Recommended meaning:

```text
controller:
  Apply correction to controller pose/model.

palm:
  Apply correction to palm orientation.

wrist:
  Apply correction to wrist orientation.

joints:
  Usually keep false to avoid breaking skeleton joint rotations.
```

### 7.3 Left/right hand corrections

Example:

```json
"left": {
  "enabled": true,
  "rx_deg": 0.0,
  "ry_deg": 0.0,
  "rz_deg": 0.0
},
"right": {
  "enabled": true,
  "rx_deg": 0.0,
  "ry_deg": 0.0,
  "rz_deg": 15.0
}
```

Use separate values when only one controller/hand model is visually rotated.

---

## 8. Stream-specific notes

### 8.1 `hmd`

Purpose:

```text
6DoF HMD pose from Basalt/VIO -> runtime_local
```

Usually safe to adjust:

```text
streams.hmd.coordinate_transform.offset_m.y
```

This changes runtime HMD height.

Avoid changing `axis_map`, axis inversions, or `orientation_transform` if 6DoF already behaves correctly.

### 8.2 `hmd_3dof`

Purpose:

```text
3DoF IMU pose -> runtime_local
```

Keep this aligned with `hmd`.

If 6DoF is correct but 3DoF is wrong, adjust only:

```text
streams.hmd_3dof.orientation_transform
```

If both 6DoF and 3DoF are wrong in the same way, adjust both:

```text
streams.hmd.orientation_transform
streams.hmd_3dof.orientation_transform
```

### 8.3 `hand_tracking_21_joint`

Purpose:

```text
Mercury 21-joint hand tracking -> runtime hands/controllers
```

Important block:

```json
"hmd_relative": {
  "enabled": true,
  "offset_m": {
    "x": 0.0,
    "y": -0.05,
    "z": -0.15
  },
  "rotate_with_hmd_orientation": true
}
```

This means the hands are placed relative to the HMD, slightly lowered, and slightly moved forward.

Common tuning:

```text
Hands too close:
  decrease z, for example -0.25

Hands too far:
  increase z, for example -0.08

Hands too high:
  decrease y, for example -0.10

Hands too low:
  increase y, for example 0.05
```

### 8.4 `hand_skeleton26`

Purpose:

```text
26-joint hand skeleton -> runtime skeleton
```

Only tune this block if the runtime is actually using the 26-joint skeleton stream.

Check runtime logs for skeleton usage before changing it.

If the system primarily uses `hand_tracking_21_joint`, leave `hand_skeleton26` unchanged.

### 8.5 `body_trackers`

Purpose:

```text
Generic body tracker stream -> runtime body trackers
```

This block has no practical effect when body tracker input is disabled.

Tune it only when body tracker input is active.

### 8.6 `spatial_proxy_mesh`

Purpose:

```text
xr_spatial spatial_proxy_mesh -> runtime_spatial_proxy_mesh
```

For normal 6DoF/Basalt mode, `xr_spatial` should publish world-space mesh:

```bash
SPATIAL_POSE_INPUT=shm
```

For 3DoF/no-Basalt live passthrough, `xr_spatial` should publish camera-frame mesh:

```bash
SPATIAL_POSE_INPUT=none
```

The transform mode must match the `xr_spatial` pose mode.

---

## 9. `mesh_runtime`

The `spatial_proxy_mesh` stream has an additional block:

```json
"mesh_runtime": {
  "triangle_winding": "auto",
  "extra_rotation_deg": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.0
  },
  "extra_offset_m": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.0
  },
  "runtime_binding": {
    "enabled": true,
    "apply_origin_position": true,
    "apply_origin_orientation": true
  },
  "camera_relative_runtime": {
    "enabled": false,
    "require_hmd": true,
    "apply_hmd_position": true,
    "apply_hmd_orientation": true,
    "max_hmd_age_ms": 250.0,
    "offset_m": {
      "x": 0.0,
      "y": 0.0,
      "z": 0.0
    }
  }
}
```

### 9.1 `triangle_winding`

```json
"triangle_winding": "auto"
```

Controls triangle vertex order after coordinate transforms.

Allowed values:

```text
auto
  Let runtime adapter decide whether winding must be flipped.

keep
  Preserve original triangle winding.

swap
  Force triangle winding flip.
```

If the mesh exists but appears invisible or inside-out with backface culling, try:

```json
"triangle_winding": "swap"
```

If that makes it worse, use:

```json
"triangle_winding": "keep"
```

Default recommended value:

```json
"triangle_winding": "auto"
```

### 9.2 `extra_rotation_deg`

```json
"extra_rotation_deg": {
  "x": 0.0,
  "y": 0.0,
  "z": 0.0
}
```

Additional rotation for the spatial mesh only.

Use it when:

```text
HMD and hands are correct, but the spatial mesh alone is rotated incorrectly.
```

Example:

```json
"extra_rotation_deg": {
  "x": 0.0,
  "y": 180.0,
  "z": 0.0
}
```

### 9.3 `extra_offset_m`

```json
"extra_offset_m": {
  "x": 0.0,
  "y": 0.0,
  "z": 0.0
}
```

Additional offset for the spatial mesh only.

Use it when:

```text
HMD and hands are correct, but the spatial mesh alone is shifted.
```

### 9.4 `runtime_binding`

```json
"runtime_binding": {
  "enabled": true,
  "apply_origin_position": true,
  "apply_origin_orientation": true
}
```

Binds the spatial mesh to the runtime origin.

For normal 6DoF/Basalt mode, keep:

```json
"enabled": true
```

If the mesh moves incorrectly after recenter/origin reset, check:

```json
"apply_origin_position": true,
"apply_origin_orientation": true
```

### 9.5 `camera_relative_runtime`

```json
"camera_relative_runtime": {
  "enabled": false,
  "require_hmd": true,
  "apply_hmd_position": true,
  "apply_hmd_orientation": true,
  "max_hmd_age_ms": 250.0,
  "offset_m": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.0
  }
}
```

This mode places a camera-frame spatial mesh relative to the current HMD pose.

Use it for:

```text
3DoF live spatial passthrough
no-Basalt spatial preview
camera-relative mesh overlay
```

Enable it for 3DoF/no-Basalt passthrough:

```json
"camera_relative_runtime": {
  "enabled": true,
  "require_hmd": true,
  "apply_hmd_position": true,
  "apply_hmd_orientation": true,
  "max_hmd_age_ms": 250.0,
  "offset_m": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.0
  }
}
```

In that mode, start `xr_spatial` with:

```bash
SPATIAL_POSE_INPUT=none
```

For normal 6DoF/Basalt world-space mesh, keep:

```json
"enabled": false
```

and start `xr_spatial` with:

```bash
SPATIAL_POSE_INPUT=shm
```

Do not use camera-relative mode for scanner/object accumulation. Scanning requires real 6DoF translation.

---

## 10. `controller_override`

Example:

```json
"controller_override": {
  "mode": "hand_tracking_with_button_priority",
  "hand_gestures": {
    "left_enabled": true,
    "right_enabled": true
  },
  "runtime_controller_state": {
    "publish_shm": true,
    "registry": "/tmp/runtime_tracking_streams.json",
    "stream": "runtime_controller_state",
    "shm_name": "runtime_controller_state",
    "slots": 1024
  },
  "synthetic_hmd_relative_pose": {
    "left_offset_m": {
      "x": -0.22,
      "y": -0.22,
      "z": -0.35
    },
    "right_offset_m": {
      "x": 0.22,
      "y": -0.22,
      "z": -0.35
    }
  },
  "static_orientation": {
    "left_euler_deg": {
      "roll": 0.0,
      "pitch": 0.0,
      "yaw": 0.0
    },
    "right_euler_deg": {
      "roll": 0.0,
      "pitch": 0.0,
      "yaw": 0.0
    }
  }
}
```

### 10.1 `mode`

Current mode:

```json
"mode": "hand_tracking_with_button_priority"
```

Meaning:

```text
Hand tracking provides pose.
Physical controller input provides buttons.
Physical buttons have priority over derived hand gestures.
```

Common mode meanings:

```text
hand_tracking_with_button_priority
  Hand tracking pose + physical buttons priority.

hand_tracking_buttons_only
  Hand tracking pose + physical buttons only; gestures ignored.

hand_tracking_static_wrist
  Hand tracking pose + static wrist/controller orientation.

buttons_without_hand_tracking
  Synthetic HMD-relative controller pose + physical buttons.

pose_invalid
  Controller pose is invalid.
```

The exact values must match the modes supported by the current `xr_runtime_adapter`.

### 10.2 `hand_gestures`

```json
"hand_gestures": {
  "left_enabled": true,
  "right_enabled": true
}
```

Disable left-hand gestures:

```json
"hand_gestures": {
  "left_enabled": false,
  "right_enabled": true
}
```

Disable right-hand gestures:

```json
"hand_gestures": {
  "left_enabled": true,
  "right_enabled": false
}
```

Disable all gestures:

```json
"hand_gestures": {
  "left_enabled": false,
  "right_enabled": false
}
```

### 10.3 `runtime_controller_state`

```json
"runtime_controller_state": {
  "publish_shm": true,
  "registry": "/tmp/runtime_tracking_streams.json",
  "stream": "runtime_controller_state",
  "shm_name": "runtime_controller_state",
  "slots": 1024
}
```

This publishes final runtime controller state through SHM.

Normally keep these values unchanged. Changing `registry`, `stream`, or `shm_name` may break OpenVR/Monado/bridge consumers.

### 10.4 `synthetic_hmd_relative_pose`

```json
"synthetic_hmd_relative_pose": {
  "left_offset_m": {
    "x": -0.22,
    "y": -0.22,
    "z": -0.35
  },
  "right_offset_m": {
    "x": 0.22,
    "y": -0.22,
    "z": -0.35
  }
}
```

Used when the runtime needs to synthesize controller poses relative to the HMD.

Typical use cases:

```text
buttons_without_hand_tracking
hand tracking lost
static fallback pose
```

Meaning:

```text
x:
  left/right from HMD

y:
  up/down from HMD

z:
  forward/backward from HMD
```

Make synthetic controllers farther forward:

```json
"z": -0.45
```

Bring them closer:

```json
"z": -0.25
```

Raise them:

```json
"y": -0.10
```

Lower them:

```json
"y": -0.35
```

### 10.5 `static_orientation`

```json
"static_orientation": {
  "left_euler_deg": {
    "roll": 0.0,
    "pitch": 0.0,
    "yaw": 0.0
  },
  "right_euler_deg": {
    "roll": 0.0,
    "pitch": 0.0,
    "yaw": 0.0
  }
}
```

Used for static wrist/controller orientation.

If controller models point in the wrong direction in static mode, tune these values.

Example:

```json
"right_euler_deg": {
  "roll": 0.0,
  "pitch": -20.0,
  "yaw": 0.0
}
```

---

## 11. Common recipes

### 11.1 Change runtime user height

Edit both:

```text
streams.hmd.coordinate_transform.offset_m.y
streams.hmd_3dof.coordinate_transform.offset_m.y
```

Example:

```json
"offset_m": {
  "x": 0.0,
  "y": 1.7,
  "z": 0.0
}
```

### 11.2 6DoF is correct, but 3DoF points the wrong way

Edit:

```text
streams.hmd_3dof.orientation_transform.basis_rotation
```

Do not change `streams.hmd` if Basalt/6DoF is already correct.

### 11.3 6DoF and 3DoF are both wrong in the same way

Edit both:

```text
streams.hmd.orientation_transform.basis_rotation
streams.hmd_3dof.orientation_transform.basis_rotation
```

### 11.4 Hands are too close to the face

Edit:

```text
streams.hand_tracking_21_joint.hmd_relative.offset_m.z
```

Example:

```json
"z": -0.25
```

### 11.5 Hands are too high or too low

Edit:

```text
streams.hand_tracking_21_joint.hmd_relative.offset_m.y
```

Raise hands:

```json
"y": 0.05
```

Lower hands:

```json
"y": -0.12
```

### 11.6 Controller model is rotated incorrectly

Edit:

```text
streams.hand_tracking_21_joint.hand_orientation_offset.left
streams.hand_tracking_21_joint.hand_orientation_offset.right
```

Example:

```json
"right": {
  "enabled": true,
  "rx_deg": 0.0,
  "ry_deg": 0.0,
  "rz_deg": 15.0
}
```

### 11.7 Spatial mesh is inside-out or invisible with backface culling

Edit:

```text
streams.spatial_proxy_mesh.mesh_runtime.triangle_winding
```

Try:

```json
"triangle_winding": "swap"
```

or:

```json
"triangle_winding": "keep"
```

Default:

```json
"triangle_winding": "auto"
```

### 11.8 Spatial mesh is rotated, but HMD and hands are correct

Edit only:

```text
streams.spatial_proxy_mesh.mesh_runtime.extra_rotation_deg
```

Example:

```json
"extra_rotation_deg": {
  "x": 0.0,
  "y": 180.0,
  "z": 0.0
}
```

### 11.9 Spatial mesh is shifted, but HMD and hands are correct

Edit only:

```text
streams.spatial_proxy_mesh.mesh_runtime.extra_offset_m
```

Example:

```json
"extra_offset_m": {
  "x": 0.0,
  "y": 0.05,
  "z": 0.0
}
```

### 11.10 Enable 3DoF spatial passthrough

In:

```text
streams.spatial_proxy_mesh.mesh_runtime.camera_relative_runtime
```

set:

```json
"enabled": true
```

Recommended full block:

```json
"camera_relative_runtime": {
  "enabled": true,
  "require_hmd": true,
  "apply_hmd_position": true,
  "apply_hmd_orientation": true,
  "max_hmd_age_ms": 250.0,
  "offset_m": {
    "x": 0.0,
    "y": 0.0,
    "z": 0.0
  }
}
```

Start `xr_spatial` with:

```bash
SPATIAL_POSE_INPUT=none
```

For normal 6DoF world-space spatial mesh, set:

```json
"enabled": false
```

and start `xr_spatial` with:

```bash
SPATIAL_POSE_INPUT=shm
```

---

## 12. Validation

### 12.1 Validate JSON syntax

```bash
python3 -m json.tool xr_21_joint_hand_viewer_verified.json >/tmp/xr_transform_check.json
```

or:

```bash
jq . xr_21_joint_hand_viewer_verified.json >/tmp/xr_transform_check.json
```

### 12.2 Restart runtime adapter

After editing this file, restart `xr_runtime_adapter`.

Through `xr_client`, use:

```text
XR backend controls:
  restart running backends
```

or manually restart the runtime adapter service.

### 12.3 Check runtime adapter logs

Expected log lines:

```text
[xr_runtime_adapter] transform hmd axis_map=...
[xr_runtime_adapter] transform hmd_3dof axis_map=...
[xr_runtime_adapter] transform hand_tracking_21_joint axis_map=...
[xr_runtime_adapter] transform spatial_proxy_mesh axis_map=...
```

For spatial mesh, also check:

```text
spatial_proxy_mesh=yes
spatial_proxy_mesh_vertices=4800
spatial_proxy_mesh_triangles>0
runtime_spatial_proxy_mesh_published>0
```

If a stream is missing from logs, check:

```text
- the stream is enabled
- the input stream is actually publishing
- TRACKING_TRANSFORM_CONFIG points to the expected file
- xr_runtime_adapter was restarted after editing
```

### 12.4 Check which config is active in package mode

From package root:

```bash
cd ~/src/xr_tracking/out/xreal_ultra

source devices/xreal_ultra/xreal_ultra.env
echo "$TRACKING_TRANSFORM_CONFIG"
```

The path should point to the config under:

```text
out/xreal_ultra/devices/xreal_ultra/configs/...
```

---

## 13. Safe tuning rules

### 13.1 Change one thing at a time

Bad workflow:

```text
change axis_map
change invert_x
change offset
change orientation_transform
change mesh_runtime
restart once
```

Good workflow:

```text
change one field
restart runtime adapter
check viewer / SteamVR / logs
commit or backup
change the next field
```

### 13.2 Fix position before orientation

Recommended order:

```text
1. coordinate_transform.axis_map
2. coordinate_transform.invert_x/y/z
3. coordinate_transform.offset_m
4. orientation_transform
5. hand_orientation_offset
6. mesh_runtime.extra_*
```

### 13.3 Keep `hmd` and `hmd_3dof` consistent

If changing the global runtime convention, update both:

```text
streams.hmd
streams.hmd_3dof
```

Otherwise switching between 6DoF and 3DoF may cause jumps.

### 13.4 Do not tune unused streams

Before tuning `hand_skeleton26` or `body_trackers`, verify that they are actually used by the runtime.

### 13.5 Keep spatial modes separate

6DoF/Basalt world mesh:

```text
SPATIAL_POSE_INPUT=shm
camera_relative_runtime.enabled=false
```

3DoF/no-Basalt live passthrough:

```text
SPATIAL_POSE_INPUT=none
camera_relative_runtime.enabled=true
```

Scanner/object accumulation:

```text
SPATIAL_POSE_INPUT=shm
requires valid 6DoF pose
```

---

## 14. Troubleshooting

### HMD appears below the floor or too high

Adjust:

```text
streams.hmd.coordinate_transform.offset_m.y
streams.hmd_3dof.coordinate_transform.offset_m.y
```

### HMD faces backwards

Check:

```text
streams.hmd.orientation_transform.basis_rotation.ry_deg
streams.hmd_3dof.orientation_transform.basis_rotation.ry_deg
```

### Switching 3DoF/6DoF rotates the view

Compare:

```text
streams.hmd.orientation_transform
streams.hmd_3dof.orientation_transform
```

They should be compatible.

### Hands are glued to the face

Adjust:

```text
streams.hand_tracking_21_joint.hmd_relative.offset_m.z
```

### Hands do not follow head orientation

Check:

```json
"rotate_with_hmd_orientation": true
```

inside:

```text
streams.hand_tracking_21_joint.hmd_relative
streams.hand_skeleton26.hmd_relative
```

### Controller model is tilted

Adjust:

```text
streams.hand_tracking_21_joint.hand_orientation_offset
```

### Spatial mesh does not appear

First verify that the mesh stream exists.

Look for:

```text
spatial_proxy_mesh=yes
spatial_proxy_mesh_vertices=4800
spatial_proxy_mesh_triangles>0
runtime_spatial_proxy_mesh_published>0
```

If `spatial_proxy_mesh=no`, this is not a transform-config issue. Check `xr_spatial`, SHM registry, and stream names.

### Spatial mesh moves with the head but should be world-stabilized

Use 6DoF world mode:

```text
SPATIAL_POSE_INPUT=shm
camera_relative_runtime.enabled=false
```

### Spatial mesh should be 3DoF passthrough but does not rotate with the head

Check:

```json
"camera_relative_runtime": {
  "enabled": true,
  "apply_hmd_orientation": true
}
```

---

## 15. Recommended profiles

### 15.1 Normal 6DoF/Basalt profile

Use for:

```text
Basalt 6DoF
hand tracking
xr_spatial world mesh
scanner
SteamVR/OpenVR
Monado
```

Spatial mesh settings:

```json
"camera_relative_runtime": {
  "enabled": false
}
```

`xr_spatial`:

```bash
SPATIAL_POSE_INPUT=shm
```

### 15.2 3DoF passthrough profile

Use for:

```text
3DoF mode
live depth mesh/points overlay
no Basalt
debug passthrough
```

Spatial mesh settings:

```json
"camera_relative_runtime": {
  "enabled": true,
  "require_hmd": true,
  "apply_hmd_position": true,
  "apply_hmd_orientation": true
}
```

`xr_spatial`:

```bash
SPATIAL_POSE_INPUT=none
```

Do not use this mode for scanner/object accumulation.

### 15.3 Hand controller tuning profile

Use for controller placement and orientation tuning.

Main blocks:

```text
streams.hand_tracking_21_joint.hmd_relative.offset_m
streams.hand_tracking_21_joint.hand_orientation_offset
controller_override.synthetic_hmd_relative_pose
controller_override.static_orientation
```

---

## 16. Quick reference

### Runtime user height

```text
streams.hmd.coordinate_transform.offset_m.y
streams.hmd_3dof.coordinate_transform.offset_m.y
```

### HMD orientation

```text
streams.hmd.orientation_transform.basis_rotation
streams.hmd_3dof.orientation_transform.basis_rotation
```

### Hand position

```text
streams.hand_tracking_21_joint.hmd_relative.offset_m
```

### Hand/controller orientation

```text
streams.hand_tracking_21_joint.hand_orientation_offset
```

### Spatial mesh axis/direction

```text
streams.spatial_proxy_mesh.coordinate_transform.axis_map
streams.spatial_proxy_mesh.coordinate_transform.invert_x/y/z
```

### Spatial mesh backface/winding

```text
streams.spatial_proxy_mesh.mesh_runtime.triangle_winding
```

### 3DoF spatial passthrough

```text
streams.spatial_proxy_mesh.mesh_runtime.camera_relative_runtime.enabled=true
```

### 6DoF spatial world mesh

```text
streams.spatial_proxy_mesh.mesh_runtime.camera_relative_runtime.enabled=false
```

### Disable hand gestures

```text
controller_override.hand_gestures.left_enabled=false
controller_override.hand_gestures.right_enabled=false
```
