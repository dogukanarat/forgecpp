#!/usr/bin/env python3
"""Send a test tone to radio-controller's TX input (ZeroMQ PUSH).

    pip install pyzmq numpy protobuf
    ./tx_tone.py tcp://radio:5556 --offset 1e6 --rate 30.72e6            # cyclic: repeated by the hardware
    ./tx_tone.py tcp://radio:5556 --offset 1e6 --rate 30.72e6 --stream 5  # stream for 5 seconds
    ./tx_tone.py tcp://radio:5556 --offset=-2e6                          # negative offsets need "="

TX must be enabled first:  forgectl -e tcp://radio:5557 set tx_enabled true
Messages: [b"tx"][forge.v1.Header][radio.v1.TxBlock][cs16 samples].
Needs the forge-v1 Python package (installed with forgecpp) and radio.v1
(installed with radio-controller); from build directories set PYTHONPATH to
<forgecpp build>/python:<build>/examples/common/ad936x/python.
"""
import argparse
import time

import numpy as np
import zmq

from forge.v1 import framing
from radio.v1 import radio_pb2


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
        block = radio_pb2.TxBlock(format="cs16", cyclic=True)
        sock.send_multipart(framing.frames("tx", block, iq.tobytes(), source="tx_tone"))
        print(f"cyclic tone at {args.offset / 1e3:+.1f} kHz ({n} samples) sent")
    else:
        block = radio_pb2.TxBlock(format="cs16")
        end = time.monotonic() + args.stream
        sent = 0
        while time.monotonic() < end:
            # blocks when the radio's queue is full
            sock.send_multipart(framing.frames("tx", block, iq.tobytes(), source="tx_tone"))
            sent += n
        print(f"streamed {sent} samples")
    sock.close(linger=2000)


if __name__ == "__main__":
    main()
