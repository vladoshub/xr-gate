#!/usr/bin/env python3
"""
Post-process xr_mercury_dataset_probe results.csv with a conservative
last-good-pose hold + reacquire jump gate.

This is a dataset/debug implementation of the runtime controller stability
policy. It does not change Mercury internals; it filters whether a Mercury
active hand pose would be published as a stable controller pose.
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Optional, Tuple

Vec3 = Tuple[float, float, float]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Filter Mercury results.csv with reacquire/hold gate")
    p.add_argument("--input", required=True, type=Path, help="xr_mercury_dataset_probe results.csv")
    p.add_argument("--output", required=True, type=Path, help="filtered/gated results CSV")
    p.add_argument("--debug", required=True, type=Path, help="gate debug CSV")
    p.add_argument("--summary", type=Path, default=None, help="optional summary txt")
    p.add_argument("--max-jump-m", type=float, default=0.10, help="max immediate reacquire jump from last good pose")
    p.add_argument("--confirm-frames", type=int, default=3, help="far reacquire candidates must be stable this many active frames")
    p.add_argument("--confirm-max-step-m", type=float, default=0.04, help="max candidate-to-candidate step while confirming")
    p.add_argument("--hold-lost-ms", type=float, default=200.0, help="hold last good pose for this long after loss/rejected reacquire")
    p.add_argument("--hand-prefixes", default="left,right", help="comma-separated hand prefixes in CSV")
    return p.parse_args()


def f(row: Dict[str, str], name: str, default: float = 0.0) -> float:
    try:
        return float(row.get(name, default))
    except Exception:
        return default


def is_active(row: Dict[str, str], prefix: str) -> bool:
    return row.get(f"{prefix}_active", "0") == "1"


def xyz(row: Dict[str, str], prefix: str) -> Optional[Vec3]:
    """Return palm position if present and nonzero/finite, else wrist if present."""
    for joint in ("palm", "wrist"):
        keys = [f"{prefix}_{joint}_x", f"{prefix}_{joint}_y", f"{prefix}_{joint}_z"]
        if all(k in row for k in keys):
            v = (f(row, keys[0]), f(row, keys[1]), f(row, keys[2]))
            if all(math.isfinite(x) for x in v) and not (abs(v[0]) < 1e-12 and abs(v[1]) < 1e-12 and abs(v[2]) < 1e-12):
                return v
    return None


def dist(a: Optional[Vec3], b: Optional[Vec3]) -> Optional[float]:
    if a is None or b is None:
        return None
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def copy_pose(row: Dict[str, str], out: Dict[str, str], src_prefix: str, dst_prefix: str, pos: Optional[Vec3]) -> None:
    """Copy all known hand columns; optionally override palm xyz with held position."""
    for k, v in row.items():
        if k.startswith(src_prefix + "_"):
            out[dst_prefix + k[len(src_prefix):]] = v
    if pos is not None:
        for axis, val in zip("xyz", pos):
            key = f"{dst_prefix}_palm_{axis}"
            if key in out or f"{src_prefix}_palm_{axis}" in row:
                out[key] = f"{val:.9g}"


@dataclass
class HandState:
    last_good_pos: Optional[Vec3] = None
    last_good_ts_ns: Optional[int] = None
    pending_pos: Optional[Vec3] = None
    pending_count: int = 0


def gate_one(
    row: Dict[str, str],
    prefix: str,
    state: HandState,
    ts_ns: int,
    max_jump_m: float,
    confirm_frames: int,
    confirm_max_step_m: float,
    hold_lost_ms: float,
) -> Tuple[bool, str, Optional[float], int, Optional[Vec3], Optional[Vec3]]:
    """
    Return: gated_active, mode, jump_from_last_good, pending_count, output_pos, candidate_pos.

    Modes:
      accept_initial          no last good yet, active candidate accepted
      accept_continuity       active candidate close to last good, accepted
      accept_confirmed        far candidate accepted after stable confirmation
      reject_jump_hold        far active candidate rejected, last good held
      reject_jump_inactive    far active candidate rejected, no hold left
      hold_lost               original inactive, last good held
      inactive                original inactive, no hold left / no last good
      active_no_pose          original active but no usable pose columns
    """
    orig_active = is_active(row, prefix)
    cand = xyz(row, prefix)

    def hold_available() -> bool:
        if state.last_good_pos is None or state.last_good_ts_ns is None:
            return False
        age_ms = (ts_ns - state.last_good_ts_ns) / 1_000_000.0
        return age_ms >= 0.0 and age_ms <= hold_lost_ms

    if not orig_active:
        # Inactive Mercury estimates are diagnostic only; do not publish them.
        state.pending_count = 0
        state.pending_pos = None
        if hold_available():
            return True, "hold_lost", None, state.pending_count, state.last_good_pos, cand
        return False, "inactive", None, state.pending_count, None, cand

    if cand is None:
        if hold_available():
            return True, "active_no_pose_hold", None, state.pending_count, state.last_good_pos, cand
        return False, "active_no_pose", None, state.pending_count, None, cand

    if state.last_good_pos is None:
        state.last_good_pos = cand
        state.last_good_ts_ns = ts_ns
        state.pending_pos = None
        state.pending_count = 0
        return True, "accept_initial", None, 0, cand, cand

    jump = dist(state.last_good_pos, cand)
    assert jump is not None

    if jump <= max_jump_m:
        state.last_good_pos = cand
        state.last_good_ts_ns = ts_ns
        state.pending_pos = None
        state.pending_count = 0
        return True, "accept_continuity", jump, 0, cand, cand

    # Far reacquire candidate. Require multiple stable candidate frames before accepting.
    step_from_pending = dist(state.pending_pos, cand)
    if state.pending_pos is None or step_from_pending is None or step_from_pending > confirm_max_step_m:
        state.pending_pos = cand
        state.pending_count = 1
    else:
        state.pending_pos = cand
        state.pending_count += 1

    if state.pending_count >= max(1, confirm_frames):
        # Candidate is far from last good, but stable across confirm_frames. Accept as genuine reacquire.
        state.last_good_pos = cand
        state.last_good_ts_ns = ts_ns
        state.pending_pos = None
        accepted_count = state.pending_count
        state.pending_count = 0
        return True, "accept_confirmed", jump, accepted_count, cand, cand

    if hold_available():
        return True, "reject_jump_hold", jump, state.pending_count, state.last_good_pos, cand
    return False, "reject_jump_inactive", jump, state.pending_count, None, cand


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.debug.parent.mkdir(parents=True, exist_ok=True)
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)

    prefixes = [p.strip() for p in args.hand_prefixes.split(",") if p.strip()]
    states = {p: HandState() for p in prefixes}

    summary = {p: {"orig_active": 0, "gated_active": 0, "held": 0, "jump_rejected": 0, "confirmed": 0} for p in prefixes}
    rows = 0

    with args.input.open(newline="") as inf, args.output.open("w", newline="") as outf, args.debug.open("w", newline="") as dbg:
        reader = csv.DictReader(inf)
        if reader.fieldnames is None:
            raise RuntimeError("input CSV has no header")

        extra_fields = []
        for p in prefixes:
            extra_fields += [
                f"{p}_gated_active",
                f"{p}_gate_mode",
                f"{p}_gate_jump_m",
                f"{p}_gate_pending_count",
                f"{p}_gate_candidate_x",
                f"{p}_gate_candidate_y",
                f"{p}_gate_candidate_z",
                f"{p}_gate_output_x",
                f"{p}_gate_output_y",
                f"{p}_gate_output_z",
            ]

        writer = csv.DictWriter(outf, fieldnames=list(reader.fieldnames) + extra_fields)
        writer.writeheader()

        debug_fields = [
            "dump_index",
            "label",
            "pair_timestamp_ns",
            "hand",
            "orig_active",
            "gated_active",
            "mode",
            "jump_m",
            "pending_count",
            "candidate_x",
            "candidate_y",
            "candidate_z",
            "output_x",
            "output_y",
            "output_z",
        ]
        debug_writer = csv.DictWriter(dbg, fieldnames=debug_fields)
        debug_writer.writeheader()

        for row in reader:
            rows += 1
            outrow = dict(row)
            ts_ns = int(float(row.get("pair_timestamp_ns", row.get("out_timestamp_ns", "0"))))

            for p in prefixes:
                gated, mode, jump, pending_count, out_pos, cand = gate_one(
                    row,
                    p,
                    states[p],
                    ts_ns,
                    args.max_jump_m,
                    args.confirm_frames,
                    args.confirm_max_step_m,
                    args.hold_lost_ms,
                )

                if is_active(row, p):
                    summary[p]["orig_active"] += 1
                if gated:
                    summary[p]["gated_active"] += 1
                if "hold" in mode:
                    summary[p]["held"] += 1
                if mode.startswith("reject_jump"):
                    summary[p]["jump_rejected"] += 1
                if mode == "accept_confirmed":
                    summary[p]["confirmed"] += 1

                outrow[f"{p}_gated_active"] = "1" if gated else "0"
                outrow[f"{p}_gate_mode"] = mode
                outrow[f"{p}_gate_jump_m"] = "" if jump is None else f"{jump:.9g}"
                outrow[f"{p}_gate_pending_count"] = str(pending_count)

                for suffix, vec in (("candidate", cand), ("output", out_pos)):
                    if vec is None:
                        outrow[f"{p}_gate_{suffix}_x"] = ""
                        outrow[f"{p}_gate_{suffix}_y"] = ""
                        outrow[f"{p}_gate_{suffix}_z"] = ""
                    else:
                        outrow[f"{p}_gate_{suffix}_x"] = f"{vec[0]:.9g}"
                        outrow[f"{p}_gate_{suffix}_y"] = f"{vec[1]:.9g}"
                        outrow[f"{p}_gate_{suffix}_z"] = f"{vec[2]:.9g}"

                debug_writer.writerow({
                    "dump_index": row.get("dump_index", ""),
                    "label": row.get("label", ""),
                    "pair_timestamp_ns": row.get("pair_timestamp_ns", ""),
                    "hand": p,
                    "orig_active": "1" if is_active(row, p) else "0",
                    "gated_active": "1" if gated else "0",
                    "mode": mode,
                    "jump_m": "" if jump is None else f"{jump:.9g}",
                    "pending_count": pending_count,
                    "candidate_x": "" if cand is None else f"{cand[0]:.9g}",
                    "candidate_y": "" if cand is None else f"{cand[1]:.9g}",
                    "candidate_z": "" if cand is None else f"{cand[2]:.9g}",
                    "output_x": "" if out_pos is None else f"{out_pos[0]:.9g}",
                    "output_y": "" if out_pos is None else f"{out_pos[1]:.9g}",
                    "output_z": "" if out_pos is None else f"{out_pos[2]:.9g}",
                })

            writer.writerow(outrow)

    lines = [
        f"input={args.input}",
        f"output={args.output}",
        f"debug={args.debug}",
        f"rows={rows}",
        f"max_jump_m={args.max_jump_m}",
        f"confirm_frames={args.confirm_frames}",
        f"confirm_max_step_m={args.confirm_max_step_m}",
        f"hold_lost_ms={args.hold_lost_ms}",
    ]
    for p in prefixes:
        s = summary[p]
        lines.append(
            f"{p}: orig_active={s['orig_active']} gated_active={s['gated_active']} "
            f"held={s['held']} jump_rejected={s['jump_rejected']} confirmed={s['confirmed']}"
        )

    text = "\n".join(lines) + "\n"
    if args.summary:
        args.summary.write_text(text)
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
