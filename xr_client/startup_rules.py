"""Configurable service startup checks shared by prestart options."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re
from typing import Any, Dict, Optional, Pattern


@dataclass(frozen=True)
class ServiceStartupRule:
    enabled: bool = True
    line_buffered_output: bool = False
    confirmation_regex: Optional[Pattern[str]] = None
    match_expected_value: bool = False


@dataclass
class ServiceStartupRules:
    enabled: bool = True
    services: Dict[str, ServiceStartupRule] = field(default_factory=dict)

    def get(self, service_name: str, selected_rule: Optional[str] = None) -> ServiceStartupRule:
        if not self.enabled:
            return ServiceStartupRule(enabled=False)
        key = selected_rule if selected_rule is not None else service_name
        if selected_rule is not None and key not in self.services:
            raise ValueError(f"unknown service startup rule: {key!r}")
        return self.services.get(key, ServiceStartupRule(enabled=False))


def _boolean(data: Dict[str, Any], key: str, default: bool, context: str) -> bool:
    value = data.get(key, default)
    if not isinstance(value, bool):
        raise ValueError(f"{context}.{key} must be a boolean")
    return value


def load_startup_rules(path: Optional[Path] = None, *, enabled: bool = True) -> ServiceStartupRules:
    if not enabled:
        return ServiceStartupRules(enabled=False)
    path = path if path is not None else Path(__file__).resolve().parent / "configs/service_startup_rules.yaml"
    try:
        import yaml
    except ImportError as exc:
        raise RuntimeError("Service startup rules require PyYAML; install it with python -m pip install PyYAML") from exc
    with path.open(encoding="utf-8-sig") as reader:
        data = yaml.safe_load(reader)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a YAML mapping")
    rules = ServiceStartupRules(enabled=_boolean(data, "enabled", True, str(path)))
    if not rules.enabled:
        return rules
    services = data.get("services", {})
    if not isinstance(services, dict):
        raise ValueError(f"{path}: services must be a mapping")
    for name, item in services.items():
        context = f"{path}: services.{name}"
        if not isinstance(name, str) or not name or not isinstance(item, dict):
            raise ValueError(f"{context} must be a named mapping")
        rule_enabled = _boolean(item, "enabled", True, context)
        if not rule_enabled:
            rules.services[name] = ServiceStartupRule(enabled=False)
            continue
        pattern = item.get("confirmation_regex", "")
        if not isinstance(pattern, str):
            raise ValueError(f"{context}.confirmation_regex must be a string")
        try:
            compiled = re.compile(pattern) if pattern else None
        except re.error as exc:
            raise ValueError(f"{context}.confirmation_regex: {exc}") from exc
        match_value = _boolean(item, "match_expected_value", False, context)
        if match_value and (compiled is None or compiled.groups < 1):
            raise ValueError(f"{context}: match_expected_value requires a capture group in confirmation_regex")
        rules.services[name] = ServiceStartupRule(
            line_buffered_output=_boolean(item, "line_buffered_output", False, context),
            confirmation_regex=compiled,
            match_expected_value=match_value,
        )
    return rules
