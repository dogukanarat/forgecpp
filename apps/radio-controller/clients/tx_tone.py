#!/usr/bin/env python3
"""Send a test tone to radio-controller's TX input (ZeroMQ PUSH).

    pip install pyzmq numpy
    ./tx_tone.py tcp://radio:5556 --offset 1e6 --rate 30.72e6            # cyclic: repeated by the hardware
    ./tx_tone.py tcp://radio:5556 --offset 1e6 --rate 30.72e6 --stream 5  # stream for 5 seconds
    ./tx_tone.py tcp://radio:5556 --offset=-2e6                          # negative offsets need "="

TX must be enabled first:  radioctl.py tcp://radio:5557 set tx_enabled true
"""
import argparse
import json
import time

import numpy as np
import zmq


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("endpoint", nargs="?", default="tcp://127.0.0.1:5556")
    p.add_argument("--offset", type=float, default=1e6, help="tone offset from the TX LO in Hz")
    p.add_argument("--rate", type=float, default=30.72e6, help="sample rate the radio runs at")
    p.add_argument("--amplitude", type=float, default=0.5)
    p.add_argument("--stream", type=float, metavar="SECONDS", help="stream instead of a cyclic buffer")
    args = p.parse_args()

    # A whole number of periods so the cyclic buffer loops without a phase jump.
    periods = max(1, round(abs(args.offset) * 8192 / args.rate))
    n = round(periods * args.rate / abs(args.offset)) if args.offset else 8192
    t = np.arange(n)
    tone = args.amplitude * np.exp(2j * np.pi * args.offset * t / args.rate)
    iq = np.empty(2 * n, dtype="<i2")
    iq[0::2] = np.round(tone.real * 32767)
    iq[1::2] = np.round(tone.imag * 32767)

    sock = zmq.Context.instance().socket(zmq.PUSH)
    sock.connect(args.endpoint)
    time.sleep(0.2)  # let the connection come up

    if args.stream is None:
        sock.send_multipart([b"tx", json.dumps({"format": "cs16", "cyclic": True}).encode(), iq.tobytes()])
        print(f"cyclic tone at {args.offset / 1e3:+.1f} kHz ({n} samples) sent")
    else:
        header = json.dumps({"format": "cs16"}).encode()
        end = time.monotonic() + args.stream
        sent = 0
        while time.monotonic() < end:
            sock.send_multipart([b"tx", header, iq.tobytes()])  # blocks when the radio's queue is full
            sent += n
        print(f"streamed {sent} samples")
    sock.close(linger=2000)


if __name__ == "__main__":
    main()
