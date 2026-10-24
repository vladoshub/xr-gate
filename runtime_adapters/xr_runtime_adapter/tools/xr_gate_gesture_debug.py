#!/usr/bin/env python3
"""Timed XR Gate hand/controller diagnostic recording; Python standard library only."""

import argparse
from datetime import datetime
import io
import json
import math
from pathlib import Path
import select
import shutil
import socket
import sys
import time
import zipfile


NAMES = {"left": "ЛЕВАЯ", "right": "ПРАВАЯ", "both": "ОБЕ РУКИ"}
SCHEMA = "xr_gate_gesture_debug_v1"


def make_followup_plan(hand="both"):
    """Recalibrate only the right wrist; inspect Trigger and V on selected hands."""
    plan = []

    def add(side, label, instruction, variant=""):
        plan.append(dict(hand=side, label=label, instruction=instruction, variant=variant))

    if hand in ("right", "both"):
        for label, instruction in [
            ("thumb_rest", "Сожми обычный кулак, большой снаружи. Кисть ПРЯМО, как держишь в игре."),
            ("open", "Выпрями четыре пальца, большой прижми к ладони. Кисть держи прямо."),
            ("closed", "Сожми обычный кулак. Не соединяй кончики большого и указательного."),
            ("neutral", "Большой отогнут, четыре пальца согнуты. Кисть ПРЯМО в удобной игровой НЕЙТРАЛИ."),
            ("up", "Из этой нейтрали наклони КИСТЬ ВПЕРЁД, от себя. Большой отогнут, четыре согнуты."),
            ("neutral", "Верни кисть ТОЧНО в игровую НЕЙТРАЛЬ. Большой отогнут, четыре согнуты."),
            ("down", "Из нейтрали наклони КИСТЬ НАЗАД, к себе. Большой отогнут, четыре согнуты."),
            ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, четыре согнуты."),
            ("left", "Из нейтрали наклони КИСТЬ ВЛЕВО. Большой отогнут, четыре согнуты."),
            ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, четыре согнуты."),
            ("right", "Из нейтрали наклони КИСТЬ ВПРАВО. Большой отогнут, четыре согнуты."),
            ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, четыре согнуты."),
        ]:
            add("right", label, instruction)
    sides = ("right", "left") if hand == "both" else (hand,)
    for side in sides:
        if side == "left":
            add(side, "release", "Сожми обычный кулак, большой снаружи. Кисть держи прямо.")
        for variant, instruction in [
            ("straight", "Выпрями УКАЗАТЕЛЬНЫЙ, остальные три согни, большой отогни. Кисть ПРЯМО, как в игре."),
            ("forward", "Сохрани прямой УКАЗАТЕЛЬНЫЙ и три согнутых. Наклони кисть ВПЕРЁД, от себя."),
            ("back", "Те же пальцы: указательный прямой, три согнуты. Наклони кисть НАЗАД, к себе."),
            ("straight_repeat", "Не меняя пальцы, снова направь кисть ПРЯМО, как в игре. Указательный прямой."),
        ]:
            add(side, "trigger", instruction, variant)
        add(side, "release", "Сожми обычный кулак, большой снаружи. Кончики большого и указательного НЕ касаются.")
        if side == "right":
            add(side, "grip", "Кисть ПРЯМО. Выпрями МИЗИНЕЦ, большой отогни. Остальные три пальца согнуты.")
            add(side, "combo", "Кисть ПРЯМО. Выпрями УКАЗАТЕЛЬНЫЙ и МИЗИНЕЦ; средний/безымянный согнуты, большой отогнут.")
            add(side, "thumb_rest_combo", "Не меняя пальцы и кисть, ПРИЖМИ БОЛЬШОЙ. Указательный/мизинец прямые.")
            add(side, "release", "Сожми обычный кулак, большой снаружи. Кончики большого и указательного НЕ касаются.")
        add(side, "pad_click", "Покажи V: УКАЗАТЕЛЬНЫЙ и СРЕДНИЙ прямые. Безымянный/мизинец согнуты, БОЛЬШОЙ ПРИЖАТ.", "thumb_tucked")
        add(side, "pad_click", "Сохрани V: указательный/средний прямые, безымянный/мизинец согнуты. Теперь ОТОГНИ БОЛЬШОЙ.", "thumb_raised")
        add(side, "release", "Сожми обычный кулак, большой снаружи. Кончики большого и указательного НЕ касаются.")
    return plan


def make_plan(hand="both", full=False, follow_up=False):
    if follow_up:
        return make_followup_plan(hand)
    poses = [
        ("thumb_rest", "Сожми обычный кулак, большой снаружи. Кисть держи прямо, как обычно в игре."),
        ("open", "Выпрями указательный, средний, безымянный и мизинец. Большой прижми к ладони."),
        ("closed", "Согни все четыре пальца и прижми большой к ладони."),
        ("neutral", "Четыре пальца согнуты, большой отогнут. Кисть ПРЯМО в удобной игровой НЕЙТРАЛИ."),
        ("up", "Большой отогнут, остальные согнуты. Наклони КИСТЬ ВПЕРЁД, от себя. Удерживай."),
        ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, остальные пальцы согнуты."),
        ("down", "Большой отогнут, остальные согнуты. Наклони КИСТЬ НАЗАД, к себе. Удерживай."),
        ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, остальные пальцы согнуты."),
        ("left", "Большой отогнут, остальные согнуты. Наклони КИСТЬ ВЛЕВО. Удерживай."),
        ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, остальные пальцы согнуты."),
        ("right", "Большой отогнут, остальные согнуты. Наклони КИСТЬ ВПРАВО. Удерживай."),
        ("neutral", "Верни кисть в ту же игровую НЕЙТРАЛЬ. Большой отогнут, остальные пальцы согнуты."),
        ("grip", "Выпрями только МИЗИНЕЦ. Большой отогнут; указательный, средний, безымянный согнуты."),
        ("neutral", "Снова согни МИЗИНЕЦ. Все четыре согнуты, большой остаётся отогнутым."),
        ("grip", "Кисть ПРЯМО. Ещё раз выпрями МИЗИНЕЦ для Grip. Остальные три согнуты, большой отогнут."),
    ]
    if full:
        poses += [
            ("grip", "Сохрани прямой МИЗИНЕЦ и три согнутых пальца. Наклони кисть ВПЕРЁД, от себя.", "forward"),
            ("grip", "Мизинец остаётся прямым, остальные три согнуты. Наклони кисть НАЗАД, к себе.", "back"),
            ("grip", "Не меняя пальцы, снова направь кисть ПРЯМО, как в игре. Мизинец прямой.", "straight_repeat"),
        ]
    poses += [
        ("trigger", "Кисть ПРЯМО. Согни мизинец, средний и безымянный. Выпрями УКАЗАТЕЛЬНЫЙ, большой отогнут."),
    ]
    if full:
        poses += [
            ("trigger", "Сохрани прямой УКАЗАТЕЛЬНЫЙ и три согнутых пальца. Наклони кисть ВПЕРЁД, от себя.", "forward"),
            ("trigger", "Указательный остаётся прямым, остальные три согнуты. Наклони кисть НАЗАД, к себе.", "back"),
            ("trigger", "Не меняя пальцы, снова направь кисть ПРЯМО, как в игре. Указательный прямой.", "straight_repeat"),
        ]
    poses += [
        ("combo", "Выпрями УКАЗАТЕЛЬНЫЙ и МИЗИНЕЦ. Средний/безымянный согнуты, большой отогнут."),
        ("thumb_rest_combo", "Сохрани прямые указательный и мизинец, но ПРИЖМИ БОЛЬШОЙ к ладони."),
    ]
    if full:
        poses += [
            ("release", "Сожми обычный кулак, большой снаружи. Кончики большого и указательного НЕ касаются."),
            ("button_a", "Сделай КОЛЬЦО большим и указательным. Средний/безымянный/мизинец СОГНУТЫ."),
            ("release", "Разведи кончики большого и указательного. Сожми обычный кулак, большой снаружи."),
            ("button_b", "Сделай КОЛЬЦО большим и указательным. Средний/безымянный/мизинец ПРЯМЫЕ."),
            ("release", "Разведи кончики большого и указательного. Сожми обычный кулак, большой снаружи."),
            ("pad_click", "Покажи V: УКАЗАТЕЛЬНЫЙ и СРЕДНИЙ прямые, безымянный/мизинец согнуты. Большой прижми.", "thumb_tucked"),
            ("pad_click", "Сохрани V: указательный/средний прямые, безымянный/мизинец согнуты. ОТОГНИ БОЛЬШОЙ.", "thumb_raised"),
            ("release", "Сожми обычный кулак, большой снаружи. Кончики большого и указательного НЕ касаются."),
        ]
    sides = ("left", "right") if hand == "both" else (hand,)
    plan = []
    for side in sides:
        for label, instruction, *variant in poses:
            pose = dict(hand=side, label=label, instruction=instruction)
            if variant:
                pose["variant"] = variant[0]
            elif full and label in ("trigger", "grip"):
                pose["variant"] = "straight"
            plan.append(pose)
    if full and hand == "both":
        plan += [
            {"hand": "both", "label": "menu", "instruction":
             "Раскрой ОБЕ ладони одновременно, все пять пальцев ПРЯМЫЕ и разведены. Удерживай Menu."},
            {"hand": "both", "label": "release", "instruction": "Согни ВСЕ пальцы на обеих руках."},
        ]
    return plan


class RuntimeClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setblocking(False)
        self.buffer = b""
        self.hello = None
        self.last_frame_at = time.monotonic()
        self.last_frame = None

    def frames(self, timeout=.2):
        frames = []
        ready, _, _ = select.select([self.sock], [], [], max(0, timeout))
        if ready:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("Runtime закрыл соединение. Частичная запись будет сохранена.")
            self.buffer += chunk
            if len(self.buffer) > 8*1024*1024:
                raise RuntimeError("Слишком большая строка runtime; проверь адрес и порт TCP.")
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise RuntimeError("На этом порту нет JSON-протокола XR Gate.")
                if value.get("type") == "hello":
                    self.hello = value
                    if value.get("protocol") != "xr_gate_app_v1":
                        raise RuntimeError("Нужен app TCP-порт XR Gate (обычно 45810).")
                elif value.get("type") == "frame":
                    frames.append(value)
                    self.last_frame = value
                    self.last_frame_at = time.monotonic()
        if time.monotonic()-self.last_frame_at > 3:
            raise RuntimeError("Runtime не передаёт кадры 3 секунды. Проверь трекинг и RUNTIME_APP_TCP=1.")
        return frames

    def close(self):
        self.sock.close()


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def fresh_hand(frame, name):
    hands = frame.get("hands") or {}
    side = hands.get(name) or {}
    joints = side.get("joints") or []
    return side.get("status") == 1 and len(joints) == 21


def update_range(ranges, name, value):
    if not finite(value):
        return
    if name not in ranges:
        ranges[name] = [value, value]
    else:
        ranges[name][0] = min(ranges[name][0], value)
        ranges[name][1] = max(ranges[name][1], value)


class Archive:
    def __init__(self, path, metadata):
        self.path = Path(path)
        self.metadata = metadata
        self.summary = []
        self.frames_count = 0
        self.zip = zipfile.ZipFile(self.path, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=3)
        self.output = io.TextIOWrapper(self.zip.open("frames.jsonl", "w"), encoding="utf-8")
        self.closed = False

    def phase(self, index, spec):
        stats = {"phase_id": index, **spec, "frames": 0, "hold_frames": 0, "sides": {}}
        for name in ("left", "right"):
            stats["sides"][name] = {"tracked_hold_frames": 0, "unique_hand_samples": 0,
                "valid_joint_hold_samples": [0]*21, "buttons_histogram": {}, "ranges": {}}
        self.summary.append(stats)
        return stats

    def write(self, frame, stats, stage, elapsed):
        # Keep all runtime ticks, including repeated optical samples. This lets
        # replay distinguish tracking rate, debounce, button edges and stale data.
        frame = dict(frame)
        frame.update(label=stats["label"] if stage == "hold" else "transition",
            record_hand=stats["hand"], debug_schema=SCHEMA, phase_id=stats["phase_id"],
            phase_label=stats["label"], phase_stage=stage, phase_elapsed_ms=round(elapsed*1000))
        if stats.get("variant"):
            frame["phase_variant"] = stats["variant"]
        self.output.write(json.dumps(frame, ensure_ascii=False, allow_nan=False, separators=(",", ":"))+"\n")
        self.frames_count += 1
        stats["frames"] += 1
        if stage != "hold":
            return
        stats["hold_frames"] += 1
        hands = frame.get("hands") or {}
        for name in ("left", "right"):
            result = stats["sides"][name]
            side = hands.get(name) or {}
            stamp = hands.get("source_timestamp_ns")
            if stamp in (None, 0, "0"):
                stamp = hands.get("timestamp_ns")
            sample = (stamp, hands.get("reset_counter"))
            unique = sample[0] is not None and sample != result.get("last_sample")
            result["last_sample"] = sample
            if fresh_hand(frame, name):
                result["tracked_hold_frames"] += 1
                if unique:
                    result["unique_hand_samples"] += 1
                for joint in side.get("joints", []):
                    number = joint.get("joint_id", -1)
                    if unique and isinstance(number, int) and 0 <= number < 21 and joint.get("flags", 0) & 2:
                        result["valid_joint_hold_samples"][number] += 1
            update_range(result["ranges"], "hand_confidence", side.get("confidence"))
            controller = (frame.get("controllers") or {}).get(name) or {}
            buttons = str(controller.get("buttons", "missing"))
            counts = result["buttons_histogram"]
            counts[buttons] = counts.get(buttons, 0)+1
            for field in ("trigger", "grip"):
                update_range(result["ranges"], field, controller.get(field))
            axes = controller.get("thumbstick") or []
            if len(axes) == 2:
                update_range(result["ranges"], "stick_x", axes[0])
                update_range(result["ranges"], "stick_y", axes[1])

    def close(self, status, error=None):
        if self.closed:
            return
        self.closed = True
        self.output.close()
        self.metadata.update(status=status, error=error, frames=self.frames_count,
            finished_at=datetime.now().astimezone().isoformat(timespec="seconds"))
        for stats in self.summary:
            for side in stats["sides"].values():
                side.pop("last_sample", None)
        self.zip.writestr("metadata.json", json.dumps(self.metadata, ensure_ascii=False, indent=2))
        self.zip.writestr("summary.json", json.dumps(self.summary, ensure_ascii=False, indent=2))
        self.zip.close()


def runtime_options():
    """Record only XR input switches, never the process environment or unrelated args."""
    options = {"--emulation-controller", "--emulation-controller-config", "--controller-input-mode",
        "--controller-input-transport", "--override-controller-block-gestures-while-stream-present",
        "--override-controller-gesture-block-latch-ms", "--max-controller-age-ms",
        "--runtime-controller-mode", "--runtime-left-hand-gestures-enabled",
        "--runtime-right-hand-gestures-enabled", "--tracking-transform-config"}
    result = []
    for process in Path("/proc").glob("[0-9]*"):
        try:
            if Path((process/"exe").readlink()).name != "xr_runtime_adapter":
                continue
            args = (process/"cmdline").read_bytes().decode("utf-8", errors="replace").split("\0")
            selected = {}
            for i, argument in enumerate(args):
                key, separator, value = argument.partition("=")
                if key in options:
                    selected[key] = value if separator else (args[i+1] if i+1 < len(args) else "")
            result.append({"pid": int(process.name), "options": selected})
        except OSError:
            continue
    return result


def status_line(frame, hand):
    if frame is None:
        return "ожидаю кадры"
    names = ("left", "right") if hand == "both" else (hand,)
    values = []
    for name in names:
        controller = (frame.get("controllers") or {}).get(name) or {}
        grip = controller.get("grip")
        trigger = controller.get("trigger")
        axes = controller.get("thumbstick")
        grip_text = f"{grip:.2f}" if finite(grip) else "?"
        trigger_text = f"{trigger:.2f}" if finite(trigger) else "?"
        axes_text = (f"({axes[0]:+.2f},{axes[1]:+.2f})" if isinstance(axes, list) and
            len(axes) == 2 and all(finite(x) for x in axes) else "?")
        tracking = "21 joints" if fresh_hand(frame, name) else "НЕТ РУКИ"
        values.append(f"{NAMES[name]}: {tracking}; T={trigger_text}; G={grip_text}; оси={axes_text}; кнопки={controller.get('buttons', '?')}")
    return " | ".join(values)


def collect_stage(client, archive, stats, stage, duration, quiet=False):
    start = time.monotonic()
    end = start+duration
    displayed = 0
    while time.monotonic() < end:
        for frame in client.frames(min(.15, max(0, end-time.monotonic()))):
            archive.write(frame, stats, stage, time.monotonic()-start)
        now = time.monotonic()
        if not quiet and now-displayed >= .25:
            prefix = "ПОДГОТОВКА" if stage == "prepare" else "ДЕРЖИ ПОЗУ"
            line = f"{prefix}: {max(0, math.ceil(end-now))} с | {status_line(client.last_frame, stats['hand'])}"
            width = max(20, shutil.get_terminal_size((120, 20)).columns-1)
            print("\r"+line[:width].ljust(width), end="", flush=True)
            displayed = now
    if not quiet:
        print()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Запись жестов XR Gate с русскими подсказками и таймером.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45810)
    parser.add_argument("--hand", choices=("left", "right", "both"), default="both")
    parser.add_argument("--hold", type=float, default=4, help="секунды удержания каждой позы (по умолчанию 4)")
    parser.add_argument("--prepare", type=float, default=3, help="секунды подготовки к каждой позе (по умолчанию 3)")
    session = parser.add_mutually_exclusive_group()
    session.add_argument("--full", action="store_true",
        help="полная запись: D-pad и пальцы выбранных рук, Trigger/Grip при наклонах, A/B, V, двуручный Menu")
    session.add_argument("--follow-up", action="store_true",
        help="правая: новая калибровка D-pad; выбранные руки: Trigger при наклонах и V (левый D-pad не калибруется)")
    parser.add_argument("--no-beep", action="store_true", help="отключить сигнал терминала")
    parser.add_argument("--yes", action="store_true", help="начать без начального Enter")
    parser.add_argument("--plan-only", action="store_true", help="показать сценарий без подключения и записи")
    parser.add_argument("--profile", type=Path, help="приложить используемый JSON-профиль эмуляции")
    parser.add_argument("--output", type=Path, default=Path("xr-gate-gestures-"+datetime.now().strftime("%Y%m%d-%H%M%S")+".zip"))
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("порт должен быть от 1 до 65535")
    if not math.isfinite(args.hold) or not 1 <= args.hold <= 60:
        parser.error("--hold: от 1 до 60 секунд")
    if not math.isfinite(args.prepare) or not 1 <= args.prepare <= 60:
        parser.error("--prepare: от 1 до 60 секунд")
    plan = make_plan(args.hand, args.full, args.follow_up)
    duration = len(plan)*(args.hold+args.prepare)+5
    print(f"XR Gate: {len(plan)} поз, примерно {math.ceil(duration/60)} мин. Руки: {NAMES[args.hand]}.")
    if args.plan_only:
        for i, pose in enumerate(plan, 1):
            print(f"{i:02d}. {NAMES[pose['hand']]}: {pose['instruction']}")
        return 0
    if args.output.suffix.lower() != ".zip":
        parser.error("--output должен оканчиваться на .zip")
    if args.output.exists():
        parser.error(f"Файл уже существует: {args.output}. Выбери другое имя.")
    metadata = {"schema": SCHEMA, "started_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "host": args.host, "port": args.port, "hand": args.hand, "hold_seconds": args.hold,
        "session": "follow_up" if args.follow_up else "full" if args.full else "standard",
        "prepare_seconds": args.prepare, "plan": plan, "runtime_processes": runtime_options(),
        "joint_order": "0 wrist; 1..4 thumb; 5..8 index; 9..12 middle; 13..16 ring; 17..20 little"}
    if args.profile:
        try:
            metadata["emulation_profile"] = json.loads(args.profile.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            parser.error(f"Не удалось прочитать --profile: {exc}")
    print("Запусти XR Gate с RUNTIME_APP_TCP=1 и включённым новым режимом эмуляции.")
    print("На время записи жестов отключи override_controller или поставь его блокировку жестов в 0.")
    print("Сними джойстики. Держи обе руки в поле зрения камер, без перемещения всего предплечья.")
    print("Когда проверяем одну руку, вторую держи спокойно: все пальцы, включая большой, согнуты.")
    if args.follow_up:
        print("Сценарий не калибрует левый D-pad. На выбранных руках проверяем Trigger и новый V.")
    elif args.full:
        print("Полная перекалибровка: нейтраль, четыре направления и пальцы каждой выбранной руки; A/B, V и Menu.")
    if args.full or args.follow_up:
        print("Нейтраль выбирай по удобству в игре. НЕ пытайся обнулить текущие числа осей во время записи.")
    print("Скрипт только записывает данные. Жест V начнёт нажимать кнопку после установки соответствующего патча.")
    print(f"На каждую позу: {args.prepare:g} с подготовки, затем {args.hold:g} с удержания.")
    print("После старта Enter больше не нужен. Ctrl+C сохранит частичный архив.")
    client = None
    archive = None
    status, error, code = "completed", None, 0
    try:
        if not args.yes:
            input("\nПодготовься и нажми Enter для начала: ")
        client = RuntimeClient(args.host, args.port)
        deadline = time.monotonic()+3
        while client.last_frame is None and time.monotonic() < deadline:
            client.frames(.1)
        if client.last_frame is None:
            raise RuntimeError("Нет кадров runtime. Нужен app TCP-порт, обычно 45810.")
        metadata["runtime_hello"] = client.hello
        archive = Archive(args.output, metadata)
        print("\nПроверка видимости: покажи выбранные руки камерам на 5 секунд, БОЛЬШИЕ ПАЛЬЦЫ ПРИЖМИ.")
        warmup = archive.phase(0, {"hand": args.hand, "label": "setup",
            "instruction": "Покажи ладони камерам, большие пальцы прижми к ладоням."})
        collect_stage(client, archive, warmup, "hold", 5)
        required = ("left", "right") if args.hand == "both" else (args.hand,)
        for name in required:
            if warmup["sides"][name]["unique_hand_samples"] < 5:
                raise RuntimeError(f"Не видны 21 сустава: {NAMES[name]}. Проверь трекинг или выбери --hand left/right.")
        previous_hand = None
        for i, pose in enumerate(plan, 1):
            if pose["hand"] != previous_hand:
                print(f"\n=== {NAMES[pose['hand']]} ===")
                previous_hand = pose["hand"]
            print(f"\n[{i}/{len(plan)}] {NAMES[pose['hand']]}: {pose['instruction']}", flush=True)
            stats = archive.phase(i, pose)
            collect_stage(client, archive, stats, "prepare", args.prepare)
            if not args.no_beep:
                print("\a", end="", flush=True)
            collect_stage(client, archive, stats, "hold", args.hold)
            selected = ("left", "right") if pose["hand"] == "both" else (pose["hand"],)
            missing = [NAMES[name] for name in selected if stats["sides"][name]["unique_hand_samples"] < 10]
            if missing:
                print("Мало достоверных кадров этой позы: "+", ".join(missing)+". Отмечено в архиве.")
    except KeyboardInterrupt:
        status, error, code = "interrupted", "Остановлено пользователем", 130
    except (OSError, ValueError, RuntimeError) as exc:
        status, error, code = "error", str(exc), 1
    finally:
        if client:
            client.close()
        if archive:
            archive.close(status, error)
            print(f"\nАрхив: {args.output.resolve()}")
            print(f"Кадров: {archive.frames_count}. В архиве: frames.jsonl, metadata.json, summary.json.")
            print("Пришли этот ZIP для проверки направлений и распознавания пальцев.")
    if error:
        print("Запись остановлена: "+error, file=sys.stderr)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
