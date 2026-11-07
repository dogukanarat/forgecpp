#!/usr/bin/env python3
"""Command line client for radio-controller's ZeroMQ control socket.

    ./radioctl.py tcp://radio:5557 state
    ./radioctl.py tcp://radio:5557 set rx_lo_mhz 2437
    ./radioctl.py tcp://radio:5557 set gain_mode manual
    ./radioctl.py tcp://radio:5557 press tx_stop
    ./radioctl.py tcp://radio:5557 alarms
    ./radioctl.py tcp://radio:5557 ack all

Works with every forgecpp daemon that calls forge::zmq::serve_application().
"""
import argparse
import json
import sys

import zmq


def parse_value(text):
    try:
        return json.loads(text)
    except ValueError:
        return text  # bare words are strings


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("endpoint")
    p.add_argument("cmd", choices=["ping", "info", "ui", "state", "get", "set", "press", "alarms", "ack"])
    p.add_argument("id", nargs="?")
    p.add_argument("value", nargs="?")
    p.add_argument("--timeout", type=float, default=3.0)
    args = p.parse_args()

    req = {"cmd": args.cmd}
    if args.id is not None:
        req["id"] = args.id
    if args.value is not None:
        req["value"] = parse_value(args.value)

    sock = zmq.Context.instance().socket(zmq.REQ)
    sock.setsockopt(zmq.LINGER, 0)
    sock.setsockopt(zmq.RCVTIMEO, int(args.timeout * 1000))
    sock.connect(args.endpoint)
    sock.send_string(json.dumps(req))
    try:
        reply = json.loads(sock.recv())
    except zmq.Again:
        sys.exit(f"no reply from {args.endpoint}")
    print(json.dumps(reply, indent=2))
    sys.exit(0 if reply.get("ok") else 1)


if __name__ == "__main__":
    main()
