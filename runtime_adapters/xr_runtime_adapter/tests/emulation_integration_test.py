#!/usr/bin/env python3
"""Exercise actual adapter transport, new photo poses, recording and wrist calibration."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import socket
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
import app_tcp_server_test as harness

TOOL = None
STATE = {}

def face_bits(bits, hand):
    return ((bits & ~24) | (2048 if bits & 8 else 0) | (4096 if bits & 16 else 0)) if hand == 'left' else bits


def coordinates(extended=0, thumb="up", tilt_x=0.0, tilt_y=0.0, left=False, finger_bends=None, offset=(0,0,0), v_spread=False, menu=None):
    joints = [[0.0, 0.0, 0.0] for _ in range(21)]
    joints[1:5] = [[.025,.015,0],[.035,.025,0],[.06,.032,.002],[.115,.04,.005]]
    for f,(x,y) in enumerate(zip((.04,0,-.02,-.04),(.07,.08,.075,.065))):
        for k in range(4):
            joints[5+4*f+k] = [x,y+k*.02,0]
        if not extended & (1<<f):
            joints[6+4*f] = [x,y+.02,0]
            joints[7+4*f] = [x,y+.015,-.025]
            joints[8+4*f] = [x,y,-.025]
    for finger, angles in (finger_bends or {}).items():
        base = 5+4*int(finger)
        x, y, z = joints[base]
        for k, (angle, length) in enumerate(zip(angles, (.025,.020,.017)), 1):
            y += length*math.cos(math.radians(angle))
            z -= length*math.sin(math.radians(angle))
            joints[base+k] = [x,y,z]
    if thumb == "down":
        joints[4] = [.005,.045,.005]
    if thumb == "pinch":
        joints[6:9] = [[.04,.10,0],[.04,.115,-.02598],[.04,.109795,-.05553]]
        joints[4] = [joints[8][0]+.006,joints[8][1],joints[8][2]]
    if thumb == "open_pinch":
        base=list(joints[1]);tip=[joints[8][0]+.006,joints[8][1],joints[8][2]]
        for k in range(1,4):joints[1+k]=[a+(b-a)*k/3 for a,b in zip(base,tip)]
    if thumb == "thumbs_up":
        joints[1:5]=[[.05,y,0] for y in (.015,.055,.10,.145)]
    if v_spread:
        for f in range(2):
            for k in range(1,4):joints[5+4*f+k][0]+=(1 if f==0 else -1)*k*.012
    output = []
    for x,y,z in joints:
        if left:
            x = -x
        if menu:
            sign=1 if left or menu=="parallel" else -1
            output.append([sign*y+(-.2 if left else .2)+offset[0],1+z+offset[1],-.4+sign*x+offset[2]])
            continue
        y,z = (y+z)/math.sqrt(2), (z-y)/math.sqrt(2)
        x1,y1 = x*math.cos(tilt_y)+y*math.sin(tilt_y), -x*math.sin(tilt_y)+y*math.cos(tilt_y)
        output.append([x1+offset[0],y1*math.cos(tilt_x)+z*math.sin(tilt_x)+offset[1],
            -y1*math.sin(tilt_x)+z*math.cos(tilt_x)+offset[2]])
    return output


def source_side(side, visible):
    config = STATE["left" if side == 1 else "right"]
    visible = visible and config.get("visible", True)
    p = [0.1*side,1.0,-.3,1.0,0.0,0.0,0.0]
    base = harness.HAND_SIDE.pack(side,int(visible),57 if visible else 0,float(visible),
        *(p*3+[0.0]*6+[1.0,1.0]),1,1,21,8)
    options = {k:v for k,v in config.items() if k != "visible"}
    joints = coordinates(**options,left=side==1)
    return base+b"".join(harness.JOINT.pack(i,2,*p,1,0,0,0,.008,1) for i,p in enumerate(joints))

harness.hand_side = source_side


class ControllerSource:
    """Feed the same ControllerInputV3 TCP ABI as override_controller."""
    device = struct.Struct("<4I3Q6f2Q32I32I64s288s")
    frame = struct.Struct("<2I4Q4I")
    header = struct.Struct("<IHHIIQQ")

    def __init__(self):
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.paused = threading.Event()
        self.sides = {name: dict(present=False, buttons=0, touches=0, trigger=0, grip=0,
            x=0, y=0, press=[0]*32, release=[0]*32) for name in ("left", "right")}
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.server.listen()
        self.server.settimeout(.05)
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def update(self, name, **values):
        with self.lock:
            side = self.sides[name]
            before = side["buttons"]
            side.update(values)
            for i in range(32):
                if (before ^ side["buttons"]) & (1 << i):
                    side["press" if side["buttons"] & (1 << i) else "release"][i] += 1

    def run(self):
        sequence = 0
        while not self.stop.is_set():
            try:
                client, _ = self.server.accept()
            except socket.timeout:
                continue
            with client:
                client.settimeout(.1)
                while not self.stop.is_set():
                    if not self.paused.is_set():
                        sequence += 1
                        stamp = time.monotonic_ns()
                        payload = self.frame.pack(3, 1432, sequence, stamp, stamp, 0, 0, 0, 0, 0)
                        with self.lock:
                            for role, name in enumerate(("left", "right"), 1):
                                side = self.sides[name]
                                payload += self.device.pack(2 if side["present"] else 4, role,
                                    6 | (24 if side.get("imu") else 0), 3,
                                    side["buttons"], side["touches"], 0, side["trigger"], side["grip"],
                                    side["x"], side["y"], 0, 0, role+40, role+40,
                                    *side["press"], *side["release"], bytes(64),
                                    side["imu"](sequence,stamp) if side.get("imu") else bytes(288))
                        try:
                            client.sendall(self.header.pack(0x43495633, 3, 32, len(payload), 0,
                                sequence, stamp)+payload)
                        except OSError:
                            break
                    self.stop.wait(1/240)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join(timeout=2)
        self.server.close()


class EmulationIntegrationTest(unittest.TestCase):
    def setUp(self):
        STATE.clear()
        STATE.update(left={},right={})

    def test_default_simple_radius_and_outward_yaw(self):
        snapshots=[]
        for extra in ([],["--simple-gestures-pose-yaw-offset-degrees","0"]):
            STATE.update(left=dict(extended=15,tilt_x=1.2+math.pi),
                         right=dict(extended=15,tilt_x=1.2+math.pi))
            with harness.runtime(extra=extra,default_mode=True) as (_,_,port):
                client=harness.Client(port)
                try:
                    f=client.frame(lambda f:f.get("controllers") and all(
                        int(f["controllers"][n]["touches"])==32 for n in ("left","right")))
                    snapshots.append(f)
                    STATE["right"]["offset"]=(.05,0,0)
                    client.frame(lambda f:f.get("controllers") and
                        abs(f["controllers"]["right"]["thumbstick"][0]-.5)<.001)
                finally:client.close()
        for name,sign in (("left",1),("right",-1)):
            base=snapshots[1]["controllers"][name]["orientation_xyzw"]
            actual=snapshots[0]["controllers"][name]["orientation_xyzw"]
            x,y,z,w=base;c=math.cos(math.pi/12);s=sign*math.sin(math.pi/12)
            expected=[c*x+s*z,c*y+s*w,c*z-s*x,c*w-s*y]
            self.assertAlmostEqual(abs(sum(a*b for a,b in zip(actual,expected))),1,places=5)
            # The published skeleton and controller use the same correction.
            self.assertNotEqual(snapshots[0]["hands"][name]["joints"][8]["position"],
                                snapshots[1]["hands"][name]["joints"][8]["position"])

    def test_simple_panel_pinch_palm_grip_and_v_keep_live_axes(self):
        away=1.2+math.pi
        def pose(name,options,dx):
            left=name=="left"
            initial=coordinates(extended=15,tilt_x=away,left=left)
            current=coordinates(**options,left=left)
            # Keep the same palm centre while rotating the synthetic hand, then
            # move 2.5/5 cm to verify axes alongside each recognized button.
            offset=tuple(sum(initial[j][k]-current[j][k] for j in (0,5,9,17))/4
                         +(dx if k==0 else 0) for k in range(3))
            return dict(options,offset=offset)
        STATE.update(left=dict(extended=15,tilt_x=away),right=dict(extended=15,tilt_x=away))
        with harness.runtime(default_mode=True) as (_,_,port):
            client=harness.Client(port)
            try:
                client.frame(lambda f:f.get("controllers") and all(
                    int(f["controllers"][n]["touches"])&32 for n in ("left","right")))
                for options,lbit,rbit,trigger,grip in [
                    (dict(extended=0,thumb="pinch",tilt_x=away),1,1,1,0),
                    (dict(extended=15,tilt_x=1.2),2,2,0,1),
                    (dict(extended=3,v_spread=True,tilt_x=away),32,32,0,0),
                    (dict(extended=3,v_spread=True,thumb="open_pinch",tilt_x=away),32,32,0,0)]:
                    frames=[]
                    for dx in (.025,.05):
                        STATE.update({n:pose(n,options,dx) for n in ("left","right")})
                        f=client.frame(lambda f:f.get("controllers") and all(
                            int(f["controllers"][n]["buttons"])==bit and
                            abs(f["controllers"][n]["thumbstick"][0]-dx/.10)<.001
                            for n,bit in (("left",lbit),("right",rbit))))
                        frames.append(f)
                        for n in ("left","right"):
                            c=f["controllers"][n]
                            self.assertTrue(int(c["touches"])&32)
                            self.assertEqual(c["trigger"],trigger);self.assertEqual(c["grip"],grip)
                    for n in ("left","right"):
                        self.assertEqual(frames[0]["controllers"][n]["press_counters"],
                                         frames[1]["controllers"][n]["press_counters"])
                STATE.update(left=dict(extended=0,thumb="down",tilt_x=away),
                             right=dict(extended=0,thumb="down",tilt_x=away))
                f=client.frame(lambda f:f.get("controllers") and all(
                    int(f["controllers"][n]["buttons"])==0 and
                    int(f["controllers"][n]["touches"])==32 for n in ("left","right")))
                for n in ("left","right"):
                    self.assertEqual(f["controllers"][n]["trigger"],0)
                    self.assertEqual(f["controllers"][n]["grip"],0)
            finally:client.close()

    def test_thumb_up_exits_panel_without_sending_face_buttons(self):
        away=1.2+math.pi
        STATE.update(left=dict(extended=15,tilt_x=away),right=dict(extended=15,tilt_x=away))
        with harness.runtime(default_mode=True) as (_,_,port):
            client=harness.Client(port)
            try:
                def both(predicate):
                    return client.frame(lambda f:f.get("controllers") and all(
                        predicate(f["controllers"][n]) for n in ("left","right")))
                both(lambda c:int(c["touches"])==32)
                STATE.update(left=dict(extended=0,thumb="thumbs_up",tilt_x=away),
                             right=dict(extended=0,thumb="thumbs_up",tilt_x=away))
                both(lambda c:int(c["touches"])==0 and int(c["buttons"])==0 and c["thumbstick"]==[0,0])
                until=time.monotonic()+.15
                while time.monotonic()<until:
                    f=client.frame(lambda f:f.get("controllers"))
                    for name in ("left","right"):
                        self.assertEqual(int(f["controllers"][name]["buttons"]),0)
                        self.assertEqual(int(f["controllers"][name]["touches"]),0)
                # Release the exit pose, then the normal thumbs-up still sends Y/B.
                STATE.update(left=dict(extended=3,v_spread=True),right=dict(extended=3,v_spread=True))
                client.frame(lambda f:f.get("controllers") and int(f["controllers"]["left"]["buttons"])==2048
                             and int(f["controllers"]["right"]["buttons"])==8)
                STATE.update(left=dict(extended=0,thumb="thumbs_up"),right=dict(extended=0,thumb="thumbs_up"))
                client.frame(lambda f:f.get("controllers") and int(f["controllers"]["left"]["buttons"])==4096
                             and int(f["controllers"]["right"]["buttons"])==16)
                STATE.update(left=dict(extended=15,tilt_x=away,offset=(.02,0,0)),
                             right=dict(extended=15,tilt_x=away,offset=(.02,0,0)))
                both(lambda c:int(c["touches"])==32 and c["thumbstick"]==[0,0])
            finally:client.close()

    def test_simple_free_wrist_rotation_and_right_axes_without_left_tracking(self):
        away=1.2+math.pi
        STATE.update(left=dict(extended=15,tilt_x=away),right=dict(extended=15,tilt_x=away))
        with harness.runtime(default_mode=True) as (_,source,port):
            client=harness.Client(port)
            try:
                def right(predicate):
                    return client.frame(lambda f:f.get("controllers") and predicate(f["controllers"]["right"]))
                right(lambda c:int(c["touches"])==32)
                STATE["right"]=dict(extended=3,v_spread=True,tilt_x=away,offset=(.05,0,0))
                right(lambda c:int(c["buttons"])==32 and abs(c["thumbstick"][0]-.5)<.001)
                # Closing the fingers does not change the palm normal; turn the
                # whole V pose out of the entry cone without releasing the panel.
                STATE["right"]["tilt_x"]=away+math.pi/2
                f=right(lambda c:int(c["touches"])==32 and int(c["buttons"])==32)
                self.assertEqual(f["controllers"]["right"]["release_counters"][5],0)
                STATE["right"]["visible"]=False
                f=right(lambda c:int(c["touches"])==0 and c["thumbstick"]==[0,0])
                self.assertEqual(f["controllers"]["right"]["release_counters"][5],1)
                self.assertEqual(int(f["controllers"]["left"]["touches"]),32)
                STATE["right"]=dict(extended=15,tilt_x=away,offset=(.02,0,0))
                right(lambda c:int(c["touches"])==32 and c["thumbstick"]==[0,0])
                STATE["left"]["visible"]=False;source.hand_count=1
                for x in (.07,-.03,.07):
                    STATE["right"]["offset"]=(x,0,0)
                    expected=(x-.02)/.10
                    f=client.frame(lambda f:f.get("controllers") and f.get("hands") and
                        f["hands"]["left"]["status"]!=1 and
                        abs(f["controllers"]["right"]["thumbstick"][0]-expected)<.001)
                    self.assertTrue(f["controllers"]["right"]["input_valid"])
                    self.assertTrue(f["controllers"]["right"]["pose_valid"])
                    self.assertEqual(int(f["controllers"]["right"]["touches"]),32)
            finally:client.close()

    def test_simple_touchpad_does_not_steal_open_pinch_or_claw(self):
        pose=dict(extended=15,thumb="open_pinch",tilt_x=1.2+math.pi)
        STATE.update(left=dict(pose),right=dict(pose))
        with harness.runtime(extra=["--simple-gestures"]) as (_,_,port):
            client=harness.Client(port)
            try:
                def pair(predicate):
                    return client.frame(lambda f:f.get("controllers") and all(
                        predicate(f["controllers"][name]) for name in ("left","right")))
                pair(lambda c:c["trigger"]==1 and int(c["touches"])==0)
                # The loose gameplay extension threshold sees these straight
                # phalanges as open; all MCP joints are actually bent 60 deg.
                pose=dict(extended=15,tilt_x=1.2+math.pi,
                          finger_bends={f:[60,60,60] for f in range(4)})
                STATE.update(left=dict(pose),right=dict(pose))
                pair(lambda c:c["trigger"]==0 and int(c["touches"])==0)
                for _ in range(8):
                    f=client.frame(lambda f:f.get("controllers"))
                    for name in ("left","right"):
                        self.assertEqual(int(f["controllers"][name]["touches"]),0)
                STATE.update(left=dict(extended=15,tilt_x=1.2+math.pi),
                             right=dict(extended=15,tilt_x=1.2+math.pi))
                pair(lambda c:int(c["touches"])==32 and c["thumbstick"]==[0,0])
                STATE.update(left=dict(extended=3,v_spread=True,tilt_x=1.2+math.pi,offset=(.15,.15,0)),
                             right=dict(extended=3,v_spread=True,tilt_x=1.2+math.pi,offset=(.15,.15,0)))
                pair(lambda c:int(c["buttons"])==32 and c["trigger"]==0 and c["grip"]==0
                     and all(abs(v-.5)<.001 for v in c["thumbstick"]))
            finally:client.close()

    def test_simple_touchpad_axes_click_and_frozen_centre(self):
        STATE.update(left=dict(extended=15,tilt_x=1.2+math.pi),right={})
        with harness.runtime(extra=["--simple-gestures"]) as (_,_,port):
            client=harness.Client(port)
            def right(predicate):
                return client.frame(lambda f:f.get("controllers") and predicate(f["controllers"]["right"]))["controllers"]["right"]
            try:
                initial=right(lambda c:c["input_valid"])
                self.assertEqual(int(initial["buttons"])&32,0)  # A cold fist cannot click.
                STATE["right"]=dict(extended=15,tilt_x=1.2+math.pi)
                centred=right(lambda c:int(c["touches"])&32)
                self.assertEqual(centred["thumbstick"],[0,0]);self.assertEqual(int(centred["buttons"]),0)
                STATE["right"]["offset"]=(.15,0,0)
                shifted=right(lambda c:abs(c["thumbstick"][0]-.5)<.001)
                self.assertEqual(shifted["grip"],0);self.assertEqual(shifted["trigger"],0)
                STATE["right"]["offset"]=(.6,.6,0)
                diagonal=right(lambda c:c["thumbstick"][0]>.7 and c["thumbstick"][1]>.7)
                self.assertAlmostEqual(math.hypot(*diagonal["thumbstick"]),1,places=4)
                STATE["right"]=dict(extended=3,v_spread=True,tilt_x=1.2+math.pi,offset=(.15,0,0))
                click=right(lambda c:int(c["buttons"])==32)
                self.assertEqual(click["press_counters"][5],1)
                self.assertAlmostEqual(click["thumbstick"][0],.5,places=4)
                STATE["right"]=dict(extended=15,tilt_x=1.2+math.pi,offset=(.15,0,0))
                released=right(lambda c:int(c["buttons"])==0 and c["release_counters"][5]==1)
                self.assertTrue(int(released["touches"])&32)
                self.assertAlmostEqual(released["thumbstick"][0],.5,places=4)
                STATE["right"]=dict(extended=15,tilt_x=1.2)
                right(lambda c:int(c["touches"])&32 and c["grip"]==1)
                STATE["right"]["visible"]=False
                right(lambda c:int(c["touches"])==0)
                STATE["right"]=dict(extended=15,tilt_x=1.2+math.pi,offset=(.15,0,0))
                right(lambda c:int(c["touches"])&32 and c["thumbstick"]==[0,0])
            finally:client.close()

    def test_simple_touchpad_head_motion_loss_and_rearming(self):
        STATE.update(left={},right=dict(extended=15,tilt_x=1.2+math.pi))
        with harness.runtime(extra=["--simple-gestures","--max-hmd-age-ms","80"]) as (_,source,port):
            client=harness.Client(port)
            def right(predicate):
                return client.frame(lambda f:f.get("controllers") and predicate(f["controllers"]["right"]))["controllers"]["right"]
            try:
                right(lambda c:int(c["touches"])&32)
                STATE["right"]["offset"]=(.15,0,0)
                right(lambda c:abs(c["thumbstick"][0]-.5)<.001)
                source.hmd_position=(.4,1.5,-.75)
                right(lambda c:int(c["touches"])&32 and abs(c["thumbstick"][0])<.001)
                STATE["right"].update(extended=3,v_spread=True)
                right(lambda c:int(c["buttons"])==32)
                source.send_head=False
                lost=right(lambda c:int(c["touches"])==0 and not int(c["buttons"])&32)
                self.assertEqual(lost["release_counters"][5],1)
                source.send_head=True
                until=time.monotonic()+.15
                while time.monotonic()<until:
                    c=right(lambda c:True)
                    self.assertEqual(int(c["buttons"])&32,0);self.assertEqual(int(c["touches"])&32,0)
                STATE["right"]["extended"]=15
                right(lambda c:int(c["touches"])&32 and c["thumbstick"]==[0,0])
            finally:client.close()

    def test_simple_touchpad_can_be_disabled_by_flag_or_environment(self):
        import os
        from unittest.mock import patch
        for flags,env in [(["--simple-gestures-touchpad","0"],{}),([],{"SIMPLE_GESTURES_TOUCHPAD":"0"})]:
            STATE.update(left=dict(extended=15,tilt_x=1.2+math.pi),right=dict(extended=15,tilt_x=1.2+math.pi))
            with self.subTest(flags=flags),patch.dict(os.environ,env),harness.runtime(extra=["--simple-gestures",*flags]) as (_,_,port):
                client=harness.Client(port)
                try:
                    until=time.monotonic()+.12
                    while time.monotonic()<until:
                        f=client.frame(lambda f:f.get("controllers") is not None)
                        for name in ("left","right"):
                            self.assertEqual(f["controllers"][name]["thumbstick"],[0,0])
                            self.assertEqual(int(f["controllers"][name]["touches"])&32,0)
                finally:client.close()

    def test_independent_extension_and_wrist_in_real_adapter(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Path(tmp)/"profile.json"
            profile.write_text(json.dumps({"version":2,"smoothing_ms":0,"left":{"enabled":False}}))
            with harness.runtime(extra=["--emulation-controller","1","--emulation-controller-config",str(profile)]) as (_,_,port):
                client = harness.Client(port)
                try:
                    neutral = client.frame(lambda f:f["controllers"] is not None and f["hands"] is not None)
                    self.assertEqual(int(neutral["controllers"]["right"]["buttons"]),0)
                    self.assertEqual(neutral["controllers"]["left"]["trigger"],1)
                    self.assertFalse(neutral["hands"]["right"]["pinch_active"])
                    STATE["right"].update(extended=9,tilt_y=.65)
                    moving = client.frame(lambda f:f["controllers"] is not None and
                        f["controllers"]["right"]["thumbstick"][0]>.8 and int(f["controllers"]["right"]["buttons"])&3==3)
                    self.assertEqual(int(moving["controllers"]["right"]["buttons"])&8,0)
                    STATE["right"]["thumb"] = "down"
                    stopped = client.frame(lambda f:f["controllers"] is not None and
                        f["controllers"]["right"]["thumbstick"]==[0,0] and int(f["controllers"]["right"]["buttons"])&3==3)
                    self.assertEqual(int(stopped["controllers"]["right"]["buttons"])& (15<<6),0)
                    STATE["right"]["visible"] = False
                    lost = client.frame(lambda f:f["controllers"] is not None and not f["controllers"]["right"]["input_valid"])
                    self.assertEqual(int(lost["controllers"]["right"]["buttons"]),0)
                    STATE["right"] = {}
                    client.frame(lambda f:f["controllers"] is not None and f["controllers"]["right"]["input_valid"])
                finally:
                    client.close()

    def test_forward_backward_and_natural_grip_in_real_adapter(self):
        with harness.runtime(extra=["--emulation-controller", "1"]) as (_,_,port):
            client = harness.Client(port)
            def right_when(predicate):
                return client.frame(lambda f: f["controllers"] is not None and
                    predicate(f["controllers"]["right"]))["controllers"]["right"]
            try:
                right_when(lambda c: c["input_valid"] and c["thumbstick"] == [0,0])
                STATE["right"] = dict(extended=9, tilt_x=.65)
                out = right_when(lambda c: c["thumbstick"][1] > .8 and int(c["buttons"]) & 3 == 3)
                self.assertTrue(int(out["buttons"]) & 64)
                self.assertFalse(int(out["buttons"]) & 128)
                STATE["right"]["tilt_x"] = -.65
                out = right_when(lambda c: c["thumbstick"][1] < -.8 and int(c["buttons"]) & 3 == 3)
                self.assertTrue(int(out["buttons"]) & 128)
                for folded in ([35,75,125], [80,80,80]):
                    STATE["right"] = dict(extended=8, finger_bends={"1":folded, "2":folded, "3":[5,15,25]})
                    out = right_when(lambda c: int(c["buttons"]) & 3 == 2)
                    self.assertGreater(out["grip"], .9)
                    STATE["right"] = dict(extended=8, finger_bends={"1":folded, "2":folded, "3":[85,85,85]})
                    out = right_when(lambda c: int(c["buttons"]) & 3 == 0)
                    self.assertEqual(out["grip"], 0)
            finally:
                client.close()

    def test_new_photo_poses_are_exclusive(self):
        with harness.runtime(extra=["--emulation-controller","1"]) as (_,_,port):
            client = harness.Client(port)
            try:
                client.frame(lambda f:f["controllers"] is not None and f["controllers"]["right"]["input_valid"])
                for pose,expected in (({"extended":0,"thumb":"pinch"},8),
                                      ({"extended":14,"thumb":"pinch"},16),
                                      ({"extended":3,"thumb":"down"},32),
                                      ({"extended":3,"thumb":"up"},32)):
                    STATE["right"] = pose
                    pressed = client.frame(lambda f:f["controllers"] is not None and int(f["controllers"]["right"]["buttons"])==expected)
                    out = pressed["controllers"]["right"]
                    self.assertEqual(out["trigger"],0)
                    self.assertEqual(out["grip"],0)
                    self.assertEqual(out["thumbstick"],[0,0])
                    self.assertEqual(int(pressed["controllers"]["left"]["buttons"]),0)
                    STATE["right"] = {"thumb":"down"}
                    client.frame(lambda f:f["controllers"] is not None and int(f["controllers"]["right"]["buttons"])==0)
            finally:
                client.close()

    def test_menu_requires_both_open_hands(self):
        with harness.runtime(extra=["--emulation-controller","1"]) as (_,_,port):
            client = harness.Client(port)
            try:
                client.frame(lambda f:f["controllers"] is not None and f["controllers"]["right"]["input_valid"])
                STATE["left"] = {"extended":15}
                deadline = time.monotonic()+.45
                while time.monotonic()<deadline:
                    f = client.frame(lambda f:f["controllers"] is not None)
                    self.assertEqual(int(f["controllers"]["left"]["buttons"])&4,0)
                    self.assertEqual(int(f["controllers"]["right"]["buttons"])&4,0)
                STATE["right"] = {"extended":15}
                shown = client.frame(lambda f:f["controllers"] is not None and int(f["controllers"]["left"]["buttons"])==4)
                self.assertEqual(int(shown["controllers"]["right"]["buttons"]),0)
                self.assertEqual(shown["controllers"]["left"]["trigger"],0)
                self.assertEqual(shown["controllers"]["right"]["grip"],0)
                STATE["right"]["visible"] = False
                released = client.frame(lambda f:f["controllers"] is not None and not f["controllers"]["right"]["input_valid"])
                self.assertEqual(int(released["controllers"]["left"]["buttons"]),0)
            finally:
                client.close()

    def test_simple_fist_combines_trigger_and_grip_without_legacy_a(self):
        with harness.runtime(extra=["--simple-gestures"]) as (_,_,port):
            client = harness.Client(port)
            try:
                f = client.frame(lambda f:f.get("controllers") and int(f["controllers"]["right"]["buttons"])==3)
                self.assertEqual(f["controllers"]["right"]["trigger"],1)
                self.assertEqual(f["controllers"]["right"]["grip"],1)
                self.assertEqual(int(f["controllers"]["left"]["buttons"]),3)
            finally:
                client.close()

    def test_simple_v_thumb_fist_pinch_and_two_hand_pointing_menu(self):
        with harness.runtime(extra=["--simple-gestures"]) as (_,_,port):
            client=harness.Client(port)
            def pair(left,right):
                return client.frame(lambda f:f.get("controllers") and
                    int(f["controllers"]["left"]["buttons"])==left and int(f["controllers"]["right"]["buttons"])==right)
            try:
                for pose,left,right in [
                    (dict(extended=3,thumb="down",v_spread=True),2048,8),
                    (dict(extended=0,thumb="thumbs_up"),4096,16),
                    (dict(extended=0,thumb="down"),3,3),
                    (dict(extended=0,thumb="pinch"),1,1)]:
                    STATE["left"]=dict(pose);STATE["right"]=dict(pose)
                    f=pair(left,right)
                    if right in (8,16):
                        self.assertEqual(f["controllers"]["left"]["trigger"],0)
                        self.assertEqual(f["controllers"]["right"]["grip"],0)
                    STATE["left"]=dict(extended=1,thumb="down")
                    STATE["right"]=dict(extended=1,thumb="down")
                    pair(0,0)
                STATE["left"]=dict(extended=1,thumb="down",menu="inward")
                STATE["right"]=dict(extended=1,thumb="down",menu="inward")
                f=pair(4,0)
                self.assertEqual(f["controllers"]["left"]["press_counters"][2],1)
                STATE["right"]=dict(extended=1,thumb="down",menu="parallel")
                pair(0,0)
                STATE["right"]=dict(extended=1,thumb="down",menu="inward",visible=False)
                pair(0,0)
            finally:client.close()

    def test_simple_palm_grip_on_both_hands_and_release(self):
        STATE.update(left=dict(extended=15,tilt_x=1.2),right=dict(extended=15,tilt_x=1.2))
        with harness.runtime(extra=["--simple-gestures"]) as (_,source,port):
            client=harness.Client(port)
            try:
                both=client.frame(lambda f:f.get("controllers") and all(
                    f["controllers"][name]["grip"]==1 for name in ("left","right")))
                for name in ("left","right"):
                    self.assertEqual(both["hands"][name]["grab_active"],1)
                    self.assertTrue(int(both["controllers"][name]["buttons"])&2)
                    self.assertEqual(int(both["controllers"][name]["buttons"])&4,0)
                # Turning away releases only this hand; the other still holds.
                STATE["right"]["tilt_x"]=1.2+math.pi
                client.frame(lambda f:f.get("controllers") and f["controllers"]["right"]["grip"]==0
                    and f["controllers"]["left"]["grip"]==1)
                # A tucked thumb is not a spread palm. Source grab=1 cannot bypass this.
                STATE["left"]["thumb"]="down"
                client.frame(lambda f:f.get("controllers") and all(
                    f["controllers"][name]["grip"]==0 for name in ("left","right")))
                STATE["left"]=dict(extended=15,tilt_x=1.2)
                client.frame(lambda f:f.get("controllers") and f["controllers"]["left"]["grip"]==1)
                source.visible=False
                client.frame(lambda f:f.get("controllers") and f["controllers"]["left"]["grip"]==0)
            finally:client.close()

    def test_disabling_emulation_without_simple_keeps_old_fist_grip(self):
        with harness.runtime(extra=["--emulation-controller","0"]) as (_,_,port):
            client=harness.Client(port)
            try:
                f=client.frame(lambda f:f.get("controllers") is not None)
                self.assertEqual(f["controllers"]["right"]["grip"],1)
            finally:client.close()

    def test_simple_respects_physical_controller_block_and_passes_physical_grip(self):
        STATE.update(left=dict(extended=15,tilt_x=1.2+math.pi),right=dict(extended=15,tilt_x=1.2+math.pi))
        with ControllerSource() as physical:
            physical.update("right",present=True,grip=.8,touches=32,x=-.4,y=.3)
            args=["--simple-gestures","--controller-input-mode","controller_buttons_runtime_only",
                  "--controller-input-transport","tcp","--controller-input-host","127.0.0.1",
                  "--controller-input-port",str(physical.port)]
            with harness.runtime(extra=args) as (_,_,port):
                client=harness.Client(port)
                try:
                    f=client.frame(lambda f:f.get("controllers") and
                        abs(f["controllers"]["right"]["grip"]-.8)<.001)
                    self.assertEqual(f["controllers"]["left"]["grip"],0)
                    self.assertEqual(int(f["controllers"]["left"]["touches"]),0)
                    self.assertEqual(int(f["controllers"]["right"]["touches"]),32)
                    self.assertAlmostEqual(f["controllers"]["right"]["thumbstick"][0],-.4,places=4)
                    self.assertAlmostEqual(f["controllers"]["right"]["thumbstick"][1],.3,places=4)
                    physical.update("right",buttons=0,grip=0)
                    client.frame(lambda f:f.get("controllers") and f["controllers"]["right"]["grip"]==0)
                    until=time.monotonic()+.15
                    while time.monotonic()<until:
                        f=client.frame(lambda f:f.get("controllers") is not None)
                        self.assertEqual(f["controllers"]["left"]["grip"],0)
                        self.assertEqual(f["controllers"]["right"]["grip"],0)
                finally:client.close()

    def test_override_controller_buttons_axes_and_loss(self):
        for mode in ("hand_plus_controller", "controller_buttons_runtime_only"):
            with self.subTest(mode=mode), ControllerSource() as physical:
                STATE.update(left={}, right={})
                physical.update("right", present=True)
                args = ["--emulation-controller", "1", "--controller-input-mode", mode,
                    "--controller-input-transport", "tcp", "--controller-input-host", "127.0.0.1",
                    "--controller-input-port", str(physical.port), "--max-controller-age-ms", "80"]
                if mode == "controller_buttons_runtime_only":
                    args += ["--override-controller-block-gestures-while-stream-present", "0"]
                with harness.runtime(extra=args) as (_, _, port):
                    client = harness.Client(port)
                    def right_when(predicate):
                        return client.frame(lambda f: f["controllers"] is not None and
                            predicate(f["controllers"]["right"]))["controllers"]["right"]
                    try:
                        # Neutral hands have backend pinch/grab/button A set in
                        # the source. Those old gestures must stay suppressed.
                        right_when(lambda c: int(c["source_device_hash"]) == 42 and int(c["buttons"]) == 0)
                        physical.update("right", buttons=16)
                        out = right_when(lambda c: int(c["buttons"]) == 16)
                        self.assertEqual(out["press_counters"][4], 1)
                        self.assertTrue(out["source_mask"] & 16)
                        STATE["right"] = dict(extended=9, tilt_y=.65)
                        out = right_when(lambda c: int(c["buttons"]) & 19 == 19 and c["thumbstick"][0] > .8)
                        physical.update("right", x=-.7, y=.2, touches=32)
                        out = right_when(lambda c: abs(c["thumbstick"][0]+.7) < 1e-5)
                        self.assertAlmostEqual(out["thumbstick"][1], .2, places=5)
                        self.assertEqual(int(out["buttons"]) & (15 << 6), 0)
                        physical.update("right", x=0, y=0)
                        right_when(lambda c: c["thumbstick"] == [0, 0] and int(c["buttons"]) & 19 == 19)
                        physical.update("right", touches=0)
                        right_when(lambda c: c["thumbstick"][0] > .8)
                        STATE["right"]["visible"] = False
                        out = right_when(lambda c: int(c["buttons"]) == 16 and c["thumbstick"] == [0, 0])
                        self.assertTrue(out["input_valid"])
                        self.assertEqual(out["trigger"], 0)
                        physical.update("right", buttons=4)
                        out = right_when(lambda c: int(c["buttons"]) == 4)
                        self.assertEqual(out["release_counters"][4], 1)
                        # A fresh stream reporting the device lost releases it.
                        physical.update("right", present=False)
                        out = right_when(lambda c: int(c["buttons"]) == 0 and not c["input_valid"])
                        self.assertEqual(out["release_counters"][2], 1)
                        physical.update("right", present=True, buttons=16)
                        right_when(lambda c: int(c["buttons"]) == 16)
                        # Staleness releases held hardware input too.
                        physical.paused.set()
                        right_when(lambda c: int(c["buttons"]) == 0 and not c["input_valid"])
                        physical.paused.clear()
                        physical.update("right", buttons=0)
                        STATE["right"] = dict(extended=9)
                        right_when(lambda c: int(c["buttons"]) & 3 == 3)
                    finally:
                        client.close()

    def test_override_blocks_new_gestures_by_default_and_keeps_latch(self):
        with ControllerSource() as physical:
            physical.update("right", present=True, buttons=16)
            STATE["right"] = dict(extended=9, tilt_y=.65)
            args = ["--emulation-controller", "1", "--controller-input-mode", "controller_buttons_runtime_only",
                "--controller-input-transport", "tcp", "--controller-input-host", "127.0.0.1",
                "--controller-input-port", str(physical.port), "--max-controller-age-ms", "80"]
            with harness.runtime(extra=args) as (_, _, port):
                client = harness.Client(port)
                def states():
                    return client.frame(lambda f: f["controllers"] is not None)["controllers"]
                try:
                    f = client.frame(lambda f: f["controllers"] is not None and
                        int(f["controllers"]["right"]["buttons"]) == 16)
                    self.assertEqual(f["controllers"]["right"]["thumbstick"], [0, 0])
                    physical.update("right", buttons=0)
                    client.frame(lambda f: f["controllers"] is not None and
                        int(f["controllers"]["right"]["buttons"]) == 0)
                    # Even idle hardware must own the input in this mode.
                    STATE["left"], STATE["right"] = dict(extended=15), dict(extended=15)
                    until = time.monotonic()+.45
                    while time.monotonic() < until:
                        current = states()
                        self.assertEqual(int(current["left"]["buttons"]), 0)
                        self.assertEqual(int(current["right"]["buttons"]), 0)
                    physical.update("right", buttons=4)
                    client.frame(lambda f: f["controllers"] is not None and
                        int(f["controllers"]["right"]["buttons"]) == 4)
                    STATE["left"], STATE["right"] = {}, dict(extended=9)
                    physical.paused.set()
                    # No gestures leak during the default 2000 ms latch after
                    # physical input has expired (80 ms in this test).
                    client.frame(lambda f: f["controllers"] is not None and
                        not f["controllers"]["right"]["input_valid"])
                    until = time.monotonic()+.2
                    while time.monotonic() < until:
                        self.assertEqual(int(states()["right"]["buttons"]), 0)
                    client.frame(lambda f: f["controllers"] is not None and
                        int(f["controllers"]["right"]["buttons"]) & 3 == 3)
                finally:
                    client.close()

    def test_record_real_server_frames(self):
        path = Path(__file__).resolve().parents[1]/"tools"/"record_emulation_hands.py"
        spec = importlib.util.spec_from_file_location("recorder",path)
        recorder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(recorder)
        with tempfile.TemporaryFile(mode="w+") as output:
            with harness.runtime() as (_,_,port):
                recorder.record(output,"127.0.0.1",port,.15,"neutral","right")
            output.seek(0)
            frames = [json.loads(line) for line in output]
            self.assertGreater(len(frames),1)
            self.assertEqual(len(frames[0]["hands"]["right"]["joints"]),21)

    def test_timed_debug_recording_zip_and_partial_save(self):
        path = Path(__file__).resolve().parents[1]/"tools"/"xr_gate_gesture_debug.py"
        spec = importlib.util.spec_from_file_location("timed_debug", path)
        debug = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(debug)
        STATE["right"] = dict(extended=8)
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/"hands.zip"
            with harness.runtime(extra=["--emulation-controller", "1"]) as (_,_,port):
                client = debug.RuntimeClient("127.0.0.1", port)
                archive = debug.Archive(target, {"schema": debug.SCHEMA})
                try:
                    phase = archive.phase(1, {"hand": "right", "label": "grip", "instruction": "test"})
                    debug.collect_stage(client, archive, phase, "prepare", .05, quiet=True)
                    debug.collect_stage(client, archive, phase, "hold", .25, quiet=True)
                finally:
                    client.close()
                    # The same close path is used by Ctrl+C/network errors.
                    archive.close("interrupted", "test interruption")
            with zipfile.ZipFile(target) as packed:
                self.assertIsNone(packed.testzip())
                metadata = json.loads(packed.read("metadata.json"))
                summary = json.loads(packed.read("summary.json"))
                raw = packed.read("frames.jsonl")
            self.assertEqual(metadata["status"], "interrupted")
            self.assertGreater(summary[0]["sides"]["right"]["unique_hand_samples"], 0)
            frames = [json.loads(line) for line in raw.splitlines()]
            self.assertEqual(metadata["frames"], len(frames))
            self.assertEqual({f["label"] for f in frames}, {"transition", "grip"})
            self.assertEqual(len(frames[-1]["hands"]["right"]["joints"]), 21)
            self.assertEqual(frames[-1]["record_hand"], "right")
            self.assertGreater(summary[0]["sides"]["right"]["ranges"]["grip"][1], .9)
            source, replay = Path(tmp)/"hands.jsonl", Path(tmp)/"replay.jsonl"
            source.write_bytes(raw)
            result = subprocess.run([TOOL,"replay",str(source),str(replay)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreater(json.loads(replay.read_text().splitlines()[-1])["right"]["grip"], .9)
            with self.assertRaises(FileExistsError):
                debug.Archive(target, {})

    def test_followup_scenario_only_recalibrates_right_wrist(self):
        path = Path(__file__).resolve().parents[1]/"tools"/"xr_gate_gesture_debug.py"
        spec = importlib.util.spec_from_file_location("followup_debug", path)
        debug = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(debug)
        plan = debug.make_plan(follow_up=True)
        calibration = {"neutral", "left", "right", "up", "down", "open", "closed"}
        self.assertEqual({p["hand"] for p in plan if p["label"] in calibration}, {"right"})
        self.assertTrue(calibration <= {p["label"] for p in plan})
        for side in ("left", "right"):
            self.assertEqual({p["variant"] for p in plan if p["hand"]==side and p["label"]=="trigger"},
                {"straight", "forward", "back", "straight_repeat"})
            self.assertEqual({p["variant"] for p in plan if p["hand"]==side and p["label"]=="pad_click"},
                {"thumb_tucked", "thumb_raised"})
            self.assertEqual({p["hand"] for p in debug.make_plan(hand=side, follow_up=True)}, {side})

    def test_timed_debug_cli_ctrl_c_saves_archive(self):
        script = Path(__file__).resolve().parents[1]/"tools"/"xr_gate_gesture_debug.py"
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp)/"interrupted.zip"
            with harness.runtime() as (_,_,port):
                process = subprocess.Popen([sys.executable, str(script), "--port", str(port),
                    "--hand", "right", "--yes", "--no-beep", "--output", str(target)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                try:
                    deadline = time.monotonic()+3
                    while (not target.exists() or target.stat().st_size < 1000) and time.monotonic() < deadline:
                        if process.poll() is not None:
                            break
                        time.sleep(.01)
                    process.send_signal(signal.SIGINT)
                    output, _ = process.communicate(timeout=3)
                    self.assertEqual(process.returncode, 130, output)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
            with zipfile.ZipFile(target) as packed:
                self.assertIsNone(packed.testzip())
                metadata = json.loads(packed.read("metadata.json"))
                frames = packed.read("frames.jsonl").splitlines()
            self.assertEqual(metadata["status"], "interrupted")
            self.assertGreater(len(frames), 0)
            self.assertEqual(metadata["frames"], len(frames))

    def test_wrist_calibration_and_replay(self):
        def json_hand(config):
            return {"status":1,"flags":9,"confidence":1,"joints":[
                {"joint_id":i,"position":p,"flags":2,"confidence":1} for i,p in enumerate(coordinates(**config))]}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source,base,profile,replay = (root/name for name in ("hands.jsonl","base.json","profile.json","result.jsonl"))
            base.write_text(json.dumps({"version":2,"smoothing_ms":0,"dpad_mode":"off"}))
            # Wrist calibration must accept a direction even if the independent
            # thumb activation measurement is below its gameplay threshold.
            phases = [("neutral",{}),("left",{"tilt_y":-.5,"thumb":"down"}),("right",{"tilt_y":.5}),
                ("up",{"tilt_x":.5}),("down",{"tilt_x":-.5}),
                ("open",{"extended":15,"thumb":"down"}),("closed",{"thumb":"down"}),
                ("combo",{"extended":9,"tilt_y":.5}),("thumb_rest",{"extended":9,"thumb":"down","tilt_y":.5}),
                ("button_a",{"thumb":"pinch"}),("button_b",{"extended":14,"thumb":"pinch"}),
                ("pad_click",{"extended":3,"thumb":"down"})]
            with source.open("w") as output:
                sequence = 0
                def emit(label, config):
                    nonlocal sequence
                    sequence += 1
                    stamp = 1000000000+sequence*30000000
                    output.write(json.dumps({"type":"frame","label":label,"record_hand":"right","read_timestamp_ns":str(stamp),
                        "hands":{"sequence":sequence,"timestamp_ns":str(stamp),"source_timestamp_ns":str(stamp),"reset_counter":0,
                        "left":json_hand({"thumb":"down"}),"right":json_hand(config)}})+"\n")
                for label,config in phases:
                    if label!="neutral":
                        for _ in range(8):
                            emit("release",{"thumb":"down"})
                    for _ in range(20):
                        emit(label,config)
            result = subprocess.run([TOOL,"calibrate",str(source),str(profile),"--config",str(base)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            saved = json.loads(profile.read_text())
            self.assertEqual(saved["version"],3)
            self.assertFalse(saved["left"]["enabled"])
            # Right-only follow-up data must retain the user's known-good left
            # calibration and hand settings exactly, including its deadzone.
            previous = json.loads((Path(__file__).parent/"data"/"emulation_gestures13.json").read_text())["profile"]
            base.write_text(json.dumps(previous))
            right_only = root/"right-only.json"
            result = subprocess.run([TOOL,"calibrate",str(source),str(right_only),"--config",str(base)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            recalibrated = json.loads(right_only.read_text())
            self.assertEqual(recalibrated["left"], previous["left"])
            self.assertNotEqual(recalibrated["right"]["calibration"], previous["right"]["calibration"])
            result = subprocess.run([TOOL,"replay",str(source),str(replay),"--config",str(profile)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            frames = [json.loads(line) for line in replay.read_text().splitlines()]
            last = lambda label: next(f["right"] for f in reversed(frames) if f["label"]==label)
            self.assertGreater(last("up")["thumbstick"][1], .8)
            self.assertLess(last("down")["thumbstick"][1], -.8)
            self.assertEqual(int(last("combo")["buttons"])&3,3)
            self.assertGreater(last("combo")["thumbstick"][0],.8)
            self.assertEqual(last("thumb_rest")["thumbstick"],[0,0])
            self.assertEqual(int(last("thumb_rest")["buttons"])&3,3)
            for label,button in (("button_a",8),("button_b",16),("pad_click",32)):
                self.assertEqual(int(last(label)["buttons"]),button)
                self.assertEqual(last(label)["trigger"],0)
                self.assertEqual(last(label)["grip"],0)

    def test_full_calibration_recorded_poses(self):
        fixture = json.loads((Path(__file__).parent/"data"/"emulation_full_calibration2.json").read_text())
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile, source, replay = (root/name for name in ("profile.json", "hands.jsonl", "replay.jsonl"))
            profile.write_text(json.dumps(fixture["profile"]))
            count = 0
            with source.open("w") as output:
                for index, case in enumerate(fixture["cases"], 1):
                    sides = {}
                    for name in ("left", "right"):
                        points = case["hands"].get(name)
                        sides[name] = dict(status=1 if points else 0, flags=9 if points else 0,
                            confidence=1 if points else 0, joints=[] if not points else [
                                dict(joint_id=i, position=p, flags=2, confidence=1) for i,p in enumerate(points)])
                    for _ in range(20):
                        count += 1
                        stamp = 1000000000+count*30000000
                        # Each case starts from a source reset, so a previous
                        # held button cannot hide a failed fresh activation.
                        hands = dict(sequence=count, timestamp_ns=str(stamp), source_timestamp_ns=str(stamp),
                            reset_counter=index, **sides)
                        output.write(json.dumps(dict(type="frame", label=str(index),
                            read_timestamp_ns=str(stamp+10000000), hands=hands))+"\n")
            result = subprocess.run([TOOL,"replay",str(source),str(replay),"--config",str(profile)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            results = {f["label"]:f for f in (json.loads(line) for line in replay.read_text().splitlines())}
            for index, case in enumerate(fixture["cases"], 1):
                for name, expected in case["expect"].items():
                    out = results[str(index)][name]
                    context = (case["phase"], name, out)
                    if expected.get("axes_zero"):
                        self.assertLess(math.hypot(*out["thumbstick"]), .05, context)
                    if "direction" in expected:
                        axis, sign, button = {"up":(1,1,64), "down":(1,-1,128),
                            "left":(0,-1,256), "right":(0,1,512)}[expected["direction"]]
                        self.assertGreater(sign*out["thumbstick"][axis], .8, context)
                        self.assertLess(abs(out["thumbstick"][1-axis]), .15, context)
                        self.assertTrue(int(out["buttons"]) & button, context)
                    if "channel_mask" in expected:
                        self.assertEqual(int(out["buttons"]) & 3, expected["channel_mask"], context)
                    if "buttons" in expected:
                        self.assertEqual(int(out["buttons"]), face_bits(expected["buttons"],name), context)
                        self.assertEqual(out["trigger"],0,context)
                        self.assertEqual(out["grip"],0,context)

    def test_recorded_joint_regression(self):
        fixture = json.loads((Path(__file__).parent/"data"/"emulation_gestures13.json").read_text())
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile, source, replay = (root/name for name in ("profile.json", "source.jsonl", "replay.jsonl"))
            profile.write_text(json.dumps(fixture["profile"]))
            count = 0
            poses = fixture["poses"]
            with source.open("w") as output:
                def emit(pose, repeats=20, label=None):
                    nonlocal count
                    selected = pose["hand"]
                    joints = [{"joint_id":i, "position":p, "flags":2, "confidence":1}
                        for i,p in enumerate(pose["points"])]
                    side = dict(status=1, flags=9, confidence=1, joints=joints)
                    absent = dict(status=0, flags=0, confidence=0, joints=[])
                    for _ in range(repeats):
                        count += 1
                        stamp = 1000000000+count*30000000
                        hands = dict(sequence=count, timestamp_ns=str(stamp), source_timestamp_ns=str(stamp),
                            reset_counter=0, left=side if selected=="left" else absent,
                            right=side if selected=="right" else absent)
                        output.write(json.dumps(dict(type="frame", label=label or str(pose["phase"]),
                            read_timestamp_ns=str(stamp+10000000), hands=hands))+"\n")
                for pose in poses:
                    if pose["phase"] == 400:
                        emit(next(p for p in poses if p["phase"]==44), label="hook_release")
                        emit(next(p for p in poses if p["phase"]==40), label="hook_prepare")
                    emit(pose)
            result = subprocess.run([TOOL,"replay",str(source),str(replay),"--config",str(profile)], capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            results = {f["label"]:f for f in (json.loads(line) for line in replay.read_text().splitlines())}
            for pose in poses:
                phase, hand = pose["phase"], pose["hand"]
                out = results[str(phase)][hand]
                if pose["label"] == "neutral":
                    self.assertLess(math.hypot(*out["thumbstick"]), .05, (phase,out))
                if pose["label"] in ("up","down","left","right"):
                    axis, sign, button = {"up":(1,1,64), "down":(1,-1,128),
                        "left":(0,-1,256), "right":(0,1,512)}[pose["label"]]
                    self.assertGreater(sign*out["thumbstick"][axis], .85, (phase,out))
                    self.assertLess(abs(out["thumbstick"][1-axis]), .15, (phase,out))
                    self.assertTrue(int(out["buttons"]) & button, (phase,out))
                if phase in (15,40,400):
                    self.assertEqual(int(out["buttons"]) & 3, 2, (phase,out))
                if phase in (16,41):
                    self.assertEqual(int(out["buttons"]) & 3, 1, (phase,out))
                if phase in (42,43):
                    self.assertEqual(int(out["buttons"]) & 3, 3, (phase,out))
                if pose["label"] == "thumb_rest_combo":
                    self.assertEqual(out["thumbstick"], [0,0], (phase,out))
                if pose["label"] in ("button_a","button_b"):
                    self.assertEqual(int(out["buttons"]), face_bits(8 if pose["label"]=="button_a" else 16,hand), (phase,out))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter",required=True)
    parser.add_argument("--tool",required=True)
    args,remaining = parser.parse_known_args()
    harness.ADAPTER,TOOL = args.adapter,args.tool
    unittest.main(argv=[sys.argv[0],*remaining])
