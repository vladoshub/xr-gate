#!/usr/bin/env python3
"""Render an OpenVR driver package's display settings.

The source settings and optional device overlay stay declarative. Environment
variables provide one-off build overrides without adding a new device profile.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
from typing import Any, Iterable


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
    # Projection half-angles must stay below 90 degrees so their tangents keep
    # the expected sign and remain finite.
    if degrees <= 0.0 or degrees >= 89.9:
        raise ValueError(f"{label} must be in (0, 89.9) degrees, got {degrees}")
    return math.tan(math.radians(degrees))


def apply_geometry_overrides(xr: dict[str, Any], steamvr: dict[str, Any]) -> None:
    eye_width = env_int("XR_OPENVR_EYE_WIDTH", minimum=1, maximum=32768)
    eye_height = env_int("XR_OPENVR_EYE_HEIGHT", minimum=1, maximum=32768)

    # The eye-size shortcuts configure the typical side-by-side display. More
    # specific render/window variables below always take precedence.
    if eye_width is not None:
        xr["renderWidth"] = eye_width
        xr["windowWidth"] = eye_width * 2
    if eye_height is not None:
        xr["renderHeight"] = eye_height
        xr["windowHeight"] = eye_height

    set_if_not_none(xr, "windowX", env_int("XR_OPENVR_WINDOW_X", "XR_STEAMVR_WINDOW_X"))
    set_if_not_none(xr, "windowY", env_int("XR_OPENVR_WINDOW_Y", "XR_STEAMVR_WINDOW_Y"))
    set_if_not_none(
        xr,
        "windowWidth",
        env_int("XR_OPENVR_WINDOW_WIDTH", "XR_STEAMVR_WINDOW_WIDTH", minimum=1, maximum=65536),
    )
    set_if_not_none(
        xr,
        "windowHeight",
        env_int("XR_OPENVR_WINDOW_HEIGHT", "XR_STEAMVR_WINDOW_HEIGHT", minimum=1, maximum=65536),
    )
    set_if_not_none(
        xr,
        "renderWidth",
        env_int("XR_OPENVR_RENDER_WIDTH", "XR_STEAMVR_RENDER_WIDTH", minimum=1, maximum=32768),
    )
    set_if_not_none(
        xr,
        "renderHeight",
        env_int("XR_OPENVR_RENDER_HEIGHT", "XR_STEAMVR_RENDER_HEIGHT", minimum=1, maximum=32768),
    )

    for key in ("windowX", "windowY", "windowWidth", "windowHeight", "renderWidth", "renderHeight"):
        if key in xr:
            steamvr[key] = xr[key]


def apply_identity_overrides(xr: dict[str, Any]) -> None:
    set_if_not_none(xr, "serialNumber", env_string("XR_OPENVR_SERIAL_NUMBER"))
    set_if_not_none(xr, "modelNumber", env_string("XR_OPENVR_MODEL_NUMBER"))
    set_if_not_none(xr, "ipdMeters", env_float("XR_OPENVR_IPD_M", minimum=0.001, maximum=1.0))
    set_if_not_none(
        xr,
        "secondsFromVsyncToPhotons",
        env_float("XR_OPENVR_SECONDS_FROM_VSYNC_TO_PHOTONS", minimum=0.0, maximum=1.0),
    )


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


def apply_projection_overrides(xr: dict[str, Any]) -> None:
    # Named profiles may express FOV in degrees instead of raw OpenVR tangents.
    # Symmetric keys are complete per-eye FOVs; directional keys are half-angles.
    projection: dict[str, float] = {}
    profile_horizontal = json_float(xr, "fovHorizontalDeg")
    profile_vertical = json_float(xr, "fovVerticalDeg")
    if profile_horizontal is not None:
        half = angle_tangent(profile_horizontal / 2.0, "fovHorizontalDeg/2")
        projection["projectionLeft"] = -half
        projection["projectionRight"] = half
    if profile_vertical is not None:
        half = angle_tangent(profile_vertical / 2.0, "fovVerticalDeg/2")
        projection["projectionTop"] = -half
        projection["projectionBottom"] = half

    profile_directional = {
        "projectionLeft": ("fovLeftDeg", -1.0),
        "projectionRight": ("fovRightDeg", 1.0),
        "projectionTop": ("fovUpDeg", -1.0),
        "projectionBottom": ("fovDownDeg", 1.0),
    }
    for key, (json_key, sign) in profile_directional.items():
        degrees = json_float(xr, json_key)
        if degrees is not None:
            projection[key] = sign * angle_tangent(degrees, json_key)

    # Environment values override profile degree keys.
    horizontal = env_float("XR_OPENVR_FOV_HORIZONTAL_DEG")
    vertical = env_float("XR_OPENVR_FOV_VERTICAL_DEG")
    if horizontal is not None:
        half = angle_tangent(horizontal / 2.0, "XR_OPENVR_FOV_HORIZONTAL_DEG/2")
        projection["projectionLeft"] = -half
        projection["projectionRight"] = half
    if vertical is not None:
        half = angle_tangent(vertical / 2.0, "XR_OPENVR_FOV_VERTICAL_DEG/2")
        projection["projectionTop"] = -half
        projection["projectionBottom"] = half

    directional = {
        "projectionLeft": ("XR_OPENVR_FOV_LEFT_DEG", -1.0),
        "projectionRight": ("XR_OPENVR_FOV_RIGHT_DEG", 1.0),
        "projectionTop": ("XR_OPENVR_FOV_UP_DEG", -1.0),
        "projectionBottom": ("XR_OPENVR_FOV_DOWN_DEG", 1.0),
    }
    for key, (env_name, sign) in directional.items():
        degrees = env_float(env_name)
        if degrees is not None:
            projection[key] = sign * angle_tangent(degrees, env_name)

    # Degree-based values intentionally override raw values from the base/overlay
    # only when a degree key was supplied. Raw environment tangents are the most
    # explicit form and always have highest precedence.
    xr.update(projection)
    raw_projection = {
        "projectionLeft": "XR_OPENVR_PROJECTION_LEFT",
        "projectionRight": "XR_OPENVR_PROJECTION_RIGHT",
        "projectionTop": "XR_OPENVR_PROJECTION_TOP",
        "projectionBottom": "XR_OPENVR_PROJECTION_BOTTOM",
    }
    for key, env_name in raw_projection.items():
        value = env_float(env_name, minimum=-1000.0, maximum=1000.0)
        if value is not None:
            xr[key] = value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--settings", required=True, type=Path, help="Base/package default.vrsettings to modify")
    parser.add_argument("--output", type=Path, help="Output file; defaults to --settings")
    parser.add_argument("--device-settings", type=Path, help="Optional named-device overlay")
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

    apply_identity_overrides(xr)
    apply_geometry_overrides(xr, steamvr)
    apply_projection_overrides(xr)

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

    projection = (
        xr.get("projectionLeft"),
        xr.get("projectionRight"),
        xr.get("projectionTop"),
        xr.get("projectionBottom"),
    )
    print(
        "[render_display_settings] "
        f"profile={args.device_profile} mode={args.display_mode} frequency={args.display_frequency:g} "
        f"window={xr.get('windowWidth')}x{xr.get('windowHeight')} "
        f"render={xr.get('renderWidth')}x{xr.get('renderHeight')} projection={projection}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"[render_display_settings][ERROR] {exc}", file=os.sys.stderr)
        raise SystemExit(2)
