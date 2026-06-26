#!/usr/bin/env python3
"""
Realtime-ish viewer for Mercury's native image-space debug overlay dumps.

This does NOT read HAND_TRACKING_V2 3D joints and does NOT project 3D points.
It simply displays the newest *_ann.ppm written by Mercury's hg_sync.cpp debug dump.
That makes it the right tool for checking raw Mercury 2D keypoint/optimizer overlay,
ROI/keypoint drift, finger order, and stale detector/tracker behavior.
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path
from typing import Optional

import cv2


def newest_file(directory: Path, pattern: str, last_path: Optional[Path]) -> Optional[Path]:
    try:
        files = [p for p in directory.glob(pattern) if p.is_file()]
    except FileNotFoundError:
        return None
    if not files:
        return None

    # mtime_ns is enough here and much cheaper than sorting by parsed frame timestamp.
    files.sort(key=lambda p: (p.stat().st_mtime_ns, p.name), reverse=True)
    p = files[0]
    if last_path is not None and p == last_path:
        return last_path
    return p


def draw_status(img, text: str) -> None:
    font = cv2.FONT_HERSHEY_SIMPLEX
    scale = 0.45
    thickness = 1
    x, y = 8, max(18, img.shape[0] - 10)
    cv2.putText(img, text, (x + 1, y + 1), font, scale, (0, 0, 0), thickness + 2, cv2.LINE_AA)
    cv2.putText(img, text, (x, y), font, scale, (255, 255, 255), thickness, cv2.LINE_AA)


def main() -> int:
    ap = argparse.ArgumentParser(description="View Mercury native 2D debug overlay dumps")
    ap.add_argument("--dump-dir", default="/tmp/xr_mercury_debug", help="MERCURY_XR_DEBUG_DUMP_DIR root")
    ap.add_argument("--ann-subdir", default="ann", help="subdir containing *_ann.ppm")
    ap.add_argument("--pattern", default="*_ann.ppm", help="glob pattern inside ann dir")
    ap.add_argument("--scale", type=float, default=1.0, help="display scale")
    ap.add_argument("--poll-ms", type=float, default=15.0, help="poll interval in ms")
    ap.add_argument("--window", default="XR Mercury raw 2D overlay", help="OpenCV window name")
    ap.add_argument("--no-status", action="store_true", help="do not draw status text")
    ap.add_argument("--wait", action="store_true", help="wait for dump dir/files instead of exiting")
    ap.add_argument("--screenshot-dir", default="/tmp", help="where screenshots are written")
    args = ap.parse_args()

    root = Path(args.dump_dir).expanduser()
    ann_dir = root / args.ann_subdir
    screenshot_dir = Path(args.screenshot_dir).expanduser()
    screenshot_dir.mkdir(parents=True, exist_ok=True)

    print(f"[viewer] dump_dir={root}")
    print(f"[viewer] watching {ann_dir}/{args.pattern}")
    print("[viewer] keys: q/Esc exit, Space pause, s screenshot")
    print("[viewer] This is Mercury native 2D overlay; no HAND_TRACKING_V2 3D projection is used.")

    paused = False
    last_path: Optional[Path] = None
    last_img = None
    last_mtime_ns = 0
    shown_count = 0

    cv2.namedWindow(args.window, cv2.WINDOW_NORMAL)

    while True:
        if not paused:
            p = newest_file(ann_dir, args.pattern, last_path)
            if p is None:
                if not args.wait:
                    print(f"[viewer] no files matching {ann_dir}/{args.pattern}")
                    return 1
                time.sleep(max(0.001, args.poll_ms / 1000.0))
                key = cv2.waitKey(1) & 0xFF
                if key in (ord('q'), 27):
                    return 0
                continue

            try:
                st = p.stat()
                # If newest path unchanged and mtime unchanged, keep current image.
                if p != last_path or st.st_mtime_ns != last_mtime_ns:
                    img = cv2.imread(str(p), cv2.IMREAD_COLOR)
                    if img is not None and img.size > 0:
                        last_path = p
                        last_mtime_ns = st.st_mtime_ns
                        last_img = img
                        shown_count += 1
            except FileNotFoundError:
                pass

        if last_img is not None:
            disp = last_img.copy()
            if not args.no_status:
                age_ms = 0.0
                try:
                    age_ms = (time.time_ns() - last_mtime_ns) / 1e6
                except Exception:
                    pass
                draw_status(
                    disp,
                    f"{last_path.name if last_path else ''} | shown={shown_count} | age={age_ms:.0f}ms | raw Mercury 2D overlay",
                )

            if args.scale != 1.0:
                w = max(1, int(disp.shape[1] * args.scale))
                h = max(1, int(disp.shape[0] * args.scale))
                disp = cv2.resize(disp, (w, h), interpolation=cv2.INTER_AREA if args.scale < 1.0 else cv2.INTER_LINEAR)

            cv2.imshow(args.window, disp)

        key = cv2.waitKey(max(1, int(args.poll_ms))) & 0xFF
        if key in (ord('q'), 27):
            return 0
        if key == ord(' '):
            paused = not paused
            print("[viewer] paused" if paused else "[viewer] resumed")
        if key == ord('s') and last_img is not None:
            ts = int(time.time())
            out = screenshot_dir / f"mercury_raw2d_overlay_{ts}.png"
            cv2.imwrite(str(out), last_img)
            print(f"[viewer] screenshot: {out}")


if __name__ == "__main__":
    raise SystemExit(main())
