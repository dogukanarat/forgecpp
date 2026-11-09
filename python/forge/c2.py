"""Commands on a forgecpp program's [forge].control endpoint (ZeroMQ REQ).

Each ZeroMQ frame is one c2 message: an 8-byte big-endian header
(version, flags, command id, seq, payload length) and a protobuf payload.
The same frames as zephyrus, so this also talks to a device through a
gateway that passes them on.

    from forge import c2, messages_pb2 as m

    link = c2.Client("tcp://127.0.0.1:5555")
    reply = link.call(m.set, m.set_request(id="freq", value="2400"), m.set_reply)  # m.set: msg_id
    for page in link.call_parts(m.alarms, m.alarms_request(), m.alarms_reply):
        ...

A refused command raises c2.Refused with the status the program sent. The
same calls on a serial line: forge.serial.Client.
"""
import struct

__all__ = ["Client", "Calls", "Refused", "VERSION", "REQ", "RSP", "PUSH", "ACK", "NACK", "PROTO", "MORE"]

VERSION = 1
REQ, RSP, PUSH, ACK, NACK, PROTO, MORE = 1, 2, 4, 8, 16, 32, 64
_HEADER = struct.Struct(">BBHHH")


class Refused(Exception):
    """A NACK: `status` is the 32-bit status (bit 31 | module << 16 | code)."""

    def __init__(self, cmd_id, status):
        self.cmd_id, self.status = cmd_id, status
        self.module, self.code = (status >> 16) & 0x7FFF, status & 0xFFFF
        super().__init__(f"command 0x{cmd_id:04x} refused: module {self.module}, code {self.code}")


class Calls:
    """call() and call_parts() on top of _request(cmd_id, payload) -> [(flags, payload)]."""

    def call(self, cmd_id, request, reply_type):
        """One reply (an ACK gives an empty reply)."""
        parts = list(self.call_parts(cmd_id, request, reply_type))
        return parts[0] if parts else reply_type()

    def call_parts(self, cmd_id, request, reply_type):
        """Every part of a reply that comes in pages."""
        out = []
        for flags, payload in self._request(cmd_id, request.SerializeToString()):
            if flags & ACK:  # success without data; ends a list in parts
                if not out:
                    out.append(reply_type())
                continue
            msg = reply_type()
            msg.ParseFromString(payload)
            out.append(msg)
        return out


class Client(Calls):
    def __init__(self, endpoint, timeout_ms=2000, context=None):
        import zmq  # only here: forge.serial works without pyzmq

        self._zmq = zmq
        self.endpoint, self.timeout_ms = endpoint, timeout_ms
        self._ctx = context or zmq.Context.instance()
        self._seq = 0
        self._sock = None

    def _request(self, cmd_id, payload):
        zmq = self._zmq
        if self._sock is None:
            self._sock = self._ctx.socket(zmq.REQ)
            self._sock.setsockopt(zmq.LINGER, 0)
            self._sock.connect(self.endpoint)
        self._seq = (self._seq + 1) & 0xFFFF
        self._sock.send(_HEADER.pack(VERSION, REQ | PROTO, cmd_id, self._seq, len(payload)) + payload)
        if not self._sock.poll(self.timeout_ms):
            self._sock.close()  # a REQ socket without its answer cannot send again
            self._sock = None
            raise TimeoutError(f"{self.endpoint}: no answer to command 0x{cmd_id:04x}")
        answers = []
        for frame in self._sock.recv_multipart():
            _, flags, cid, seq, length = _HEADER.unpack_from(frame)
            body = frame[_HEADER.size:_HEADER.size + length]
            if cid != cmd_id or seq != self._seq:
                raise ValueError(f"answer to 0x{cid:04x}/{seq}, asked 0x{cmd_id:04x}/{self._seq}")
            if flags & NACK:
                raise Refused(cmd_id, struct.unpack(">I", body)[0])
            answers.append((flags, body))
        return answers
