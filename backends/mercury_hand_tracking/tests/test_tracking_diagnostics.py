#!/usr/bin/env python3
"""Compile the standalone CSV logger and verify paired rows and unavailable data."""
import csv
import math
from pathlib import Path
import subprocess
import tempfile

backend = Path(__file__).resolve().parents[1]
include = backend / 'monado_overlay/src/xrt/tracking/hand/mercury'
with tempfile.TemporaryDirectory(prefix='mercury-diag-test-') as td:
    temp = Path(td)
    binary = temp / 'logger-test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(include), str(backend / 'tests/tracking_diagnostics_smoke.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), 'disabled'], check=True)
    target = temp / 'tracking.csv'
    subprocess.run([str(binary), str(target)], check=True)
    with target.open() as handle:
        reader = csv.DictReader(handle)
        fields = reader.fieldnames
        rows = list(reader)
    assert fields is not None and len(set(fields)) == len(fields)
    assert len(rows) == 62
    assert all(None not in row and all(v is not None for v in row.values()) for row in rows)
    for i in range(31):
        left, right = rows[2*i:2*i+2]
        assert left['hand_idx'] == '0' and right['hand_idx'] == '1'
        assert left['frame'] == right['frame'] == str(i+1)
        assert left['timestamp_ns'] == right['timestamp_ns'] == str(9007199254740993 + i*33333333)
    assert float(rows[0]['v0_is_hand_probability']) == 0.975
    assert float(rows[0]['v0_confidence_xy_mean']) == 0.4
    assert rows[0]['v0_model_ran'] == '1'
    assert rows[-2]['optimizer_ran'] == '0' and rows[-2]['v0_model_ran'] == '0'
    assert math.isnan(float(rows[-2]['palm_z']))
    assert math.isnan(float(rows[-2]['v0_is_hand_probability']))
    assert int(rows[-2]['rejection_mask']) == 33
    assert int(rows[-2]['final_active']) == 0
    assert not (temp / 'disabled').exists()
print('CSV logger: 31 paired frames, exact timestamps, rejection flags, and stale-data checks passed')
