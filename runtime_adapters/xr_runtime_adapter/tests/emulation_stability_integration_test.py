#!/usr/bin/env python3
"""Runtime wrist stabilization, launcher profile loading, and Grip-only calibration."""
import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest

import app_tcp_server_test as harness
from emulation_integration_test import ControllerSource

TOOL = None
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = json.loads((ROOT/'tests/data/emulation_stability_poses.json').read_text())
STATE = {}

def points(phase, side):
    return next(c['hands'][side] for c in FIXTURE['cases'] if c['phase']==phase)

def source_side(role, visible):
    name = 'left' if role==1 else 'right'
    value = STATE[name]
    angle = math.radians(value.get('yaw',0))
    q = [math.cos(angle/2),0,math.sin(angle/2),0]
    pose = [0,.03,0,*q]
    header = harness.HAND_SIDE.pack(role,int(visible),57 if visible else 0,float(visible),
        *(pose*3+[0.0]*6+[1.0,1.0]),1,1,21,8)
    vertices = [[math.cos(angle)*x+math.sin(angle)*z,y,-math.sin(angle)*x+math.cos(angle)*z]
                for x,y,z in points(value['phase'],name)]
    return header+b''.join(harness.JOINT.pack(i,2,*p,*q,.008,1) for i,p in enumerate(vertices))

harness.hand_side = source_side

class StabilityIntegrationTest(unittest.TestCase):
    def setUp(self):
        STATE.clear()
        STATE.update(left=dict(phase=15),right=dict(phase=50))

    def test_optical_alignment_survives_override_shutdown(self):
        def imu_payload(sequence, stamp):
            # ControllerImuStateV1: fresh identity orientation, no motion samples.
            return struct.pack('<4I4Q8f',3,8,8|256,0,sequence,stamp,0,stamp,
                               0,0,0,1,0,0,0,0)+bytes(208)

        for enabled in (False, True):
            with self.subTest(emulation=enabled), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                cfg=copy.deepcopy(FIXTURE['profile'])
                cfg.update(wrist_filter_enabled=False,left=dict(enabled=False))
                profile=root/'profile.json'
                profile.write_text(json.dumps(cfg))
                offset=dict(enabled=True,only_optic=True,multiply_order='pre',
                    apply_to=dict(controller=True,palm=True,wrist=True,joints=False),
                    left=dict(enabled=True,ry_deg=40),right=dict(enabled=True,ry_deg=-40))
                transform=root/'transform.json'
                transform.write_text(json.dumps(dict(enabled=True,streams=dict(
                    hand_tracking_21_joint=dict(hand_orientation_offset=offset)))))
                with ControllerSource() as physical:
                    physical.update('left',present=True,imu=imu_payload)
                    physical.update('right',present=True,imu=imu_payload)
                    extra=['--emulation-controller',str(int(enabled)),
                        '--emulation-controller-config',str(profile),
                        '--tracking-transform-config',str(transform),
                        '--controller-input-mode','hand_plus_controller',
                        '--controller-input-transport','tcp',
                        '--controller-input-host','127.0.0.1',
                        '--controller-input-port',str(physical.port),
                        '--runtime-controller-left-orientation-source','IMU_OVERRIDE_CONTROLLER_RUNTIME',
                        '--runtime-controller-right-orientation-source','IMU_OVERRIDE_CONTROLLER_RUNTIME',
                        '--hand-orientation-offset-only-runtime','1']
                    with harness.runtime(extra=extra) as (_,_,port):
                        client=harness.Client(port)
                        try:
                            def aligned(f, left_angle, right_angle):
                                if not f.get('hands') or not f.get('controllers'):
                                    return False
                                for side,expected in (('left',left_angle),('right',right_angle)):
                                    q=f['hands'][side]['controller']['orientation_xyzw']
                                    if abs(math.degrees(2*math.atan2(q[1],q[3]))-expected)>.01:
                                        return False
                                return True
                            # Active IMU uses its dedicated alignment, never the optical offset.
                            client.frame(lambda f:aligned(f,0,0) and
                                int(f['controllers']['right']['source_mask'])&16)
                            physical.update('left',imu=None)
                            physical.update('right',imu=None)
                            client.frame(lambda f:aligned(f,40,-40))
                            # Stop all override packets: only the enabled emulated side keeps
                            # its configured optical alignment under the default runtime gate.
                            physical.paused.set()
                            event=client.frame(lambda f:aligned(f,0,-40 if enabled else 0) and
                                not int(f['controllers']['right']['source_mask'])&16)
                            for side in ('left','right'):
                                self.assertEqual(event['hands'][side]['joints'][8]['orientation_xyzw'],[0,0,0,1])
                                self.assertAlmostEqual(event['hands'][side]['joints'][8]['position'][0],
                                                       points(STATE[side]['phase'],side)[8][0],places=5)
                                self.assertEqual(event['controllers'][side]['orientation_xyzw'],
                                                 event['hands'][side]['controller']['orientation_xyzw'])
                            physical.update('left',imu=imu_payload)
                            physical.update('right',imu=imu_payload)
                            physical.paused.clear()
                            client.frame(lambda f:aligned(f,0,0) and
                                int(f['controllers']['right']['source_mask'])&16)
                        finally:
                            client.close()

    def test_filter_in_real_runtime_and_physical_bypass(self):
        for mode, enabled in ((False,True),(True,False),(True,True)):
            with self.subTest(mode=mode,filter=enabled), tempfile.TemporaryDirectory() as tmp:
                STATE['right'] = dict(phase=50)
                profile = Path(tmp)/'profile.json'
                cfg = copy.deepcopy(FIXTURE['profile'])
                cfg.update(wrist_filter_enabled=enabled,wrist_filter_degrees=2)
                profile.write_text(json.dumps(cfg))
                with ControllerSource() as physical:
                    extra=['--emulation-controller',str(int(mode)), '--emulation-controller-config',str(profile),
                        '--controller-input-mode','hand_plus_controller','--controller-input-transport','tcp',
                        '--controller-input-host','127.0.0.1','--controller-input-port',str(physical.port)]
                    with harness.runtime(extra=extra) as (_,_,port):
                        client=harness.Client(port)
                        try:
                            first=client.frame(lambda f:f.get('hands') and f.get('controllers'))
                            start=int(first['hands']['source_timestamp_ns'])
                            STATE['right']['yaw']=.7
                            latest=client.frame(lambda f:f.get('hands') and int(f['hands']['source_timestamp_ns'])>start+150000000)
                            h=latest['hands']['right']
                            q=h['controller']['orientation_xyzw']
                            angle=math.degrees(2*math.atan2(q[1],q[3]))
                            self.assertAlmostEqual(angle,0 if mode and enabled else .7,places=3)
                            if mode and enabled:
                                self.assertAlmostEqual(h['joints'][8]['position'][0],points(50,'right')[8][0],places=5)
                                STATE['right']['phase']=51
                                event=client.frame(lambda f:f.get('controllers') and int(f['controllers']['right']['buttons'])&1)
                                self.assertGreater(event['controllers']['right']['trigger'],.9)
                                # Fresh physical controller presence disables this gesture-only pose filter.
                                physical.update('right',present=True,buttons=16)
                                event=client.frame(lambda f:f.get('controllers') and int(f['controllers']['right']['buttons'])&16)
                                q=event['hands']['right']['controller']['orientation_xyzw']
                                self.assertAlmostEqual(math.degrees(2*math.atan2(q[1],q[3])),.7,places=3)
                        finally:
                            client.close()

    def test_flexion_buttons_load_from_json_and_preserve_physical_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile=Path(tmp)/'flexion.json'
            cfg=copy.deepcopy(FIXTURE['profile'])
            cfg.update(trigger_on_curl=True,grip_on_curl=True)
            profile.write_text(json.dumps(cfg))
            STATE['right']=dict(phase=55)
            with ControllerSource() as physical:
                extra=['--emulation-controller','1','--emulation-controller-config',str(profile),
                    '--controller-input-mode','hand_plus_controller','--controller-input-transport','tcp',
                    '--controller-input-host','127.0.0.1','--controller-input-port',str(physical.port)]
                with harness.runtime(extra=extra) as (_,_,port):
                    client=harness.Client(port)
                    try:
                        first=client.frame(lambda f:f.get('hands') and f.get('controllers'))
                        for phase,expected in ((51,2),(45,1),(50,3),(55,0)):
                            previous=int(first['hands']['source_timestamp_ns'])
                            STATE['right']=dict(phase=phase)
                            first=client.frame(lambda f:f.get('hands') and f.get('controllers') and
                                int(f['hands']['source_timestamp_ns'])>previous+150000000 and
                                int(f['controllers']['right']['buttons'])&3==expected)
                            self.assertEqual(first['controllers']['right']['grip']>.5,bool(expected&2))
                            self.assertEqual(first['controllers']['right']['trigger']>.5,bool(expected&1))
                        physical.update('right',present=True,buttons=1,trigger=.75)
                        event=client.frame(lambda f:f.get('controllers') and
                            int(f['controllers']['right']['source_mask'])&16 and
                            int(f['controllers']['right']['buttons'])&3==1)
                        # A physical digital Trigger also supplies a full analog
                        # value; that pre-existing normalization stays intact.
                        self.assertEqual(event['controllers']['right']['trigger'],1)
                        self.assertEqual(event['controllers']['right']['grip'],0)
                        physical.update('right',buttons=0,trigger=.75)
                        event=client.frame(lambda f:f.get('controllers') and
                            int(f['controllers']['right']['source_mask'])&16 and
                            int(f['controllers']['right']['buttons'])&3==0)
                        self.assertAlmostEqual(event['controllers']['right']['trigger'],.75,places=4)
                        self.assertEqual(event['controllers']['right']['grip'],0)
                    finally:
                        client.close()

    def test_launcher_loads_profile_from_runtime_configs(self):
        script=ROOT/'scripts/linux/start_xr_runtime_adapter_shm.sh'
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            profile=root/'runtime_adapters/xr_runtime_adapter/configs/emulation-controller.json'
            profile.parent.mkdir(parents=True)
            profile.write_text(json.dumps(FIXTURE['profile']))
            capture=root/'args.json'
            fake=root/'fake_adapter.py'
            fake.write_text('#!/usr/bin/env python3\nimport json,os,sys\nfrom pathlib import Path\nPath(os.environ["CAPTURE_ARGS"]).write_text(json.dumps(sys.argv[1:]))\n')
            fake.chmod(0o755)
            env=dict(os.environ,ROOT_PROJECT=str(root),XR_RUNTIME_ADAPTER_BIN=str(fake),
                     EMULATION_CONTROLLER='1',CAPTURE_ARGS=str(capture),EMULATION_CONTROLLER_CONFIG='')
            result=subprocess.run(['bash',str(script)],env=env,capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            args=json.loads(capture.read_text())
            self.assertEqual(args[args.index('--emulation-controller-config')+1],str(profile))
            explicit=root/'explicit.json'
            explicit.write_text('{}')
            env['EMULATION_CONTROLLER_CONFIG']=str(explicit)
            result=subprocess.run(['bash',str(script)],env=env,capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            args=json.loads(capture.read_text())
            self.assertEqual(args[args.index('--emulation-controller-config')+1],str(explicit))

    def test_grip_only_calibration_preserves_other_settings_and_rejects_overlap(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            source,profile,output=(root/p for p in ('hands.jsonl','profile.json','grip.json'))
            original=copy.deepcopy(FIXTURE['profile'])
            original.update(wrist_filter_enabled=True,wrist_filter_degrees=2,
                            trigger_on_curl=True,grip_on_curl=True)
            profile.write_text(json.dumps(original))
            def write_samples(open_phase):
                with source.open('w') as out:
                    count=0
                    for label,phase in (('grip_closed',50),('grip_open',open_phase)):
                        for _ in range(20):
                            count+=1
                            stamp=1000000000+count*33000000
                            h=dict(status=1,flags=9,confidence=1,joints=[dict(joint_id=i,flags=2,confidence=1,position=p)
                                for i,p in enumerate(points(phase,'right'))])
                            out.write(json.dumps(dict(type='frame',label=label,record_hand='right',read_timestamp_ns=str(stamp),
                                hands=dict(sequence=count,timestamp_ns=str(stamp),source_timestamp_ns=str(stamp),reset_counter=0,
                                    left=dict(status=0,flags=0,confidence=0,joints=[]),right=h)))+'\n')
            write_samples(45)
            result=subprocess.run([TOOL,'calibrate-grip',str(source),str(output),'--config',str(profile)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text())
            calibration=saved['right'].pop('grip_calibration')
            self.assertLess(calibration['open_curl'],.1)
            self.assertGreater(calibration['closed_curl'],.9)
            self.assertEqual(saved,original)
            write_samples(50)
            rejected=root/'rejected.json'
            result=subprocess.run([TOOL,'calibrate-grip',str(source),str(rejected),'--config',str(profile)],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('overlap',result.stderr)
            self.assertFalse(rejected.exists())

    def test_grip_recorder_plan_has_both_states_for_both_hands(self):
        script=ROOT/'tools/xr_gate_gesture_debug.py'
        spec=importlib.util.spec_from_file_location('grip_recorder',script)
        recorder=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(recorder)
        plan=recorder.make_plan(grip_only=True)
        self.assertEqual({p['hand'] for p in plan},{'left','right'})
        for hand in ('left','right'):
            self.assertEqual({p['label'] for p in plan if p['hand']==hand},{'grip_open','grip_closed'})

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--adapter',required=True)
    parser.add_argument('--tool',required=True)
    args,rest=parser.parse_known_args()
    harness.ADAPTER,TOOL=args.adapter,args.tool
    unittest.main(argv=[sys.argv[0],*rest])
