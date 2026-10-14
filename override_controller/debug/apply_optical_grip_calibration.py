#!/usr/bin/env python3
"""Apply a generated optical grip calibration by device_id (Linux).

The wizard fits an absolute replacement mount and records the original config
hash. --replace-existing --write requires a stable candidate, an unchanged
config, patched native components and a stopped runtime.
"""
import argparse
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import tempfile

RUNTIME_BINARIES = (
    "bin/override_controller/override_controller",
    "bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter",
)


def require_stopped(runtime_root):
    """Refuse changes while either configured native executable is running."""
    processes=Path("/proc")
    if not processes.is_dir():
        raise ValueError("This config application tool requires Linux /proc")
    targets={str((runtime_root/path).resolve()) for path in RUNTIME_BINARIES}
    for process in processes.iterdir():
        if not process.name.isdecimal():
            continue
        try:
            executable=os.readlink(process/"exe")
        except OSError:
            continue
        if executable.removesuffix(" (deleted)") in targets:
            raise ValueError("Stop XR Gate with Ctrl+C before applying calibration")


def apply(config_path,candidate_path,runtime_root,write=False,replace_existing=False):
    raw=config_path.read_bytes()
    config=json.loads(raw)
    candidate=json.loads(candidate_path.read_text())
    if candidate.get("schema")!="gearvr_optical_grip_trial_v1":
        raise ValueError("Unknown calibration schema")
    entries=candidate["devices"]
    if len({x["device_id"] for x in entries})!=len(entries):
        raise ValueError("Duplicate device_id in calibration")
    if replace_existing and (candidate.get("status")!="stable_optical_reference" or
            not isinstance(candidate.get("expected_config_sha256"),str) or
            len(candidate["expected_config_sha256"])!=64 or
            any("expected_orientation_offset" not in x for x in entries)):
        raise ValueError("Replacement requires a stable guided calibration with its original config snapshot")
    for relative,marker in [
        ("bin/override_controller/override_controller",b"optical_grip_vectors_revision=1"),
        ("bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter",b"controller_left_yaw_reference_ns=")]:
        binary=runtime_root/relative
        if not binary.is_file() or marker not in binary.read_bytes():
            raise ValueError("Native optical grip/yaw support is required first: "+str(binary))
    changes=[]
    for entry in entries:
        found=[d for d in config.get("devices",[]) if d.get("id")==entry["device_id"]]
        if len(found)!=1 or found[0].get("imu_side")!=entry["side"]:
            raise ValueError("Expected unique device_id "+str(entry["device_id"])+" for "+entry["side"])
        d=found[0]
        transform=d.get("orientation_transform",{})
        basis=transform.get("basis_rotation",{})
        if not transform.get("enabled") or any(transform.get(k,False) for k in
                ("invert_x","invert_y","invert_z")) or any(
                abs(float(basis.get(k,0))-v)>1e-6 for k,v in
                (("rx_deg",-90),("ry_deg",0),("rz_deg",0))):
            raise ValueError("Expected recorded -90 X orientation basis")
        q=[float(x) for x in entry["quaternion_xyzw"]]
        norm=math.sqrt(sum(x*x for x in q))
        if len(q)!=4 or not all(math.isfinite(x) for x in q) or abs(norm-1)>1e-5:
            raise ValueError("Invalid normalized quaternion")
        q=[x/norm for x in q]
        old=d.get("orientation_offset",{})
        if replace_existing and old!=entry["expected_orientation_offset"]:
            # Exact same calibrated offset is handled idempotently below.
            already_same=old.get("enabled") and old.get("multiply_order")=="post" and \
                len(old.get("quaternion_xyzw",[]))==4 and all(
                    abs(float(x)-y)<1e-7 for x,y in zip(old["quaternion_xyzw"],q))
            if not already_same:
                raise ValueError("The active offset changed since guided recording; record again")
        if old.get("enabled"):
            old_q=old.get("quaternion_xyzw",[])
            if old.get("multiply_order")=="post" and len(old_q)==4 and all(
                    abs(float(x)-y)<1e-7 for x,y in zip(old_q,q)):
                continue
            if not replace_existing:
                raise ValueError("Another offset is already active; restore the original config first")
        d["orientation_offset"]={"enabled":True,"multiply_order":"post","quaternion_xyzw":q}
        changes.append(entry["device_id"])
    if not changes:
        return {"status":"already_applied","config":str(config_path)}
    if replace_existing and hashlib.sha256(raw).hexdigest()!=candidate["expected_config_sha256"]:
        raise ValueError("Config changed since guided recording; record again")
    if not write:
        return {"status":"ready","device_ids":changes,"config":str(config_path),"write":False}
    if replace_existing:
        require_stopped(runtime_root)
        backup=config_path.with_name(config_path.name+".before-guided-grip-"+
                                    datetime.now().strftime("%Y%m%d_%H%M%S_%f")+".bak")
    else:
        backup=config_path.with_name(config_path.name+".before-optical-yaw.bak")
    if not backup.exists():
        with backup.open("xb") as f:
            f.write(raw)
        os.chmod(backup,stat.S_IMODE(config_path.stat().st_mode))
    elif backup.read_bytes()!=raw:
        raise ValueError("An original backup already exists and differs from this config: "+str(backup))
    fd,name=tempfile.mkstemp(prefix=config_path.name+".",dir=config_path.parent)
    try:
        with os.fdopen(fd,"w",encoding="utf-8") as f:
            json.dump(config,f,indent=2,ensure_ascii=False,allow_nan=False)
            f.write("\n")
        os.chmod(name,stat.S_IMODE(config_path.stat().st_mode))
        os.replace(name,config_path)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    return {"status":"applied","device_ids":changes,"config":str(config_path),"backup":str(backup)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config",type=Path,required=True)
    p.add_argument("--candidate",type=Path,required=True)
    p.add_argument("--runtime-root",type=Path,default=Path("~/xr-gate-release/xr-gate"))
    p.add_argument("--write",action="store_true")
    p.add_argument("--replace-existing",action="store_true",
                   help="Replace an offset using a stable guided calibration; requires unchanged recording config")
    a=p.parse_args()
    try:
        result=apply(a.config.expanduser().resolve(),a.candidate.expanduser().resolve(),
                     a.runtime_root.expanduser().resolve(),a.write,a.replace_existing)
    except (ValueError,OSError,KeyError,TypeError) as e:
        p.exit(2,"Calibration was not applied: "+str(e)+"\n")
    print(json.dumps(result,ensure_ascii=False))
if __name__=="__main__":
    main()
