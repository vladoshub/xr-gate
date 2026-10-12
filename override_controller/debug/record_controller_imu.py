#!/usr/bin/env python3
"""Record ControllerInputV3 IMU input to the runtime without changing tracking.

CSV contains one row per sensor sample (or a status-only row when there are no
samples). Each batch has one provider quaternion; it is repeated on its sample
rows, with its own orientation timestamp. Gyro/accel remain in sensor axes.
"""

import argparse
import csv
import json
import mmap
import os
from pathlib import Path
import struct
import time

FRAME = struct.Struct('<IIQQQQIIII')
SLOT = struct.Struct('<QQQQII')
RING = struct.Struct('<8s7IQ')
IMU = struct.Struct('<IIIIQQQQ4f3ff')
SAMPLE = struct.Struct('<QQ6fII')
FRAME_SIZE, SIDE_SIZE, IMU_OFFSET = 1432, 688, 400

COLUMNS = (
    'recorded_ns frame_sequence frame_timestamp_ns frame_source_ns reset_counter '
    'frame_flags active_mask connected_mask reader_skipped_frames side '
    'controller_status controller_flags source_type device_id buttons '
    'imu_status capability_flags data_flags sample_count imu_sequence '
    'latest_sample_ns latest_device_ticks orientation_ns qx qy qz qw '
    'epoch dropped_batches device_ticks_per_second sample_index sample_ns '
    'sample_device_ticks gx gy gz ax ay az sample_flags'
).split()


def decode_payload(payload, recorded_ns, skipped):
    if len(payload) < FRAME_SIZE:
        raise ValueError('ControllerInputV3 payload is truncated')
    version, size, seq, ts, source, reset, flags, active, connected, _ = FRAME.unpack_from(payload)
    if version != 3 or size != FRAME_SIZE:
        raise ValueError(f'Expected ControllerInputV3 ({FRAME_SIZE} bytes), got V{version} ({size})')
    common = [recorded_ns, seq, ts, source, reset, flags, active, connected, skipped]
    rows = []
    for index, side in enumerate(('left', 'right')):
        base = FRAME.size + index * SIDE_SIZE
        status, _, controller_flags, source_type = struct.unpack_from('<4I', payload, base)
        buttons = struct.unpack_from('<Q', payload, base + 16)[0]
        device = payload[base + 336:base + 400].split(b'\0', 1)[0].decode('utf-8', 'replace')
        imu_base = base + IMU_OFFSET
        imu = IMU.unpack_from(payload, imu_base)
        count = imu[3]
        if count > 4:
            raise ValueError(f'Invalid {side} IMU sample count: {count}')
        diagnostics = struct.unpack_from('<4I', payload, imu_base + 272)
        epoch, dropped, ticks_per_second = diagnostics[:3] if imu[1] & 128 else ('', '', '')
        prefix = common + [side, status, controller_flags, source_type, device, buttons]
        prefix += list(imu[:12]) + [epoch, dropped, ticks_per_second]
        for sample_index in range(max(1, count)):
            if count:
                sample = SAMPLE.unpack_from(payload, imu_base + IMU.size + sample_index * SAMPLE.size)
                values = [sample_index] + list(sample[:9])
            else:
                values = [-1] + [''] * 9
            rows.append(prefix + values)
    return rows


def read_slot(mapping, stream, sequence):
    offset = stream['header_size'] + ((sequence - 1) % stream['slot_count']) * stream['slot_stride']
    if offset + stream['slot_stride'] > len(mapping):
        raise ValueError('Controller SHM ring is shorter than its registry layout')
    first = SLOT.unpack_from(mapping, offset)
    if first[0] != first[1] or first[0] & 1:
        return None
    payload_size = first[4]
    if payload_size < FRAME_SIZE or payload_size > stream['payload_size']:
        raise ValueError(f'Invalid controller payload size: {payload_size}')
    start = offset + stream['slot_header_size']
    payload = bytes(mapping[start:start + payload_size])
    second = SLOT.unpack_from(mapping, offset)
    if first != second or struct.unpack_from('<Q', payload, 8)[0] != sequence:
        return None
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--registry', default=os.environ.get('CONTROLLER_INPUT_REGISTRY',
                        os.environ.get('XR_CONTROLLER_INPUT_REGISTRY', '/tmp/tracking_streams.json')))
    parser.add_argument('--stream', default=os.environ.get('CONTROLLER_INPUT_STREAM', 'controller_input'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--duration-sec', type=float, default=120.0, help='0 records until Ctrl+C')
    parser.add_argument('--poll-ms', type=float, default=2.0)
    args = parser.parse_args()
    if args.poll_ms <= 0 or args.duration_sec < 0:
        parser.error('poll-ms must be positive and duration-sec nonnegative')
    with open(args.registry, encoding='utf-8') as handle:
        streams = json.load(handle).get('streams', {})
    if args.stream not in streams:
        raise ValueError(f'Stream {args.stream!r} not found in {args.registry}; start override_controller first')
    entry = streams[args.stream]
    defaults = dict(header_size=4096, slot_count=1024, slot_stride=128 + FRAME_SIZE,
                    slot_header_size=128, payload_size=FRAME_SIZE)
    stream = {key: int(entry.get(key, default)) for key, default in defaults.items()}
    if (min(stream.values()) <= 0 or stream['slot_header_size'] < SLOT.size or
            stream['payload_size'] < FRAME_SIZE or stream['header_size'] < RING.size or
            stream['slot_stride'] < stream['slot_header_size'] + stream['payload_size']):
        raise ValueError('Invalid controller ring layout in registry')
    shm = Path('/dev/shm') / str(entry.get('shm_name', args.stream)).lstrip('/')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    last = None
    frame_count = row_count = missed = 0
    start = time.monotonic()
    with shm.open('rb') as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as mapping:
        with args.output.open('w', newline='', encoding='utf-8') as output:
            writer = csv.writer(output)
            writer.writerow(COLUMNS)
            print(f'[controller_imu_record] {shm} -> {args.output}', flush=True)
            try:
                while args.duration_sec == 0 or time.monotonic() - start < args.duration_sec:
                    latest = RING.unpack_from(mapping)[-1]
                    if latest and (last is None or latest != last):
                        if last is not None and latest < last:
                            print('[controller_imu_record] producer sequence reset', flush=True)
                            last = None
                        first = latest if last is None else max(last + 1, latest - stream['slot_count'] + 1)
                        gap = 0 if last is None else max(0, first - last - 1)
                        for sequence in range(first, latest + 1):
                            payload = read_slot(mapping, stream, sequence)
                            if payload is None:
                                break
                            rows = decode_payload(payload, time.monotonic_ns(), gap)
                            writer.writerows(rows)
                            frame_count += 1
                            row_count += len(rows)
                            missed += gap
                            gap = 0
                            last = sequence
                        output.flush()
                    time.sleep(args.poll_ms / 1000)
            except KeyboardInterrupt:
                pass
    print(f'[controller_imu_record] frames={frame_count} rows={row_count} reader_skipped={missed}', flush=True)


if __name__ == '__main__':
    main()
