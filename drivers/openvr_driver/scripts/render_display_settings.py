#!/usr/bin/env python3
"""Render an OpenVR driver package's display settings.

The base vrsettings and optional device overlay remain declarative. A shared
``display``/``optics`` YAML profile supplies geometry and optical parameters;
environment variables remain the highest-priority one-off overrides.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any, Iterable

from display_optics_config import ConfigError, load_config


def deep_merge(dst: dict[str, Any], src: dict[str, Any]) -> dict[str, Any]:
    for key, value in src.items():
        if isinstance(value, dict) and isinstance(dst.get(key), dict):
            deep_merge(dst[key], value)
        else:
            dst[key] = value
    return dst


def env_first(names: Iterable[str]) -> str | None:
    for name in names:
        value = os.environ.get(name)
        if value is not None and value.strip() != "":
            return value.strip()
    return None


def env_int(*names: str, minimum: int | None = None, maximum: int | None = None) -> int | None:
    raw = env_first(names)
    if raw is None:
        return None
    try:
        value_f = float(raw)
    except ValueError as exc:
        raise ValueError(f"{names[0]} must be numeric, got {raw!r}") from exc
    if not math.isfinite(value_f) or abs(value_f - round(value_f)) > 1e-6:
        raise ValueError(f"{names[0]} must be an integer, got {raw!r}")
    value = int(round(value_f))
    if minimum is not None and value < minimum:
        raise ValueError(f"{names[0]} must be >= {minimum}, got {value}")
    if maximum is not None and value > maximum:
        raise ValueError(f"{names[0]} must be <= {maximum}, got {value}")
    return value


def env_float(*names: str, minimum: float | None = None, maximum: float | None = None) -> float | None:
    raw = env_first(names)
    if raw is None:
        return None
    try:
        value = float(raw)
    except ValueError as exc:
        raise ValueError(f"{names[0]} must be numeric, got {raw!r}") from exc
    if not math.isfinite(value):
        raise ValueError(f"{names[0]} must be finite, got {raw!r}")
    if minimum is not None and value < minimum:
        raise ValueError(f"{names[0]} must be >= {minimum}, got {value}")
    if maximum is not None and value > maximum:
        raise ValueError(f"{names[0]} must be <= {maximum}, got {value}")
    return value


def env_string(*names: str) -> str | None:
    return env_first(names)


def set_if_not_none(obj: dict[str, Any], key: str, value: Any | None) -> None:
    if value is not None:
        obj[key] = value


def angle_tangent(degrees: float, label: str) -> float:
    if degrees <= 0.0 or degrees >= 89.9:
        raise ValueError(f"{label} must be in (0, 89.9) degrees, got {degrees}")
    value = math.tan(math.radians(degrees))
    # Preserve the historical OpenVR default exactly: 45 degrees was stored as
    # a raw projection tangent of 1.0 rather than 0.9999999999999999.
    if abs(value - 1.0) <= 1e-12:
        return 1.0
    return value


def json_float(obj: dict[str, Any], key: str) -> float | None:
    if key not in obj:
        return None
    raw = obj[key]
    if isinstance(raw, bool):
        raise ValueError(f"{key} must be numeric, got boolean")
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{key} must be numeric, got {raw!r}") from exc
    if not math.isfinite(value):
        raise ValueError(f"{key} must be finite, got {raw!r}")
    return value


EYE_PREFIXES = ("left", "right")
DIRECTIONS = ("Left", "Right", "Top", "Bottom")


def projection_from_fov(eye: dict[str, Any], label: str) -> dict[str, float]:
    fov = eye["fov_deg"]
    return {
        "Left": -angle_tangent(float(fov["left"]), f"{label}.fov_deg.left"),
        "Right": angle_tangent(float(fov["right"]), f"{label}.fov_deg.right"),
        "Top": -angle_tangent(float(fov["up"]), f"{label}.fov_deg.up"),
        "Bottom": angle_tangent(float(fov["down"]), f"{label}.fov_deg.down"),
    }


def get_projection(xr: dict[str, Any], eye: str) -> dict[str, float]:
    legacy = {
        "Left": float(xr.get("projectionLeft", -1.0)),
        "Right": float(xr.get("projectionRight", 1.0)),
        "Top": float(xr.get("projectionTop", -1.0)),
        "Bottom": float(xr.get("projectionBottom", 1.0)),
    }
    title = eye.capitalize()
    return {
        direction: float(xr.get(f"{eye}Projection{direction}", legacy[direction]))
        for direction in DIRECTIONS
    }


def set_projection(xr: dict[str, Any], eye: str, projection: dict[str, float]) -> None:
    for direction in DIRECTIONS:
        xr[f"{eye}Projection{direction}"] = projection[direction]


def apply_display_config(xr: dict[str, Any], steamvr: dict[str, Any], config: dict[str, Any]) -> None:
    display = config["display"]
    optics = config["optics"]

    xr["windowWidth"] = display["width_px"]
    xr["windowHeight"] = display["height_px"]
    xr["renderWidth"] = display["eye_width_px"]
    xr["renderHeight"] = display["eye_height_px"]
    xr["displayLayout"] = display["layout"]
    xr["displayRotationDeg"] = display["rotation_deg"]
    xr["displayFrequency"] = display["refresh_hz"]

    xr["ipdMeters"] = optics["ipd_m"]
    xr["interLensDistanceMeters"] = optics["inter_lens_distance_m"]
    xr["screenToLensDistanceMeters"] = optics["screen_to_lens_distance_m"]
    xr["eyeToLensDistanceMeters"] = optics["eye_to_lens_distance_m"]
    for eye in EYE_PREFIXES:
        eye_cfg = optics[f"{eye}_eye"]
        xr[f"{eye}LensCenterU"] = eye_cfg["lens_center_uv"][0]
        xr[f"{eye}LensCenterV"] = eye_cfg["lens_center_uv"][1]
        set_projection(xr, eye, projection_from_fov(eye_cfg, f"optics.{eye}_eye"))

    # Keep legacy projection keys for old driver binaries and diagnostics. The
    # updated driver reads the per-eye keys above.
    left_projection = get_projection(xr, "left")
    xr["projectionLeft"] = left_projection["Left"]
    xr["projectionRight"] = left_projection["Right"]
    xr["projectionTop"] = left_projection["Top"]
    xr["projectionBottom"] = left_projection["Bottom"]

    for key in ("windowX", "windowY", "windowWidth", "windowHeight", "renderWidth", "renderHeight"):
        if key in xr:
            steamvr[key] = xr[key]
    steamvr["displayFrequency"] = display["refresh_hz"]


def apply_geometry_overrides(xr: dict[str, Any], steamvr: dict[str, Any]) -> None:
    eye_width = env_int("XR_OPENVR_EYE_WIDTH", minimum=1, maximum=32768)
    eye_height = env_int("XR_OPENVR_EYE_HEIGHT", minimum=1, maximum=32768)

    if eye_width is not None:
        xr["renderWidth"] = eye_width
        if xr.get("displayLayout", "side_by_side_horizontal") == "top_bottom_vertical":
            xr["windowWidth"] = eye_width
        else:
            xr["windowWidth"] = eye_width * 2
    if eye_height is not None:
        xr["renderHeight"] = eye_height
        if xr.get("displayLayout", "side_by_side_horizontal") == "top_bottom_vertical":
            xr["windowHeight"] = eye_height * 2
        else:
            xr["windowHeight"] = eye_height

    set_if_not_none(xr, "windowX", env_int("XR_OPENVR_WINDOW_X", "XR_STEAMVR_WINDOW_X"))
    set_if_not_none(xr, "windowY", env_int("XR_OPENVR_WINDOW_Y", "XR_STEAMVR_WINDOW_Y"))
    set_if_not_none(xr, "windowWidth", env_int("XR_OPENVR_WINDOW_WIDTH", "XR_STEAMVR_WINDOW_WIDTH", minimum=1, maximum=65536))
    set_if_not_none(xr, "windowHeight", env_int("XR_OPENVR_WINDOW_HEIGHT", "XR_STEAMVR_WINDOW_HEIGHT", minimum=1, maximum=65536))
    set_if_not_none(xr, "renderWidth", env_int("XR_OPENVR_RENDER_WIDTH", "XR_STEAMVR_RENDER_WIDTH", minimum=1, maximum=32768))
    set_if_not_none(xr, "renderHeight", env_int("XR_OPENVR_RENDER_HEIGHT", "XR_STEAMVR_RENDER_HEIGHT", minimum=1, maximum=32768))
    set_if_not_none(xr, "displayRotationDeg", env_int("XR_OPENVR_DISPLAY_ROTATION_DEG", minimum=0, maximum=270))
    layout = env_string("XR_OPENVR_DISPLAY_LAYOUT")
    if layout is not None:
        normalized = layout.lower().replace("-", "_")
        if normalized not in ("side_by_side_horizontal", "top_bottom_vertical"):
            raise ValueError(f"XR_OPENVR_DISPLAY_LAYOUT unsupported: {layout}")
        xr["displayLayout"] = normalized

    for key in ("windowX", "windowY", "windowWidth", "windowHeight", "renderWidth", "renderHeight"):
        if key in xr:
            steamvr[key] = xr[key]


def apply_identity_overrides(xr: dict[str, Any]) -> None:
    set_if_not_none(xr, "serialNumber", env_string("XR_OPENVR_SERIAL_NUMBER"))
    set_if_not_none(xr, "modelNumber", env_string("XR_OPENVR_MODEL_NUMBER"))
    set_if_not_none(xr, "ipdMeters", env_float("XR_OPENVR_IPD_M", minimum=0.001, maximum=1.0))
    set_if_not_none(xr, "interLensDistanceMeters", env_float("XR_OPENVR_INTER_LENS_DISTANCE_M", minimum=0.001, maximum=1.0))
    set_if_not_none(xr, "screenToLensDistanceMeters", env_float("XR_OPENVR_SCREEN_TO_LENS_DISTANCE_M", minimum=0.0, maximum=10.0))
    set_if_not_none(xr, "eyeToLensDistanceMeters", env_float("XR_OPENVR_EYE_TO_LENS_DISTANCE_M", minimum=0.0, maximum=10.0))
    set_if_not_none(xr, "leftLensCenterU", env_float("XR_OPENVR_LEFT_LENS_CENTER_U", minimum=0.0, maximum=1.0))
    set_if_not_none(xr, "leftLensCenterV", env_float("XR_OPENVR_LEFT_LENS_CENTER_V", minimum=0.0, maximum=1.0))
    set_if_not_none(xr, "rightLensCenterU", env_float("XR_OPENVR_RIGHT_LENS_CENTER_U", minimum=0.0, maximum=1.0))
    set_if_not_none(xr, "rightLensCenterV", env_float("XR_OPENVR_RIGHT_LENS_CENTER_V", minimum=0.0, maximum=1.0))
    set_if_not_none(xr, "secondsFromVsyncToPhotons", env_float("XR_OPENVR_SECONDS_FROM_VSYNC_TO_PHOTONS", minimum=0.0, maximum=1.0))


def apply_profile_projection_aliases(xr: dict[str, Any]) -> None:
    # Legacy device overlays may still use one symmetric FOV for both eyes.
    projection: dict[str, float] = {}
    horizontal = json_float(xr, "fovHorizontalDeg")
    vertical = json_float(xr, "fovVerticalDeg")
    if horizontal is not None:
        half = angle_tangent(horizontal / 2.0, "fovHorizontalDeg/2")
        projection.update(Left=-half, Right=half)
    if vertical is not None:
        half = angle_tangent(vertical / 2.0, "fovVerticalDeg/2")
        projection.update(Top=-half, Bottom=half)
    aliases = {
        "Left": ("fovLeftDeg", -1.0),
        "Right": ("fovRightDeg", 1.0),
        "Top": ("fovUpDeg", -1.0),
        "Bottom": ("fovDownDeg", 1.0),
    }
    for direction, (key, sign) in aliases.items():
        value = json_float(xr, key)
        if value is not None:
            projection[direction] = sign * angle_tangent(value, key)
    if projection:
        for eye in EYE_PREFIXES:
            current = get_projection(xr, eye)
            current.update(projection)
            set_projection(xr, eye, current)


def apply_projection_overrides(xr: dict[str, Any]) -> None:
    apply_profile_projection_aliases(xr)

    shared: dict[str, float] = {}
    horizontal = env_float("XR_OPENVR_FOV_HORIZONTAL_DEG")
    vertical = env_float("XR_OPENVR_FOV_VERTICAL_DEG")
    if horizontal is not None:
        half = angle_tangent(horizontal / 2.0, "XR_OPENVR_FOV_HORIZONTAL_DEG/2")
        shared.update(Left=-half, Right=half)
    if vertical is not None:
        half = angle_tangent(vertical / 2.0, "XR_OPENVR_FOV_VERTICAL_DEG/2")
        shared.update(Top=-half, Bottom=half)
    directional = {
        "Left": ("XR_OPENVR_FOV_LEFT_DEG", -1.0),
        "Right": ("XR_OPENVR_FOV_RIGHT_DEG", 1.0),
        "Top": ("XR_OPENVR_FOV_UP_DEG", -1.0),
        "Bottom": ("XR_OPENVR_FOV_DOWN_DEG", 1.0),
    }
    for direction, (env_name, sign) in directional.items():
        degrees = env_float(env_name)
        if degrees is not None:
            shared[direction] = sign * angle_tangent(degrees, env_name)

    for eye in EYE_PREFIXES:
        current = get_projection(xr, eye)
        current.update(shared)
        eye_upper = eye.upper()
        for direction, suffix in (("Left", "LEFT"), ("Right", "RIGHT"), ("Top", "UP"), ("Bottom", "DOWN")):
            env_name = f"XR_OPENVR_{eye_upper}_EYE_FOV_{suffix}_DEG"
            degrees = env_float(env_name)
            if degrees is not None:
                sign = -1.0 if direction in ("Left", "Top") else 1.0
                current[direction] = sign * angle_tangent(degrees, env_name)
        set_projection(xr, eye, current)

    # Raw shared tangents remain the highest-priority backward-compatible form.
    raw_shared = {
        "Left": "XR_OPENVR_PROJECTION_LEFT",
        "Right": "XR_OPENVR_PROJECTION_RIGHT",
        "Top": "XR_OPENVR_PROJECTION_TOP",
        "Bottom": "XR_OPENVR_PROJECTION_BOTTOM",
    }
    for eye in EYE_PREFIXES:
        current = get_projection(xr, eye)
        for direction, env_name in raw_shared.items():
            value = env_float(env_name, minimum=-1000.0, maximum=1000.0)
            if value is not None:
                current[direction] = value
        eye_upper = eye.upper()
        for direction in DIRECTIONS:
            env_name = f"XR_OPENVR_{eye_upper}_PROJECTION_{direction.upper()}"
            value = env_float(env_name, minimum=-1000.0, maximum=1000.0)
            if value is not None:
                current[direction] = value
        set_projection(xr, eye, current)

    left = get_projection(xr, "left")
    xr["projectionLeft"] = left["Left"]
    xr["projectionRight"] = left["Right"]
    xr["projectionTop"] = left["Top"]
    xr["projectionBottom"] = left["Bottom"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--settings", required=True, type=Path, help="Base/package default.vrsettings to modify")
    parser.add_argument("--output", type=Path, help="Output file; defaults to --settings")
    parser.add_argument("--device-settings", type=Path, help="Optional named-device overlay")
    parser.add_argument("--display-config", type=Path, help="display/optics YAML profile")
    parser.add_argument("--device-profile", default="generic")
    parser.add_argument("--display-frequency", required=True, type=float)
    parser.add_argument("--display-mode", required=True, choices=("direct", "extended_sbs"))
    args = parser.parse_args()

    if not math.isfinite(args.display_frequency) or not (1.0 <= args.display_frequency <= 1000.0):
        parser.error("--display-frequency must be finite and in range 1..1000")

    data = json.loads(args.settings.read_text())
    if args.device_settings is not None:
        overlay = json.loads(args.device_settings.read_text())
        deep_merge(data, overlay)

    xr = data.setdefault("xr_tracking", {})
    steamvr = data.setdefault("steamvr", {})
    xr["deviceProfile"] = args.device_profile

    if args.display_config is not None:
        apply_display_config(xr, steamvr, load_config(args.display_config))
        xr["displayConfig"] = "resources/settings/display_config.yaml"

    apply_identity_overrides(xr)
    apply_geometry_overrides(xr, steamvr)
    apply_projection_overrides(xr)

    # Explicit build-script frequency always wins over the config. The scripts
    # use config.display.refresh_hz only when no frequency override was supplied.
    xr["displayFrequency"] = args.display_frequency
    steamvr["displayFrequency"] = args.display_frequency

    if args.display_mode == "extended_sbs":
        xr["isDisplayOnDesktop"] = True
        xr["isDisplayRealDisplay"] = True
        xr["displayDebugMode"] = False
        steamvr["directMode"] = False
        steamvr["displayDebugMode"] = False
        steamvr["debugMode"] = False
        steamvr["DebugMode"] = False
    else:
        xr["isDisplayOnDesktop"] = False
        xr["isDisplayRealDisplay"] = True
        xr["displayDebugMode"] = False
        steamvr["directMode"] = True

    output = args.output or args.settings
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, indent=2) + "\n")

    print(
        "[render_display_settings] "
        f"profile={args.device_profile} mode={args.display_mode} frequency={args.display_frequency:g} "
        f"config={args.display_config or '<none>'} layout={xr.get('displayLayout')} "
        f"rotation={xr.get('displayRotationDeg')} window={xr.get('windowWidth')}x{xr.get('windowHeight')} "
        f"render={xr.get('renderWidth')}x{xr.get('renderHeight')} "
        f"left_projection={get_projection(xr, 'left')} right_projection={get_projection(xr, 'right')}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, ConfigError, json.JSONDecodeError) as exc:
        print(f"[render_display_settings][ERROR] {exc}", file=sys.stderr)
        raise SystemExit(2)
