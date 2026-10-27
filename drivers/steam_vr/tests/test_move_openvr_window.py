import json
import os
from pathlib import Path
import pty
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "drivers/steam_vr/scripts/linux/move_openvr_window.sh"


class MoveWindowTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.state = self.root / "state"
        self.state.mkdir()
        self.settings = self.home / ".local/share/Steam/config/steamvr.vrsettings"
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(json.dumps({"xr_tracking": {"windowWidth": 3840, "windowHeight": 1080}}))
        self.write_state("monitors", "Monitors: 1\n 0: +DP-2 3840/120x1080/70+7680+1080  DP-2\n")
        self.write_state("pids", "59714\n")
        self.write_state("windows", json.dumps({"59714": ["60817410"]}))
        self.write_state("sizes", json.dumps({"60817410": [3840, 1080]}))
        self.write_state("moves", "")
        self.make_tool("xrandr", '''import os
from pathlib import Path
print((Path(os.environ["MOCK_STATE"]) / "monitors").read_text(), end="")
''')
        self.make_tool("pgrep", '''import os
from pathlib import Path
print((Path(os.environ["MOCK_STATE"]) / "pids").read_text(), end="")
''')
        self.make_tool("xdotool", '''import json, os, sys
from pathlib import Path
s = Path(os.environ["MOCK_STATE"])
args = sys.argv[1:]
if args[0] == "search":
    data = json.loads((s / "windows").read_text())
    print("\\n".join(data.get(args[-1], [])))
elif args[0] == "getwindowgeometry":
    data = json.loads((s / "sizes").read_text())
    if args[-1] not in data: sys.exit(1)
    width, height = data[args[-1]]
    print(f"WIDTH={width}\\nHEIGHT={height}")
elif args[0] == "windowmove":
    with (s / "moves").open("a") as f: f.write(" ".join(args[1:]) + "\\n")
else: sys.exit(2)
''')
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("XR_OPENVR_")}
        self.env.update(HOME=str(self.home), XDG_CONFIG_HOME=str(self.home / ".config"),
                        DISPLAY=":0", PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        MOCK_STATE=str(self.state))

    def write_state(self, name, value):
        path = self.state / name
        temp = path.with_suffix(".tmp")
        temp.write_text(value)
        temp.replace(path)

    def make_tool(self, name, body):
        path = self.bin / name
        path.write_text("#!/usr/bin/python3\n" + body)
        path.chmod(0o755)

    def run_watcher(self, **extra_env):
        process = subprocess.Popen(["bash", str(SCRIPT)], env=dict(self.env, **extra_env),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        self.addCleanup(self.stop, process)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
        if process.stderr:
            process.stderr.close()

    def wait_moves(self, count, process):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            lines = (self.state / "moves").read_text().splitlines()
            if len(lines) >= count:
                return lines
            if process.poll() is not None:
                self.fail("Watcher exited: " + process.stderr.read())
            time.sleep(0.1)
        self.fail(f"Timed out waiting for {count} moves")

    def test_moves_current_window_and_tracks_geometry_and_restart(self):
        process = self.run_watcher()
        self.assertEqual(self.wait_moves(1, process), ["60817410 +7680 +1080"])
        self.assertEqual((self.home / ".config/xr-gate/steamvr-window-output").read_text(), "DP-2\n")
        self.write_state("monitors", "Monitors: 1\n 0: +DP-2 3840/120x1080/70-3840+0  DP-2\n")
        self.assertEqual(self.wait_moves(2, process)[-1], "60817410 -3840 +0")
        self.write_state("sizes", json.dumps({"42": [3840, 1080]}))
        self.write_state("windows", json.dumps({"42": ["42"]}))
        self.write_state("pids", "42\n")
        self.assertEqual(self.wait_moves(3, process)[-1], "42 -3840 +0")

    def test_does_not_move_nonmatching_or_ambiguous_windows(self):
        self.write_state("windows", json.dumps({"59714": ["1", "2"]}))
        self.write_state("sizes", json.dumps({"1": [3840, 1080], "2": [3840, 1080]}))
        process = self.run_watcher()
        time.sleep(1.5)
        self.assertEqual((self.state / "moves").read_text(), "")
        self.write_state("sizes", json.dumps({"1": [800, 600], "2": [3840, 1080]}))
        self.assertEqual(self.wait_moves(1, process)[0], "2 +7680 +1080")

    def test_two_monitors_require_selection_without_a_terminal(self):
        self.write_state("monitors", "Monitors: 2\n 0: +*DP-4 7680/1390x2160/390+0+0 DP-4\n"
                         " 1: +DP-2 3840/120x1080/70+7680+1080 DP-2\n")
        result = subprocess.run(["bash", str(SCRIPT)], env=self.env, stdin=subprocess.DEVNULL,
                                text=True, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Multiple displays", result.stderr)
        self.assertEqual((self.state / "moves").read_text(), "")

    def test_explicit_output_selects_secondary_monitor(self):
        self.write_state("monitors", "Monitors: 2\n 0: +*DP-4 7680/1390x2160/390+0+0 DP-4\n"
                         " 1: +DP-2 3840/120x1080/70+7680+1080 DP-2\n")
        process = self.run_watcher(XR_OPENVR_WINDOW_OUTPUT="DP-2")
        self.assertEqual(self.wait_moves(1, process)[0], "60817410 +7680 +1080")

    def test_interactive_selection_is_remembered(self):
        self.write_state("monitors", "Monitors: 2\n 0: +*DP-4 7680/1390x2160/390+0+0 DP-4\n"
                         " 1: +DP-2 3840/120x1080/70+7680+1080 DP-2\n")
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        process = subprocess.Popen(["bash", str(SCRIPT)], env=self.env, stdin=slave,
                                   stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        self.addCleanup(self.stop, process)
        os.write(master, b"2\n")
        self.assertEqual(self.wait_moves(1, process)[0], "60817410 +7680 +1080")
        self.assertEqual((self.home / ".config/xr-gate/steamvr-window-output").read_text(), "DP-2\n")

    def test_missing_settings_has_actionable_error(self):
        self.settings.unlink()
        result = subprocess.run(["bash", str(SCRIPT)], env=self.env, stdin=subprocess.DEVNULL,
                                text=True, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Register the driver first", result.stderr)

    def test_missing_xdotool_has_install_command(self):
        path = self.bin / "xdotool"
        path.unlink()
        # Restrict PATH so a host-installed xdotool cannot mask the missing dependency.
        env = dict(self.env, PATH=str(self.bin))
        result = subprocess.run(["/bin/bash", str(SCRIPT)], env=env, stdin=subprocess.DEVNULL,
                                text=True, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sudo apt install xdotool", result.stderr)


if __name__ == "__main__":
    unittest.main()
