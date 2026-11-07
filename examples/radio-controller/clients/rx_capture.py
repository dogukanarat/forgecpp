#!/usr/bin/env python3
"""Capture IQ samples from radio-controller's streams ([forge].data).

    pip install pyzmq numpy protobuf
    ./rx_capture.py tcp://radio:5555 --seconds 2 --out capture.cs16
    ./rx_capture.py tcp://radio:5555 --topic rx2      # a shared channel (tap_rx)

Messages: [topic][forge.v1.Header][radio.v1.IqBlock][cs16 samples].
Needs the forge-v1 Python package (installed with forgecpp) and radio.v1
(installed with radio-controller); from build directories set PYTHONPATH to
<forgecpp build>/python:<build>/examples/radio-controller/ad936x/python.
"""
import argparse
import sys
import time

import numpy as np
import zmq

from forge.v1 import framing
from radio.v1 import radio_pb2


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("endpoint", nargs="?", default="tcp://127.0.0.1:5555")
    p.add_argument("--topic", default="iq", help="iq (own channels), rx1 or rx2 (shared channels)")
    p.add_argument("--channel", type=int, help="only this channel (iq carries every channel radio-controller listens to)")
    p.add_argument("--seconds", type=float, default=1.0)
    p.add_argument("--out", help="write raw cs16 samples to this file")
    args = p.parse_args()

    sock = zmq.Context.instance().socket(zmq.SUB)
    sock.setsockopt(zmq.SUBSCRIBE, args.topic.encode())
    sock.setsockopt(zmq.RCVTIMEO, 3000)
    sock.connect(args.endpoint)

    out = open(args.out, "wb") if args.out else None
    total, frames, last_seq, lost = 0, 0, None, 0
    deadline = time.monotonic() + args.seconds
    try:
        while time.monotonic() < deadline:
            topic, h, block, blob = framing.parse(sock.recv_multipart(), radio_pb2.IqBlock)
            if last_seq is not None and h.seq != last_seq + 1:
                lost += h.seq - last_seq - 1
            last_seq = h.seq
            if args.channel and block.channel != args.channel:
                continue
            iq = np.frombuffer(blob, dtype="<i2").astype(np.float32).view(np.complex64) / 32768.0
            total += len(iq)
            frames += 1
            if out:
                out.write(blob)
        power = 10 * np.log10(np.mean(np.abs(iq) ** 2) + 1e-20)
        print(f"{frames} blocks, {total} samples, {lost} lost, last RX{block.channel} {power:.1f} dBFS, "
              f"center {block.center_freq / 1e6:.3f} MHz @ {block.sample_rate / 1e6:.3f} MSPS from {h.source}")
    except zmq.Again:
        sys.exit("no data from " + args.endpoint)
    finally:
        if out:
            out.close()


if __name__ == "__main__":
    main()
