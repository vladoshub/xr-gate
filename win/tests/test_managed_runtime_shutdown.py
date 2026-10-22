import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HELPER = Path(__file__).resolve().parents[1] / "linux/xr_gate_wsl.py"
SPEC = importlib.util.spec_from_file_location("managed_runtime", HELPER)
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


PROC_MATCHES = sys.platform == "linux" and Path("/proc/self/stat").is_file() and int(Path("/proc/self/stat").read_text().split(" ", 1)[0]) == os.getpid()


@unittest.skipUnless(PROC_MATCHES and hasattr(os, "pidfd_open"), "Requires /proc mounted for the current PID namespace; runs on Ubuntu CI")
class ManagedRuntimeShutdownTest(unittest.TestCase):
    def spawn(self, folder, *, ignore_term=False):
        worker = folder / "worker.py"
        # xr_client services use separate sessions. Verify those are collected,
        # including a child which outlives its immediate parent.
        worker.write_text("""import os, subprocess, sys, time
from pathlib import Path
child = subprocess.Popen([sys.executable, '-c',
    'import signal,time; ' + ('signal.signal(signal.SIGTERM, signal.SIG_IGN); ' if sys.argv[2] == '1' else '') + 'time.sleep(60)'], start_new_session=True)
Path(sys.argv[1]).write_text(str(child.pid))
time.sleep(60)
""")
        supervisor = folder / "supervisor.py"
        supervisor.write_text("""import importlib.util,json,os,sys,uuid
from pathlib import Path
s=importlib.util.spec_from_file_location('managed',sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
session=dict(m.process_identity(os.getpid()),session_id=uuid.uuid4().hex)
Path(sys.argv[2]).write_text(json.dumps(session))
m.supervise_runtime([sys.executable,sys.argv[3],sys.argv[4],sys.argv[5]],dict(os.environ),session,graceful_seconds=0.2)
""")
        session = folder / "runtime-session.json"
        leaf = folder / "leaf.pid"
        process = subprocess.Popen([sys.executable, str(supervisor), str(HELPER), str(session), str(worker), str(leaf), "1" if ignore_term else "0"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, start_new_session=True)
        deadline = time.monotonic() + 5
        while not leaf.is_file():
            if process.poll() is not None:
                self.fail("Supervisor exited: " + process.communicate(timeout=2)[1])
            if time.monotonic() >= deadline:
                process.kill()
                self.fail("Supervisor did not start its descendant")
            time.sleep(0.02)
        return process, json.loads(session.read_text())

    def test_stop_collects_detached_children_and_never_signals_an_unrelated_process(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True)
            process = None
            session = None
            try:
                process, session = self.spawn(folder, ignore_term=True)
                self.assertGreaterEqual(len(runtime.owned_processes(session)), 2)
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=12)
                self.assertEqual(process.returncode, 0, process.communicate()[1])
                self.assertEqual(runtime.owned_processes(session), [])
                self.assertIsNone(unrelated.poll())
            finally:
                if session:
                    for identity in runtime.owned_processes(session):
                        runtime.signal_identity(identity, signal.SIGKILL)
                if process:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=3)
                unrelated.kill()
                unrelated.wait(timeout=3)

    def test_status_and_stop_recover_owned_children_after_the_supervisor_crashes(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            process, session = self.spawn(folder)
            try:
                process.kill()
                process.wait(timeout=3)
                instance = runtime.Runtime.__new__(runtime.Runtime)
                instance.base = folder
                instance.lock = folder / "runtime.lock"
                instance.current = folder / "current"
                instance.user_required = lambda: None
                self.assertTrue(instance.status()["running"], "Released lock hid detached live services")
                instance.stop()
                self.assertFalse(instance.status()["running"])
                self.assertFalse((folder / "runtime-session.json").exists())
            finally:
                for identity in runtime.owned_processes(session):
                    runtime.signal_identity(identity, signal.SIGKILL)
                process.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main()
