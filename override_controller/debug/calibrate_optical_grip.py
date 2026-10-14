#!/usr/bin/env python3
"""Guided two-hand optical grip calibration; Python 3 + NumPy/SciPy.

Fit O(t) = world_yaw * I(t) * mount, using camera-source-time IMU
interpolation. Existing provider offsets are removed before fitting a new
absolute local mount. Runtime/presentation yaw never enters the observations.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import sys
import time

try:
    import numpy as np
    from scipy.optimize import least_squares
    from scipy.spatial.transform import Rotation as R, Slerp
except ImportError:
    raise SystemExit("Запусти через bash calibrate_grip.sh: нужны NumPy и SciPy.")

FIELDS = re.compile(r"([A-Za-z0-9_]+)=(\([^)]*\)|[^\s]+)")
PHASES = [
    ("neutral", "Держи руку перед собой в обычном игровом хвате, спокойно."),
    ("pitch", "Медленно наклоняй кисть вперёд/назад на 30–45°, 3 раза. Верни в центр."),
    ("roll", "Медленно вращай кисть вокруг продольной оси на ±30–45°, 3 раза. Верни в центр."),
    ("yaw", "Медленно поверни кисть влево/вправо на ±40–60°, 3 раза. Верни в центр."),
]


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def normalized(value):
    q = np.asarray(value, dtype=float)
    if q.shape != (4,) or not np.isfinite(q).all() or np.linalg.norm(q) < 1e-6:
        raise ValueError("Некорректный quaternion")
    return q / np.linalg.norm(q)


def quaternion(text):
    values = text.strip("()").split(",")
    return normalized([float(v) for v in values])


def load_devices(config):
    selected = {}
    for side in ("left", "right"):
        found = [d for d in config.get("devices", []) if d.get("imu_side") == side]
        if len(found) != 1:
            raise ValueError(f"Нужен ровно один devices[] с imu_side={side}")
        device = found[0]
        if not isinstance(device.get("id"), int) or device["id"] <= 0:
            raise ValueError("Устройство должно иметь положительный device_id")
        transform = device.get("orientation_transform", {})
        basis = transform.get("basis_rotation", {})
        if not transform.get("enabled") or any(transform.get(k, False) for k in
                ("invert_x", "invert_y", "invert_z")) or any(
                abs(float(basis.get(k, 0)) - v) > 1e-6 for k, v in
                (("rx_deg", -90), ("ry_deg", 0), ("rz_deg", 0))):
            raise ValueError("Этот мастер рассчитан на исходные оси Gear VR: −90° X, без инверсий")
        offset = device.get("orientation_offset", {})
        if offset.get("enabled"):
            normalized(offset.get("quaternion_xyzw"))
            if offset.get("multiply_order", "post") not in ("post", "local", "pre", "world"):
                raise ValueError("Неизвестный порядок orientation_offset")
        selected[side] = {"id": device["id"], "imu_side": side,
                          "orientation_transform": transform, "orientation_offset": offset}
    if selected["left"]["id"] == selected["right"]["id"]:
        raise ValueError("device_id левой и правой руки должны различаться")
    return selected


def remove_offset(rotations, offset):
    if not offset.get("enabled"):
        return rotations
    inverse = R.from_quat(normalized(offset["quaternion_xyzw"])).inv()
    if offset.get("multiply_order", "post") in ("pre", "world"):
        return inverse * rotations
    return rotations * inverse


def heading(rotations):
    forward = rotations.apply([0, 0, -1])
    return np.arctan2(-forward[:, 0], -forward[:, 2]), np.hypot(forward[:, 0], forward[:, 2])


def wrapped(a):
    return np.arctan2(np.sin(a), np.cos(a))


@dataclass
class Pairs:
    imu: object
    optical: object
    phases: object
    frames: int
    head_motion_deg: float
    coverage_deg: list


def make_pairs(records, side, offset):
    prefix = "controller_" + side + "_"
    imu, optical, heads, epochs, resets = {}, {}, [], set(), set()
    for record in records:
        f = record["fields"]
        try:
            tick = int(f["controller_tick_ns"])
            it = int(f[prefix + "imu_orientation_ns"])
            if (f.get(prefix + "imu_active") != "yes" or
                    not (int(f.get(prefix + "imu_data_flags", "0")) & (1 << 3)) or
                    not (0 < it <= tick and tick - it < 150_000_000)):
                continue
            imu[it] = quaternion(f[prefix + "imu_q"])
            epochs.add(f.get(prefix + "imu_epoch", "unknown"))
            resets.add(f.get("hmd_reset_counter", "unknown"))
            ot = int(f["controller_optical_source_ns"])
            if f.get(prefix + "raw_optical_status") != "1" or f.get(prefix + "accepted_optical_status") != "1":
                continue
            if not (0 < ot <= tick and tick - ot <= 150_000_000):
                continue
            optical[ot] = (quaternion(f[prefix + "raw_optical_q"]), record["phase"])
            if "hmd_q" in f:
                heads.append(quaternion(f["hmd_q"]))
        except (KeyError, ValueError, TypeError):
            continue
    if len(epochs) != 1 or len(resets) != 1:
        raise ValueError("В этом круге перезапустился IMU/HMD: повтори круг")
    if len(imu) < 3 or len(optical) < 30:
        raise ValueError("Мало живых IMU/оптических кадров: проверь камеру и PRINT_EVERY=1")
    its = np.array(sorted(imu), dtype=np.int64)
    ots = np.array(sorted(optical), dtype=np.int64)
    # Exact matches are valid too; never interpolate over an IMU gap >80 ms.
    j = np.searchsorted(its, ots)
    exact = (j < len(its)) & (its[np.minimum(j, len(its) - 1)] == ots)
    bracket = (j > 0) & (j < len(its))
    eligible = np.flatnonzero(bracket)
    bracket[eligible] &= its[j[eligible]] - its[j[eligible] - 1] <= 80_000_000
    good = np.flatnonzero(exact | bracket)
    # Keep at most ~25 unique source poses/second; cached runtime ticks add no weight.
    retained = []
    for i in good:
        if not retained or ots[i] - ots[retained[-1]] >= 35_000_000:
            retained.append(i)
    ots = ots[retained]
    if len(ots) < 60:
        raise ValueError("Нужно не меньше 60 уникальных совпавших кадров; проверь задержки и видимость руки")
    origin = int(its[0])
    ir = Slerp((its - origin) / 1e9, R.from_quat([imu[t] for t in its]))((ots - origin) / 1e9)
    ir = remove_offset(ir, offset)
    op = R.from_quat([optical[t][0] for t in ots])
    phases = np.array([optical[t][1] for t in ots])
    for phase in ("pitch", "roll", "yaw"):
        if np.count_nonzero(phases == phase) < 15:
            raise ValueError(f"Мало пригодных кадров движения {phase}; повтори круг с рукой в кадре")
    relative = (ir[0].inv() * ir).as_rotvec()
    spectrum = np.linalg.eigvalsh(np.cov(relative.T))
    coverage = np.degrees(np.sqrt(np.maximum(spectrum, 0))).tolist()
    if coverage[-2] < 8:
        raise ValueError("Недостаточно разных осей: нужны наклоны и вращение кисти, одного yaw недостаточно")
    head_motion = 0.0
    if heads:
        hr = R.from_quat(heads)
        head_motion = float(np.percentile(np.degrees((hr[0].inv() * hr).magnitude()), 90))
        if head_motion > 8:
            raise ValueError(f"Голова двигалась слишком сильно ({head_motion:.1f}°); повтори круг")
    return Pairs(ir, op, phases, len(ots), head_motion, coverage)


def residual(mount, world_yaw, data):
    prediction = R.from_rotvec([0, world_yaw, 0]) * data.imu * mount
    error = (prediction * data.optical.inv()).as_rotvec().ravel()
    py, pn = heading(prediction)
    oy, on = heading(data.optical)
    yaw = wrapped(py - oy)
    yaw[(pn < .25) | (on < .25)] = 0
    return np.r_[error, yaw]


def fit_mount(sets):
    best = None
    for angle in (0, math.pi / 2, -math.pi / 2, math.pi):
        world_inv = R.from_rotvec([0, -angle, 0])
        mount = (sets[0].imu.inv() * world_inv * sets[0].optical).mean()
        initial = np.r_[mount.as_rotvec(), np.full(len(sets), angle)]
        def objective(p):
            m = R.from_rotvec(p[:3])
            return np.concatenate([residual(m, p[3 + i], data) for i, data in enumerate(sets)])
        fit = least_squares(objective, initial, loss="soft_l1", f_scale=.10, max_nfev=200)
        if best is None or fit.cost < best.cost:
            best = fit
    if not best.success or not np.isfinite(best.x).all():
        raise ValueError("Оптимизация не сошлась: повтори круг")
    singular = np.linalg.svd(best.jac, compute_uv=False)
    condition = float(singular[-1] / singular[0])
    if condition < .02:
        raise ValueError("Привязка не наблюдаема: добавь наклоны вперёд/назад и вращения кисти")
    return R.from_rotvec(best.x[:3]), best.x[3:], condition


def metrics(mount, world_yaw, data):
    prediction = R.from_rotvec([0, world_yaw, 0]) * data.imu * mount
    error = np.degrees((prediction * data.optical.inv()).magnitude())
    py, pn = heading(prediction)
    oy, on = heading(data.optical)
    yaw = np.abs(np.degrees(wrapped(py - oy)))[(pn >= .25) & (on >= .25)]
    if len(yaw) < 30:
        raise ValueError("Слишком мало кадров с наблюдаемым yaw; держи кисть ближе к горизонтали")
    return {"frames": data.frames, "yaw_frames": len(yaw),
            "orientation_median_deg": float(np.median(error)),
            "orientation_p90_deg": float(np.percentile(error, 90)),
            "yaw_median_deg": float(np.median(yaw)), "yaw_p90_deg": float(np.percentile(yaw, 90)),
            "head_motion_p90_deg": data.head_motion_deg, "motion_axis_std_deg": data.coverage_deg}


def quality(stat, args):
    return stat["orientation_median_deg"] <= args.max_orientation_median_deg and stat["yaw_p90_deg"] <= args.max_yaw_p90_deg


def assess(rounds, device, args):
    fits, details = [], []
    for number in sorted(rounds):
        if fits and number != fits[-1][0] + 1:
            fits.clear()
        try:
            data = make_pairs(rounds[number], device["imu_side"], device["orientation_offset"])
            mount, yaws, condition = fit_mount([data])
            stat = metrics(mount, yaws[0], data)
            details.append({"round": number, "metrics": stat, "jacobian_ratio": condition,
                            "quality_passed": quality(stat, args)})
            if quality(stat, args):
                fits.append((number, data, mount, yaws[0]))
            else:
                fits.clear()
        except ValueError as exc:
            details.append({"round": number, "rejected": str(exc)})
            fits.clear()
    result = {"side": device["imu_side"], "device_id": device["id"], "rounds": details, "stable": False}
    if len(fits) < args.min_rounds:
        return None, result
    window = fits[-args.min_rounds:]
    spread = max(float(np.degrees((a[2].inv() * b[2]).magnitude())) for a in window for b in window)
    result["mount_spread_deg"] = spread
    if spread > args.stable_deg:
        result["reason"] = "Привязка меняется между кругами: сохрани хват, повтори движения"
        return None, result
    # Fit on preceding rounds; the newest complete round verifies a fixed mount.
    mount, _, condition = fit_mount([x[1] for x in window[:-1]])
    last = window[-1]
    verification_yaw = least_squares(lambda p: residual(mount, p[0], last[1]),
        [last[3]], loss="soft_l1", f_scale=.10).x[0]
    check = metrics(mount, verification_yaw, last[1])
    result["verification_round"] = last[0]
    result["verification_metrics"] = check
    result["jacobian_ratio"] = condition
    if not quality(check, args):
        result["reason"] = "Калибровка не прошла проверку на последнем круге"
        return None, result
    q = mount.as_quat()
    if q[3] < 0:
        q *= -1
    result["stable"] = True
    result["quaternion_xyzw"] = q.tolist()
    result["session_world_yaw_persisted"] = False
    entry = {"device_id": device["id"], "side": device["imu_side"],
             "method": "guided_independent_optical_grip", "quaternion_xyzw": q.tolist(),
             "expected_orientation_offset": device["orientation_offset"]}
    return entry, result


class LiveLog:
    def __init__(self, path):
        self.path = path
        self.file = path.open("rb")
        self.identity = (path.stat().st_dev, path.stat().st_ino)
        startup = self.file.read(200_000).decode(errors="replace")
        if re.search(r"runtime_controller_(left|right)_imu_orientation_transform: enabled=true", startup):
            self.file.close()
            raise ValueError("В runtime включено дополнительное преобразование осей IMU. "
                             "Этот мастер требует штатных осей provider без дополнительного runtime transform.")
        self.file.seek(0, os.SEEK_END)
        self.partial = b""

    def close(self):
        self.file.close()

    def frames(self):
        stat = self.path.stat()
        if (stat.st_dev, stat.st_ino) != self.identity or stat.st_size < self.file.tell():
            raise ValueError("Лог сменился/обнулён: runtime перезапущен, начни мастер заново")
        data = self.partial + self.file.read()
        lines = data.split(b"\n")
        self.partial = lines.pop()
        frames = []
        for raw in lines:
            line = raw.decode(errors="replace")
            if line.startswith("[xr_runtime_adapter] frame="):
                frames.append(dict(FIELDS.findall(line)))
        return frames


def collect(reader, side, number, seconds, settle, sink):
    records = []
    prefix = "controller_" + side + "_"
    for phase, command in PHASES:
        print(f"\n{'Левая' if side == 'left' else 'Правая'} рука, круг {number}: {command}", flush=True)
        print(f"Начни через {settle:g} с. Затем выполняй {seconds:g} с; руку держи в кадре.", flush=True)
        start = time.monotonic()
        deadline = start + settle + seconds
        next_progress = start + settle
        while time.monotonic() < deadline:
            now = time.monotonic()
            for fields in reader.frames():
                if now < start + settle:
                    continue
                chosen = {k: v for k, v in fields.items() if k.startswith(prefix) or k in
                          ("controller_tick_ns", "controller_optical_source_ns", "hmd_q", "hmd_reset_counter")}
                record = {"kind": "frame", "side": side, "round": number, "phase": phase, "fields": chosen}
                records.append(record)
                sink.write(json.dumps(record, separators=(",", ":")) + "\n")
            if now >= next_progress:
                print(f"  {phase}: осталось {max(0, math.ceil(deadline - now))} с", flush=True)
                next_progress = now + 2
            time.sleep(.05)
        sink.flush()
    return records


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def print_result(result):
    last = result["rounds"][-1] if result["rounds"] else {}
    if "rejected" in last:
        print("Круг отклонён:", last["rejected"], flush=True)
    elif "metrics" in last:
        m = last["metrics"]
        print(f"Кадров: {m['frames']}; медиана ориентации {m['orientation_median_deg']:.1f}°; "
              f"yaw p90 {m['yaw_p90_deg']:.1f}°; пригоден: {last['quality_passed']}", flush=True)
    if "mount_spread_deg" in result:
        print(f"Разброс привязки между кругами: {result['mount_spread_deg']:.2f}°", flush=True)
    print("Стабильно." if result["stable"] else "Нужен следующий круг.", flush=True)


def run(args):
    args.out.mkdir(parents=True, exist_ok=False)
    grouped = {"left": {}, "right": {}}
    if args.replay:
        with args.replay.open() as f:
            header = json.loads(next(f))
            if header.get("schema") != "gearvr_guided_grip_recording_v1":
                raise ValueError("Неизвестный формат записи мастера")
            for line in f:
                record = json.loads(line)
                if record.get("kind") == "frame":
                    grouped[record["side"]].setdefault(record["round"], []).append(record)
        devices = load_devices({"devices": list(header["devices"].values())})
        reports, entries = [], []
        for side in ("left", "right"):
            entry, report = assess(grouped[side], devices[side], args)
            reports.append(report)
            if entry:
                entries.append(entry)
            print_result(report)
    else:
        raw = args.config.read_bytes()
        devices = load_devices(json.loads(raw))
        header = {"schema": "gearvr_guided_grip_recording_v1", "config_sha256": digest(raw),
                  "devices": devices, "created_at": datetime.now().astimezone().isoformat()}
        print("\nКалибровка обеих рук по живой оптике. Сядь, держи голову спокойно.")
        print("Сохраняй обычный игровой хват. Вторую руку убери из кадра, не перекрывай первую.")
        print("XR Gate должен быть запущен заново с этим gearvr.json, PRINT_EVERY=1 и диагностикой.")
        print("Старая локальная поправка будет снята из данных перед расчётом. Конфиг пока не меняется.")
        input("Нажми Enter, когда всё готово: ")
        reports, entries = [], []
        reader = LiveLog(args.log)
        try:
            with (args.out / "recording.jsonl").open("w") as sink:
                sink.write(json.dumps(header, ensure_ascii=False) + "\n")
                for side in ("right", "left"):
                    input(f"\nВозьми {'правый' if side == 'right' else 'левый'} контроллер обычным хватом. Enter: ")
                    report = {"side": side, "stable": False, "rounds": []}
                    for number in range(1, args.max_rounds + 1):
                        if digest(args.config.read_bytes()) != header["config_sha256"]:
                            raise ValueError("Конфиг изменился во время записи; начни заново")
                        grouped[side][number] = collect(reader, side, number, args.phase_seconds, args.settle_seconds, sink)
                        entry, report = assess(grouped[side], devices[side], args)
                        print_result(report)
                        write_json(args.out / f"{side}_progress.json", report)
                        if entry:
                            entries.append(entry)
                            break
                        if number < args.max_rounds:
                            input("Верни кисть в центр. Enter — повторить круг: ")
                    reports.append(report)
        finally:
            reader.close()
        if digest(args.config.read_bytes()) != header["config_sha256"]:
            raise ValueError("Конфиг изменился во время записи; результат не применяется")
    report = {"schema": "gearvr_guided_grip_report_v1", "hands": reports,
              "status": "stable_optical_reference" if len(entries) == 2 else "insufficient_or_unstable",
              "thresholds": {"min_rounds": args.min_rounds, "stable_deg": args.stable_deg,
                             "max_yaw_p90_deg": args.max_yaw_p90_deg,
                             "max_orientation_median_deg": args.max_orientation_median_deg},
              "limitations": ["Stability and residuals are relative to Mercury optical estimates, not physical ground truth.",
                              "The last round verifies a fixed mount but is also used for the stability stopping decision.",
                              "A changed grip or unreliable optical orientation may prevent calibration."]}
    write_json(args.out / "calibration_report.json", report)
    if len(entries) != 2:
        print("\nСтабильная калибровка обеих рук не получена. Конфиг не изменён; запись и отчёт сохранены.")
        return 2
    candidate = {"schema": "gearvr_optical_grip_trial_v1", "status": "stable_optical_reference",
                 "expected_config_sha256": header["config_sha256"], "devices": entries}
    write_json(args.out / "calibration_candidate.json", candidate)
    print(f"\nГотово: {args.out / 'calibration_candidate.json'}")
    print("Останови XR Gate через Ctrl+C. Затем примени созданный файл:")
    print(shlex.join(["python3", str(Path(__file__).resolve().parent / "apply_optical_grip_calibration.py"),
          "--config", str(args.config), "--candidate", str(args.out / "calibration_candidate.json"),
          "--runtime-root", str(args.runtime_root), "--replace-existing", "--write"]))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config", type=Path, default=Path("~/.config/xr_tracking/override_controller/gearvr.json"))
    p.add_argument("--runtime-root", type=Path, default=Path("~/xr-gate-release/xr-gate"))
    p.add_argument("--log", type=Path, default=Path(os.environ.get("LOG_DIR", "/tmp/xr_backend_client_logs")) / "xr_runtime_adapter.log")
    p.add_argument("--out", type=Path, default=Path("~/.local/share/xr_tracking/override_controller/optical_grip") / datetime.now().strftime("%Y%m%d_%H%M%S_%f"))
    p.add_argument("--replay", type=Path, help="Recalculate a recording.jsonl without live hardware")
    p.add_argument("--max-rounds", type=int, default=6)
    p.add_argument("--min-rounds", type=int, default=3)
    p.add_argument("--phase-seconds", type=float, default=8)
    p.add_argument("--settle-seconds", type=float, default=2)
    p.add_argument("--stable-deg", type=float, default=3)
    p.add_argument("--max-yaw-p90-deg", type=float, default=12)
    p.add_argument("--max-orientation-median-deg", type=float, default=25)
    a = p.parse_args(argv)
    for name in ("config", "runtime_root", "log", "out", "replay"):
        if getattr(a, name) is not None:
            setattr(a, name, getattr(a, name).expanduser().resolve())
    if a.min_rounds < 3 or a.max_rounds < a.min_rounds:
        p.error("Нужно минимум 3 круга; max-rounds должен быть не меньше min-rounds")
    for name in ("phase_seconds", "stable_deg", "max_yaw_p90_deg", "max_orientation_median_deg"):
        if not math.isfinite(getattr(a, name)) or getattr(a, name) <= 0:
            p.error(name + " должен быть положительным и конечным")
    if not math.isfinite(a.settle_seconds) or a.settle_seconds < 0:
        p.error("settle-seconds должен быть конечным и неотрицательным")
    try:
        return run(a)
    except (OSError, ValueError, KeyError, StopIteration, EOFError) as exc:
        print("Мастер остановлен:", exc, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nЗапись остановлена. Конфиг не изменён.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
