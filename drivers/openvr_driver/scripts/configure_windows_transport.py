#!/usr/bin/env python3
"""Set Windows transport defaults without losing case-sensitive SteamVR keys."""
import argparse
import ipaddress
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', type=Path, required=True)
    parser.add_argument('--transport', choices=('tcp', 'udp'), default='tcp')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=45810)
    args = parser.parse_args()
    if args.host != 'localhost':
        ipaddress.IPv4Address(args.host)
    if not 1 <= args.port <= 65535:
        parser.error('TCP port must be 1..65535')
    data = json.loads(args.settings.read_text(encoding='utf-8-sig'))
    config = data['xr_tracking']
    for name in ('runtimePoseTransport', 'runtimeHandTransport', 'runtimeControllerStateTransport'):
        config[name] = args.transport
    config['runtimeTcpHost'] = args.host
    config['runtimeTcpPort'] = args.port
    if args.transport == 'tcp':
        config['runtimeControllerInputHoldMs'] = 0
    args.settings.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
