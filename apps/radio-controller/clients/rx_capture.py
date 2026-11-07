#!/usr/bin/env python3
"""Capture IQ samples from radio-controller's ZeroMQ PUB stream.

    pip install pyzmq numpy
    ./rx_capture.py tcp://radio:5555 --seconds 2 --out capture.cs16

Frames: [b"iq"][header JSON][int16 I/Q pairs, full scale +-32767].
"""
import argparse
import json
import sys
import time

import numpy as np
import zmq


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("endpoint", nargs="?", default="tcp://127.0.0.1:5555")
    p.add_argument("--seconds", type=float, default=1.0)
    p.add_argument("--out", help="write raw cs16 samples to this file")
    args = p.parse_args()

    sock = zmq.Context.instance().socket(zmq.SUB)
    sock.setsockopt(zmq.SUBSCRIBE, b"iq")
    sock.setsockopt(zmq.RCVTIMEO, 3000)
    sock.connect(args.endpoint)

    out = open(args.out, "wb") if args.out else None
    total, frames, last_seq, lost = 0, 0, None, 0
    deadline = time.monotonic() + args.seconds
    try:
        while time.monotonic() < deadline:
            topic, header, payload = sock.recv_multipart()
            h = json.loads(header)
            if last_seq is not None and h["seq"] != last_seq + 1:
                lost += h["seq"] - last_seq - 1
            last_seq = h["seq"]
            iq = np.frombuffer(payload, dtype="<i2").astype(np.float32).view(np.complex64) / 32768.0
            total += len(iq)
            frames += 1
            if out:
                out.write(payload)
        power = 10 * np.log10(np.mean(np.abs(iq) ** 2) + 1e-20)
        print(f"{frames} frames, {total} samples, {lost} frames lost, last frame {power:.1f} dBFS, "
              f"center {h['center_freq'] / 1e6:.3f} MHz @ {h['sample_rate'] / 1e6:.3f} MSPS")
    except zmq.Again:
        sys.exit("no data from " + args.endpoint)
    finally:
        if out:
            out.close()


if __name__ == "__main__":
    main()
