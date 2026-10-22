"""Real loopback TCP deadlines; runs on Linux pwsh and Windows PowerShell 5.1."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest

WIN = Path(__file__).resolve().parents[1]
SHELL = os.environ.get('XR_GATE_TEST_POWERSHELL') or shutil.which('powershell.exe' if os.name == 'nt' else 'pwsh')


@unittest.skipUnless(SHELL, 'PowerShell is required for TCP contract integration')
class TcpDeadlineTest(unittest.TestCase):
    def probe(self, mode):
        finished = threading.Event()
        with socket.socket() as listener, tempfile.TemporaryDirectory() as temporary:
            listener.bind(('127.0.0.1',0))
            listener.listen(2)
            listener.settimeout(8)
            def serve():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        if mode != 'partial_hello':
                            connection.sendall(b'{"protocol":"xr_gate_app_v1"}\n')
                        if mode == 'ready':
                            for index in range(1,4):
                                frame = {'type':'frame','sequence':index,'hmd':{'pose_valid':True,'source_timestamp_ns':str(index)}}
                                connection.sendall((json.dumps(frame)+'\n').encode())
                            finished.wait(6)
                        else:
                            while not finished.wait(0.05): connection.sendall(b' ')
                except (OSError,TimeoutError): pass
            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            script = Path(temporary)/'tcp.ps1'
            helper = str(WIN/'XrGate-Wsl.ps1').replace("'","''")
            script.write_text("$ErrorActionPreference='Stop'\n. '"+helper+"' -Action Help *> $null\n"
                              "$script:TcpHost='127.0.0.1'\n$script:TcpPort="+str(listener.getsockname()[1])+"\nWait-XrStream 1\n", encoding='utf-8-sig')
            started = time.monotonic()
            child = subprocess.Popen([SHELL,'-NoLogo','-NoProfile','-File',str(script)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                output, error = child.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate(timeout=3)
                self.fail('A partial TCP line bypassed the total probe deadline')
            finally:
                finished.set()
                thread.join(timeout=1)
            self.assertLess(time.monotonic()-started, 7, 'Probe deadline did not bound the whole operation')
            return child.returncode, (output+error).decode('utf-8',errors='replace')

    def test_partial_hello_and_frame_obey_total_deadline(self):
        for mode in ('partial_hello','partial_frame'):
            with self.subTest(mode=mode):
                code, output = self.probe(mode)
                self.assertNotEqual(code,0,output)
                self.assertIn('TCP',output)

    def test_fresh_valid_frames_still_pass(self):
        code, output = self.probe('ready')
        self.assertEqual(code,0,output)
        self.assertIn('pose_valid=true',output)


if __name__ == '__main__': unittest.main()
