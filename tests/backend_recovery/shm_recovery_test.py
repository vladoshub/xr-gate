#!/usr/bin/env python3
"""Exercise real backends against capture SHM restarts and TCP disconnects."""
import argparse
import json
import mmap
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
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


def run_shm(binary, kind, mercury_library=None, recording_failure=False):
    with tempfile.TemporaryDirectory(prefix="xr-recovery-shm-") as tmp:
        case=Path(tmp)
        prefix=f"xr_recovery_{os.getpid()}_{kind}"
        cams=[Ring(prefix+f"_cam{i}") for i in range(2)]
        registry=case/"capture.json"
        registry.write_text(json.dumps({"streams":{f"camera{i}":ring.info for i,ring in enumerate(cams)}}))
        image=bytes(WIDTH*HEIGHT)
        for cam in cams: cam.write(1,time.monotonic_ns(),image)
        args=[str(binary),"--registry",str(registry),"--duration","3.0","--print-every","1"]
        if kind=="video":
            args += ["--input-transport","shm","--output","shm", "--video-registry",str(case/"video.json"),
                     "--video-shm-name",prefix+"_video"]
            if recording_failure:
                record=case/"record"; (record/"cam0").mkdir(parents=True)
                (record/"cam0/00000001.pgm").symlink_to("/dev/full")
                args += ["--record-dir",str(record)]
        else: args += ["--no-hand-shm"]
        if mercury_library:
            args.remove("--no-hand-shm")
            args += ["--hand-tracker","mercury","--mercury-runtime-lib",str(mercury_library),
                     "--hand-registry",str(case/"hands.json"),"--hand-shm-name",prefix+"_hands"]
        proc=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            for seq in range(2,90):
                time.sleep(.04)
                if proc.poll() is not None: break
                if seq==12:
                    for cam in cams: cam.close()
                    cams=[Ring(prefix+f"_cam{i}") for i in range(2)]
                ts=time.monotonic_ns()
                for cam in cams: cam.write(seq if seq<12 else seq-11,ts,image)
            log=proc.communicate(timeout=6)[0]
            match=re.search(r"^frames:\s+(\d+)",log,re.M)
            assert proc.returncode==0 and match and int(match.group(1))>25,log
            assert "reopened stereo input" in log,log
            if recording_failure:
                meta=json.loads((record/"metadata.json").read_text())
                assert not meta["finalized"] and meta["frames_written"]==0 and meta["error"],meta
                assert "recording stopped" in log,log
            if kind=="video":
                raw=(Path("/dev/shm")/(prefix+"_video")).read_bytes()
                assert struct.unpack_from("<Q",raw,36)[0]==int(match.group(1))
            if mercury_library:
                assert log.count("AUDIT mercury create called")>=2,log
                assert log.count("AUDIT mercury process called")==int(match.group(1)),log
            print(f"{kind} SHM restart passed ({match.group(1)} frames)",flush=True)
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
            for cam in cams: cam.close()
            (Path("/dev/shm")/(prefix+"_hands")).unlink(missing_ok=True)
            (Path("/dev/shm")/(prefix+"_video")).unlink(missing_ok=True)


def run_tcp(binary, kind):
    server=socket.socket(); server.bind(("127.0.0.1",0)); server.listen(2); server.settimeout(.1)
    port=server.getsockname()[1]
    stopping=threading.Event()
    sessions=[]
    def serve():
        while not stopping.is_set():
            try: conn,_=server.accept()
            except socket.timeout: continue
            except OSError: break
            sessions.append(1)
            with conn:
                conn.settimeout(.3)
                try:
                    hello=json.dumps({"streams":{"camera0":{},"camera1":{}}}).encode()
                    conn.sendall(f"CAPHELLO {len(hello)}\n".encode()+hello)
                    conn.recv(1024)
                    seq=0
                    while not stopping.is_set():
                        seq+=1
                        ts=time.monotonic_ns()
                        for side in range(2):
                            pixels=bytes(WIDTH*HEIGHT)
                            header=json.dumps(dict(stream_id=f"camera{side}",sequence=seq,timestamp_ns=ts,
                                monotonic_ns=ts,width=WIDTH,height=HEIGHT,format_code=1,flags=0,payload_size=len(pixels))).encode()
                            conn.sendall(f"CAPMSG1 {len(header)} {len(pixels)}\n".encode()+header+pixels)
                        if len(sessions)==1 and seq==12: break
                        time.sleep(.04)
                except (OSError,ConnectionError): pass
    thread=threading.Thread(target=serve); thread.start()
    args=[str(binary),"--tcp-host","127.0.0.1","--tcp-port",str(port),"--duration","3.2","--print-every","1"]
    if kind=="video": args += ["--input-transport","capture_tcp","--output","none"]
    else: args += ["--transport","capture_tcp","--no-hand-shm"]
    try:
        result=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=8)
        match=re.search(r"^frames:\s+(\d+)",result.stdout,re.M)
        assert result.returncode==0 and match and int(match.group(1))>25 and len(sessions)>=2,result.stdout
        print(f"{kind} TCP reconnect passed ({match.group(1)} frames)",flush=True)
    finally:
        stopping.set(); thread.join(timeout=2); server.close()


def run_runtime(binary):
    with tempfile.TemporaryDirectory(prefix="xr-recovery-runtime-") as tmp:
        case=Path(tmp)
        pose=Ring(f"xr_recovery_{os.getpid()}_hmd",pose=True)
        registry=case/"tracking.json"
        registry.write_text(json.dumps({"streams":{"hmd_pose":pose.info}}))
        def publish(seq):
            ts=time.monotonic_ns()
            reset=0 if seq<20 else (1 if seq<40 else 2)
            data=POSE.pack(1,POSE.size,seq,ts,ts,reset,0.,1.6,0.,1.,0.,0.,0.,
                           0.,0.,0.,0.,0.,0.,2,1,1.,0)
            pose.write(seq,ts,data)
        publish(1)
        proc=subprocess.Popen([str(binary),"--input","shm","--registry",str(registry),
            "--mode","tick","--tick-rate","90","--duration","1.2","--print-every","0",
            "--no-prediction","--consume-stale-as-lost"],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        try:
            for seq in range(2,70):
                time.sleep(.02)
                if proc.poll() is not None: break
                publish(seq)
            log=proc.communicate(timeout=5)[0]
            assert proc.returncode==0 and log.count("reason=hmd_reset_counter; yaw=reset")==2,log
            print("runtime HMD epochs 0 -> 1 -> 2 reset controller state exactly twice",flush=True)
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
            pose.close()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--video",type=Path,required=True)
    parser.add_argument("--hands",type=Path,required=True)
    parser.add_argument("--mercury-library",type=Path,required=True)
    parser.add_argument("--runtime",type=Path)
    args=parser.parse_args()
    for binary,kind in [(args.video,"video"),(args.hands,"hands")]:
        run_shm(binary.resolve(),kind)
        run_tcp(binary.resolve(),kind)
    run_shm(args.hands.resolve(),"mercury",args.mercury_library.resolve())
    run_shm(args.video.resolve(),"video",recording_failure=True)
    result=subprocess.run([str(args.hands.resolve()),"--hand-tracker","mercury","--hand-format-version","1"],
                          capture_output=True,text=True,timeout=2)
    assert result.returncode!=0 and "Mercury requires V2" in result.stderr,result
    print("Mercury V1 rejected before opening capture",flush=True)
    if args.runtime: run_runtime(args.runtime.resolve())

if __name__=="__main__": main()
