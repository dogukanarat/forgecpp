"""Streams: [topic][forge.stream_header][body][blob] on ZeroMQ.

Part of forgecpp's Python package (with the generated forge/messages_pb2.py),
installed into site-packages; applications install their own messages the
same way (radio/radio_pb2.py, sa/spectrum_pb2.py, ...):

    from forge import framing
    from radio import radio_pb2

    sock.send_multipart(framing.frames("tx", radio_pb2.tx_block(format="cs16"), iq.tobytes()))
    topic, header, block, blob = framing.parse(sock.recv_multipart(), radio_pb2.iq_block)

From a build directory: PYTHONPATH=<build>/python (and the application's).
Needs: protobuf (pyzmq for the sockets).
"""
import os
import socket
import time

from forge import messages_pb2

__all__ = ["frames", "parse"]

_seq = {}


def frames(topic, body, blob=None, source="python-client"):
    """Multipart frames for one message (seq counts per topic)."""
    h = messages_pb2.stream_header()
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
    """(topic, stream_header, body, blob) from received frames."""
    h = messages_pb2.stream_header()
    h.ParseFromString(parts[1])
    if h.type and h.type != body_type.DESCRIPTOR.full_name:
        raise ValueError(f"message on '{h.topic}' is {h.type}, not {body_type.DESCRIPTOR.full_name}")
    body = body_type()
    body.ParseFromString(parts[2])
    blob = parts[3] if len(parts) > 3 else b""
    return parts[0].decode(), h, body, blob
