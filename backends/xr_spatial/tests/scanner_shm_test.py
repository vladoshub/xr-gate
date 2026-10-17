#!/usr/bin/env python3
"""Run the real scanner/SGBM against synthetic stereo and pose SHM streams."""
import argparse
import json
import mmap
import os
from pathlib import Path
import random
import signal
import struct
import subprocess
import tempfile
import time

POSE = struct.Struct("<IIQQQQ13dIIfI")
WIDTH, HEIGHT = 160, 120


class Ring:
    def __init__(self, name, pose=False):
        self.path = Path("/dev/shm") / name
        self.pose = pose
        self.slots = 64
        self.payload_size = POSE.size if pose else WIDTH * HEIGHT
        self.stride = 128 + self.payload_size
        self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        size = 4096 + self.slots * self.stride
        os.ftruncate(self.fd, size)
        self.mem = mmap.mmap(self.fd, size)
        self.mem[:48] = struct.pack("<8sIIIIIIQQ", b"CAPSHM1\0", 1,
                                  4 if pose else 1, 0 if pose else WIDTH,
                                  0 if pose else HEIGHT, 201 if pose else 1,
                                  self.payload_size, self.slots, 0)
        self.info = dict(shm_name=name, kind="POSE" if pose else "IMAGE",
                         frame_id="tracking_world" if pose else "camera",
                         width=0 if pose else WIDTH, height=0 if pose else HEIGHT,
                         format_code=201 if pose else 1,
                         format_name="HMD_POSE_F64_LE" if pose else "GRAY8",
                         payload_size=self.payload_size, header_size=4096,
                         slot_header_size=128, slot_count=self.slots,
                         slot_stride=self.stride)

    def write(self, seq, ts, payload, bad_dimensions=False):
        off = 4096 + (seq - 1) % self.slots * self.stride
        if bad_dimensions:
            payload = b"\0"
        struct.pack_into("<QQqQIIIIII", self.mem, off, seq * 2 - 1, seq * 2 - 1,
                         ts, time.monotonic_ns(), len(payload),
                         0 if self.pose else (1 if bad_dimensions else WIDTH),
                         0 if self.pose else (1 if bad_dimensions else HEIGHT),
                         201 if self.pose else 1, 1 if self.pose else 0, 0)
        self.mem[off + 128:off + 128 + len(payload)] = payload
        struct.pack_into("<QQ", self.mem, off, seq * 2, seq * 2)
        struct.pack_into("<Q", self.mem, 40, seq)

    def close(self):
        self.mem.close()
        os.close(self.fd)
        self.path.unlink(missing_ok=True)


def calibration(offset=0):
    intr = dict(fx=180., fy=180., cx=WIDTH / 2, cy=HEIGHT / 2,
                k1=0., k2=0., k3=0., k4=0.)
    transform = dict(px=0., py=0., pz=0., qw=1., qx=0., qy=0., qz=0.)
    return {"value0": dict(
        intrinsics=[{"camera_type": "kb4", "intrinsics": intr}] * 2,
        resolution=[[WIDTH, HEIGHT]] * 2,
        T_imu_cam=[transform, dict(transform, px=.1)], cam_time_offset_ns=offset)}


def check_profile(root):
    code = '''set -euo pipefail
cd "$1"
source devices/xreal_ultra/xreal_ultra_new.env
source devices/xreal_ultra/xreal_ultra_new_tracking.env
if [[ -n "$2" ]]; then export CALIB_JSON="$2"; fi
source backends/xr_spatial/scripts/linux/xr_spatial_profile.sh
load_xr_spatial_profile start_xr_spatial_scan
printf '%s\n%s\n' "$BASALT_CALIB" "$CALIB_JSON"
'''
    env = {"PATH": os.environ["PATH"], "HOME": os.environ.get("HOME", "/tmp")}
    for override in ["", "/tmp/explicit-spatial-calibration.json"]:
        output = subprocess.check_output(["bash", "-c", code, "test", str(root), override],
                                         env=env, text=True).splitlines()
        assert output[-1] == (override or output[-2]), output
    print("profile calibration selection passed", flush=True)


def run_case(binary, directory, name):
    case = directory / name
    case.mkdir()
    prefix = f"xr_spatial_test_{os.getpid()}_{name}"
    cams = [Ring(prefix + f"_cam{i}") for i in range(2)]
    pose = Ring(prefix + "_pose", pose=True)
    proc = None
    offset = -28_111_831 if name == "time_offset" else 0
    (case / "capture.json").write_text(json.dumps({"streams": {
        f"camera{i}": ring.info for i, ring in enumerate(cams)}}))
    (case / "pose.json").write_text(json.dumps({"streams": {"hmd_pose": pose.info}}))
    calibration_path = case / "input calibration.json"
    calibration_path.write_text(json.dumps(calibration(offset)))
    rng = random.Random(42)
    left = bytes(rng.randrange(256) for _ in range(WIDTH * HEIGHT))
    right = b"".join(left[y * WIDTH + 10:(y + 1) * WIDTH] + bytes(10) for y in range(HEIGHT))
    if name == "no_depth":
        left = right = bytes(WIDTH * HEIGHT)
    base = time.monotonic_ns()

    def publish(seq):
        ts = base + (seq - 1) * 40_000_000
        cam_seq = seq - 6 if name == "capture_restart" and seq >= 7 else seq
        bad = (name == "bad_frame" and seq == 7) or (name == "depth_failure" and seq >= 7)
        for camera, image in zip(cams, [left, right]):
            camera.write(cam_seq, ts, image, bad)
        x = .25 if name == "reset_counter" and seq >= 7 else 0.
        if name == "pose_jump" and seq >= 7:
            x = 2.
        reset = int(name in {"reset_counter", "runtime_reset"} and seq >= 7)
        pose_seq = seq - 6 if name == "pose_restart" and seq >= 7 else seq
        data = POSE.pack(1, POSE.size, pose_seq, ts + offset, ts + offset, reset,
                         x, 0., 0., 1., 0., 0., 0., 0., 0., 0., 0., 0., 0., 2, 1, 1., 0)
        pose.write(seq, ts + offset, data)

    try:
        publish(1)
        args = [str(binary), "--mode", "scan", "--calib", str(calibration_path),
                "--capture-registry", str(case / "capture.json"),
                "--pose-registry", str(case / "pose.json"),
                "--scan-output-dir", str(case / "output"), "--scan-duration-sec", "1.0",
                "--depth-rate-hz", "25", "--num-disparities", "32", "--block-size", "3",
                "--point-decimation", "8", "--no-quality-gate-enabled", "--max-pose-age-ms", "1",
                "--no-save-debug-clouds", "--no-save-voxel-pointcloud-ply",
                "--tracking-origin", "absolute", "--print-every", "1",
                "--mapper-backend", "pointcloud_fallback"]
        if name == "capture_restart":
            args += ["--capture-reattach-on-stale-ms", "100"]
        if name == "depth_failure":
            args += ["--scan-duration-sec", "2"]
        if name == "no_rate_limit":
            args += ["--depth-rate-hz", "0", "--scan-duration-sec", ".15"]
        if name == "live_grid_scan":
            args += ["--mapper-backend", "live_depth_grid", "--num-threads", "0"]
        if name == "time_offset":
            args += ["--num-threads", "2"]
        if name == "save_error":
            (case / "output").mkdir()
            (case / "output" / "pointcloud.ply").symlink_to("/dev/full")
        if name == "point_cap":
            args += ["--max-accumulated-points", "100"]
        if name == "runtime_reset":
            args += ["--mode", "runtime"]
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if name != "no_rate_limit":
            for seq in range(2, 100):
                time.sleep(.04)
                if proc.poll() is not None:
                    break
                if name == "capture_restart" and seq == 7:
                    for camera in cams:
                        camera.close()
                    cams = [Ring(prefix + f"_cam{i}") for i in range(2)]
                publish(seq)
                if name in {"interrupt", "runtime_reset"} and seq >= 14:
                    time.sleep(.05)
                    proc.send_signal(signal.SIGINT)
                    break
        log = proc.communicate(timeout=5)[0]
        if name == "runtime_reset":
            assert proc.returncode == 0 and "reset runtime map" in log, log
            assert "integrated=" in log.split("reset runtime map")[-1], log
            print("runtime_reset: passed", flush=True)
            return
        meta = json.loads((case / "output" / "scan_metadata.json").read_text())
        expected_threads = 0 if name == "live_grid_scan" else (2 if name == "time_offset" else 1)
        assert meta["num_threads"] == expected_threads and meta["opencv_num_threads"] >= 1, meta
        if expected_threads == 1:
            assert meta["opencv_num_threads"] == 1, meta
        assert meta["calibration_path"] == str(calibration_path), meta
        trajectory = (case / "output" / "trajectory.csv").read_text().splitlines()[1:]
        xs = [float(row.split(",")[3]) for row in trajectory]
        if name in {"no_depth", "interrupt"}:
            assert proc.returncode == 2 and not meta["complete"], log
            assert meta["stop_reason"] == ("no_frames_integrated" if name == "no_depth" else "interrupted"), meta
        elif name in {"reset_counter", "pose_jump", "pose_restart"}:
            assert proc.returncode == 2 and not meta["complete"] and xs and max(xs) == 0, log
            assert meta["stop_reason"] == ("pose_source_jump" if name == "pose_jump" else "pose_source_reset"), meta
        elif name == "save_error":
            assert proc.returncode == 1 and not meta["complete"] and meta["stop_reason"] == "save_error", log
        elif name == "depth_failure":
            assert proc.returncode == 1 and not meta["complete"] and meta["frame_errors"] >= 30, log
            assert meta["accumulated_points"] > 0 and (case / "output" / "pointcloud.ply").exists(), log
        else:
            assert proc.returncode == 0 and meta["complete"] and meta["accumulated_points"] > 0, log
            if name == "capture_restart":
                assert meta["capture_reattachments"] >= 1 and meta["integrated_frames"] > 7, log
            elif name == "bad_frame":
                assert meta["frame_errors"] == 1 and meta["integrated_frames"] > 7, log
            elif name == "no_rate_limit":
                assert meta["integrated_frames"] == 1, meta
            elif name == "time_offset":
                assert max(float(row.split(",")[2]) for row in trajectory) == 0, log
                assert meta["camera_time_offset_ns"] == offset, meta
            elif name == "point_cap":
                assert meta["accumulated_points"] == 100 and meta["reservoir_sampling"], meta
        print(f"{name}: passed ({meta['integrated_frames']} frames, {meta['stop_reason']})", flush=True)
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        for ring in [*cams, pose]:
            ring.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    check_profile(args.root)
    for value in ["-1", "1.5", "bad"]:
        invalid = subprocess.run([str(args.binary.resolve()), "--calib", "/missing/calibration.json",
                                  "--num-threads=" + value], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert invalid.returncode != 0 and "--num-threads" in invalid.stdout, invalid.stdout
    with tempfile.TemporaryDirectory(prefix="xr-spatial-scanner-") as temporary:
        for name in ["normal", "time_offset", "reset_counter", "pose_jump", "pose_restart",
                     "capture_restart", "bad_frame", "depth_failure", "no_rate_limit",
                     "live_grid_scan", "point_cap", "save_error", "no_depth", "interrupt", "runtime_reset"]:
            run_case(args.binary.resolve(), Path(temporary), name)


if __name__ == "__main__":
    main()
