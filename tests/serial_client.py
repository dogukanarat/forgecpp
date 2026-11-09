"""forge.serial (Python) against a forgecpp program with [forge].serial on a pty.

    serial_client.py <build>/python <program>
"""
import os
import pty
import subprocess
import sys
import time

sys.path.insert(0, sys.argv[1])
from forge import c2, messages_pb2 as m, serial  # noqa: E402

assert serial.crc16(b"123456789") == 0x29B1
for payload in (b"", b"\x00", b"\x00" * 300, bytes(range(1, 256)) * 2, bytes(range(256)) * 3):
    frame = serial.encode_frame(payload)
    assert 0 not in frame[1:-1] and serial.decode_frame(frame[1:-1]) == payload
f = serial.Framing(frame_size=32)
assert f.receive(b"\x13\x37" + f.encode(b"x" * 500)) == [b"x" * 500]

peer, other = pty.openpty()
device = os.ttyname(other)
os.close(other)
program = subprocess.Popen([sys.argv[2], "-D", "forge.serial=" + device, "-D", "http.enabled=false"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
try:
    link = serial.Client(fd=peer, timeout_ms=500)
    for _ in range(40):  # until the program has opened its end
        try:
            info = link.call(m.info, m.info_request(), m.info_reply)
            break
        except (TimeoutError, OSError):
            time.sleep(0.25)
    else:
        sys.exit("no answer on " + device)
    assert info.name == "panel-test", info
    pages = link.call_parts(m.controls, m.controls_request(), m.controls_reply)
    assert sum(len(p.controls) for p in pages) > 0
    try:
        link.call(0x7777, m.info_request(), m.info_reply)
        sys.exit("an unknown command was answered")
    except c2.Refused as e:
        assert e.code == 2, e  # ENOENT
    print("forge.serial: info, controls in", len(pages), "parts, refusal: ok")
finally:
    program.terminate()
    program.wait(10)
