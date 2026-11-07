#!/usr/bin/env python3
"""Send numbered UDP packets through qam-modem and count what comes back.

Runs on the remote PC. The modem puts every packet it receives on the air
(16QAM), demodulates its own transmission and sends the packet back to the
sender: this script measures throughput, packet loss and round-trip time.

    python3 echo.py <board> --rate 2.0 --size 1400 --seconds 30

Packet: b"ECHO", sequence (u32), send time (u64 ns), then bytes derived from
the sequence (checked on the way back). Standard library only.
"""
import argparse
import socket
import struct
import sys
import time

MAGIC = b"ECHO"
HEAD = struct.Struct(">4sIQ")


def body(seq, size):
    out = bytearray(size - HEAD.size)
    x = (seq * 2654435761 + 1) & 0xFFFFFFFF
    for i in range(len(out)):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = x & 0xFF
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="board running qam-modem")
    ap.add_argument("--port", type=int, default=6000, help="modem [modem].udp_listen port (6000)")
    ap.add_argument("--rate", type=float, default=2.0, help="offered load, Mbit/s of UDP payload (2.0)")
    ap.add_argument("--size", type=int, default=1400, help="payload bytes per packet (1400, at most max_payload)")
    ap.add_argument("--seconds", type=float, default=10, help="test length (10)")
    ap.add_argument("--wait", type=float, default=1.0, help="wait for late packets after the end (1 s)")
    ap.add_argument("--min-mbps", type=float, default=0, help="exit 1 if the received rate is lower (for scripts)")
    ap.add_argument("--max-loss", type=float, default=1.0, help="exit 1 if more packets are lost (fraction)")
    a = ap.parse_args()
    if a.size < HEAD.size:
        sys.exit(f"--size must be at least {HEAD.size}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    sock.setblocking(False)
    dest = (socket.gethostbyname(a.host), a.port)

    bodies = {}  # payload per sequence (cached: the generator is slow in Python)
    interval = a.size * 8 / (a.rate * 1e6)
    sent = got = bad = dup = 0
    seen = set()
    rtts = []
    start = time.monotonic()
    next_send = start
    report = start + 1
    last_got, last_bytes, bytes_in = 0, 0, 0
    end = start + a.seconds

    def receive():
        nonlocal got, bad, dup, bytes_in
        while True:
            try:
                data, _ = sock.recvfrom(65536)
            except BlockingIOError:
                return
            if len(data) < HEAD.size:
                bad += 1
                continue
            magic, seq, t_ns = HEAD.unpack_from(data)
            if magic != MAGIC or data[HEAD.size:] != bodies.get(seq % 256, b"") or len(data) != a.size:
                bad += 1
                continue
            if seq in seen:
                dup += 1
                continue
            seen.add(seq)
            got += 1
            bytes_in += len(data)
            rtts.append((time.monotonic_ns() - t_ns) / 1e6)

    print(f"-> {dest[0]}:{dest[1]}  {a.rate} Mbit/s offered, {a.size} B packets, {a.seconds} s")
    print(" time   sent    recv  missing%  rx Mbit/s   rtt ms (avg/max)   (missing: lost or still on the way)")
    while True:
        now = time.monotonic()
        if now >= end + a.wait:
            break
        if now < end and now >= next_send:
            k = sent % 256
            if k not in bodies:
                bodies[k] = body(k, a.size)
            pkt = HEAD.pack(MAGIC, sent, time.monotonic_ns()) + bodies[k]
            try:
                sock.sendto(pkt, dest)
                sent += 1
            except BlockingIOError:
                pass
            next_send += interval
            if next_send < now - 0.1:  # far behind (the host was busy): do not burst
                next_send = now
            continue
        receive()
        if now >= report:
            window = rtts[-1000:]
            rtt = f"{sum(window) / len(window):6.1f}/{max(window):6.1f}" if window else "     -/     -"
            lost = 100 * (1 - got / sent) if sent else 0
            mbps = (bytes_in - last_bytes) * 8 / 1e6
            print(f"{now - start:5.0f} {sent:7d} {got:7d} {lost:9.2f} {mbps:10.3f}   {rtt}")
            last_bytes = bytes_in
            report += 1
        time.sleep(min(0.0005, max(0.0, next_send - time.monotonic())))

    lost = sent - got
    duration = a.seconds
    mbps = bytes_in * 8 / duration / 1e6
    rtts.sort()
    p50 = rtts[len(rtts) // 2] if rtts else float("nan")
    p99 = rtts[int(len(rtts) * 0.99)] if rtts else float("nan")
    print(f"\nsent {sent}, received {got}, lost {lost} ({100 * lost / max(sent, 1):.3f} %), "
          f"corrupted {bad}, duplicates {dup}")
    print(f"throughput {mbps:.3f} Mbit/s (UDP payload), rtt median {p50:.1f} ms, 99 % {p99:.1f} ms")
    ok = mbps >= a.min_mbps and lost / max(sent, 1) <= a.max_loss
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
