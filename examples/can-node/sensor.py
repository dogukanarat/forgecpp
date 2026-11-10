#!/usr/bin/env python3
"""A sensor node for the can-node example, on a real or virtual CAN bus.

    python3 sensor.py vcan0 [--node-id 5] [--max 70]

Sends a temperature frame (id 0x185, int16 little endian, 0.01 degC) every
0.5 s, a slow ramp from 20 degC up to --max and back, prints the node's
heartbeats, and asks the node for its info over ISO-TP (the same commands as
forgectl, through forge.serial.Client.isotp). Linux only (SocketCAN);
PYTHONPATH=<build>/python for the forge package.
"""
import argparse
import math
import socket
import struct
import time

from forge import messages_pb2 as m, serial

CAN_FRAME = struct.Struct("=IB3x8s")  # struct can_frame

parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
parser.add_argument("interface")
parser.add_argument("--node-id", type=int, default=5)
parser.add_argument("--max", type=float, default=70.0)
args = parser.parse_args()

bus = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
bus.bind((args.interface,))
bus.settimeout(0.5)

node = serial.Client.isotp(args.interface, tx_id=0x700, rx_id=0x701, timeout_ms=2000)
info = node.call(m.info, m.info_request(), m.info_reply)
print(f"node: {info.name} {info.version}, up {info.uptime_s} s")

start = time.monotonic()
while True:
    t = time.monotonic() - start
    celsius = 20 + (args.max - 20) * (1 - math.cos(t / 20)) / 2
    bus.send(CAN_FRAME.pack(0x185, 2, struct.pack("<h", round(celsius * 100)).ljust(8, b"\0")))
    try:
        can_id, size, data = CAN_FRAME.unpack(bus.recv(16))
        if can_id == 0x700 + args.node_id:
            state, uptime = data[0], int.from_bytes(data[1:5], "little")
            print(f"{celsius:6.2f} degC sent; heartbeat: state {state}, up {uptime} s")
    except TimeoutError:
        print(f"{celsius:6.2f} degC sent; no heartbeat")
