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

    def test_final_yaw_composes_after_mounting_offset_without_changing_input(self):
        baseline = None
        for enabled, degrees, left_enabled, rigid in ((False,45,True,False),(True,0,True,False),
                (True,45,True,False),(True,-45,True,False),(True,45,False,False),
                (True,45,True,True),(False,45,True,True)):
            with self.subTest(enabled=enabled, degrees=degrees, left=left_enabled,rigid=rigid), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                STATE.update(left=dict(phase=15,yaw=-13),right=dict(phase=50,yaw=17))
                cfg=copy.deepcopy(FIXTURE['profile'])
                cfg.update(wrist_filter_enabled=False,smoothing_ms=0,pose_yaw_offset_degrees=degrees)
                cfg['left']['enabled']=left_enabled
                profile=root/'profile.json';profile.write_text(json.dumps(cfg))
                offset=dict(enabled=True,only_optic=True,multiply_order='post',
                    apply_to=dict(controller=True,palm=True,wrist=True,joints=rigid),
                    left=dict(enabled=True,rz_deg=30),right=dict(enabled=True,rz_deg=-20))
                transform=root/'transform.json'
                transform.write_text(json.dumps(dict(enabled=True,streams=dict(
                    hand_tracking_21_joint=dict(hand_orientation_offset=offset)))))
                extra=['--emulation-controller',str(int(enabled)),
                    '--emulation-controller-config',str(profile),
                    '--tracking-transform-config',str(transform),
                    '--hand-orientation-offset-only-runtime','0']
                with harness.runtime(extra=extra) as (_,_,port):
                    client=harness.Client(port)
                    try:
                        event=client.frame(lambda f:f.get('hands') and f.get('controllers'))
                        stamp=int(event['hands']['source_timestamp_ns'])
                        event=client.frame(lambda f:f.get('hands') and f.get('controllers') and
                            int(f['hands']['source_timestamp_ns'])>stamp+200000000)
                        for side,sign,mount in (('left',1,30),('right',-1,-20)):
                            amount=sign*degrees if enabled and (side=='right' or left_enabled) else 0
                            raw_yaw=STATE[side]['yaw']
                            a=math.radians(raw_yaw+amount)/2;b=math.radians(mount)/2
                            expected=[math.sin(a)*math.sin(b),math.sin(a)*math.cos(b),
                                      math.cos(a)*math.sin(b),math.cos(a)*math.cos(b)]
                            hand=event['hands'][side];control=event['controllers'][side]
                            for actual,wanted in zip(hand['controller']['orientation_xyzw'],expected):
                                self.assertAlmostEqual(actual,wanted,places=5)
                            for actual,wanted in zip(control['orientation_xyzw'],expected):
                                self.assertAlmostEqual(actual,wanted,places=5)
                            self.assertEqual(control['position'],hand['controller']['position'])
                            self.assertEqual(hand['controller']['position'],[0,hand['controller']['position'][1],0])
                            c=math.cos(2*a);s=math.sin(2*a)
                            for joint,p in zip(hand['joints'],points(STATE[side]['phase'],side)):
                                if rigid:
                                    zangle=math.radians(mount)
                                    x=math.cos(zangle)*p[0]-math.sin(zangle)*(p[1]-.03)
                                    y=.03+math.sin(zangle)*p[0]+math.cos(zangle)*(p[1]-.03)
                                    for actual,wanted in zip(joint['orientation_xyzw'],expected):
                                        self.assertAlmostEqual(actual,wanted,places=5)
                                else:x,y=p[0],p[1]
                                for actual,wanted in zip(joint['position'],[c*x+s*p[2],y,-s*x+c*p[2]]):
                                    self.assertAlmostEqual(actual,wanted,places=5)
                            if enabled and (degrees or rigid):
                                source=event['emulation_input_hands'][side]
                                p=points(STATE[side]['phase'],side)[8]
                                angle=math.radians(raw_yaw)
                                for actual,wanted in zip(source['joints'][8]['position'],[
                                        math.cos(angle)*p[0]+math.sin(angle)*p[2],p[1],
                                        -math.sin(angle)*p[0]+math.cos(angle)*p[2]]):
                                    self.assertAlmostEqual(actual,wanted,places=5)
                        # The same physical poses must keep exactly the same ordinary buttons and axes.
                        if enabled:
                            input_state=[]
                            for phase in (51,45,55,50):
                                stamp=int(event['hands']['source_timestamp_ns'])
                                STATE['right']['phase']=phase
                                event=client.frame(lambda f:f.get('hands') and f.get('controllers') and
                                    int(f['hands']['source_timestamp_ns'])>stamp+200000000)
                                control=event['controllers']['right']
                                input_state.append((control['buttons'],control['touches'],control['trigger'],
                                                    control['grip'],control['thumbstick']))
                            if baseline is None:baseline=input_state
                            else:self.assertEqual(input_state,baseline)
                    finally:client.close()

    def test_shipped_profiles_enable_joint_offsets_without_changing_angles(self):
        for relative in ('configs/xr_21_joint_hand_viewer_verified.json',
                '../../devices/xreal_ultra/configs/xr_runtime_adapter/xr_21_joint_hand_viewer_verified.json'):
            cfg=json.loads((ROOT/relative).read_text())
            for stream in ('hand_tracking_21_joint','hand_skeleton26'):
                offset=cfg['streams'][stream]['hand_orientation_offset']
                self.assertTrue(offset['apply_to']['joints'])
                self.assertTrue(offset['only_optic'])
            offset=cfg['streams']['hand_tracking_21_joint']['hand_orientation_offset']
            self.assertEqual(offset['left']['rz_deg'],-60)
            self.assertEqual(offset['right']['rz_deg'],60)

    def test_world_offset_remains_rigid_after_wrist_stabilization(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            cfg=copy.deepcopy(FIXTURE['profile'])
            cfg.update(wrist_filter_enabled=True,wrist_filter_degrees=2,pose_yaw_offset_degrees=45)
            profile=root/'profile.json';profile.write_text(json.dumps(cfg))
            offset=dict(enabled=True,only_optic=True,multiply_order='pre',
                apply_to=dict(controller=True,palm=True,wrist=True,joints=True),
                left=dict(enabled=True,rz_deg=30),right=dict(enabled=True,rz_deg=-20))
            transform=root/'transform.json'
            transform.write_text(json.dumps(dict(enabled=True,streams=dict(
                hand_tracking_21_joint=dict(hand_orientation_offset=offset)))))
            with harness.runtime(extra=['--emulation-controller','1','--emulation-controller-config',str(profile),
                    '--tracking-transform-config',str(transform)]) as (_,_,port):
                client=harness.Client(port)
                try:
                    first=client.frame(lambda f:f.get('hands') and f.get('controllers'))
                    stamp=int(first['hands']['source_timestamp_ns'])
                    STATE['right']['yaw']=.7
                    second=client.frame(lambda f:f.get('hands') and f.get('controllers') and
                        int(f['hands']['source_timestamp_ns'])>stamp+160000000)
                    pivot0=first['hands']['right']['controller']['position']
                    pivot1=second['hands']['right']['controller']['position']
                    for old,new in zip(first['hands']['right']['joints'],second['hands']['right']['joints']):
                        # Stabilization pivots at the wrist and may move the controller;
                        # the mounting offset must preserve every bone relative to it.
                        for index,(actual,expected) in enumerate(zip(new['position'],old['position'])):
                            self.assertAlmostEqual(actual-pivot1[index],expected-pivot0[index],places=5)
                        for actual,expected in zip(new['orientation_xyzw'],old['orientation_xyzw']):
                            self.assertAlmostEqual(actual,expected,places=5)
                    for actual,expected in zip(second['controllers']['right']['orientation_xyzw'],
                            second['hands']['right']['controller']['orientation_xyzw']):
                        self.assertAlmostEqual(actual,expected,places=6)
                finally:client.close()

    def test_optical_alignment_survives_override_shutdown(self):
        def imu_payload(sequence, stamp):
            # ControllerImuStateV1: fresh identity orientation, no motion samples.
            return struct.pack('<4I4Q8f',3,8,8|256,0,sequence,stamp,0,stamp,
                               0,0,0,1,0,0,0,0)+bytes(208)

        for enabled in (False, True):
            with self.subTest(emulation=enabled), tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                cfg=copy.deepcopy(FIXTURE['profile'])
                cfg.update(wrist_filter_enabled=False,left=dict(enabled=False),pose_yaw_offset_degrees=45)
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
                            client.frame(lambda f:aligned(f,40,-85 if enabled else -40))
                            # Stop all override packets: only the enabled emulated side keeps
                            # its configured optical alignment under the default runtime gate.
                            physical.paused.set()
                            event=client.frame(lambda f:aligned(f,0,-85 if enabled else 0) and
                                not int(f['controllers']['right']['source_mask'])&16)
                            for side in ('left','right'):
                                angle=math.radians(-45 if side=='right' and enabled else 0)
                                self.assertAlmostEqual(event['hands'][side]['joints'][8]['orientation_xyzw'][1],
                                                       math.sin(angle/2),places=5)
                                point=points(STATE[side]['phase'],side)[8]
                                self.assertAlmostEqual(event['hands'][side]['joints'][8]['position'][0],
                                                       math.cos(angle)*point[0]+math.sin(angle)*point[2],places=5)
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
            cfg.update(trigger_on_curl=True,grip_on_curl=True,pose_yaw_offset_degrees=45)
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
                            trigger_on_curl=True,grip_on_curl=True,pose_yaw_offset_degrees=45)
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
