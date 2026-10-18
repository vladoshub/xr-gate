#!/usr/bin/env python3
"""Check launcher overrides without requiring cameras or starting transports."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    scripts = root / "backends/xr_spatial/scripts/linux"
    reference = root / "backends/xr_spatial/configs/profiles/reference.env"
    with tempfile.TemporaryDirectory(prefix="xr-spatial-launchers-") as temporary:
        case = Path(temporary)
        binary_dir = case / "binary directory"
        binary_dir.mkdir()
        binary = binary_dir / "xr_spatial_backend"
        binary.write_text('#!/usr/bin/env python3\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n')
        binary.chmod(0o700)
        calibration = case / "calibration directory/input calibration.json"
        calibration.parent.mkdir()
        calibration.write_text("{}")
        relative_calibration = str(calibration.relative_to(case))
        literal_profile = case / "literal.env"
        literal_profile.write_text(reference.read_text() + '\nCALIB_JSON="/missing/profile-calibration.json"\n')
        env = dict(os.environ, ROOT_PROJECT=str(root), BIN_DIR=str(binary_dir),
                   XR_SPATIAL_CONFIG=str(literal_profile),
                   CALIB_JSON="/missing/environment-calibration.json",
                   BASALT_CALIB="/missing/basalt-calibration.json",
                   XR_SPATIAL_NUM_THREADS="2", SCAN_OUTPUT_DIR=str(case / "scan"),
                   LOG_FILE="", CAPTURE_PROFILE_PROBE_ENABLED="0")
        for key in ["XR_SPATIAL_BIN", "SPATIAL_MAPPER_BIN", "XR_SPATIAL_APP_HOME"]:
            env.pop(key, None)

        def run(launcher, options, overrides=None, fail=False):
            result = subprocess.run(["bash", str(scripts / launcher), *options], cwd=case,
                                    env=dict(env, **(overrides or {})), text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
            if fail:
                assert result.returncode != 0 and "[ERROR]" in result.stdout, result.stdout
                return
            assert result.returncode == 0, result.stdout
            return json.loads(result.stdout.splitlines()[-1])

        def check_options(argv, calib, threads):
            for option, expected in [("--calib", calib), ("--num-threads", threads)]:
                assert argv.count(option) == 1 and argv[argv.index(option) + 1] == expected, argv

        launchers = ["start_xr_spatial.sh", "start_xr_spatial_scan.sh",
                     "start_xr_spatial_shm.sh", "start_xr_spatial_tcp.sh"]
        for launcher in launchers:
            for options, threads in [
                (["--calib", relative_calibration, "--num-threads", "1"], "1"),
                (["--calib=" + relative_calibration, "--num-threads=0"], "0"),
                (["--calib", "/missing/first.json", "--calib=" + relative_calibration,
                  "--num-threads=1", "--num-threads", "2"], "2"),
            ]:
                argv = run(launcher, [*options, "--print-every", "7"])
                check_options(argv, relative_calibration, threads)
                assert argv[-2:] == ["--print-every", "7"], argv
                if launcher == "start_xr_spatial_tcp.sh":
                    assert argv[argv.index("--capture-transport") + 1] == "capture_tcp", argv
            # Temporal filtering defaults on, can be disabled through environment,
            # and explicit CLI flags/options remain last for CLI11 TakeLast.
            for enabled, flag in [("", "--temporal-depth-filter"), ("0", "--no-temporal-depth-filter")]:
                argv = run(launcher, ["--calib", str(calibration)],
                           {"XR_SPATIAL_TEMPORAL_FILTER_ENABLED": enabled})
                assert flag in argv, argv
                for option, expected in [("--temporal-history-frames", "2"),
                                         ("--temporal-min-consistent-frames", "2"),
                                         ("--temporal-depth-tolerance-m", "0.04"),
                                         ("--temporal-depth-relative-tolerance", "0.03"),
                                         ("--temporal-current-weight", "0.6"),
                                         ("--temporal-max-age-ms", "500")]:
                    assert argv[argv.index(option) + 1] == expected, argv
            argv = run(launcher, ["--calib", str(calibration), "--no-temporal-depth-filter"],
                       {"XR_SPATIAL_TEMPORAL_HISTORY_FRAMES": "3",
                        "XR_SPATIAL_TEMPORAL_MIN_FRAMES": "3",
                        "XR_SPATIAL_TEMPORAL_ABS_TOLERANCE_M": "0.02",
                        "XR_SPATIAL_TEMPORAL_REL_TOLERANCE": "0.01",
                        "XR_SPATIAL_TEMPORAL_CURRENT_WEIGHT": "0.8",
                        "XR_SPATIAL_TEMPORAL_MAX_AGE_MS": "750"})
            assert argv[-1] == "--no-temporal-depth-filter" and "--temporal-depth-filter" in argv, argv
            for option, expected in [("--temporal-history-frames", "3"),
                                     ("--temporal-min-consistent-frames", "3"),
                                     ("--temporal-depth-tolerance-m", "0.02"),
                                     ("--temporal-depth-relative-tolerance", "0.01"),
                                     ("--temporal-current-weight", "0.8"),
                                     ("--temporal-max-age-ms", "750")]:
                assert argv[argv.index(option) + 1] == expected, argv
            for options in [["--calib"], ["--calib="], ["--calib", "--num-threads", "1"],
                            ["--calib", str(calibration), "--num-threads"],
                            ["--calib", str(calibration), "--num-threads=-1"],
                            ["--calib", str(calibration), "--num-threads=1.5"],
                            ["--calib", "/missing/explicit.json"]]:
                run(launcher, options, fail=True)
            # Environment/default selection must still work without CLI overrides.
            for overrides, threads in [
                ({"CALIB_JSON": str(calibration)}, "2"),
                ({"CALIB_JSON": "", "BASALT_CALIB": str(calibration), "XR_SPATIAL_NUM_THREADS": ""}, "1"),
            ]:
                argv = run(launcher, [], dict(overrides, XR_SPATIAL_CONFIG=str(reference)))
                check_options(argv, str(calibration), threads)
            run(launcher, ["--calib", str(calibration)], {"XR_SPATIAL_NUM_THREADS": "bad"}, fail=True)
            # Profile CLI parsing also applies to the generic runtime launcher.
            argv = run(launcher, ["--capture-profile", "reference", "--calib", str(calibration)],
                       {"XR_SPATIAL_CONFIG": ""})
            check_options(argv, str(calibration), "2")
            assert "--capture-profile" not in argv, argv
            print(f"{launcher}: calibration, thread and temporal filter overrides passed", flush=True)


if __name__ == "__main__":
    main()
