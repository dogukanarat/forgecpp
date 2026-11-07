"""forge.v1 framing for Python clients: [topic][Header][body][blob].

Needs the generated modules (forge/v1/*_pb2.py, radio/v1/*_pb2.py) on the
path. They are installed with forgecpp (share/forgecpp/python) and
radio-controller (share/radio-controller/python); set FORGE_PYTHONPATH
(colon separated) when they are elsewhere, e.g. in a build directory.

    pip install pyzmq numpy protobuf
"""
import os
import socket
import sys
import time

_here = os.path.dirname(os.path.abspath(__file__))
for _p in (os.environ.get("FORGE_PYTHONPATH", "").split(":") +
           [os.path.join(_here, "..", "python"),            # share/radio-controller/python
            os.path.join(_here, "..", "..", "forgecpp", "python"),
            "/usr/share/forgecpp/python", "/usr/share/radio-controller/python",
            "/usr/local/share/forgecpp/python", "/usr/local/share/radio-controller/python"]):
    if _p and os.path.isdir(_p) and _p not in sys.path:
        sys.path.append(_p)

try:
    from forge.v1 import common_pb2, control_pb2  # noqa: E402
    from radio.v1 import radio_pb2  # noqa: E402
except ImportError as e:  # pragma: no cover
    sys.exit(f"{e}: generated protobuf modules not found; set FORGE_PYTHONPATH (see forge_msg.py)")

_seq = {}


def frames(topic, body, blob=None, source="python-client"):
    """Multipart frames for a forge.v1 message."""
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
