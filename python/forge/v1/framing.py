"""forge.v1 framing: [topic][forge.v1.Header][body][blob].

Part of the forge-v1 Python package (with the generated forge/v1/*_pb2.py),
installed into site-packages by forgecpp; applications install their own
messages the same way (radio/v1/*_pb2.py, sa/v1/*_pb2.py, ...):

    from forge.v1 import common_pb2, framing
    from radio.v1 import radio_pb2

    sock.send_multipart(framing.frames("tx", radio_pb2.TxBlock(format="cs16"), iq.tobytes()))
    topic, header, block, blob = framing.parse(sock.recv_multipart(), radio_pb2.IqBlock)

From a build directory: PYTHONPATH=<build>/python (and the application's).
Needs: protobuf (pyzmq for the sockets).
"""
import os
import socket
import time

from forge.v1 import common_pb2

__all__ = ["frames", "parse"]

_seq = {}


def frames(topic, body, blob=None, source="python-client"):
    """Multipart frames for a forge.v1 message (Header.seq counts per topic)."""
    h = common_pb2.Header()
    h.source = source
    h.host = socket.gethostname()
    h.pid = os.getpid()
    h.topic = topic
    _seq[topic] = _seq.get(topic, 0) + 1
    h.seq = _seq[topic]
    h.time_unix_ns = time.time_ns()
    h.type = body.DESCRIPTOR.full_name
    parts = [topic.encode(), h.SerializeToString(), body.SerializeToString()]
    if blob is not None:
        parts.append(blob)
    return parts


def parse(parts, body_type):
    """(topic, Header, body, blob) from received frames."""
    h = common_pb2.Header()
    h.ParseFromString(parts[1])
    body = body_type()
    body.ParseFromString(parts[2])
    blob = parts[3] if len(parts) > 3 else b""
    return parts[0].decode(), h, body, blob
