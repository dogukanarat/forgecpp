#!/usr/bin/env python3
"""Watch spectrum-analyzer's live trace (stream "spectrum" on [forge].data).

    pip install pyzmq protobuf
    ./spectrum_watch.py tcp://board:5566            # strongest signal per sweep
    ./spectrum_watch.py tcp://board:5566 --json     # every sweep as JSON

Messages: [b"spectrum"][forge.stream_header][sa.spectrum][levels]: the levels
are `points` float32 values, little endian; level i is at
start_hz + i * (stop_hz - start_hz) / (points - 1). Needs forgecpp's Python
package "forge" and "sa" (installed with spectrum-analyzer); from build
directories set PYTHONPATH to
<forgecpp build>/python:<build>/examples/spectrum-analyzer/python.
"""
import argparse
import array
import json
import sys

import zmq
from google.protobuf import json_format

from forge import framing
from sa import spectrum_pb2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("endpoint", nargs="?", default="tcp://127.0.0.1:5566")
    ap.add_argument("--count", type=int, default=0, help="stop after this many sweeps (0 = run until Ctrl-C)")
    ap.add_argument("--json", action="store_true", help="print each sweep as JSON")
    args = ap.parse_args()

    sock = zmq.Context.instance().socket(zmq.SUB)
    sock.setsockopt(zmq.SUBSCRIBE, b"spectrum")
    sock.setsockopt(zmq.RCVTIMEO, 5000)
    sock.connect(args.endpoint)
    n = 0
    try:
        while not args.count or n < args.count:
            _, h, sp, blob = framing.parse(sock.recv_multipart(), spectrum_pb2.spectrum)
            levels = array.array("f", blob)  # float32; every target we build for is little endian
            n += 1
            if args.json:
                print(json.dumps(dict(json_format.MessageToDict(sp), levels=levels.tolist())))
                continue
            if not levels:
                continue
            i = max(range(len(levels)), key=levels.__getitem__)
            f = sp.start_hz + i * (sp.stop_hz - sp.start_hz) / max(sp.points - 1, 1)
            print(f"#{h.seq:<6} {sp.start_hz / 1e6:.3f}-{sp.stop_hz / 1e6:.3f} MHz RBW {sp.rbw_hz / 1e3:.1f} kHz "
                  f"({sp.sweep_mode})  peak {f / 1e6:.4f} MHz {levels[i]:.1f} {sp.unit}"
                  f"{'  OVERLOAD' if sp.overload else ''}")
    except zmq.Again:
        sys.exit("no spectrum from " + args.endpoint)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
