#!/usr/bin/env python3
"""Compile the quality filter; optionally evaluate decisions on an existing tracking CSV."""
import csv
from pathlib import Path
import subprocess
import sys
import tempfile

backend = Path(__file__).resolve().parents[1]
include = backend / 'monado_overlay/src/xrt/tracking/hand/mercury'
with tempfile.TemporaryDirectory(prefix='mercury-quality-test-') as td:
    binary = Path(td) / 'quality-test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(include), str(backend / 'tests/quality_filter_test.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('Quality filter unit checks passed')
    if len(sys.argv) > 1:
        with Path(sys.argv[1]).open() as handle:
            rows = [r for r in csv.DictReader(handle) if float(r['final_active']) == 1]
        inputs = []
        for row in rows:
            active = [float(row[f'v{v}_roi_input_found']) == 1 and
                      float(row[f'v{v}_keypoint_active']) == 1 for v in range(2)]
            means = [float(row[f'v{v}_confidence_xy_mean']) if active[v] else 0.0 for v in range(2)]
            inputs.append(f'{int(active[0])} {int(active[1])} {means[0]} {means[1]} {row["reprojection_error"]}')
        output = subprocess.run([str(binary), '--replay'], input='\n'.join(inputs)+'\n',
                                text=True, capture_output=True, check=True)
        decisions = [int(v) for v in output.stdout.splitlines()]
        assert len(decisions) == len(rows)
        for hand in (0, 1):
            total = sum(int(r['hand_idx']) == hand for r in rows)
            rejected = sum(d for r, d in zip(rows, decisions) if int(r['hand_idx']) == hand)
            print(f'Hand {hand}: {rejected}/{total} previously published poses would be rejected')
        if len(sys.argv) > 2 and sys.argv[2] == '--check-oct12-cases':
            by_frame = {(int(r['frame']), int(r['hand_idx'])):d for r,d in zip(rows,decisions)}
            assert by_frame[717, 0] == 0 and by_frame[718, 0] == 1
            assert by_frame[2240, 0] == 0 and by_frame[2241, 0] == 1 and by_frame[2242, 0] == 1
            assert by_frame[718, 1] == 0 and by_frame[2241, 1] == 0
            print('Both reported left-hand events are rejected before the published rebound')
        print('This evaluates recorded frame decisions, not a new run of the stateful tracker')
