#!/usr/bin/env python3
"""Load and validate XR Gate display/optics YAML without external dependencies.

This intentionally supports the small YAML subset used by display profiles:
nested mappings, numeric/string scalars, booleans/null, and inline lists.
"""

from __future__ import annotations

import argparse
import ast
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class ConfigError(ValueError):
    pass


def _strip_comment(line: str) -> str:
    quote: str | None = None
    escaped = False
    out: list[str] = []
    for ch in line:
        if escaped:
            out.append(ch)
            escaped = False
            continue
        if ch == "\\" and quote is not None:
            out.append(ch)
            escaped = True
            continue
        if ch in ("'", '"'):
            if quote is None:
                quote = ch
            elif quote == ch:
                quote = None
            out.append(ch)
            continue
        if ch == "#" and quote is None:
            break
        out.append(ch)
    return "".join(out).rstrip()


def _parse_scalar(raw: str, *, line_no: int) -> Any:
    text = raw.strip()
    if text == "":
        return {}
    lowered = text.lower()
    if lowered in ("true", "yes", "on"):
        return True
    if lowered in ("false", "no", "off"):
        return False
    if lowered in ("null", "none", "~"):
        return None
    if text.startswith(("[", "{", "'", '"')):
        try:
            return ast.literal_eval(text)
        except (SyntaxError, ValueError) as exc:
            raise ConfigError(f"line {line_no}: invalid scalar {text!r}: {exc}") from exc
    try:
        if any(ch in text for ch in ".eE"):
            return float(text)
        return int(text, 10)
    except ValueError:
        return text


def parse_simple_yaml(path: Path) -> dict[str, Any]:
    root: dict[str, Any] = {}
    stack: list[tuple[int, dict[str, Any]]] = [(-1, root)]
    for line_no, source_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = _strip_comment(source_line)
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(" "))
        if "\t" in source_line[: len(source_line) - len(source_line.lstrip())]:
            raise ConfigError(f"line {line_no}: tabs are not allowed for indentation")
        if indent % 2 != 0:
            raise ConfigError(f"line {line_no}: indentation must use multiples of two spaces")
        stripped = line.strip()
        if ":" not in stripped:
            raise ConfigError(f"line {line_no}: expected key: value")
        key, raw_value = stripped.split(":", 1)
        key = key.strip()
        if not _KEY_RE.fullmatch(key):
            raise ConfigError(f"line {line_no}: invalid key {key!r}")

        while stack and indent <= stack[-1][0]:
            stack.pop()
        if not stack:
            raise ConfigError(f"line {line_no}: invalid indentation")
        parent = stack[-1][1]
        if key in parent:
            raise ConfigError(f"line {line_no}: duplicate key {key!r}")
        value = _parse_scalar(raw_value, line_no=line_no)
        parent[key] = value
        if isinstance(value, dict):
            stack.append((indent, value))
    return root


def _mapping(obj: Any, path: str) -> dict[str, Any]:
    if not isinstance(obj, dict):
        raise ConfigError(f"{path} must be a mapping")
    return obj


def _number(obj: Any, path: str, *, minimum: float | None = None, maximum: float | None = None) -> float:
    if isinstance(obj, bool) or not isinstance(obj, (int, float)):
        raise ConfigError(f"{path} must be numeric")
    value = float(obj)
    if not math.isfinite(value):
        raise ConfigError(f"{path} must be finite")
    if minimum is not None and value < minimum:
        raise ConfigError(f"{path} must be >= {minimum}")
    if maximum is not None and value > maximum:
        raise ConfigError(f"{path} must be <= {maximum}")
    return value


def _integer(obj: Any, path: str, *, minimum: int = 0, maximum: int = 65536) -> int:
    value = _number(obj, path, minimum=minimum, maximum=maximum)
    rounded = round(value)
    if abs(value - rounded) > 1e-9:
        raise ConfigError(f"{path} must be an integer")
    return int(rounded)


def _string(obj: Any, path: str) -> str:
    if not isinstance(obj, str) or not obj.strip():
        raise ConfigError(f"{path} must be a non-empty string")
    return obj.strip()


def _vec2(obj: Any, path: str) -> list[float]:
    if not isinstance(obj, list) or len(obj) != 2:
        raise ConfigError(f"{path} must be a two-element inline list")
    return [
        _number(obj[0], f"{path}[0]", minimum=0.0, maximum=1.0),
        _number(obj[1], f"{path}[1]", minimum=0.0, maximum=1.0),
    ]


def _eye(obj: Any, path: str) -> dict[str, Any]:
    eye = _mapping(obj, path)
    center = _vec2(eye.get("lens_center_uv"), f"{path}.lens_center_uv")
    fov_raw = _mapping(eye.get("fov_deg"), f"{path}.fov_deg")
    fov = {
        name: _number(fov_raw.get(name), f"{path}.fov_deg.{name}", minimum=0.01, maximum=89.9)
        for name in ("left", "right", "up", "down")
    }
    return {"lens_center_uv": center, "fov_deg": fov}


def validate_config(raw: dict[str, Any]) -> dict[str, Any]:
    display_raw = _mapping(raw.get("display"), "display")
    optics_raw = _mapping(raw.get("optics"), "optics")

    layout = _string(display_raw.get("layout"), "display.layout").lower().replace("-", "_")
    aliases = {
        "sbs": "side_by_side_horizontal",
        "side_by_side": "side_by_side_horizontal",
        "horizontal_sbs": "side_by_side_horizontal",
        "top_bottom": "top_bottom_vertical",
        "vertical_sbs": "top_bottom_vertical",
    }
    layout = aliases.get(layout, layout)
    if layout not in ("side_by_side_horizontal", "top_bottom_vertical"):
        raise ConfigError("display.layout must be side_by_side_horizontal or top_bottom_vertical")

    rotation = _integer(display_raw.get("rotation_deg"), "display.rotation_deg", maximum=359)
    if rotation not in (0, 90, 180, 270):
        raise ConfigError("display.rotation_deg must be one of 0, 90, 180, 270")

    display = {
        "width_px": _integer(display_raw.get("width_px"), "display.width_px", minimum=1),
        "height_px": _integer(display_raw.get("height_px"), "display.height_px", minimum=1),
        "layout": layout,
        "eye_width_px": _integer(display_raw.get("eye_width_px"), "display.eye_width_px", minimum=1),
        "eye_height_px": _integer(display_raw.get("eye_height_px"), "display.eye_height_px", minimum=1),
        "refresh_hz": _number(display_raw.get("refresh_hz"), "display.refresh_hz", minimum=1.0, maximum=1000.0),
        "rotation_deg": rotation,
    }
    if layout == "side_by_side_horizontal":
        expected = display["eye_width_px"] * 2
        if display["width_px"] != expected or display["height_px"] != display["eye_height_px"]:
            raise ConfigError(
                "side_by_side_horizontal requires width_px == 2 * eye_width_px and height_px == eye_height_px"
            )
    else:
        expected = display["eye_height_px"] * 2
        if display["height_px"] != expected or display["width_px"] != display["eye_width_px"]:
            raise ConfigError(
                "top_bottom_vertical requires height_px == 2 * eye_height_px and width_px == eye_width_px"
            )

    optics = {
        "ipd_m": _number(optics_raw.get("ipd_m"), "optics.ipd_m", minimum=0.001, maximum=1.0),
        "inter_lens_distance_m": _number(
            optics_raw.get("inter_lens_distance_m"), "optics.inter_lens_distance_m", minimum=0.001, maximum=1.0
        ),
        "screen_to_lens_distance_m": _number(
            optics_raw.get("screen_to_lens_distance_m"), "optics.screen_to_lens_distance_m", minimum=0.0, maximum=10.0
        ),
        "eye_to_lens_distance_m": _number(
            optics_raw.get("eye_to_lens_distance_m"), "optics.eye_to_lens_distance_m", minimum=0.0, maximum=10.0
        ),
        "left_eye": _eye(optics_raw.get("left_eye"), "optics.left_eye"),
        "right_eye": _eye(optics_raw.get("right_eye"), "optics.right_eye"),
    }
    return {"display": display, "optics": optics}


def load_config(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise ConfigError(f"display config not found: {path}")
    return validate_config(parse_simple_yaml(path))


def get_path(data: dict[str, Any], dotted: str) -> Any:
    value: Any = data
    for part in dotted.split("."):
        if not isinstance(value, dict) or part not in value:
            raise ConfigError(f"unknown config path: {dotted}")
        value = value[part]
    return value


def monado_env(data: dict[str, Any]) -> dict[str, str]:
    display = data["display"]
    optics = data["optics"]
    left = optics["left_eye"]
    right = optics["right_eye"]
    result: dict[str, Any] = {
        "XR_MONADO_WINDOW_WIDTH": display["width_px"],
        "XR_MONADO_WINDOW_HEIGHT": display["height_px"],
        "XR_MONADO_EYE_WIDTH": display["eye_width_px"],
        "XR_MONADO_EYE_HEIGHT": display["eye_height_px"],
        "XR_MONADO_RENDER_WIDTH": display["eye_width_px"],
        "XR_MONADO_RENDER_HEIGHT": display["eye_height_px"],
        "XR_MONADO_REFRESH_HZ": display["refresh_hz"],
        "XR_MONADO_DISPLAY_LAYOUT": display["layout"],
        "XR_MONADO_DISPLAY_ROTATION_DEG": display["rotation_deg"],
        "XR_MONADO_IPD_M": optics["ipd_m"],
        "XR_MONADO_INTER_LENS_DISTANCE_M": optics["inter_lens_distance_m"],
        "XR_MONADO_SCREEN_TO_LENS_DISTANCE_M": optics["screen_to_lens_distance_m"],
        "XR_MONADO_EYE_TO_LENS_DISTANCE_M": optics["eye_to_lens_distance_m"],
        "XR_MONADO_LEFT_LENS_CENTER_U": left["lens_center_uv"][0],
        "XR_MONADO_LEFT_LENS_CENTER_V": left["lens_center_uv"][1],
        "XR_MONADO_RIGHT_LENS_CENTER_U": right["lens_center_uv"][0],
        "XR_MONADO_RIGHT_LENS_CENTER_V": right["lens_center_uv"][1],
    }
    for eye_name, eye in (("LEFT", left), ("RIGHT", right)):
        for direction in ("left", "right", "up", "down"):
            result[f"XR_MONADO_{eye_name}_EYE_FOV_{direction.upper()}_DEG"] = eye["fov_deg"][direction]
    return {key: repr(value) if isinstance(value, float) else str(value) for key, value in result.items()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--json", action="store_true", help="print normalized JSON")
    group.add_argument("--get", metavar="PATH", help="print one dotted value")
    group.add_argument("--monado-env", action="store_true", help="print KEY=VALUE lines for Monado")
    args = parser.parse_args()

    data = load_config(args.config)
    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
    elif args.get:
        value = get_path(data, args.get)
        if isinstance(value, (dict, list)):
            print(json.dumps(value, separators=(",", ":")))
        elif isinstance(value, bool):
            print("true" if value else "false")
        else:
            print(value)
    else:
        for key, value in monado_env(data).items():
            print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ConfigError) as exc:
        print(f"[display_optics_config][ERROR] {exc}", file=sys.stderr)
        raise SystemExit(2)
