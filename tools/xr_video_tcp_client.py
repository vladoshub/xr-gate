#!/usr/bin/env python3
"""Read the existing XR_STEREO_VIDEO_V1 binary TCP stream without dependencies.

read_frame() returns metadata and raw left/right bytes for an application.
The CLI discards image bytes and reports actual receive FPS and throughput.
"""

import argparse
import socket
import struct
import sys
import time


NET_HEADER = struct.Struct("<8s6I3Q")
FRAME_HEADER = struct.Struct("<2I4Q2q10I")
MAX_IMAGE_BYTES = 64 * 1024 * 1024


def recv_exact(sock, size):
    data = bytearray(size)
    view = memoryview(data)
    offset = 0
    while offset < size:
        received = sock.recv_into(view[offset:])
        if not received:
            raise EOFError("XR video disconnected")
        offset += received
    return data


def read_frame(sock):
    net = NET_HEADER.unpack(recv_exact(sock, NET_HEADER.size))
    magic, version, header_size, frame_header_size, left_size, right_size, _, sequence, source_ns, publish_ns = net
    if magic != b"XSVNET1\0" or version != 1 or header_size != NET_HEADER.size or frame_header_size != FRAME_HEADER.size:
        raise ValueError("unsupported XR video TCP header")
    if left_size + right_size > MAX_IMAGE_BYTES:
        raise ValueError("oversized XR video frame")
    header = FRAME_HEADER.unpack(recv_exact(sock, FRAME_HEADER.size))
    (version, size, frame_sequence, timestamp_ns, frame_source_ns, frame_publish_ns,
     left_ns, right_ns, width, height, stride_left, stride_right, pixel_format, layout,
     frame_left_size, frame_right_size, flags, _) = header
    bpp = {1: 1, 2: 3, 3: 3}.get(pixel_format, 0)
    if (version != 1 or size != FRAME_HEADER.size or layout != 1 or not width or not height or not bpp
            or sequence != frame_sequence or source_ns != frame_source_ns or publish_ns != frame_publish_ns
            or frame_left_size != left_size or frame_right_size != right_size
            or stride_left < width * bpp or stride_right < width * bpp
            or left_size != stride_left * height or right_size != stride_right * height):
        raise ValueError("invalid XR stereo frame metadata")
    metadata = {"sequence": sequence, "timestamp_ns": timestamp_ns, "source_timestamp_ns": source_ns,
                "publish_timestamp_ns": publish_ns, "left_timestamp_ns": left_ns, "right_timestamp_ns": right_ns,
                "width": width, "height": height, "stride_left": stride_left, "stride_right": stride_right,
                "pixel_format": pixel_format, "flags": flags}
    return metadata, recv_exact(sock, left_size), recv_exact(sock, right_size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45700)
    parser.add_argument("--frames", type=int, default=0, help="stop after N frames; 0 streams until Ctrl+C")
    args = parser.parse_args()
    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.settimeout(None)
        total = count = byte_count = 0
        started = time.monotonic()
        while args.frames <= 0 or total < args.frames:
            metadata, left, right = read_frame(sock)
            total += 1
            count += 1
            byte_count += NET_HEADER.size + FRAME_HEADER.size + len(left) + len(right)
            now = time.monotonic()
            elapsed = now - started
            if elapsed >= 1 or (args.frames > 0 and total >= args.frames):
                print(f"seq={metadata['sequence']} size={metadata['width']}x{metadata['height']} per eye "
                      f"format={metadata['pixel_format']} fps={count / max(elapsed, 1e-9):.1f} "
                      f"MB/s={byte_count / max(elapsed, 1e-9) / 1e6:.2f}", flush=True)
                count = byte_count = 0
                started = now


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, EOFError, ValueError) as error:
        print(f"xr_video_tcp_client: {error}", file=sys.stderr)
        sys.exit(1)
