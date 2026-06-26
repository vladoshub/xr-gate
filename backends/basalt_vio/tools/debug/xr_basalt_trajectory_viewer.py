#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import time
import tkinter as tk
from pathlib import Path
from typing import List, Optional, Tuple


Point3 = Tuple[float, float, float]


def try_float(v: str) -> Optional[float]:
    try:
        x = float(v.strip())
        if math.isfinite(x):
            return x
    except Exception:
        pass
    return None


def parse_trajectory_csv(path: Path) -> List[Point3]:
    if not path.exists():
        return []

    try:
        lines = [ln.strip() for ln in path.read_text(errors="ignore").splitlines() if ln.strip()]
    except Exception:
        return []

    pts: List[Point3] = []

    for ln in lines:
        if ln.startswith("#"):
            ln = ln[1:].strip()
        if not ln:
            continue

        parts = ln.replace(",", " ").split()
        vals = []
        for x in parts:
            try:
                vals.append(float(x))
            except Exception:
                pass

        if len(vals) < 3:
            continue

        # Basalt trajectory format:
        #   timestamp_ns px py pz qx qy qz qw
        #
        # Important: column 0 is timestamp, not X.
        if len(vals) >= 4 and abs(vals[0]) > 1e6:
            x, y, z = vals[1], vals[2], vals[3]
        else:
            x, y, z = vals[0], vals[1], vals[2]

        if not all(math.isfinite(v) for v in (x, y, z)):
            continue

        if max(abs(x), abs(y), abs(z)) > 1000.0:
            continue

        pts.append((x, y, z))

    return pts

def path_length(points: List[Point3]) -> float:
    if len(points) < 2:
        return 0.0
    total = 0.0
    for a, b in zip(points, points[1:]):
        total += math.dist(a, b)
    return total


class TrajectoryViewer:
    def __init__(self, root: tk.Tk, args: argparse.Namespace) -> None:
        self.root = root
        self.args = args
        self.path = Path(args.trajectory).expanduser()

        self.root.title("XR Basalt trajectory viewer")

        self.info = tk.Label(root, text="", anchor="w", justify="left", font=("monospace", 10))
        self.info.pack(fill="x")

        self.canvas = tk.Canvas(root, width=args.width, height=args.height, bg="#111111")
        self.canvas.pack(fill="both", expand=True)

        self.origin: Optional[Point3] = None
        self.points: List[Point3] = []

        self.root.bind("q", lambda _e: self.root.destroy())
        self.root.bind("<Escape>", lambda _e: self.root.destroy())
        self.root.bind("r", lambda _e: self.reset_origin())

        self.tick()

    def reset_origin(self) -> None:
        pts = parse_trajectory_csv(self.path)
        if pts:
            self.origin = self.transform_point(pts[-1])

    def transform_point(self, p: Point3) -> Point3:
        x, y, z = p

        if self.args.swap_x_y:
            x, y = y, x
        if self.args.swap_x_z:
            x, z = z, x
        if self.args.swap_y_z:
            y, z = z, y

        if self.args.flip_x:
            x = -x
        if self.args.flip_y:
            y = -y
        if self.args.flip_z:
            z = -z

        return x, y, z

    def relative_points(self, pts: List[Point3]) -> List[Point3]:
        if not pts:
            return []

        tpts = [self.transform_point(p) for p in pts]

        if self.origin is None:
            self.origin = tpts[0]

        ox, oy, oz = self.origin
        return [(x - ox, y - oy, z - oz) for x, y, z in tpts]

    def project_panel(
        self,
        pts: List[Point3],
        panel: Tuple[int, int, int, int],
        axes: Tuple[int, int],
    ) -> List[Tuple[float, float]]:
        x0, y0, x1, y1 = panel
        w, h = x1 - x0, y1 - y0
        pad = 28

        if not pts:
            return []

        values = [(p[axes[0]], p[axes[1]]) for p in pts]
        xs = [v[0] for v in values]
        ys = [v[1] for v in values]

        max_abs = max([abs(v) for v in xs + ys] + [self.args.min_scale_m])
        scale = 0.5 * min(w - 2 * pad, h - 2 * pad) / max_abs

        cx = x0 + w / 2
        cy = y0 + h / 2

        out = []
        for a, b in values:
            px = cx + a * scale
            py = cy - b * scale
            out.append((px, py))
        return out

    def draw_grid(self, panel: Tuple[int, int, int, int], title: str) -> None:
        x0, y0, x1, y1 = panel
        w, h = x1 - x0, y1 - y0
        cx, cy = x0 + w / 2, y0 + h / 2

        self.canvas.create_rectangle(x0 + 4, y0 + 4, x1 - 4, y1 - 4, outline="#444444")
        self.canvas.create_text(x0 + 12, y0 + 12, text=title, fill="#dddddd", anchor="nw", font=("monospace", 12))

        self.canvas.create_line(x0 + 10, cy, x1 - 10, cy, fill="#333333")
        self.canvas.create_line(cx, y0 + 10, cx, y1 - 10, fill="#333333")

    def draw_path(self, panel: Tuple[int, int, int, int], pts2: List[Tuple[float, float]]) -> None:
        if len(pts2) >= 2:
            flat = []
            for p in pts2:
                flat.extend(p)
            self.canvas.create_line(*flat, fill="#00ccff", width=2)

        if pts2:
            x, y = pts2[-1]
            self.canvas.create_oval(x - 5, y - 5, x + 5, y + 5, fill="#ffcc00", outline="")
            self.canvas.create_text(x + 8, y - 8, text="current", fill="#ffcc00", anchor="sw", font=("monospace", 9))

        if pts2:
            x, y = pts2[0]
            self.canvas.create_oval(x - 4, y - 4, x + 4, y + 4, fill="#00ff66", outline="")
            self.canvas.create_text(x + 8, y + 8, text="origin", fill="#00ff66", anchor="nw", font=("monospace", 9))

    def tick(self) -> None:
        pts = parse_trajectory_csv(self.path)
        rel = self.relative_points(pts)

        self.canvas.delete("all")

        width = max(self.canvas.winfo_width(), 400)
        height = max(self.canvas.winfo_height(), 300)

        left = (0, 0, width // 2, height)
        right = (width // 2, 0, width, height)

        self.draw_grid(left, "Top-down: X / Z")
        self.draw_grid(right, "Side: X / Y")

        self.draw_path(left, self.project_panel(rel, left, (0, 2)))
        self.draw_path(right, self.project_panel(rel, right, (0, 1)))

        if rel:
            x, y, z = rel[-1]
            disp = math.sqrt(x * x + y * y + z * z)
            plen = path_length(rel)
            text = (
                f"file: {self.path}\n"
                f"points: {len(rel)}   current: x={x:+.3f} y={y:+.3f} z={z:+.3f} m   "
                f"displacement={disp:.3f} m   path={plen:.3f} m\n"
                f"transform: flip_x={self.args.flip_x} flip_y={self.args.flip_y} flip_z={self.args.flip_z} "
                f"swap_xy={self.args.swap_x_y} swap_xz={self.args.swap_x_z} swap_yz={self.args.swap_y_z}\n"
                f"keys: r=reset origin, q/esc=quit"
            )
        else:
            text = (
                f"file: {self.path}\n"
                f"waiting for trajectory points...\n"
                f"keys: r=reset origin, q/esc=quit"
            )

        self.info.config(text=text)
        self.root.after(int(self.args.interval_ms), self.tick)


def main() -> None:
    import sys
    print("[viewer] file:", __file__)
    print("[viewer] argv:", " ".join(sys.argv[1:]))
    ap = argparse.ArgumentParser()
    ap.add_argument("--trajectory", default="/tmp/xr_basalt_unified_live/trajectory.csv")
    ap.add_argument("--interval-ms", type=int, default=100)
    ap.add_argument("--width", type=int, default=1100)
    ap.add_argument("--height", type=int, default=650)
    ap.add_argument("--min-scale-m", type=float, default=0.25)

    ap.add_argument("--swap-x-y", action="store_true", help="Swap X and Y coordinates before drawing")
    ap.add_argument("--swap-x-z", action="store_true", help="Swap X and Z coordinates before drawing")
    ap.add_argument("--swap-y-z", action="store_true", help="Swap Y and Z coordinates before drawing")

    ap.add_argument("--flip-x", action="store_true", help="Invert X coordinate before drawing")
    ap.add_argument("--flip-y", action="store_true", help="Invert Y coordinate before drawing")
    ap.add_argument("--flip-z", action="store_true", help="Invert Z coordinate before drawing")

    args = ap.parse_args()

    root = tk.Tk()
    root.geometry(f"{args.width}x{args.height}")
    TrajectoryViewer(root, args)
    root.mainloop()


if __name__ == "__main__":
    main()
