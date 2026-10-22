import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "doctor_linux", Path(__file__).resolve().parents[1] / "linux/xr_gate_wsl.py"
)
linux = importlib.util.module_from_spec(spec)
spec.loader.exec_module(linux)


class DoctorTimeoutTest(unittest.TestCase):
    def test_hung_bluetooth_probe_does_not_skip_remaining_checks(self):
        with tempfile.TemporaryDirectory() as folder:
            runtime = linux.Runtime.__new__(linux.Runtime)
            runtime.current = Path(folder)
            runtime.status = lambda: {"installed": False, "running": False}

            def execute(args, **kwargs):
                if args[0] == "bluetoothctl":
                    raise subprocess.TimeoutExpired(args, kwargs["timeout"])
                return subprocess.CompletedProcess(args, 0)

            with (
                patch.object(linux.shutil, "which", return_value="/diagnostic/tool"),
                patch.object(linux.subprocess, "run", side_effect=execute) as native,
                patch.object(linux, "log") as log,
            ):
                runtime.doctor()
                commands = [call.args[0][0] for call in native.call_args_list]
                self.assertEqual(commands, ["lsusb", "bluetoothctl", "rfkill", "systemctl"])
                self.assertTrue(all(call.kwargs["timeout"] == 10 for call in native.call_args_list))
                self.assertTrue(any("timed out" in call.args[0] for call in log.call_args_list))


if __name__ == "__main__":
    unittest.main()
