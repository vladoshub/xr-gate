#!/usr/bin/env python3
"""Exercise labelled optical holds, calibration, and both exclusive D-pad sources."""
import argparse
import contextlib
import copy
import importlib.util
import io
import json
import math
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile

from emulation_integration_test import coordinates
import app_tcp_server_test as harness

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('controls_live',ROOT/'tools/xr_gate_grip_live.py')
live=importlib.util.module_from_spec(spec)
spec.loader.exec_module(live)
TOOL=None
TARGETS={'thumb_neutral':(0,.25),'thumb_hidden':(0,.25),'thumb_up':(0,1),
         'thumb_down':(0,-.2),'thumb_left':(-.5,.25),'thumb_right':(.5,.25),
         'thumb_up_left':(-.5,1),'thumb_up_right':(.5,1),
         'thumb_down_left':(-.5,-.2),'thumb_down_right':(.5,-.2)}


def vertices(name,label,variant='neutral',layout='legacy'):
    if layout=='middle_grip' and label=='click_three_fingers':
        return coordinates(extended=7,thumb='down',left=name=='left')
    if layout=='middle_grip' and label in ('ring_a','ring_b'):
        return coordinates(extended=14 if label=='ring_a' else 0,thumb='pinch',left=name=='left')
    if layout=='middle_grip':
        index,middle,little=live.control_shape(label,layout)
    else:
        index,little=live.control_shape(label);middle=True
    points=coordinates(extended=(0 if index else 1)|(0 if middle else 2)|(0 if little else 8),left=name=='left')
    forward=live.unit(live.sub(points[9],points[0]))
    across=live.sub(points[5],points[17])
    if name=='left':across=[-v for v in across]
    across=live.unit(live.sub(across,[v*live.dot(across,forward) for v in forward]))
    normal=live.unit(live.cross(across,forward))
    sweep,bend=TARGETS.get(label,(0,.25))
    hidden=label=='thumb_hidden' or variant=='thumb_tucked'
    angle=(-.6435011088 if name=='left' else .6435011088)+sweep
    base=[points[5][k]+across[k]*(0 if hidden else -.06 if name=='left' else .06)-
          forward[k]*(.01 if hidden else .025) for k in range(3)]
    prox=forward if hidden else [math.sin(angle)*a+math.cos(angle)*f for a,f in zip(across,forward)]
    distal=[-v for v in forward] if hidden else [math.cos(bend)*p-math.sin(bend)*n for p,n in zip(prox,normal)]
    points[1]=[v-.02*f for v,f in zip(base,forward)]
    points[2]=base
    points[3]=[v+(.005 if hidden else .035)*p for v,p in zip(base,prox)]
    points[4]=[v+.025*d for v,d in zip(points[3],distal)]
    if variant=='forward':
        points=[[x,math.cos(.6)*y+math.sin(.6)*z,-math.sin(.6)*y+math.cos(.6)*z] for x,y,z in points]
    return points


def optical_frame(name,label,stamp,variant='neutral',layout='legacy'):
    hands=dict(sequence=str(stamp),timestamp_ns=str(stamp),source_timestamp_ns=str(stamp),reset_counter='0')
    for side in ('left','right'):
        p=vertices(side,label if name==side else 'buttons_open',variant if name==side else 'neutral',layout)
        pose=dict(position=[0,0,0],orientation_xyzw=[0,0,0,1])
        hands[side]=dict(status=1,flags=9,confidence=1,controller=pose,palm=pose,wrist=pose,
            joints=[dict(joint_id=i,flags=2,confidence=1,position=v,orientation_xyzw=[0,0,0,1]) for i,v in enumerate(p)])
    return dict(type='frame',hands=hands,read_timestamp_ns=str(stamp+10000000),
                controllers={s:dict(trigger=0,grip=0,buttons='0',thumbstick=[0,0]) for s in ('left','right')})


class Frames:
    def __init__(self,values):self.values=iter(values)
    def frames(self,_):return [next(self.values)]


class ControlsCalibrationTest(unittest.TestCase):
    def record(self,path,layout='legacy',click_gesture='layout_default'):
        archive=live.Archive(path,{})
        references={'left':{},'right':{}}
        with contextlib.redirect_stdout(io.StringIO()):
            for i,pose in enumerate(live.make_controls_plan('both',layout=layout,click_gesture=click_gesture),1):
                stats=archive.phase(i,pose)
                values=[optical_frame(pose['hand'],pose['label'],1000000000000+i*2000000000+j*50000000,
                                      pose.get('variant','neutral'),layout) for j in range(25)]
                live.collect_verified(Frames(values),archive,stats,1,5,0,references[pose['hand']],True)
        archive.close('completed')
        with zipfile.ZipFile(path) as z:return z.read('frames.jsonl')

    def fit(self,root,data,layout='legacy',click_gesture='layout_default'):
        source,profile,output=(root/p for p in ('frames.jsonl','base.json','calibrated.json'))
        source.write_bytes(data)
        original=json.loads((ROOT/'configs/emulation-controller.json').read_text())
        original['button_layout']=layout
        original['thumbstick_click_gesture']=click_gesture
        profile.write_text(json.dumps(original))
        result=subprocess.run([TOOL,'calibrate-controls',str(source),str(output),'--config',str(profile)],capture_output=True,text=True)
        return result,original,output

    def test_verified_recording_calibrates_both_hands_and_preserves_wrist(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            data=self.record(root/'recording.zip')
            result,original,output=self.fit(root,data)
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text())
            for side in ('left','right'):
                self.assertEqual(saved[side]['trigger_calibration'],dict(open_curl=0,closed_curl=1))
                self.assertEqual(saved[side]['grip_calibration'],dict(open_curl=0,closed_curl=1))
                self.assertEqual(saved[side]['calibration'],original[side]['calibration'])
                self.assertEqual(saved[side]['open_curl'],original[side]['open_curl'])
                self.assertGreater(saved[side]['thumb_calibration']['enable_clearance'],
                                   saved[side]['thumb_calibration']['disable_clearance'])
                thumb=saved[side]['thumb_calibration']
                self.assertEqual(thumb['feature'],'thumb_skeleton_v2')
                self.assertEqual(len(thumb['neutral_poses']),2)
                self.assertEqual(len(thumb['directions']),8)
                self.assertTrue(all(len(v)==12 for v in thumb['directions'].values()))
            self.assertEqual(saved['dpad_source'],'wrist')
            saved['dpad_source']='thumb'
            output.write_text(json.dumps(saved))
            records=[]; expected=[]
            for side in ('left','right'):
                for label,bits in [('thumb_neutral',0),('thumb_hidden',0),('thumb_up',64),('thumb_down',128),
                                   ('thumb_left',256),('thumb_right',512),('thumb_up_left',64|256),
                                   ('thumb_up_right',64|512),('thumb_down_left',128|256),('thumb_down_right',128|512)]:
                    for _ in range(30):
                        records.append(optical_frame(side,label,2000000000000+len(records)*50000000))
                        expected.append((side,bits))
            source=root/'replay.jsonl'
            source.write_text(''.join(json.dumps(r)+'\n' for r in records))
            target=root/'result.jsonl'
            result=subprocess.run([TOOL,'replay',str(source),str(target),'--config',str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            values=[json.loads(line) for line in target.read_text().splitlines()]
            for i in range(29,len(values),30):
                side,bits=expected[i]
                self.assertEqual(int(values[i][side]['buttons']),bits,(side,bits,values[i][side]))

    def test_overlap_missing_thumb_and_stale_frames_cannot_produce_a_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            data=self.record(root/'recording.zip')
            for fault in ('buttons','thumb','stale'):
                with self.subTest(fault=fault),tempfile.TemporaryDirectory() as dest:
                    records=[json.loads(line) for line in data.splitlines()]
                    for r in records:
                        name=r['record_hand']
                        if fault=='buttons' and r['label']=='trigger_closed':
                            r['hands'][name]=optical_frame(name,'buttons_open',int(r['hands']['source_timestamp_ns']))['hands'][name]
                        if fault=='thumb' and r['label']=='thumb_up':r['hands'][name]['joints'][4]['flags']=0
                        if fault=='stale':r['read_timestamp_ns']=str(int(r['hands']['source_timestamp_ns'])+500000000)
                    result,_,output=self.fit(Path(dest),''.join(json.dumps(r)+'\n' for r in records).encode())
                    self.assertNotEqual(result.returncode,0,result.stdout)
                    self.assertFalse(output.exists())

    def test_duplicate_hold_and_invalid_thumb_do_not_relabel_controls(self):
        m=optical_frame('left','buttons_open',1000000000000)
        gate=live.ControlsHold('buttons_open',1,{})
        for _ in range(100):self.assertFalse(gate.push(m,live.measure(m,'left'),0)[1])
        self.assertEqual(len(gate.samples),1)
        bad=copy.deepcopy(m);bad['hands']['left']['joints'][3]['flags']=0
        measured=live.measure(bad,'left')
        self.assertTrue(gate.matches(measured)[0])
        self.assertFalse(live.ControlsHold('thumb_neutral',1,{}).matches(measured)[0])
        self.assertEqual(len(live.make_controls_plan('both')),46)
        self.assertEqual(len(live.make_controls_plan('both',buttons_only=True)),24)

    def test_invalid_skeleton_profiles_and_duplicate_directions_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);data=self.record(root/'record.zip')
            result,_,output=self.fit(root,data)
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text());saved['dpad_source']='thumb'
            for fault in ('empty_neutral','missing_diagonal','duplicate_direction','wrong_dimension','invalid_number'):
                bad=copy.deepcopy(saved);thumb=bad['right']['thumb_calibration']
                if fault=='empty_neutral':thumb['neutral_poses']=[]
                if fault=='missing_diagonal':del thumb['directions']['down_right']
                if fault=='duplicate_direction':thumb['directions']['up_right']=thumb['directions']['right']
                if fault=='wrong_dimension':thumb['directions']['left']=[0,0,0]
                if fault=='invalid_number':thumb['neutral_poses'][0][0]=float('nan')
                profile=root/(fault+'.json');profile.write_text(json.dumps(bad))
                target=root/(fault+'.jsonl')
                failed=subprocess.run([TOOL,'replay',str(root/'frames.jsonl'),str(target),'--config',str(profile)],capture_output=True,text=True)
                self.assertNotEqual(failed.returncode,0,fault)
                self.assertFalse(target.exists(),fault)

    def test_thumb_settling_is_excluded_without_refitting_buttons(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);records=[]
            for phase,pose in enumerate(live.make_controls_plan('both',thumb_only=True),1):
                for j in range(60):
                    label=pose['label']
                    settling='thumb_neutral' if label=='thumb_hidden' else 'thumb_hidden'
                    frame=optical_frame(pose['hand'],settling if j<20 else label,
                                        5000000000000+phase*10000000000+j*50000000)
                    frame.update(label=label,phase_label=label,phase_id=phase,phase_stage='hold',
                                 record_hand=pose['hand'],button_layout='legacy')
                    records.append(frame)
            result,original,output=self.fit(root,''.join(json.dumps(r)+'\n' for r in records).encode())
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text())
            for side in ('left','right'):
                self.assertEqual(saved[side]['thumb_calibration']['deadzone'],.12)
                actual=copy.deepcopy(saved[side]);actual.pop('thumb_calibration')
                self.assertEqual(actual,original[side])
            # The live recorder must use the same settled references; a
            # transient extended thumb must not block every later direction.
            references={}
            archive=live.Archive(root/'settling.zip',{})
            pose=dict(hand='right',label='thumb_hidden')
            frames=[r for r in records if r['record_hand']=='right' and r['label']=='thumb_hidden']
            with contextlib.redirect_stdout(io.StringIO()):
                stats=archive.phase(1,pose)
                live.collect_verified(Frames(frames),archive,stats,2.9,5,0,references,True)
            archive.close('completed')
            self.assertGreaterEqual(len(references['thumb_hidden']),10)
            self.assertGreaterEqual(references['thumb_hidden'][0]['stamp'],
                                    int(frames[0]['hands']['source_timestamp_ns'])+1000000000)
            gate=live.ControlsHold('thumb_right',1,references)
            self.assertTrue(gate.matches(live.measure(optical_frame('right','thumb_right',9000000000000),'right'))[0])

    def test_middle_grip_calibration_and_all_three_channels(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            data=self.record(root/'middle.zip','middle_grip')
            result,original,output=self.fit(root,data,'middle_grip')
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text())
            for side in ('left','right'):
                self.assertEqual(saved[side]['grip_calibration'],dict(finger='middle',open_curl=0,closed_curl=1))
                self.assertEqual(saved[side]['thumbstick_click_calibration'],dict(finger='little',open_curl=0,closed_curl=1))
                self.assertEqual(saved[side]['calibration'],original[side]['calibration'])
                self.assertEqual(saved[side]['open_curl'],original[side]['open_curl'])
            saved.update(dpad_source='thumb',thumbstick_click_on_curl=False)
            output.write_text(json.dumps(saved))
            records=[];expected=[]
            cases=[(label,(mask&3)|(0 if mask&4 else 32)) for label,mask in live.MIDDLE_STATES.items()]
            cases += [('ring_a',8),('ring_b',16),('control_neutral',0),('trigger_grip_closed',35)]
            for side in ('left','right'):
                for label,bits in cases:
                    for _ in range(30):
                        records.append(optical_frame(side,label,3000000000000+len(records)*50000000,layout='middle_grip'))
                        expected.append((side,bits))
            source=root/'middle-replay.jsonl';source.write_text(''.join(json.dumps(r)+'\n' for r in records))
            target=root/'middle-result.jsonl'
            result=subprocess.run([TOOL,'replay',str(source),str(target),'--config',str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            values=[json.loads(line) for line in target.read_text().splitlines()]
            for i in range(29,len(values),30):
                side,bits=expected[i]
                self.assertEqual(int(values[i][side]['buttons']),bits,(side,bits,values[i][side]))
            # A recording made for one layout must never recalibrate another finger silently.
            dest=root/'wrong';dest.mkdir()
            result,_,wrong=self.fit(dest,data,'legacy')
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(wrong.exists())
            self.assertEqual(len(live.make_controls_plan('both',layout='middle_grip')),56)
            self.assertEqual(len(live.make_controls_plan('both',buttons_only=True,layout='middle_grip')),34)

    def test_new_cli_records_the_thumb_plan_over_tcp(self):
        plan=live.make_controls_plan('left',thumb_only=True)
        state={'phase':1,'sequence':0}; stop=threading.Event()
        server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(5)
        def serve():
            try:
                client,_=server.accept()
                with client:
                    client.sendall(b'{"type":"hello","protocol":"xr_gate_app_v1"}\n')
                    while not stop.is_set():
                        state['sequence']+=1
                        pose=plan[state['phase']-1]
                        value=optical_frame('left',pose['label'],1000000000000+state['sequence']*50000000)
                        client.sendall((json.dumps(value)+'\n').encode());stop.wait(.004)
            except OSError:pass
        worker=threading.Thread(target=serve,daemon=True);worker.start()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'thumb.zip'
            child=subprocess.Popen([sys.executable,str(ROOT/'tools/xr_gate_gesture_debug.py'),'--thumb-dpad','--layout','legacy',
                '--yes','--hand','left','--prepare','0','--hold','1','--timeout','5',
                '--port',str(server.getsockname()[1]),'--output',str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            lines=[]
            def read():
                for line in child.stdout:
                    lines.append(line)
                    match=re.match(r'\[(\d+)/11\]',line)
                    if match:state['phase']=int(match.group(1))
            reader=threading.Thread(target=read,daemon=True);reader.start()
            try:
                child.wait(timeout=15);reader.join(timeout=1)
                self.assertEqual(child.returncode,0,child.stderr.read()+''.join(lines))
                with zipfile.ZipFile(path) as z:
                    self.assertEqual(json.loads(z.read('metadata.json'))['status'],'completed')
                    self.assertEqual(len(json.loads(z.read('summary.json'))),11)
            finally:
                stop.set()
                if child.poll() is None:child.kill();child.wait()
                child.stdout.close();child.stderr.close();server.close();worker.join(timeout=1)

    def test_three_finger_click_calibrates_only_trigger_and_middle_grip(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            data=self.record(root/'three.zip','middle_grip','three_fingers')
            records=[json.loads(line) for line in data.splitlines()]
            labels={r['phase_label'] for r in records}
            self.assertIn('click_three_fingers',labels)
            self.assertTrue(labels.isdisjoint({'click_closed','trigger_click_closed','grip_click_closed','trigger_grip_closed'}))
            result,original,output=self.fit(root,data,'middle_grip','three_fingers')
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text())
            for side in ('left','right'):
                self.assertEqual(saved[side]['trigger_calibration'],dict(open_curl=0,closed_curl=1))
                self.assertEqual(saved[side]['grip_calibration'],dict(finger='middle',open_curl=0,closed_curl=1))
                self.assertNotIn('thumbstick_click_calibration',saved[side])
                self.assertEqual(saved[side]['calibration'],original[side]['calibration'])
            self.assertEqual(saved['thumbstick_click_gesture'],'three_fingers')
            # Migration removes an obsolete little-finger endpoint block.
            for side in ('left','right'):
                original[side]['thumbstick_click_calibration']=dict(finger='little',open_curl=0,closed_curl=1)
            (root/'base.json').write_text(json.dumps(original))
            target=root/'refitted.json'
            result=subprocess.run([TOOL,'calibrate-controls',str(root/'frames.jsonl'),str(target),
                                   '--config',str(root/'base.json')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertTrue(all('thumbstick_click_calibration' not in json.loads(target.read_text())[s]
                                for s in ('left','right')))
            # A short candidate must also release and require a neutral pose.
            saved.update(dpad_source='thumb')
            output.write_text(json.dumps(saved))
            replay=[];expected=[]
            cases=[('control_neutral',0),('trigger_closed',1),('grip_closed',2),('buttons_closed',3),
                   ('click_three_fingers',32),('buttons_closed',0),('ring_a',0),
                   ('control_neutral',0),('ring_a',8),('ring_b',16),('control_neutral',0),
                   ('click_three_fingers',32),('control_neutral',0),('buttons_closed',3)]
            for side in ('left','right'):
                for label,bits in cases:
                    for _ in range(30):
                        replay.append(optical_frame(side,label,4000000000000+len(replay)*50000000,layout='middle_grip'))
                        expected.append((side,bits))
            source=root/'replay.jsonl';source.write_text(''.join(json.dumps(r)+'\n' for r in replay))
            target=root/'result.jsonl'
            result=subprocess.run([TOOL,'replay',str(source),str(target),'--config',str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            values=[json.loads(line) for line in target.read_text().splitlines()]
            for i in range(29,len(values),30):
                side,bits=expected[i]
                self.assertEqual(int(values[i][side]['buttons'])&~(64|128|256|512),bits,(side,bits,values[i][side]))
                if bits==32:
                    self.assertEqual(values[i][side]['trigger'],0)
                    self.assertEqual(values[i][side]['grip'],0)
                    self.assertEqual(values[i][side]['thumbstick'],[0,0])
            wrong=root/'wrong';wrong.mkdir()
            result,_,output=self.fit(wrong,data,'middle_grip','layout_default')
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(output.exists())

    def test_middle_layout_in_actual_runtime_tcp(self):
        self.check_runtime_layout()

    def test_three_finger_layout_in_actual_runtime_tcp(self):
        self.check_runtime_layout('three_fingers')

    def check_runtime_layout(self,click_gesture='layout_default'):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            data=self.record(root/'runtime-calibration.zip','middle_grip',click_gesture)
            result,_,profile=self.fit(root,data,'middle_grip',click_gesture)
            self.assertEqual(result.returncode,0,result.stderr)
            cfg=json.loads(profile.read_text());cfg.update(dpad_source='thumb',thumbstick_click_on_curl=False,
                                                          pose_yaw_offset_degrees=45)
            profile.write_text(json.dumps(cfg))
            transform=root/'transform.json'
            offset=dict(enabled=True,only_optic=True,multiply_order='post',
                apply_to=dict(controller=True,palm=True,wrist=True,joints=True),
                left=dict(enabled=True,rz_deg=-60),right=dict(enabled=True,rz_deg=60))
            transform.write_text(json.dumps(dict(enabled=True,streams=dict(
                hand_tracking_21_joint=dict(hand_orientation_offset=offset)))))
            state={'left':'buttons_open','right':'buttons_open','pad':False}
            def source_side(role,visible):
                name='left' if role==1 else 'right'
                p=[0,.03,0,1,0,0,0]
                header=harness.HAND_SIDE.pack(role,int(visible),57 if visible else 0,float(visible),
                    *(p*3+[0.0]*6+[1.0,1.0]),1,1,21,8)
                points=vertices(name,state[name],layout='middle_grip')
                if name=='right' and state['pad']:
                    other=vertices(name,'thumb_up',layout='middle_grip')
                    points[1:5]=other[1:5]
                return header+b''.join(harness.JOINT.pack(i,2,*p,1,0,0,0,.008,1) for i,p in enumerate(points))
            previous=harness.hand_side;harness.hand_side=source_side
            try:
                with harness.runtime(extra=['--emulation-controller','1','--emulation-controller-config',str(profile),
                        '--tracking-transform-config',str(transform)]) as (_,_,port):
                    client=harness.Client(port)
                    try:
                        event=client.frame(lambda f:f.get('hands') and f.get('controllers'))
                        cases=[(label,(mask&3)|(0 if mask&4 else 32)) for label,mask in live.MIDDLE_STATES.items()]
                        cases += [('ring_a',8),('ring_b',16),('control_neutral',0)]
                        if click_gesture=='three_fingers':
                            cases=[('buttons_open',0),('trigger_closed',1),('grip_closed',2),('buttons_closed',3),
                                   ('click_three_fingers',32),('buttons_closed',0),('ring_a',0),
                                   ('control_neutral',0),('ring_a',8),('ring_b',16),('control_neutral',0)]
                        for label,bits in cases:
                            stamp=int(event['hands']['source_timestamp_ns']);state['right']=label
                            event=client.frame(lambda f:f.get('hands') and f.get('controllers') and
                                int(f['hands']['source_timestamp_ns'])>stamp+160000000 and
                                int(f['controllers']['right']['buttons'])==bits)
                            if label.startswith('ring_') or label=='click_three_fingers':
                                self.assertEqual(event['controllers']['right']['thumbstick'],[0,0])
                        state.update(right='trigger_grip_closed',pad=True)
                        expected=67 if click_gesture=='three_fingers' else 99
                        event=client.frame(lambda f:f.get('controllers') and int(f['controllers']['right']['buttons'])==expected)
                        self.assertGreater(event['controllers']['right']['thumbstick'][1],.65)
                        self.assertIn('emulation_input_hands',event)
                        self.assertNotEqual(event['hands']['right']['joints'][8]['position'],
                                            event['emulation_input_hands']['right']['joints'][8]['position'])
                        self.assertIsNotNone(live.measure(event,'right'))
                    finally:client.close()
            finally:harness.hand_side=previous

    def test_calibration_and_recorders_use_recognition_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            data=self.record(root/'original.zip','middle_grip')
            encoded=[]
            for line in data.splitlines():
                value=json.loads(line)
                value['emulation_input_hands']=value['hands']
                value['hands']=None
                encoded.append(json.dumps(value)+'\n')
            source=''.join(encoded).encode()
            result,original,output=self.fit(root,source,'middle_grip')
            self.assertEqual(result.returncode,0,result.stderr)
            saved=json.loads(output.read_text())
            self.assertEqual(saved['pose_yaw_offset_degrees'],45)
            for name in ('left','right'):
                self.assertEqual(saved[name]['calibration'],original[name]['calibration'])
                self.assertEqual(saved[name]['grip_calibration']['finger'],'middle')
                self.assertEqual(saved[name]['thumbstick_click_calibration']['finger'],'little')
            spec=importlib.util.spec_from_file_location('offset_legacy_recorder',ROOT/'tools/xr_gate_gesture_debug.py')
            old=importlib.util.module_from_spec(spec);spec.loader.exec_module(old)
            frame=optical_frame('right','trigger_closed',3000000000000,layout='middle_grip')
            frame['emulation_input_hands']=frame['hands'];frame['hands']=None
            self.assertIsNotNone(live.measure(frame,'right'))
            for recorder in (live,old):
                path=root/(recorder.__name__+'.zip')
                archive=recorder.Archive(path,{})
                stats=archive.phase(1,dict(label='trigger_closed',hand='right'))
                archive.write(frame,stats,'hold',0)
                archive.close('completed')
                with zipfile.ZipFile(path) as z:value=json.loads(z.read('frames.jsonl'))
                self.assertEqual(value['hands'],frame['emulation_input_hands'])
                self.assertIsNone(value['presentation_hands'])
                self.assertEqual(value['hands_source'],'emulation_input_hands')
                self.assertEqual(stats['sides']['right']['tracked_hold_frames'],1)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--tool',required=True);parser.add_argument('--adapter')
    args,rest=parser.parse_known_args();TOOL=args.tool;harness.ADAPTER=args.adapter
    if not args.adapter:
        ControlsCalibrationTest.test_middle_layout_in_actual_runtime_tcp=unittest.skip('adapter not supplied')(ControlsCalibrationTest.test_middle_layout_in_actual_runtime_tcp)
    unittest.main(argv=[__file__,*rest])
